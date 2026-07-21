#include "services/wifi_setup.h"

#include <Arduino.h>
#include <WiFi.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include <esp_wifi.h>

#include "config.h"
#include "core/coordinates.h"
#include "core/factory_erase.h"
#include "core/http_request.h"
#include "core/network_work_intent.h"
#include "core/portal_auth.h"
#include "core/portal_secrets.h"
#include "core/portal_session.h"
#include "core/provision_button.h"
#include "core/time_math.h"
#include "core/url_form.h"
#include "services/adsb_worker.h"  // PLANE_RADAR_ADSB_WORKER gate (Arduino-free)
#include "services/config_portal.h"
#include "services/device_identity.h"
#include "services/provision_marker.h"
#include "services/radar_location.h"
#include "services/settings_events.h"
#include "services/timekeeper.h"
#include "services/wifi_credentials.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

// Forward declaration: config_portal binds its save callback to this global-scope
// trampoline, which is defined at the bottom of this file and reaches the
// anonymous-namespace controller state in the same translation unit.
services::portal::SaveOutcome portalSaveTrampoline(const core::HttpRequest& req,
                                                   void* ctx);

namespace {

using core::PortalAck;
using core::PortalActions;
using core::PortalInput;
using core::PortalState;

constexpr core::PortalSessionPolicy kPolicy = core::kDefaultPortalSessionPolicy;
constexpr core::ProvisionButtonPolicy kBtnPolicy = config::kButtonPolicy;

// ===========================================================================
// Optional ADS-B network-worker deferral (compiled ONLY for the worker build).
// ===========================================================================
// In the default firmware PLANE_RADAR_ADSB_WORKER is 0, so none of this state,
// the altered button latching, or the pause/resume hooks exist -- Configure and
// factory Erase keep their exact immediate behavior with no object/ELF delta.
#if PLANE_RADAR_ADSB_WORKER
// Main-registered hooks (null until wifiSetNetworkWorkHooks runs).
WifiNetworkWorkHooks s_nw_hooks{};
// Deferred Configure/Erase latch (pure core; Configure idempotent, Erase wins).
core::NetworkWorkIntentState s_nw_intent{};
bool s_nw_intent_initialized = false;

void nwRequestPause() {
  if (s_nw_hooks.request_pause != nullptr) {
    s_nw_hooks.request_pause(s_nw_hooks.ctx);
  }
}

// True when there is no worker to wait on, or when the worker is provably Paused
// with all results resolved. Fail-closed: a faulted worker's hook returns false,
// so a pending intent simply keeps waiting (it never fabricates quiescence).
bool nwQuiesced() {
  return s_nw_hooks.quiesced == nullptr || s_nw_hooks.quiesced(s_nw_hooks.ctx);
}

void nwResume() {
  if (s_nw_hooks.resume != nullptr) {
    s_nw_hooks.resume(s_nw_hooks.ctx);
  }
}

core::NetworkWorkIntentState* nwIntent() {
  if (!s_nw_intent_initialized) {
    core::networkWorkIntentInit(&s_nw_intent);
    s_nw_intent_initialized = true;
  }
  return &s_nw_intent;
}

core::NetworkWorkIntent nwPendingIntent() {
  return core::pendingIntent(*nwIntent());
}
#endif  // PLANE_RADAR_ADSB_WORKER


// ===========================================================================
// Wi-Fi event tracking (filtered, allocation-free, ISR/event-task safe)
// ===========================================================================
//
// Two lock-free single-writer/single-reader counters. The disconnect counter is
// the unchanged Phase 6 mid-fetch flap detector; the GOT_IP counter lets each STA
// connect generation require a FRESH link-up event rather than a stale IP.
std::atomic<uint32_t> s_disconnect_seq{0};
std::atomic<uint32_t> s_got_ip_seq{0};
bool s_events_registered = false;

void onWifiStaDisconnectedEvent(arduino_event_id_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    s_disconnect_seq.fetch_add(1U, std::memory_order_relaxed);
  }
}

void onWifiStaGotIpEvent(arduino_event_id_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    s_got_ip_seq.fetch_add(1U, std::memory_order_relaxed);
  }
}

// ===========================================================================
// Provisioning button: ISR-safe fixed edge ring
// ===========================================================================
//
// The ISR only records (timestamp, level) into a fixed ring under the mux -- no
// draw, log, allocation, or action. Draining in wifiLoop replays the exact edge
// timestamps into the pure gesture FSM, so a press/release entirely inside a
// blocking DNS/TLS write is never lost. A ring overflow fails safe: the gesture
// is reset (never an erase).
portMUX_TYPE s_btn_mux = portMUX_INITIALIZER_UNLOCKED;
struct Edge {
  uint32_t t_ms;
  bool down;
};
constexpr uint8_t kEdgeRingCap = 16;  // two-stage gesture needs <= 4 edges; 16 is slack
volatile Edge s_edge_ring[kEdgeRingCap];
volatile uint8_t s_edge_head = 0;
volatile uint8_t s_edge_tail = 0;
volatile bool s_edge_overflow = false;
bool s_isr_attached = false;

core::ProvisionButton s_button;
core::ProvisionButtonPrompt s_button_prompt = core::ProvisionButtonPrompt::None;

void IRAM_ATTR onButtonIsr() {
  portENTER_CRITICAL_ISR(&s_btn_mux);
  const bool down = digitalRead(config::kBootPin) == LOW;
  const uint32_t now = millis();
  const uint8_t next = static_cast<uint8_t>((s_edge_head + 1) % kEdgeRingCap);
  if (next == s_edge_tail) {
    s_edge_overflow = true;  // full: drop and fail safe on drain
  } else {
    s_edge_ring[s_edge_head].t_ms = now;
    s_edge_ring[s_edge_head].down = down;
    s_edge_head = next;
  }
  portEXIT_CRITICAL_ISR(&s_btn_mux);
}

// ===========================================================================
// Portal session + secrets + credential transaction state
// ===========================================================================
core::PortalSession s_session;
core::PortalSecrets s_secrets;      // SoftAP SSID + one-time password + CSRF (secret)
wifi_config_t s_old_config;         // working STA creds captured before setup
bool s_have_old_config = false;
wifi_config_t s_candidate_config;   // RAM-only trial credential

struct StagedSettings {
  bool valid;
  double lat;
  double lon;
  bool use_miles;
  bool show_runways;
};
StagedSettings s_staged = {};

// Identity to echo on the NEXT completion (captured from each action set that
// starts an operation / STA generation).
uint32_t s_ack_session = 0;
uint32_t s_ack_trial = 0;
uint32_t s_ack_operation = 0;
uint32_t s_ack_sta = 0;

// Async side-effect completion latches (fed by the state handlers once ready).
enum class OpResult : uint8_t { None, Ok, Fail };
OpResult s_secrets_result = OpResult::None;
OpResult s_commit_result = OpResult::None;
OpResult s_restore_result = OpResult::None;

// STA connect-in-progress (StaConnecting).
bool s_connect_active = false;
uint32_t s_connect_gotip_baseline = 0;
uint32_t s_connect_attempt_start = 0;
uint8_t s_connect_attempt = 0;
// The driver config a stored/restored STA connect was started against, captured
// at connect time. Every accepted GOT_IP must still match it (never accept a link
// that came up on a different/previous config).
wifi_config_t s_connect_expected_config;
bool s_connect_expected_present = false;

// Candidate trial-in-progress (CandidateTrial). A trial is either actively
// connecting, or in a pending-failure phase awaiting a proven cancellation before
// the (identity-bound) CandidateFailed is reported.
bool s_candidate_active = false;
uint32_t s_candidate_gotip_baseline = 0;
uint32_t s_candidate_start = 0;
bool s_candidate_pending_fail = false;

// OfflineIdle background retry timing.
bool s_offline_anchored = false;
uint32_t s_offline_since_ms = 0;
uint32_t s_last_retry_ms = 0;

// StaOnline link-loss grace: a first drop does NOT immediately reconnect; a 4 s
// grace is anchored and only a still-down link past it feeds StaLost.
bool s_link_loss_anchored = false;
uint32_t s_link_loss_since_ms = 0;

// Ordered-close quiescence watchdog (SetupCleanup / TrialCancel): a bounded
// forced radio reset if the teardown cannot reach no-IP/quiesced in time. The ack
// is still gated on the predicate actually holding.
bool s_cleanup_anchored = false;
uint32_t s_cleanup_since_ms = 0;
bool s_cleanup_forced = false;

// Fail-closed credential fault: a commit could neither verify the new flash
// credential nor verify a rollback to the old one, a credential read/restore
// could not be proven, or a required RAM-storage selection failed -- so the
// stored credential is in an unknown state. The controller freezes (no
// listener/reconnect/false success) and only the physical factory-erase gesture
// recovers.
bool s_credential_fault = false;

// Fail-closed erase-incomplete fault: a factory erase could not verifiably clear
// every subsystem (or its transaction marker could not clear), so the device
// stays network-off and refuses normal operation. A second confirmed erase
// gesture (or a power-cycle that resumes the erase marker) may retry.
bool s_erase_incomplete = false;

// Non-credential portal settings (location/units/runways) could not be
// verifiably persisted even though the Wi-Fi credential committed. A truthful
// on-screen warning is latched before returning to the radar; the verified Wi-Fi
// credential is NOT rolled back for a display-setting write failure.
bool s_settings_save_failed = false;
bool s_settings_warn_anchored = false;
uint32_t s_settings_warn_since_ms = 0;

// Transactional listener bring-up failed: driveController drives an ordered
// SessionCancel so the core never believes a listener exists.
bool s_listener_start_failed = false;

// The old-config RAM bytes are captured only for the duration of a setup
// transaction and wiped once the controller returns to a steady state.
bool s_old_config_bytes_present = false;

// Facade latches / display ownership.
bool s_initialized = false;
bool s_immediate_fetch_pending = false;
bool s_range_tap_pending = false;
bool s_boot_ui_active = false;
bool s_boot_failed_offline = false;

// Status-screen dedup key.
uint8_t s_last_screen_kind = 0xFF;
uint32_t s_last_screen_param = 0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Forward declarations for helpers referenced before their definitions.
void wipeCandidate();
void enterCredentialFault();
bool ensureStorageRam();
bool terminalNetworkOff();

