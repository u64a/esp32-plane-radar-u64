/**
 * Plane Radar — Wi-Fi setup, then a live ADS-B radar on the round GC9A01 display.
 *
 * Phase 6 runtime: every rendered frame is a deterministic RadarDisplayModel
 * built from the freshness view, the published snapshot, the actual Wi-Fi
 * status, and a 500 ms Loading activity phase. Rendering is edge/key driven,
 * ADS-B polling is completion-relative with backoff, and a Wi-Fi drop pauses
 * fetches without hiding the radar.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "core/frame_render.h"
#include "core/poll_policy.h"
#include "core/radar_data_state.h"
#include "core/time_trust.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/adsb_fetch.h"
#include "services/radar_location.h"
#include "services/settings_events.h"
#include "services/timekeeper.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

// Canonical Arduino-free policies (config.h static_asserts its mirrors match).
constexpr core::AdsbPollPolicy kPollPolicy = core::kDefaultAdsbPollPolicy;
constexpr core::RadarFreshnessPolicy kFreshnessPolicy =
    core::kDefaultRadarFreshnessPolicy;

// Deterministic Loading activity phase: three 500 ms buckets, rollover-safe by
// construction (unsigned millis()).
uint8_t loadingPhase(uint32_t now_ms) {
  return static_cast<uint8_t>((now_ms / 500U) % 3U);
}

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

// --- runtime state -----------------------------------------------------------

core::RadarDataState g_data = {};
core::AdsbPollState g_poll = {};
core::ReconnectState g_reconnect = {};

core::FrameRenderKey g_last_key = {};
bool g_has_last_key = false;
bool g_force_redraw = false;
bool g_wifi_connected = false;

// --- rendering ---------------------------------------------------------------

// Advance freshness for `now`, then render only when an event forced it or the
// frame key changed. Exactly one full-frame draw per redraw (one framebuffer,
// no large copies): the model is a few machine words passed by value.
void renderIfNeeded(uint32_t now_ms) {
  core::radarDataAdvance(&g_data, now_ms, kFreshnessPolicy);
  const core::RadarDataView view = core::radarDataView(g_data, now_ms);
  const services::adsb::SnapshotView snapshot =
      services::adsb::publishedSnapshot();
  const bool wifi = wifiConnected();
  const uint8_t phase = loadingPhase(now_ms);

  const core::FrameRenderKey key = core::frameRenderKey(
      view.mode, view.age_seconds, view.settings_revision,
      snapshot.settings_revision, snapshot.count, wifi, phase);

  if (!g_force_redraw && g_has_last_key &&
      core::frameRenderKeyEqual(key, g_last_key)) {
    return;  // steady frame: skip the redraw (Live/Offline stay quiet)
  }

  const ui::RadarDisplayModel model{view, snapshot, wifi, phase};
  ui::radarDisplayDraw(model);
  g_last_key = key;
  g_has_last_key = true;
  g_force_redraw = false;
}

// --- settings events ---------------------------------------------------------

// Consume any pending effective-query and visual-only settings events and apply
// their side effects. A query change resets freshness to the new revision and
// forces one immediate fetch; a visual-only change forces a redraw only. Both
// are single-shot latches, so calling this repeatedly per loop is safe.
void applyPendingSettings(uint32_t now_ms) {
  if (services::settings::consumeQueryChange()) {
    const uint32_t revision = services::settings::revision();
    core::radarDataRevisionChanged(&g_data, revision, now_ms);
    core::adsbSettingsChanged(&g_poll);
    // The published snapshot still carries the previous revision, so the model's
    // revision mismatch hides its now-stale targets until a new publish.
    g_force_redraw = true;
  }
  if (services::settings::consumeVisualChange()) {
    // Units / runway overlay: redraw only — no revision, freshness, or backoff
    // change.
    g_force_redraw = true;
  }
}

// --- BOOT button -------------------------------------------------------------

void onRangeTap() {
  ui::radar::rangeNext();  // marks an effective query change only on a real change
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    onRangeTap();
  }
}

// --- ADS-B fetch flow --------------------------------------------------------

const char* outcomeName(core::PollOutcome outcome) {
  switch (outcome) {
    case core::PollOutcome::Success:
      return "success";
    case core::PollOutcome::Transient:
      return "transient";
    case core::PollOutcome::RateLimited:
      return "rate-limited";
    case core::PollOutcome::Permanent:
      return "permanent";
    case core::PollOutcome::Obsolete:
      return "obsolete";
  }
  return "unknown";
}

const char* publishName(services::adsb::PublishResult result) {
  switch (result) {
    case services::adsb::PublishResult::Published:
      return "published";
    case services::adsb::PublishResult::ObsoleteRevision:
      return "obsolete-rev";
    case services::adsb::PublishResult::InvalidHandle:
      return "invalid-handle";
    case services::adsb::PublishResult::NoCandidate:
      return "no-candidate";
  }
  return "none";
}

// Perform one completion-relative ADS-B poll when connected and due. Captures the
// query atomically, fetches a revision-bound candidate, re-consumes settings that
// changed during the blocking I/O, and publishes only against the still-current
// revision.
void serviceAdsb() {
  // Defense in depth: hold the (possibly immediate) fetch latch pending until
  // trusted UTC is established this boot. adsbFetchAllowed() does NOT mutate poll
  // state, so a pending immediate-fetch survives untrusted time and fires on the
  // first loop after trust. ADS-B connections are forbidden before trusted UTC.
  if (!core::adsbFetchAllowed(core::adsbFetchDue(g_poll, millis()),
                              services::timekeeper::trusted())) {
    return;
  }

  // Consume pending settings first so the capture below reflects the newest query.
  applyPendingSettings(millis());

  // Atomically capture the query parameters and the revision they belong to.
  const double lat = services::location::lat();
  const double lon = services::location::lon();
  const float fetch_km = ui::radar::fetchRadiusKm();
  const uint32_t query_revision = services::settings::revision();

  // Snapshot the Wi-Fi disconnect sequence before the (blocking) fetch so a
  // drop that occurs entirely inside uninterruptible DNS/TCP/TLS is observable
  // afterward even if the stack auto-reconnected in the meantime.
  const uint32_t disconnect_seq_before = wifiDisconnectSeq();

  core::adsbFetchStarted(&g_poll);
  const services::adsb::CandidateResult candidate =
      services::adsb::fetchCandidate(lat, lon, fetch_km, query_revision);

  // Before publication, run the controllable-latency side effects and re-consume
  // settings so a range tap or portal save during the blocking fetch is visible.
  handleBootButton();
  wifiLoop();
  applyPendingSettings(millis());

  const uint32_t completed_ms = millis();
  const bool wifi_up = wifiConnected();
  const uint32_t current_revision = services::settings::revision();
  // Re-read the disconnect sequence after all post-fetch pumps: a change means a
  // Wi-Fi flap happened at some point during the fetch (a "flap"), regardless of
  // the current level-based link state.
  const bool disconnect_flap = core::disconnectSeqChanged(
      disconnect_seq_before, wifiDisconnectSeq());

  services::adsb::PublishResult publish_result =
      services::adsb::PublishResult::NoCandidate;
  bool published_success = false;
  core::PollOutcome outcome;
  if (candidate.fetch.outcome == services::adsb::FetchOutcome::Ok) {
    // A complete, CA + hostname + date verified response arrived (runFetch only
    // returns Ok after a full 200 body decode). Offer to ratchet the persisted
    // floor using the CA-signed peer leaf notBefore stamped on the result -- an
    // authenticated value an unauthenticated NTP attacker cannot choose (NOT
    // SNTP-derived time). This counts even if publication is skipped for a stale
    // revision, because the fetch itself was fully verified. It is NEVER gated on
    // a mere TCP/TLS connect.
    services::timekeeper::noteVerifiedCertFloor(
        candidate.fetch.authenticated_cert_not_before_unix, completed_ms);
    publish_result =
        services::adsb::publishCandidate(candidate.handle, current_revision);
    outcome = services::adsb::pollOutcomeForPublish(publish_result);
    if (publish_result == services::adsb::PublishResult::Published) {
      // Only a real publication advances freshness / last-success.
      published_success = true;
      core::radarDataSuccess(&g_data, current_revision, completed_ms);
    }
  } else {
    outcome = services::adsb::pollOutcomeFor(candidate.fetch.outcome);
  }

  // A Wi-Fi drop still down at completion is a pause, not an ADS-B failure.
  outcome = core::effectiveOutcomeAtCompletion(outcome, wifi_up);

  // A Wi-Fi flap during the fetch (edge-detected, even if already reconnected)
  // that did not yield a published success and whose outcome is link-sensitive
  // is likewise a pause (Obsolete), so a spurious drop never advances backoff.
  outcome = services::adsb::effectiveOutcomeAfterFlap(
      candidate.fetch.outcome, outcome, published_success, disconnect_flap);
  if (disconnect_flap) {
    // Raise a newer forced-immediate request BEFORE adsbFetchCompleted so the
    // in-flight sequence logic preserves the immediate latch across this (older)
    // completion, forcing exactly one refresh even though Wi-Fi may already have
    // reconnected. If the link is still down, the latch simply survives until a
    // later loop reconnects and services the immediate fetch.
    core::adsbSettingsChanged(&g_poll);
  }

  core::adsbFetchCompleted(&g_poll, completed_ms, kPollPolicy, outcome,
                           candidate.fetch.retry_after_present,
                           candidate.fetch.retry_after_ms);

  // Concise, payload-free outcome line (no host/URL/body/secrets).
  Serial.printf(
      "adsb: outcome=%s status=%d bytes=%lu count=%u rev=%lu publish=%s "
      "flap=%d next=%lums\n",
      outcomeName(outcome), candidate.fetch.http_status,
      static_cast<unsigned long>(candidate.fetch.bytes_received),
      candidate.fetch.aircraft_count,
      static_cast<unsigned long>(current_revision), publishName(publish_result),
      disconnect_flap ? 1 : 0,
      static_cast<unsigned long>(g_poll.next_interval_ms));

  g_force_redraw = true;  // fetch completion / publication is a render trigger
}

// --- Wi-Fi branches ----------------------------------------------------------

void serviceConnected() {
  if (!g_wifi_connected) {
    // Edge: reconnected outside the blocking reconnect path (e.g. the stack's
    // own auto-reconnect). Restore the model and force exactly one immediate
    // fetch without resetting the transient streak.
    g_wifi_connected = true;
    core::adsbSettingsChanged(&g_poll);
    g_force_redraw = true;
  }
  core::reconnectConnected(&g_reconnect);
  serviceAdsb();
}

void serviceDisconnected(uint32_t now_ms) {
  if (g_wifi_connected) {
    // Edge: Wi-Fi just dropped. Fetches pause simply by staying in this branch,
    // but the radar model is kept: targets remain with NO WIFI and age to
    // Stale/Offline normally. Do not hide the radar or reset freshness/backoff.
    g_wifi_connected = false;
    g_force_redraw = true;
    Serial.println("WiFi lost — will reconnect");
  }
  core::reconnectDisconnected(&g_reconnect, now_ms);
  if (core::reconnectAttemptDue(g_reconnect, now_ms, config::kWifiDownGraceMs,
                                config::kWifiReconnectIntervalMs)) {
    // wifiReconnect() blocks and paints the connecting screen over the panel.
    const bool connected = wifiReconnect();
    core::reconnectAttemptCompleted(&g_reconnect, millis(), connected);
    // Always restore the radar model afterward so the connecting screen cannot
    // stick, whether the attempt succeeded or failed.
    g_force_redraw = true;
    if (connected) {
      g_wifi_connected = true;
      core::adsbSettingsChanged(&g_poll);  // one immediate fetch, streak intact
    }
    // A failed attempt leaves the ADS-B backoff untouched by design.
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  bootButtonInit();
  displayInit();
  const bool frame_buffered = ui::radarDisplayPrepareFrame();
  Serial.printf("radar: rendering mode: %s\n",
                frame_buffered ? "frame sprite" : "direct draw");
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::adsb::setPollFn(wifiLoop);
  // Seed the trusted-time service (reads/validates the persisted floor, registers
  // the SNTP callback). Starts UNTRUSTED; ADS-B is gated until a fresh sample.
  services::timekeeper::init();

  // Register the filtered Wi-Fi disconnect event callback BEFORE any network
  // setup so a mid-fetch disconnect that auto-reconnects is still counted.
  wifiRegisterEventHandlers();

  // The portal connection flow may block and may mutate the settings revision.
  wifiSetupConnect();

  // Always activate the radar frame, even if Wi-Fi is down.
  const uint32_t now_ms = millis();
  core::radarDataStart(&g_data, now_ms);
  core::radarDataRevisionChanged(&g_data, services::settings::revision(),
                                 now_ms);  // adopt the boot-time revision
  // Adopt boot-time settings latches without re-triggering them on loop 1.
  services::settings::consumeQueryChange();
  services::settings::consumeVisualChange();
  core::adsbRadarDisplayed(&g_poll);  // radar visible; forces the first fetch
  g_wifi_connected = wifiConnected();
  g_force_redraw = true;
  renderIfNeeded(now_ms);  // render a real RadarDisplayModel (Loading)
}

void loop() {
  const uint32_t now_ms = millis();

  handleBootButton();
  applyPendingSettings(millis());
  wifiLoop();
  applyPendingSettings(millis());

  // Advance the non-blocking trusted-time state machine before servicing ADS-B
  // so serviceAdsb() sees the freshest trust state (and fetches immediately on
  // the first loop after trust is established).
  services::timekeeper::update(wifiConnected(), millis());

  if (wifiConnected()) {
    serviceConnected();
  } else {
    serviceDisconnected(now_ms);
  }

  renderIfNeeded(millis());
  delay(10);
}