bool linkUp() {
  return static_cast<uint32_t>(WiFi.localIP()) != 0U;
}

bool apModeUp() { return (WiFi.getMode() & WIFI_MODE_AP) != 0; }

bool staModeUp() { return (WiFi.getMode() & WIFI_MODE_STA) != 0; }

// Radio entropy prep is ready: STA radio active with no IP (never an AP yet).
bool radioReady() { return staModeUp() && !linkUp() && !apModeUp(); }

// Portal is provably down: no listener, no SoftAP, no STA IP.
bool quiesced() {
  return !services::portal::active() && !apModeUp() && !linkUp();
}

uint32_t remainingSeconds(uint32_t now_ms) {
  const uint32_t elapsed = now_ms - s_session.deadline_start_ms;  // rollover-safe
  if (elapsed >= config::kPortalSessionTimeoutMs) {
    return 0;
  }
  return (config::kPortalSessionTimeoutMs - elapsed + 999U) / 1000U;
}

// True once the ORIGINAL session deadline has passed (candidate retries never
// extend it). Injected into config_portal so a response write / accept is aborted
// the moment the deadline lapses -- the AP/HTTP/DNS surface is never held open for
// a write budget past five minutes. Meaningful only while a session owns a valid
// deadline_start_ms, which is exactly when the portal is active.
bool portalDeadlineExpired() {
  return core::elapsedAtLeast(millis(), s_session.deadline_start_ms,
                              config::kPortalSessionTimeoutMs);
}

void captureAck(const PortalActions& a) {
  if (a.ack_operation_id != 0U) {
    s_ack_session = a.ack_session_id;
    s_ack_trial = a.ack_trial_id;
    s_ack_operation = a.ack_operation_id;
  }
  if (a.ack_sta_connection_id != 0U) {
    s_ack_sta = a.ack_sta_connection_id;
  }
}

// Bounded budget for an ordered close (SetupCleanup) to reach no-IP/quiesced
// before a forced radio reset is applied.
constexpr uint32_t kCleanupQuiesceTimeoutMs = 4000;

// How long the truthful "Wi-Fi saved; settings save failed" warning owns the panel
// after a commit whose non-credential settings did not persist, before the radar
// reclaims it.
constexpr uint32_t kSettingsWarnMs = 4000;

// Zero the driver's SoftAP config so the one-time WPA2 passphrase does not linger
// in driver RAM. Intended for the raw-recovery path where the radio is STOPPED
// (so applying an empty/OPEN config never produces a transient beacon) and AP mode
// is still selected (esp_wifi_set_config(WIFI_IF_AP) requires it). Returns the
// driver result so the caller can observe the config-clear outcome, not ignore it.
bool clearApConfig() {
  wifi_config_t empty = {};
  const bool ok = esp_wifi_set_config(WIFI_IF_AP, &empty) == ESP_OK;
  core::secureZero(&empty, sizeof(empty));
  return ok;
}

// Capture the working STA credential from the driver's active (RAM) config for the
// duration of a setup transaction. If credentials are BELIEVED present
// (s_have_old_config) but the driver cannot produce a non-empty config, the old
// credential cannot be captured -- a failed trial could not restore it -- so this
// enters the fail-closed credential fault and returns false (do NOT open setup).
// Otherwise it records whether a capturable config exists and returns true.
bool snapshotOldConfig() {
  const bool believed_present = s_have_old_config;
  const bool snapped = services::wifi_creds::snapshotSta(&s_old_config) &&
                       services::wifi_creds::hasSsid(s_old_config);
  if (believed_present && !snapped) {
    services::wifi_creds::zeroize(&s_old_config);
    s_old_config_bytes_present = false;
    enterCredentialFault();  // stored creds believed present but unverifiable
    return false;
  }
  if (snapped) {
    s_have_old_config = true;
    s_old_config_bytes_present = true;
  } else {
    services::wifi_creds::zeroize(&s_old_config);
    s_have_old_config = false;
    s_old_config_bytes_present = false;
  }
  return true;
}

// Wipe the old-config RAM bytes once the controller is back in a steady state.
// The has_old_config existence flag is retained; the bytes are re-snapshotted at
// the next physical setup request.
void wipeOldConfigBytes() {
  if (s_old_config_bytes_present) {
    services::wifi_creds::zeroize(&s_old_config);
    s_old_config_bytes_present = false;
  }
}

// ---------------------------------------------------------------------------
// Central secure radio transitions.
//
// We NEVER use the Arduino soft-AP disconnect-with-wifioff call: the pinned
// implementation first writes an OPEN default AP config and only then disables the
// AP, opening a potential open-beacon window. Instead, the SoftAP passphrase is
// cleared ONLY while the radio is STOPPED (no beacon at all), and every stop /
// mode / config / start outcome is checked and propagated. If the raw stop cannot
// be proven, we never write an empty/open AP config -- we try a safe running-mode
// transition to STA that DISABLES the AP interface without clearing it, and
// succeed only if no AP and no IP can be verified; otherwise the caller stays
// faulted.
// ---------------------------------------------------------------------------

// After a raw esp_wifi_stop()/start() or a running-mode transition, the LwIP netif
// clears the STA IP on the Wi-Fi event task, which can lag this synchronous path by
// a few ms. Because a stopped/unconnected STA cannot (re)acquire an IP, spin a
// SHORT bounded settle for localIP() to reach 0 so a lagging netif does not read a
// STALE non-zero IP and drive a spurious no-IP failure. Common case: linkUp() is
// already false, so this returns immediately.
void settleNoIp() {
  for (uint8_t i = 0; i < 40 && linkUp(); ++i) {
    delay(5);  // up to ~200 ms for the event task to clear a stale netif IP
  }
}

// Securely bring a running SoftAP down and return to a STARTED STA with no
// connection, without ever exposing an OPEN/default AP. Preferred path: stop the
// radio, clear the AP passphrase from the stopped driver (only while AP mode is
// still set), select STA, and start again -- each step checked. On a raw-stop
// failure, try a safe running-mode transition to STA that merely disables the AP
// interface and succeed ONLY if no AP and no IP can be verified. Returns true ONLY
// when the end state is a started STA with no AP and no IP. The raw stop/start is
// fully paired, so Arduino's private started flag stays aligned.
bool secureApToSta() {
  if (services::portal::active()) {
    services::portal::stop();  // stop HTTP/DNS first
  }
  WiFi.setAutoReconnect(false);
  if (esp_wifi_stop() == ESP_OK) {
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) == ESP_OK && (m & WIFI_MODE_AP) != 0) {
      if (!clearApConfig()) {
        return false;  // could not clear the AP passphrase while stopped
      }
    }
    const bool mode_ok = esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK;
    const bool start_ok = esp_wifi_start() == ESP_OK;
    if (mode_ok && start_ok) {
      settleNoIp();  // let a stale netif IP clear after the paired stop/start
    }
    return mode_ok && start_ok && staModeUp() && !apModeUp() && !linkUp();
  }
  // Raw stop failed: never write an empty/open AP config while RF is running. A
  // running-mode transition to STA disables the AP interface without clearing it.
  const bool mode_ok = esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK;
  return mode_ok && !apModeUp() && !linkUp();
}

// Drive EVERY network surface off for a TERMINAL state (credential fault,
// erase-incomplete, factory-erase teardown): stop the portal, disable auto
// reconnect, drop any STA association, and disable the AP+STA radio. Never writes
// an OPEN/empty AP config while RF is running (the AP passphrase is cleared only
// while the radio is stopped). Returns true ONLY when the end state is provably
// radio-off: no AP beacon and no STA IP. A false result means the caller MUST keep
// showing a truthful radio-fault state and never restart the listener/reconnect.
bool terminalNetworkOff() {
  if (services::portal::active()) {
    services::portal::stop();
  }
  WiFi.setAutoReconnect(false);
  esp_wifi_disconnect();  // best effort: drop any pending/active STA association
  if (esp_wifi_stop() == ESP_OK) {
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) == ESP_OK && (m & WIFI_MODE_AP) != 0) {
      clearApConfig();  // zero the AP passphrase from the stopped driver
    }
    // Select STA-only (AP disabled) and leave the radio STOPPED: no beacon, no IP.
    esp_wifi_set_mode(WIFI_MODE_STA);
    settleNoIp();  // let the netif reflect the dropped association
    return !apModeUp() && !linkUp();
  }
  // Raw stop failed: do NOT write an empty/open AP config. A running-mode
  // transition to STA disables the AP interface; succeed only if no AP and no IP.
  esp_wifi_set_mode(WIFI_MODE_STA);
  settleNoIp();
  return !apModeUp() && !linkUp();
}

// Secure stopped-radio reset to a STARTED STA with no connection, used to
// DEFINITIVELY cancel any pending/active STA association (a pending connect that
// never obtained an IP included). The raw stop MUST succeed -- that is what drops
// the pending connect -- so on failure the caller enters the fault. While stopped,
// any AP config is cleared and RAM storage is selected so the caller can apply +
// verify the exact STA config it wants (empty for a cancel, the expected
// credential for a retry). Returns true ONLY when the radio is a started STA with
// no AP and no IP.
bool secureStaReset() {
  if (services::portal::active()) {
    services::portal::stop();
  }
  WiFi.setAutoReconnect(false);
  if (esp_wifi_stop() != ESP_OK) {
    return false;  // cannot prove the pending connect was dropped
  }
  wifi_mode_t m = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&m) == ESP_OK && (m & WIFI_MODE_AP) != 0) {
    if (!clearApConfig()) {
      return false;
    }
  }
  if (!services::wifi_creds::setStorageRam()) {
    return false;  // storage cannot be proven RAM: caller faults
  }
  const bool mode_ok = esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK;
  const bool start_ok = esp_wifi_start() == ESP_OK;
  if (mode_ok && start_ok) {
    settleNoIp();  // the stopped/unconnected STA has no IP; wait out a stale netif
  }
  return mode_ok && start_ok && staModeUp() && !apModeUp() && !linkUp();
}

// Forced radio reset fallback (ordered-close quiesce watchdog): drive the radio to
// a bare STA with no connection through the secure AP->STA transition so an ordered
// close can reach no-IP/quiesced even if a normal disconnect stalled. The caller
// still gates its ack on the predicate actually holding, so the result is advisory.
void forceRadioReset() { (void)secureApToSta(); }

// ---------------------------------------------------------------------------
// Radio + credential action executors
// ---------------------------------------------------------------------------
void executeDisconnectSta() {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
}

// The core's start_ap_radio means "STA quiesced + RF entropy ready, AP NOT
// exposed" -- it is a RADIO ENTROPY PREPARATION, not permission to expose an AP.
// Keep only the STA radio up (RF entropy active per esp_fill_random's docs) with
// no connection; do NOT begin() and do NOT softAP(). This avoids any transient
// default/open AP; the real SoftAP appears only later, on start_listener.
void executeStartApRadio() {
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
}

void executeRequestSecrets() {
  uint8_t mac[6];
  const bool ok = services::identity::factoryMac(mac) &&
                  core::generatePortalSecrets(
                      mac, services::identity::entropySource(), &s_secrets);
  s_secrets_result = ok ? OpResult::Ok : OpResult::Fail;
}

// Transactional secure-AP + listener bring-up. The very FIRST AP beacon must
// already carry the WPA2-PSK/CCMP session secret, so we use a controlled paired
// low-level sequence rather than WiFi.mode(WIFI_AP) (which starts the AP on the
// default/empty config BEFORE esp_wifi_set_config runs -- a brief OPEN beacon).
//
// Framework alignment: the runtime reaches here with Wi-Fi already
// initialized+started in STA mode (Arduino's private _esp_wifi_started == true).
// We call the RAW IDF esp_wifi_stop() (NOT deinit) / esp_wifi_set_mode(AP) /
// esp_wifi_set_config(AP, fully-built config) / esp_wifi_start(), and NO Arduino
// WiFi API between the stop and a successful restart. Because those raw calls
// never touch Arduino's started flag, it stays true across this fully paired
// stop/start and the radio is physically started again before we return, so later
// Arduino calls (softAPConfig / getMode) stay aligned.
//
// On ANY failure we attempt a safe paired recovery to a started STA/no-IP and
// drive an ordered SessionCancel (the core never believes a listener exists); if
// that recovery cannot be proven, we enter the radio/credential fault. There is
// NO open/default AP fallback, and zero temporary AP config is ever exposed.
void executeStartListener() {
  s_listener_start_failed = false;

  // Storage RAM so the one-time AP password is never written to NVS; failing to
  // prove RAM storage is itself a credential fault. There must be NO STA IP before
  // the secure AP comes up.
  if (!ensureStorageRam()) {
    return;  // credential fault entered; bring nothing up
  }
  if (linkUp()) {
    s_listener_start_failed = true;  // STA not quiesced: fail closed via cancel
    return;
  }

  wifi_config_t ap = {};
  const bool built = services::wifi_creds::buildAp(
      &ap, s_secrets.ssid, strlen(s_secrets.ssid), s_secrets.password,
      strlen(s_secrets.password), config::kPortalApChannel,
      config::kPortalApMaxConnections, /*hidden=*/!config::kPortalApVisible);

  // Paired low-level secure start: stop STA (do NOT deinit), switch to AP mode,
  // apply the fully-built WPA2/CCMP+secret config BEFORE start, then start. The
  // first joinable beacon therefore already carries the secret; the only
  // pre-start instant is a stopped radio (no beacon at all).
  bool secure_started = false;
  if (built) {
    secure_started = esp_wifi_stop() == ESP_OK &&
                     esp_wifi_set_mode(WIFI_MODE_AP) == ESP_OK &&
                     esp_wifi_set_config(WIFI_IF_AP, &ap) == ESP_OK &&
                     esp_wifi_start() == ESP_OK;
  }
  services::wifi_creds::zeroize(&ap);  // zero the temporary AP config after use

  bool ok = secure_started;

  if (ok) {
    // Only AFTER the secure start do we configure + verify the AP IP. Arduino's
    // enableAP(true) sees AP already up (getMode()==AP), so it does not restart.
    const IPAddress ip(config::kPortalIpOctets[0], config::kPortalIpOctets[1],
                       config::kPortalIpOctets[2], config::kPortalIpOctets[3]);
    const IPAddress mask(config::kPortalNetmaskOctets[0],
                         config::kPortalNetmaskOctets[1],
                         config::kPortalNetmaskOctets[2],
                         config::kPortalNetmaskOctets[3]);
    ok = WiFi.softAPConfig(ip, ip, mask) &&
         static_cast<uint32_t>(WiFi.softAPIP()) ==
             static_cast<uint32_t>(ip) &&
         apModeUp() && !linkUp();
  }

  if (ok) {
    services::portal::PortalContent content = {};
    content.ap_ip[0] = config::kPortalIpOctets[0];
    content.ap_ip[1] = config::kPortalIpOctets[1];
    content.ap_ip[2] = config::kPortalIpOctets[2];
    content.ap_ip[3] = config::kPortalIpOctets[3];
    content.csrf_token = s_secrets.csrf;
    content.lat = services::location::lat();
    content.lon = services::location::lon();
    content.use_miles = ui::radar::useMiles();
    content.show_runways = ui::radar::showRunways();
    content.on_save = &portalSaveTrampoline;
    content.on_save_ctx = nullptr;
    ok = services::portal::start(content);  // false unless DNS bound + listening
    core::secureZero(&content, sizeof(content));
  }

  if (!ok) {
    // Fail closed with NO open/default AP left up. secureApToSta tears the portal
    // down and returns to a started STA/no-IP without ever writing an open AP
    // config (the AP passphrase is cleared only while the radio is stopped).
    if (secureApToSta()) {
      s_listener_start_failed = true;  // ordered SessionCancel; no listener exists
    } else {
      enterCredentialFault();  // cannot prove a safe radio state: radio fault
    }
  }
}

// Bring the portal transport + SoftAP down and return to a bare STA radio with no
// connection. Idempotent; used for stop_listener (folded into the quiesce states).
// Uses the central secureApToSta() transition (checked stop -> clear-while-stopped
// -> STA -> start; never the Arduino soft-AP disconnect call). Returns true iff the
// radio reached a started STA with no AP/no-IP AND storage is provably RAM
// afterward (a storage failure is a credential fault), so the teardown outcome is
// fed into the caller's predicate rather than ignored.
bool teardownPortalToSta() {
  const bool radio_ok = secureApToSta();
  return radio_ok && ensureStorageRam();  // later ops must be provably RAM-only
}

void executeStartStaConnect() {
  s_connect_active = true;
  s_connect_attempt = 1;
  s_connect_attempt_start = millis();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  // Capture the exact driver config this attempt connects against; every accepted
  // GOT_IP must still match it, so a link that came up on a different/previous
  // config is never accepted. An unreadable/empty config leaves nothing to verify,
  // so no GOT_IP will be accepted for this attempt (it fails rather than lies).
  s_connect_expected_present =
      services::wifi_creds::snapshotSta(&s_connect_expected_config) &&
      services::wifi_creds::hasSsid(s_connect_expected_config);
  // Baseline the GOT_IP counter immediately before the connect (require a FRESH
  // event for this attempt), after the driver config is captured.
  s_connect_gotip_baseline = s_got_ip_seq.load(std::memory_order_relaxed);
  esp_wifi_connect();  // connect with the driver's current config (stored/restored)
}

// Wipe the staged candidate credential + settings (on any unsuccessful or
// completed trial). buildSta() re-populates them from scratch on the next submit.
void wipeCandidate() {
  services::wifi_creds::zeroize(&s_candidate_config);
  s_staged = StagedSettings{};
}

// Enter the fail-closed credential fault: a TERMINAL, physically network-off state.
// Take every network surface off (checked terminalNetworkOff), freeze the
// controller (no listener/reconnect/false success), and wipe every credential-
// bearing RAM copy (old / candidate / connect-expected), the staged settings, the
// portal secrets, and the on-screen credentials; reset all presence/activity flags.
// If radio-off cannot be proven, the fault STILL stands and the truthful fault
// screen is shown -- we never claim a safety we cannot prove and never restart the
// listener/reconnect. Only the physical factory-erase gesture recovers. Idempotent.
void enterCredentialFault() {
  s_credential_fault = true;
  terminalNetworkOff();  // stop portal, autoreconnect off, disconnect STA, radio off
  services::wifi_creds::zeroize(&s_old_config);
  services::wifi_creds::zeroize(&s_connect_expected_config);
  wipeCandidate();  // zeroes s_candidate_config + staged settings
  core::zeroPortalSecrets(&s_secrets);
  statusScreenClearCredentials();
  s_have_old_config = false;
  s_old_config_bytes_present = false;
  s_connect_expected_present = false;
  s_connect_active = false;
  s_candidate_active = false;
  s_candidate_pending_fail = false;
  s_link_loss_anchored = false;
  s_offline_anchored = false;
  s_cleanup_anchored = false;
  s_immediate_fetch_pending = false;
  s_range_tap_pending = false;
  // Best effort: clear the RAM STA config so a candidate/unknown credential cannot
  // linger. Meaningful only if storage is RAM; the fault stands regardless.
  wifi_config_t empty = {};
  if (services::wifi_creds::setStorageRam()) {
    services::wifi_creds::applySta(empty);
  }
  core::secureZero(&empty, sizeof(empty));
}

// Select RAM storage and treat a failure as a credential fault: once the storage
// domain cannot be proven RAM, a later runtime write could silently reach flash,
// so no subsequent operation can be trusted RAM-only. Returns true on success.
bool ensureStorageRam() {
  if (services::wifi_creds::setStorageRam()) {
    return true;
  }
  enterCredentialFault();
  return false;
}

// Prove any pending/active candidate association is cancelled: a secure
// stopped-radio reset (which DEFINITIVELY drops the pending connect) followed by
// applying an EMPTY RAM STA config, then verify no-IP AND the driver's current STA
// config is empty (not the candidate). A pending association often has no IP, so a
// no-IP alone is NOT proof; both conditions must hold. Returns false if the reset,
// the empty apply, or the verification cannot be proven -- the caller then faults.
bool proveConnectCancelled() {
  if (!secureStaReset()) {
    return false;
  }
  wifi_config_t empty = {};
  const bool applied = services::wifi_creds::applySta(empty);
  core::secureZero(&empty, sizeof(empty));
  if (!applied) {
    return false;
  }
  wifi_config_t current;
  const bool empty_now = services::wifi_creds::getSta(&current) &&
                         !services::wifi_creds::hasSsid(current);
  services::wifi_creds::zeroize(&current);
  return empty_now && !linkUp();
}

// Enter the candidate pending-failure phase. The pending-fail phase in
// serviceCandidate performs the secure stopped-radio reset that DEFINITIVELY
// cancels the pending association and proves no-IP + empty STA config before the
// identity-bound CandidateFailed is reported. Never connects; used both for a
// failed RAM stage and a mismatched/timed-out trial.
void beginCandidatePendingFail() {
  s_candidate_active = false;
  s_candidate_pending_fail = true;
  WiFi.setAutoReconnect(false);
}

void executeBeginCandidate() {
  s_candidate_active = false;
  s_candidate_pending_fail = false;
  // Portal is provably down and no STA IP exists (PortalQuiesced). Fail-closed RAM
  // staging: select RAM storage (a failure to prove RAM is a credential fault, as
  // a later write could reach flash), apply the candidate, and read it straight
  // back and compare BEFORE connecting -- never touch the working flash credential,
  // and never connect on a failed setter (which would silently trial the PREVIOUS
  // config). A stage failure reports an identity-bound CandidateFailed, but only
  // after the candidate is provably cancelled (pending-failure phase).
  if (!ensureStorageRam()) {
    return;  // credential fault entered; do not stage or connect a candidate
  }
  bool staged = services::wifi_creds::applySta(s_candidate_config);
  if (staged) {
    wifi_config_t readback;
    staged = services::wifi_creds::getSta(&readback) &&
             services::wifi_creds::sameCredentials(readback, s_candidate_config);
    services::wifi_creds::zeroize(&readback);
  }
  if (!staged) {
    beginCandidatePendingFail();  // do NOT connect on a failed stage
    return;
  }
  s_candidate_active = true;
  s_candidate_start = millis();
  // Baseline the GOT_IP counter immediately before the connect, after quiescence.
  s_candidate_gotip_baseline = s_got_ip_seq.load(std::memory_order_relaxed);
  WiFi.setAutoReconnect(false);
  esp_wifi_connect();
}

void executeCancelCandidate() {
  // Do NOT infer cancellation from no-IP here. The TrialCancel state performs a
  // secure stopped-radio reset to an empty RAM STA config that DEFINITIVELY cancels
  // any pending association, then verifies no-IP + empty STA config before acking
  // CandidateCancelled. Just stop auto reconnect and nudge a disconnect now.
  WiFi.setAutoReconnect(false);
  esp_wifi_disconnect();
}

// Persist the staged location/units/runway settings. Returns true only when every
// staged non-credential setting verifiably persisted. A false result latches a
// truthful on-screen "Wi-Fi saved; settings save failed" warning; it never rolls
// back a committed Wi-Fi credential (these are display settings, not credentials).
bool applyStagedSettings() {
  if (!s_staged.valid) {
    return true;  // nothing staged: trivially complete
  }
  char lat_buf[24];
  char lon_buf[24];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", s_staged.lat);
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", s_staged.lon);
  bool location_ok = true;
  const core::CoordinateSaveResult loc =
      services::location::saveFromStrings(lat_buf, lon_buf, &location_ok);
  // Mark an effective query change ONLY when the location actually changed AND the
  // new location verifiably persisted -- a failed write must not bump the query
  // revision (the runtime location is unchanged, so a later identical retry still
  // persists rather than being falsely treated as already saved).
  if (loc == core::CoordinateSaveResult::Changed && location_ok) {
    services::settings::markQueryChanged();
  }
  bool ok = location_ok;
  ok = ui::radar::setUseMiles(s_staged.use_miles) && ok;
  ok = ui::radar::setShowRunways(s_staged.show_runways) && ok;
  return ok;
}

void executeCommit() {
  // Persist + verify the durable commit-in-progress marker BEFORE touching the
  // candidate FLASH. If the marker cannot be created, do NOT touch flash -- keep
  // the old credentials via an ordered cancel + restore. Return storage to RAM and
  // CHECK it: if RAM cannot be proven, a later op could silently reach flash, so
  // fail closed rather than emit CommitFailed.
  if (!services::provision_marker::writeVerified(
          core::TxnMarkerState::CommitInProgress)) {
    if (!ensureStorageRam()) {
      s_commit_result = OpResult::None;  // credential fault entered (frozen)
      return;
    }
    s_commit_result = OpResult::Fail;  // ordered cancel + restore keeps old creds
    return;
  }

  // FLASH is used ONLY inside this critical section. Treat ANY attempted FLASH
  // apply as potentially having touched flash, even when applySta returns false, so
  // a verified old rollback is required on every failed/ambiguous apply/readback.
  const bool flash_selected = services::wifi_creds::setStorageFlash();
  bool apply_attempted = false;
  bool committed = false;
  if (flash_selected) {
    apply_attempted = true;  // the apply may write NVS even when it returns false
    committed = services::wifi_creds::applySta(s_candidate_config);
  }
  const bool flash_touched = apply_attempted;
  if (committed) {
    wifi_config_t readback;
    committed =
        services::wifi_creds::getSta(&readback) &&
        services::wifi_creds::sameCredentials(readback, s_candidate_config);
    services::wifi_creds::zeroize(&readback);
  }

  if (!committed && flash_touched) {
    // The flash apply was attempted (so NVS may have changed) but the new
    // credential did not verify. Synchronously re-persist the captured old config
    // to FLASH and verify it BEFORE reporting, so an unknown credential is never
    // stranded in flash.
    bool rolled_back = false;
    if (s_have_old_config && s_old_config_bytes_present &&
        services::wifi_creds::hasSsid(s_old_config)) {
      wifi_config_t rb;
      rolled_back = services::wifi_creds::applySta(s_old_config) &&
                    services::wifi_creds::getSta(&rb) &&
                    services::wifi_creds::sameCredentials(rb, s_old_config);
      services::wifi_creds::zeroize(&rb);
    }
    if (!rolled_back) {
      // Cannot verify rollback: fail closed. enterCredentialFault takes the network
      // off and wipes RAM secrets/copies; the commit marker is left in place so the
      // NEXT boot also fails closed.
      s_commit_result = OpResult::None;
      enterCredentialFault();
      return;
    }
  }

  // Either the new candidate committed+verified, or the old config was verifiably
  // rolled back: the credential FLASH is in a known-good state, so clear+verify the
  // marker. If the clear cannot be verified, do NOT continue online with a stale
  // CommitInProgress marker -- freeze network-off (the next boot fails closed too).
  if (!services::provision_marker::writeVerified(core::TxnMarkerState::None)) {
    s_commit_result = OpResult::None;
    enterCredentialFault();
    return;
  }
  if (!ensureStorageRam()) {
    // Storage could not return to RAM: later ops cannot be proven RAM-only. The
    // credential is committed/rolled-back+verified, but freeze fail-closed rather
    // than risk a later AP/portal write reaching flash.
    s_commit_result = OpResult::None;
    return;  // ensureStorageRam already entered the credential fault
  }

  if (committed) {
    // Non-credential settings are best-effort: a verified Wi-Fi credential is NEVER
    // rolled back for a display-setting write failure, but the failure is surfaced
    // truthfully rather than silently claimed as applied.
    if (!applyStagedSettings()) {
      s_settings_save_failed = true;
    }
    // The candidate is now the working credential (kept in RAM only until the
    // controller settles back to StaOnline, where the bytes are wiped).
    memcpy(&s_old_config, &s_candidate_config, sizeof(s_old_config));
    s_have_old_config = true;
    s_old_config_bytes_present = true;
    wipeCandidate();
    s_commit_result = OpResult::Ok;
  } else {
    s_commit_result = OpResult::Fail;
  }
}

void executeRestore() {
  bool ok = false;
  if (s_have_old_config && s_old_config_bytes_present &&
      services::wifi_creds::hasSsid(s_old_config)) {
    // Restore is RAM-only (never FLASH): apply the captured old config to RAM and
    // read it back to confirm the driver holds it before reporting success.
    ok = services::wifi_creds::setStorageRam() &&
         services::wifi_creds::applySta(s_old_config);
    if (ok) {
      wifi_config_t rb;
      ok = services::wifi_creds::getSta(&rb) &&
           services::wifi_creds::sameCredentials(rb, s_old_config);
      services::wifi_creds::zeroize(&rb);
    }
  }
  if (ok) {
    s_restore_result = OpResult::Ok;
    return;
  }
  // Restore could not be verified: the driver may still hold the failed candidate
  // (or a partially-applied config), so do NOT feed RestoreFailed into OfflineIdle
  // where retries/listener would resume over an unknown RAM config. Fail closed
  // into the credential fault (which wipes the candidate + secrets and best-effort
  // clears the RAM STA config); never drive RestoreFailed.
  s_restore_result = OpResult::None;
  enterCredentialFault();
}

// Wipe every owned RAM secret/config copy touched during a factory erase (staged
// candidate, old + connect-expected credentials, staged settings, on-screen
// credentials, portal secrets) and reset their presence flags. Idempotent.
void wipeEraseRam() {
  services::wifi_creds::zeroize(&s_candidate_config);
  services::wifi_creds::zeroize(&s_old_config);
  services::wifi_creds::zeroize(&s_connect_expected_config);
  s_have_old_config = false;
  s_old_config_bytes_present = false;
  s_connect_expected_present = false;
  s_staged = StagedSettings{};
  statusScreenClearCredentials();
  core::zeroPortalSecrets(&s_secrets);
}

// Enter the fail-closed erase-incomplete terminal state through ONE consistent
// cleanup path: re-CHECK terminal network-off, wipe every owned RAM secret/config
// copy, and latch the retriable erase-incomplete fault while RETAINING the durable
// ErasePending marker so a second confirmed gesture (or a boot-marker resume) can
// retry. Never clears the marker and never reports a false "erased" success.
// Idempotent.
void enterEraseIncomplete() {
  terminalNetworkOff();  // re-prove every network surface is off before freezing
  wipeEraseRam();
  s_credential_fault = false;  // erase-incomplete is the truthful terminal state
  s_erase_incomplete = true;
  s_last_screen_kind = 0xFF;   // force the persistent incomplete screen to draw next
}

// Factory erase: the ONLY erase without replacement. A durable erase-pending
// marker is persisted AND VERIFIED first (mandatory) so a power loss mid-erase
// resumes at the next boot; if it cannot be persisted+verified, NOTHING is erased.
// A PROVEN terminal network-off state is then REQUIRED before erasing anything
// (CHECKED); if it cannot be proven the AP/STA state is unknown, so NOTHING is
// erased. Otherwise each persisted subsystem is erased AND VERIFIED (STA flash
// creds, location, radar prefs, time-floor). Only a full verified wipe AND a
// verified marker clear restarts into first boot; an incomplete wipe stays
// network-off/fail-closed (s_erase_incomplete) -- re-checked network-off before
// freezing -- so a second confirmed gesture, or a power-cycle that resumes the
// marker, can retry. No power-on GPIO9 dependency. Returns only on an incomplete
// erase.
void executeFactoryErase() {
  // (Task 2) The erase marker is MANDATORY. Persist + verify ErasePending FIRST. If
  // it cannot be persisted+verified there is no durable authorization/resume state,
  // so erase NOTHING: take the network/radio off, wipe RAM secrets/copies, and show
  // a truthful incomplete screen a second confirmed gesture can retry.
  if (!services::provision_marker::writeVerified(
          core::TxnMarkerState::ErasePending)) {
    enterEraseIncomplete();  // no durable authorization: erase NOTHING
    return;
  }

  // (Task 3) The pre-erase network-off invariant is MANDATORY and CHECKED: prove
  // EVERY network surface is terminally off BEFORE erasing any subsystem.
  // terminalNetworkOff() never starts or exposes a radio; if it cannot PROVE
  // radio-off (e.g. a stop/set-mode/start sequence that could re-enable a prior AP
  // mode leaves AP/STA state unproven), erase NOTHING, RETAIN ErasePending, wipe
  // RAM secrets, and enter erase-incomplete so a second gesture or a boot-marker
  // resume retries.
  if (!terminalNetworkOff()) {
    enterEraseIncomplete();  // network-off unproven: erase NOTHING
    return;
  }

  core::FactoryEraseOutcome outcome = {};
  // eraseSta() is the SOLE checked FLASH credential erase: it persists an empty STA
  // credential to FLASH, reads it back to confirm it is empty, and returns storage
  // selection to RAM. No radio is started merely to erase NVS.
  outcome.sta_cleared = services::wifi_creds::eraseSta();
  outcome.location_cleared = services::location::clear();
  outcome.radar_cleared = ui::radar::resetAll();
  outcome.time_floor_cleared = services::timekeeper::clearPersistedFloor();

  const bool all_cleared = core::factoryEraseAllCleared(outcome);
  // Clear + verify the erase marker ONLY after every subsystem verifiably cleared,
  // so only a full erase AND a verified marker clear restarts.
  const bool marker_cleared =
      all_cleared &&
      services::provision_marker::writeVerified(core::TxnMarkerState::None);

  if (all_cleared && marker_cleared) {
    wipeEraseRam();  // wipe owned RAM secrets/copies before the erased screen
    statusScreenFactoryErase(outcome);  // truthful "Erased" screen
    delay(1500);
    esp_restart();  // full verified wipe + marker cleared: restart into first boot
  }
  // (Task 3) Incomplete erase (or the marker could not clear): re-CHECK a terminal
  // network-off before freezing, then stay network-off and fail-closed rather than
  // restart into a normal boot with settings possibly remaining. A second confirmed
  // erase gesture retries; a power-cycle resumes the erase marker at boot. The
  // persistent incomplete screen is owned by updateDisplay.
  enterEraseIncomplete();
}

// Execute one transition's action set. stop_listener is deliberately NOT torn
// down here: the quiesce state handlers own the ordered teardown so an accepted
// /save response is flushed before the SoftAP drops.
void applyActions(const PortalActions& a) {
  captureAck(a);
  if (a.factory_erase) {
    executeFactoryErase();  // never returns
    return;
  }
  if (a.zeroize_secrets) {
    core::zeroPortalSecrets(&s_secrets);
    // Every close path that zeroizes secrets also drops any staged candidate +
    // settings (belt-and-suspenders; the failure/cancel paths already wipe).
    wipeCandidate();
  }
#if PLANE_RADAR_ADSB_WORKER
  // Honor the core's pause request BEFORE any radio-changing action so the worker
  // is quiesced before the STA drops / AP radio comes up. Idempotent: the deferred
  // Configure/Erase handshake already paused the worker, and first-boot honors it
  // here too (the worker is started before wifiBootConnect()).
  if (a.pause_adsb) {
    nwRequestPause();
  }
#endif
  if (a.disconnect_sta) {
    executeDisconnectSta();
  }
  if (a.start_ap_radio) {
    executeStartApRadio();
  }
  if (a.restore_old_ram_config) {
    executeRestore();
  }
  if (a.request_secret_generation) {
    executeRequestSecrets();
  }
  if (a.start_listener) {
    executeStartListener();
  }
  if (a.begin_candidate_trial) {
    executeBeginCandidate();
  }
  if (a.cancel_candidate_trial) {
    executeCancelCandidate();
  }
  if (a.commit_candidate_flash) {
    executeCommit();
  }
  if (a.start_sta_connect) {
    executeStartStaConnect();
  }
  if (a.force_immediate_adsb_fetch) {
    s_immediate_fetch_pending = true;
  }
#if PLANE_RADAR_ADSB_WORKER
  // Resume ONLY on the core's resume_adsb edge (emitted at a steady online state).
  // The one immediate refresh is driven by the existing force_immediate_adsb_fetch
  // latch above, so no second immediate latch is introduced.
  if (a.resume_adsb) {
    nwResume();
  }
#endif
  // pause_adsb / resume_adsb / stop_listener: no direct side effect here (ADS-B is
  // gated by wifiOwnsDisplay()/link state in main; teardown is folded into the
  // quiesce handlers).
}

PortalActions feed(PortalInput input, uint32_t now_ms) {
  const PortalAck ack{s_ack_session, s_ack_trial, s_ack_operation, s_ack_sta};
  const PortalActions a =
      core::portalSessionUpdate(&s_session, kPolicy, now_ms, input, ack);
  applyActions(a);
  return a;
}

// ---------------------------------------------------------------------------
// Button drain + gesture -> portal input
// ---------------------------------------------------------------------------
void handleButtonEvent(core::ProvisionButtonEvent event, uint32_t now_ms) {
  switch (event) {
    case core::ProvisionButtonEvent::Tap:
      if (s_credential_fault || s_erase_incomplete) {
        break;  // fail-closed: ignore range taps while faulted
      }
      s_range_tap_pending = true;  // range action latch (survives blocking work)
      break;
    case core::ProvisionButtonEvent::ConfigureRequest:
      if (s_credential_fault || s_erase_incomplete) {
        break;  // fail-closed: no listener may open; only erase recovers
      }
      s_boot_failed_offline = false;
#if PLANE_RADAR_ADSB_WORKER
      // Worker build: LATCH the Configure intent and ask the worker to pause. Do
      // NOT snapshot credentials, feed the FSM, or touch the radio/NVS until the
      // worker is provably quiesced (serviced in serviceDeferredNetworkWork).
      if (!core::portalInSession(s_session.state)) {
        core::requestConfigure(nwIntent());
        nwRequestPause();
      }
      break;
#else
      // Capture the working creds for THIS setup transaction, but only when a new
      // session can actually open (not when already in a session, where the core
      // treats ConfigureButton as a no-op). If stored creds are believed present
      // but cannot be captured, snapshotOldConfig() enters the credential fault
      // and returns false -- do NOT open setup (a failed trial could not restore).
      if (!core::portalInSession(s_session.state)) {
        if (!snapshotOldConfig()) {
          break;
        }
      }
      feed(PortalInput::ConfigureButton, now_ms);
      break;
#endif
    case core::ProvisionButtonEvent::EraseConfirmed:
      if (s_erase_incomplete) {
        // The core is already in FactoryErase after an incomplete wipe; a second
        // physically-confirmed gesture retries the erase routine directly (the
        // core would treat EraseConfirmed as a no-op here). The network is already
        // terminally off in this state, so no worker deferral applies.
        executeFactoryErase();  // never returns on a full verified erase
        break;
      }
#if PLANE_RADAR_ADSB_WORKER
      // Worker build: LATCH the Erase intent (supersedes any pending Configure)
      // and pause the worker. The ErasePending marker write, terminalNetworkOff(),
      // and the erase itself are deferred until proven quiescence.
      core::requestErase(nwIntent());
      nwRequestPause();
      break;
#else
      feed(PortalInput::EraseConfirmed, now_ms);  // never returns if it erases
      break;
#endif
    case core::ProvisionButtonEvent::EraseCancelled:
    case core::ProvisionButtonEvent::None:
    default:
      break;
  }
}

void drainButton(uint32_t now_ms) {
  Edge local[kEdgeRingCap];
  uint8_t n = 0;
  bool overflow = false;
  portENTER_CRITICAL(&s_btn_mux);
  overflow = s_edge_overflow;
  s_edge_overflow = false;
  while (s_edge_tail != s_edge_head && n < kEdgeRingCap) {
    local[n].t_ms = s_edge_ring[s_edge_tail].t_ms;
    local[n].down = s_edge_ring[s_edge_tail].down;
    ++n;
    s_edge_tail = static_cast<uint8_t>((s_edge_tail + 1) % kEdgeRingCap);
  }
  const bool level_down = digitalRead(config::kBootPin) == LOW;
  portEXIT_CRITICAL(&s_btn_mux);

  if (overflow) {
    // Fail safe: a lost edge could misclassify a gesture, so cancel/reset it.
    core::provisionButtonInit(&s_button);
    s_button_prompt = core::ProvisionButtonPrompt::None;
  }

  for (uint8_t i = 0; i < n; ++i) {
    const core::ProvisionButtonOutput o = core::provisionButtonUpdate(
        &s_button, kBtnPolicy, local[i].t_ms, local[i].down);
    s_button_prompt = o.prompt;
    handleButtonEvent(o.event, local[i].t_ms);
  }
  // Advance holds/timeouts at the current level so progressive prompts update.
  const core::ProvisionButtonOutput o =
      core::provisionButtonUpdate(&s_button, kBtnPolicy, now_ms, level_down);
  s_button_prompt = o.prompt;
  handleButtonEvent(o.event, now_ms);
}

// Inter-write pump injected into config_portal: only drain the button so a
// gesture during a bounded response write is never lost. Deliberately does NOT
// step the portal FSM (which could tear the portal down mid-write).
void portalWritePump() { drainButton(millis()); }

// ---------------------------------------------------------------------------
// State-driven input derivation
// ---------------------------------------------------------------------------
// On a per-attempt timeout or a config mismatch, either retry or exhaust the
// stored/restored STA connect. A retry does NOT treat no-IP alone as a disconnect
// barrier: it performs a secure stopped-radio reset (which DEFINITIVELY drops any
// pending association), then REAPPLIES and VERIFIES the fixed expected config
// before baselining GOT_IP and reconnecting. If the reset / reapply / verify
// cannot be proven, it fails closed into the credential fault.
void staConnectRetryOrFail(uint32_t now_ms) {
  if (s_connect_attempt >= config::kWifiConnectAttempts) {
    s_connect_active = false;
    esp_wifi_disconnect();
    feed(PortalInput::StaConnectFailed, now_ms);
    return;
  }
  ++s_connect_attempt;
  bool ok = s_connect_expected_present &&
            services::wifi_creds::hasSsid(s_connect_expected_config) &&
            secureStaReset() &&
            services::wifi_creds::applySta(s_connect_expected_config);
  if (ok) {
    wifi_config_t current;
    ok = services::wifi_creds::getSta(&current) &&
         services::wifi_creds::sameCredentials(current,
                                               s_connect_expected_config);
    services::wifi_creds::zeroize(&current);
  }
  if (!ok || linkUp()) {
    s_connect_active = false;
    enterCredentialFault();  // cannot prove a clean reset + reapply: fail closed
    return;
  }
  // Baseline GOT_IP immediately before the fresh connect against the reapplied,
  // verified expected config.
  s_connect_attempt_start = now_ms;
  s_connect_gotip_baseline = s_got_ip_seq.load(std::memory_order_relaxed);
  esp_wifi_connect();
}

void serviceConnecting(uint32_t now_ms) {
  if (!s_connect_active) {
    return;
  }
  if (linkUp() &&
      s_got_ip_seq.load(std::memory_order_relaxed) != s_connect_gotip_baseline) {
    // Fresh generation-bound GOT_IP: before accepting, verify the driver's CURRENT
    // STA config still matches the config this attempt was started against. Never
    // accept a link that came up on a different/previous/unverifiable config.
    wifi_config_t current;
    const bool matches =
        s_connect_expected_present &&
        services::wifi_creds::getSta(&current) &&
        services::wifi_creds::sameCredentials(current, s_connect_expected_config);
    services::wifi_creds::zeroize(&current);
    if (matches) {
      s_connect_active = false;
      feed(PortalInput::StaConnected, now_ms);
    } else {
      // Mismatched/unverifiable link: reset + reapply + verify the fixed expected
      // config and retry (or exhaust) rather than accept it.
      staConnectRetryOrFail(now_ms);
    }
    return;
  }
  if (core::elapsedAtLeast(now_ms, s_connect_attempt_start,
                           config::kWifiConnectAttemptMs)) {
    staConnectRetryOrFail(now_ms);
  }
}

void serviceCandidate(uint32_t now_ms) {
  // Pending-failure phase: a failed stage / mismatch / timeout is being reported.
  // Before the identity-bound CandidateFailed, PROVE the pending association is
  // cancelled via a secure stopped-radio reset to an empty RAM STA config (no-IP +
  // empty STA config verified) -- a pending association often has no IP, so no-IP
  // alone is not proof. If cancellation cannot be proven, fail closed.
  if (s_candidate_pending_fail) {
    if (proveConnectCancelled()) {
      s_candidate_pending_fail = false;
      wipeCandidate();  // wipe the failed candidate before reopening the session
      feed(PortalInput::CandidateFailed, now_ms);
    } else {
      enterCredentialFault();
    }
    return;
  }
  if (!s_candidate_active) {
    return;
  }
  if (linkUp() && s_got_ip_seq.load(std::memory_order_relaxed) !=
                      s_candidate_gotip_baseline) {
    // Fresh generation-bound GOT_IP. Before accepting, verify the driver's CURRENT
    // STA credentials still match the staged candidate -- never accept a link that
    // came up on a different/previous config.
    wifi_config_t current;
    const bool matches =
        services::wifi_creds::getSta(&current) &&
        services::wifi_creds::sameCredentials(current, s_candidate_config);
    services::wifi_creds::zeroize(&current);
    s_candidate_active = false;
    if (matches) {
      feed(PortalInput::CandidateConnected, now_ms);
    } else {
      beginCandidatePendingFail();  // wrong config: fail closed once proven cancelled
    }
    return;
  }
  if (core::elapsedAtLeast(now_ms, s_candidate_start,
                           config::kCandidateConnectTimeoutMs)) {
    beginCandidatePendingFail();  // timeout: prove cancellation, then CandidateFailed
  }
}

void serviceOfflineIdle(uint32_t now_ms) {
  if (!s_offline_anchored) {
    s_offline_anchored = true;
    s_offline_since_ms = now_ms;
    // Allow the first retry once the grace has elapsed.
    s_last_retry_ms = now_ms - config::kWifiReconnectIntervalMs;
  }
  if (!s_have_old_config) {
    return;  // first boot / no creds: never auto-retry (wait for the button)
  }
  if (!core::elapsedAtLeast(now_ms, s_offline_since_ms, config::kWifiDownGraceMs)) {
    return;
  }
  if (!core::elapsedAtLeast(now_ms, s_last_retry_ms,
                            config::kWifiReconnectIntervalMs)) {
    return;
  }
  s_last_retry_ms = now_ms;
  feed(PortalInput::StaRetry, now_ms);  // mints a fresh generation + reconnect
}

void driveController(uint32_t now_ms) {
  // Fail-closed faults: freeze the controller. No ticks, reconnects, or listener
  // -- only the physical factory-erase gesture (drained before this in pumpCore)
  // can act. A credential fault needs an erase to recover; an incomplete erase
  // stays network-off until a second confirmed gesture or a power-cycle resume.
  if (s_credential_fault || s_erase_incomplete) {
    return;
  }

  if (s_session.state != PortalState::StaConnecting) {
    // Belt-and-suspenders: drop any stale connect latch if the FSM left
    // StaConnecting through a non-serviceConnecting path (e.g. ConfigureButton).
    s_connect_active = false;
  }
  if (s_session.state != PortalState::StaOfflineIdle) {
    s_offline_anchored = false;
  }
  if (s_session.state != PortalState::StaOnline) {
    s_link_loss_anchored = false;
  }
  if (s_session.state != PortalState::SetupCleanup &&
      s_session.state != PortalState::TrialCancel) {
    s_cleanup_anchored = false;
  }
  // Deadline tick + preemption live in the core; feed a plain tick every call.
  feed(PortalInput::None, now_ms);

  switch (s_session.state) {
    case PortalState::StaConnecting:
      serviceConnecting(now_ms);
      break;
    case PortalState::StaOnline:
      // Restore the 4 s reconnect grace: a first drop does NOT immediately feed
      // StaLost. Anchor a grace (radar keeps showing with NO WIFI); only a link
      // still down past the grace feeds the generation-bound loss/reconnect.
      if (!linkUp()) {
        if (!s_link_loss_anchored) {
          s_link_loss_anchored = true;
          s_link_loss_since_ms = now_ms;
        } else if (core::elapsedAtLeast(now_ms, s_link_loss_since_ms,
                                        config::kWifiDownGraceMs)) {
          s_link_loss_anchored = false;
          feed(PortalInput::StaLost, now_ms);
        }
      } else {
        s_link_loss_anchored = false;  // link returned within grace: cancel
      }
      break;
    case PortalState::StaOfflineIdle:
      serviceOfflineIdle(now_ms);
      break;
    case PortalState::PortalRadioPrep:
      if (radioReady()) {
        feed(PortalInput::PortalRadioReady, now_ms);
      }
      break;
    case PortalState::PortalAwaitSecrets:
      if (s_secrets_result == OpResult::Ok) {
        s_secrets_result = OpResult::None;
        feed(PortalInput::SecretsReady, now_ms);
      } else if (s_secrets_result == OpResult::Fail) {
        s_secrets_result = OpResult::None;
        feed(PortalInput::SecretsFailed, now_ms);
      }
      break;
    case PortalState::CandidateQuiesce:
      if (services::portal::active() || apModeUp() || linkUp()) {
        teardownPortalToSta();
      }
      if (quiesced()) {
        feed(PortalInput::PortalQuiesced, now_ms);
      }
      break;
    case PortalState::CandidateTrial:
      serviceCandidate(now_ms);
      break;
    case PortalState::Commit:
      if (s_commit_result == OpResult::Ok) {
        s_commit_result = OpResult::None;
        feed(PortalInput::CommitDone, now_ms);
      } else if (s_commit_result == OpResult::Fail) {
        s_commit_result = OpResult::None;
        feed(PortalInput::CommitFailed, now_ms);
      }
      break;
    case PortalState::ReopenRestore:
    case PortalState::Restore:
      if (s_restore_result == OpResult::Ok) {
        s_restore_result = OpResult::None;
        feed(PortalInput::RestoreDone, now_ms);
      } else if (s_restore_result == OpResult::Fail) {
        s_restore_result = OpResult::None;
        feed(PortalInput::RestoreFailed, now_ms);
      }
      break;
    case PortalState::SetupCleanup:
      if (services::portal::active() || apModeUp() || linkUp()) {
        teardownPortalToSta();
      }
      if (!s_cleanup_anchored) {
        s_cleanup_anchored = true;
        s_cleanup_since_ms = now_ms;
        s_cleanup_forced = false;
      } else if (!quiesced() && !s_cleanup_forced &&
                 core::elapsedAtLeast(now_ms, s_cleanup_since_ms,
                                      kCleanupQuiesceTimeoutMs)) {
        forceRadioReset();  // bounded fallback if teardown stalled
        s_cleanup_forced = true;
      }
      if (quiesced()) {  // ack ONLY after the predicate actually holds
        s_cleanup_anchored = false;
        feed(PortalInput::PortalQuiesced, now_ms);
      }
      break;
    case PortalState::TrialCancel:
      // Prove the candidate is cancelled -- no-IP AND the driver's current STA
      // config is empty (not the candidate) -- via a secure stopped-radio reset
      // before acking CandidateCancelled. A pending association often has no IP, so
      // no-IP alone is NOT proof. If it cannot be proven, fail closed.
      if (proveConnectCancelled()) {
        s_candidate_active = false;
        s_candidate_pending_fail = false;
        wipeCandidate();
        s_cleanup_anchored = false;
        feed(PortalInput::CandidateCancelled, now_ms);
      } else {
        enterCredentialFault();
      }
      break;
    default:
      break;
  }

  // Transactional listener bring-up failed: drive an ordered SessionCancel so the
  // core never believes a listener exists. Resample time for the fresh feed.
  if (s_listener_start_failed) {
    s_listener_start_failed = false;
    feed(PortalInput::SessionCancel, millis());
  }

  if (s_session.state == PortalState::StaOnline) {
    s_boot_failed_offline = false;
    // Truthful settings-save warning: after a commit whose display settings did
    // not persist, own the panel for a bounded window, then clear so the radar
    // reclaims it. The committed Wi-Fi credential is never rolled back for this.
    if (s_settings_save_failed) {
      if (!s_settings_warn_anchored) {
        s_settings_warn_anchored = true;
        s_settings_warn_since_ms = now_ms;
      } else if (core::elapsedAtLeast(now_ms, s_settings_warn_since_ms,
                                      kSettingsWarnMs)) {
        s_settings_save_failed = false;
        s_settings_warn_anchored = false;
      }
    }
  } else {
    s_settings_warn_anchored = false;
  }
  // Steady state: wipe the old-config RAM bytes captured for a setup transaction.
  if (s_session.state == PortalState::StaOnline ||
      s_session.state == PortalState::StaOfflineIdle) {
    wipeOldConfigBytes();
    // The connect-cycle expected-config copy is no longer needed at a steady
    // state (the driver holds the working credential; the next StaConnecting cycle
    // re-snapshots it fresh). Wipe it so the STA credential does not linger here.
    if (s_connect_expected_present) {
      services::wifi_creds::zeroize(&s_connect_expected_config);
      s_connect_expected_present = false;
    }
  }
}

// ---------------------------------------------------------------------------
// Status-screen ownership + drawing (deduped; single framebuffer)
// ---------------------------------------------------------------------------
enum ScreenKind : uint8_t {
  kNone = 0,
  kPreparing,
  kCredentials,
  kTesting,
  kCandidateFailed,
  kCommitting,
  kClosing,
  kSavedFailed,
  kCredentialFault,
  kEraseIncomplete,
  kSettingsSaveFailed,
  kButtonPrompt,
  kNetworkQuiescing,
};

void drawScreen(uint8_t kind, uint32_t param) {
  if (kind == s_last_screen_kind && param == s_last_screen_param) {
    return;
  }
  // Leaving the credentials screen: securely wipe the static password buffer so
  // the one-time WPA2 secret never lingers in status RAM.
  if (s_last_screen_kind == kCredentials && kind != kCredentials) {
    statusScreenClearCredentials();
  }
  s_last_screen_kind = kind;
  s_last_screen_param = param;
  switch (kind) {
    case kPreparing:
      statusScreenPortalPreparing();
      break;
    case kCredentials:
      statusScreenPortalCredentials(s_secrets.ssid, s_secrets.password, param);
      break;
    case kTesting:
      statusScreenCandidateTesting();
      break;
    case kCandidateFailed:
      statusScreenCandidateFailed();
      break;
    case kCommitting:
      statusScreenCommitting();
      break;
    case kClosing:
      statusScreenCandidateTesting();  // brief closing state reuses "testing"
      break;
    case kSavedFailed:
      statusScreenSavedWifiFailed();
      break;
    case kCredentialFault:
      statusScreenCredentialFault();
      break;
    case kEraseIncomplete:
      statusScreenEraseIncomplete();
      break;
    case kSettingsSaveFailed:
      statusScreenSettingsSaveFailed();
      break;
    case kButtonPrompt:
      statusScreenButtonPrompt(static_cast<core::ProvisionButtonPrompt>(param));
      break;
    case kNetworkQuiescing:
      statusScreenNetworkQuiescing(param != 0);  // param: 1 = erase, 0 = configure
      break;
    default:
      break;
  }
}

bool buttonPromptOwning() {
  return s_button_prompt != core::ProvisionButtonPrompt::None &&
         !core::portalInSession(s_session.state);
}

void updateDisplay(uint32_t now_ms) {
  // An in-progress gesture prompt takes priority even in a fault mode, so the
  // erase-recovery hold shows its progressive feedback (Release/Arm/Confirm)
  // rather than a frozen fault screen. buttonPromptOwning() is true only while a
  // gesture is actually being performed (and never during a portal session).
  if (buttonPromptOwning()) {
    drawScreen(kButtonPrompt, static_cast<uint32_t>(s_button_prompt));
    return;
  }
#if PLANE_RADAR_ADSB_WORKER
  // A latched Configure/Erase waiting for the worker to quiesce owns the panel
  // with a truthful wait screen (after the gesture prompt releases). This sits
  // above the fault screens so an Erase confirmed out of a credential fault shows
  // the honest "stopping network before erase" wait rather than the frozen fault.
  {
    const core::NetworkWorkIntent pending = nwPendingIntent();
    if (pending != core::NetworkWorkIntent::None) {
      drawScreen(kNetworkQuiescing,
                 pending == core::NetworkWorkIntent::Erase ? 1U : 0U);
      return;
    }
  }
#endif
  if (s_credential_fault) {
    drawScreen(kCredentialFault, 0);  // fail-closed: overrides every other screen
    return;
  }
  if (s_erase_incomplete) {
    drawScreen(kEraseIncomplete, 0);  // fail-closed: incomplete factory erase
    return;
  }
  if (s_settings_save_failed) {
    drawScreen(kSettingsSaveFailed, 0);  // truthful "Wi-Fi saved; settings failed"
    return;
  }
  switch (s_session.state) {
    case PortalState::PortalRadioPrep:
    case PortalState::PortalAwaitSecrets:
      drawScreen(kPreparing, 0);
      break;
    case PortalState::SetupSession:
      drawScreen(kCredentials, remainingSeconds(now_ms));
      break;
    case PortalState::CandidateQuiesce:
    case PortalState::CandidateTrial:
      drawScreen(kTesting, 0);
      break;
    case PortalState::ReopenRestore:
      drawScreen(kCandidateFailed, 0);
      break;
    case PortalState::Commit:
      drawScreen(kCommitting, 0);
      break;
    case PortalState::SetupCleanup:
    case PortalState::TrialCancel:
    case PortalState::Restore:
      drawScreen(kClosing, 0);
      break;
    default:
      if (s_boot_failed_offline) {
        drawScreen(kSavedFailed, 0);
      }
      break;
  }
}

#if PLANE_RADAR_ADSB_WORKER
// Service a latched Configure/Erase intent (worker build). Runs on the main task
// inside pumpCore(): request the worker pause idempotently, wait until it is
// provably quiesced (Paused with every result resolved), then consume the intent
// exactly once and execute the original path. Non-blocking -- it simply returns
// while the worker is still winding down or faulted, so the wait screen keeps
// owning the panel. A faulted worker never proves quiescence, so radio/NVS work
// is never fabricated (fail-closed).
void serviceDeferredNetworkWork(uint32_t now_ms) {
  if (nwPendingIntent() == core::NetworkWorkIntent::None) {
    return;
  }
  nwRequestPause();  // idempotent
  if (!nwQuiesced()) {
    return;  // worker not yet Paused / results unresolved / faulted: keep waiting
  }
  const core::NetworkWorkIntent intent = core::consumeIntent(nwIntent(), true);
  if (intent == core::NetworkWorkIntent::Erase) {
    // Feeds factory_erase -> executeFactoryErase(); never returns on a full wipe.
    feed(PortalInput::EraseConfirmed, now_ms);
    return;
  }
  if (intent == core::NetworkWorkIntent::Configure) {
    // Deferred credential snapshot (kept out of the ISR/button path): only when a
    // new session can actually open. A snapshot fault fails closed (no listener).
    if (!core::portalInSession(s_session.state)) {
      if (!snapshotOldConfig()) {
        return;
      }
    }
    feed(PortalInput::ConfigureButton, now_ms);
  }
}
#endif  // PLANE_RADAR_ADSB_WORKER

void pumpCore(uint32_t now_ms) {
  drainButton(now_ms);
  if (services::portal::active()) {
    services::portal::pump();  // may block up to the bounded write deadline (~3 s)
    now_ms = millis();  // resample: the pre-pump time is now stale (fix: fresh time)
  }
  driveController(now_ms);
#if PLANE_RADAR_ADSB_WORKER
  serviceDeferredNetworkWork(millis());  // release a latched Configure/Erase once quiesced
#endif
}

void ensureInit() {
  if (s_initialized) {
    return;
  }
  s_initialized = true;
  WiFi.persistent(false);  // the adapter owns persistence via esp_wifi_set_storage
  pinMode(config::kBootPin, INPUT_PULLUP);
  core::provisionButtonInit(&s_button);
  core::portalSessionInit(&s_session);
  memset(&s_old_config, 0, sizeof(s_old_config));
  memset(&s_candidate_config, 0, sizeof(s_candidate_config));
  memset(&s_connect_expected_config, 0, sizeof(s_connect_expected_config));
  services::portal::setPumpFn(&portalWritePump);
  // Abort any portal accept/serve/write the instant the ORIGINAL session deadline
  // lapses, so the HTTP/DNS/AP surface is never held open past five minutes.
  services::portal::setExpiredFn(&portalDeadlineExpired);
  if (!s_isr_attached) {
    attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                    onButtonIsr, CHANGE);
    s_isr_attached = true;
  }
}

}  // namespace

// Trampoline in the global namespace so config_portal's function pointer can bind
// to the anonymous-namespace save handler.
services::portal::SaveOutcome portalSaveTrampoline(const core::HttpRequest& req,
                                                   void* /*ctx*/) {
  using services::portal::SaveOutcome;
  if (s_session.state != PortalState::SetupSession) {
    return SaveOutcome::BadRequest;
  }

  static core::ProvisioningForm form;
  const core::FormParseResult pr =
      core::urlFormParse(req.body, req.body_len, /*max_pairs=*/12, &form);
  if (pr != core::FormParseResult::Ok) {
    core::secureZero(&form, sizeof(form));
    if (pr == core::FormParseResult::BadEncoding ||
        pr == core::FormParseResult::MalformedPair) {
      return SaveOutcome::BadRequest;
    }
    return SaveOutcome::Unprocessable;  // oversized / duplicate / too many fields
  }

  const core::AuthorizedProvisioning auth = core::authenticateProvisioning(
      form, s_secrets.csrf, core::kPortalCsrfTokenLen, s_session.session_id);
  if (!auth.authorized()) {
    const core::ProvisioningValidity v = auth.validity();
    core::secureZero(&form, sizeof(form));
    if (v == core::ProvisioningValidity::Ok ||
        v == core::ProvisioningValidity::CsrfMissing ||
        v == core::ProvisioningValidity::CsrfInvalid) {
      return SaveOutcome::Forbidden;  // CSRF/authentication failure
    }
    return SaveOutcome::Unprocessable;  // ssid / psk / coordinate field failure
  }

  double lat = 0.0;
  double lon = 0.0;
  if (!core::parseCoordinates(form.lat.value, form.lon.value, &lat, &lon)) {
    core::secureZero(&form, sizeof(form));
    return SaveOutcome::Unprocessable;
  }

  const bool psk_present = form.psk.present && form.psk.len > 0;
  services::wifi_creds::buildSta(&s_candidate_config, form.ssid.value,
                                 form.ssid.len,
                                 psk_present ? form.psk.value : nullptr,
                                 psk_present ? form.psk.len : 0);
  // Blank password: reuse the stored password ONLY when the submitted SSID
  // exactly matches the stored SSID; otherwise a blank password means an open
  // network (already built as open above).
  if (!psk_present && s_have_old_config &&
      services::wifi_creds::ssidEquals(s_old_config, form.ssid.value,
                                       form.ssid.len)) {
    services::wifi_creds::inheritPassword(s_old_config, &s_candidate_config);
  }

  s_staged.valid = true;
  s_staged.lat = lat;
  s_staged.lon = lon;
  s_staged.use_miles = core::formCheckboxOn(form.use_miles);
  s_staged.show_runways = core::formCheckboxOn(form.show_runways);

  const core::PortalActions a =
      core::portalSessionSubmitCandidate(&s_session, kPolicy, millis(), auth);
  // Clear the decoded form / CSRF / home-password workspace immediately.
  core::secureZero(&form, sizeof(form));
  if (!a.stop_listener) {
    services::wifi_creds::zeroize(&s_candidate_config);
    s_staged = StagedSettings{};
    return SaveOutcome::Unprocessable;  // submission not accepted (e.g. expired)
  }
  captureAck(a);  // quiesce op identity to echo on PortalQuiesced
  return SaveOutcome::Accepted;
}

// ===========================================================================
// Facade
// ===========================================================================
void wifiControllerInit() { ensureInit(); }

void wifiRegisterEventHandlers() {
  if (s_events_registered) {
    return;
  }
  WiFi.onEvent(&onWifiStaDisconnectedEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(&onWifiStaGotIpEvent, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  s_events_registered = true;
}

void wifiBootConnect() {
  ensureInit();
  // Initialize the radio safely and let the driver restore the stored STA
  // credential from NVS (FLASH storage is selected at esp_wifi_start), THEN select
  // RAM so every runtime write stays out of NVS. We do NOT call setStorageFlash()
  // before Wi-Fi init -- that is a no-op that cannot load credentials.
  WiFi.persistent(true);  // wifiLowLevelInit selects FLASH storage at radio start
  WiFi.mode(WIFI_STA);    // init + start: the driver restores the saved STA config
  WiFi.setAutoReconnect(false);
  delay(50);
  WiFi.persistent(false);  // runtime persistence is adapter-owned from here

  // (1) Durable provisioning-transaction marker check BEFORE any network
  // connection. A power loss mid-commit or mid-erase is detected here and handled
  // fail-closed: an ErasePending marker resumes the already physically-authorized
  // erase; a CommitInProgress marker (or an unreadable marker) is a credential
  // fault; None/Absent is a normal boot.
  core::TxnMarkerState marker = core::TxnMarkerState::None;
  const services::provision_marker::ReadResult mr =
      services::provision_marker::read(&marker);
  if (mr == services::provision_marker::ReadResult::Present &&
      marker == core::TxnMarkerState::ErasePending) {
    services::wifi_creds::setStorageRam();
    // Worker-firmware safety: the static worker task was created in setup()
    // (before wifiBootConnect), but NO dispatch has happened yet -- the main loop
    // has not run. The task is blocked on its empty request queue and owns no
    // transport or candidate, so this direct erase (terminalNetworkOff + wipe) is
    // safe WITHOUT a pause/quiesce handshake: there is no in-flight worker network
    // work at boot to defer. (In the default firmware there is no worker at all.)
    executeFactoryErase();    // resume erase; restarts on a full verified wipe
    updateDisplay(millis());  // incomplete resume: show the fail-closed screen
    return;
  }
  if (mr == services::provision_marker::ReadResult::Error ||
      (mr == services::provision_marker::ReadResult::Present &&
       marker == core::TxnMarkerState::CommitInProgress)) {
    // An interrupted commit, or an unreadable safety marker, means the stored
    // credential is in an unknown state. Fail closed; only a factory erase (which
    // also clears the marker) recovers -- never auto-connect or auto-open setup.
    services::wifi_creds::setStorageRam();
    enterCredentialFault();
    updateDisplay(millis());
    return;
  }

  // (2) Boot STA credential snapshot with explicit Present / Absent / Error
  // handling. A read ERROR is a fail-closed credential fault (the driver could not
  // be queried, so first boot vs. lost credentials cannot be proven); only a
  // VERIFIED empty config is genuine first boot.
  s_have_old_config = false;
  s_old_config_bytes_present = false;
  if (!services::wifi_creds::snapshotSta(&s_old_config)) {
    services::wifi_creds::zeroize(&s_old_config);
    services::wifi_creds::setStorageRam();
    enterCredentialFault();  // snapshot error: never auto portal/reconnect
    updateDisplay(millis());
    return;
  }
  const bool present = services::wifi_creds::hasSsid(s_old_config);
  if (present) {
    s_have_old_config = true;
    s_old_config_bytes_present = true;
  } else {
    services::wifi_creds::zeroize(&s_old_config);  // verified empty: first boot
  }

  // Runtime: the adapter owns persistence; all writes go to RAM unless a commit /
  // erase critical section explicitly selects FLASH. A failure to prove RAM
  // storage is itself a credential fault (later writes could silently reach flash).
  if (!services::wifi_creds::setStorageRam()) {
    enterCredentialFault();
    updateDisplay(millis());
    return;
  }

  feed(present ? PortalInput::BootHasCredentials
               : PortalInput::BootNoCredentials,
       millis());

  if (!present) {
    return;  // first boot: setup is opening; the main loop pumps the portal
  }

  // Boot connect with the connecting UI (radar not up yet). Blocks until the FSM
  // leaves StaConnecting (online, or offline-idle on exhaustion) OR a fail-closed
  // fault freezes the controller -- a stored-connect retry can enter the credential
  // fault (which does NOT change the FSM state), and driveController then freezes,
  // so the StaConnecting exit alone would spin forever. Break on the fault flags too
  // and let updateDisplay draw the truthful fault/incomplete screen.
  char ssid[33];
  services::wifi_creds::copySsid(s_old_config, ssid, sizeof(ssid));
  statusScreenConnectingBegin(ssid);
  s_boot_ui_active = true;
  while (s_session.state == PortalState::StaConnecting && !s_credential_fault &&
         !s_erase_incomplete) {
    pumpCore(millis());
    if (s_session.state == PortalState::StaConnecting && !s_credential_fault &&
        !s_erase_incomplete) {
      statusScreenConnectingTick();
    }
    delay(config::kWifiConnectingFrameMs);
  }
  s_boot_ui_active = false;
  s_last_screen_kind = kNone;  // force a redraw of whatever owns the panel next

  if (s_session.state == PortalState::StaOfflineIdle) {
    s_boot_failed_offline = true;
  }
  updateDisplay(millis());  // draw whatever now owns the panel (fault / offline / none)
}

void wifiLoop() {
  const uint32_t now = millis();
  pumpCore(now);
  updateDisplay(now);
}

bool wifiLinkUp() { return linkUp(); }

bool wifiOwnsDisplay() {
  return s_credential_fault || s_erase_incomplete || s_settings_save_failed ||
         core::portalInSession(s_session.state) || s_boot_ui_active ||
         buttonPromptOwning() || s_boot_failed_offline
#if PLANE_RADAR_ADSB_WORKER
         || nwPendingIntent() != core::NetworkWorkIntent::None
#endif
      ;
}

bool wifiConsumeImmediateFetch() {
  if (!s_immediate_fetch_pending) {
    return false;
  }
  s_immediate_fetch_pending = false;
  return true;
}

bool wifiConsumeRangeTap() {
  if (!s_range_tap_pending) {
    return false;
  }
  s_range_tap_pending = false;
  return true;
}

uint32_t wifiDisconnectSeq() {
  return s_disconnect_seq.load(std::memory_order_relaxed);
}

void wifiSetNetworkWorkHooks(const WifiNetworkWorkHooks& hooks) {
#if PLANE_RADAR_ADSB_WORKER
  s_nw_hooks = hooks;  // main wires these to the worker facade
#else
  (void)hooks;  // default (worker-free) build: inert
#endif
}
