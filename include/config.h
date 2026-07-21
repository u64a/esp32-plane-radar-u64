#pragma once

#include <cstdint>

#include <driver/gpio.h>

#include "core/http_request.h"
#include "core/poll_policy.h"
#include "core/portal_secrets.h"
#include "core/portal_session.h"
#include "core/provision_button.h"
#include "core/radar_data_state.h"

namespace config {

// --- Secure Wi-Fi setup portal (temporary SoftAP; see README + core/portal_*) ---
//
// There is NO permanent LAN listener, no mDNS/OTA, and no open AP. The portal is
// a temporary SoftAP that exists ONLY while the core PortalSession is in a setup
// session, and only after the radio + secrets handshake. The SSID is derived at
// runtime from the factory MAC by core::formatPortalSsid ("PlaneRadar-XXYYZZ"),
// so there is no static AP name here.
constexpr char kPortalIp[] = "192.168.4.1";      // SoftAP address (exact)
constexpr uint8_t kPortalIpOctets[4] = {192, 168, 4, 1};
constexpr uint8_t kPortalNetmaskOctets[4] = {255, 255, 255, 0};  // /24
constexpr uint8_t kPortalApChannel = 1;          // fixed 2.4 GHz channel
constexpr uint8_t kPortalApMaxConnections = 1;   // at most ONE station
constexpr bool kPortalApVisible = true;          // WPA2-PSK, broadcast SSID (never open)

/** Session liveness deadline: the portal closes 5 min after the original session
 *  start; candidate retries do NOT extend it. Mirrors the core policy. */
constexpr uint32_t kPortalSessionTimeoutMs = 300000;  // 5 minutes
static_assert(kPortalSessionTimeoutMs ==
                  core::kDefaultPortalSessionPolicy.session_timeout_ms,
              "portal session timeout drifted from core policy");

// --- Captive HTTP + DNS ---
constexpr uint16_t kPortalHttpPort = 80;
constexpr uint16_t kPortalDnsPort = 53;
/** Per-connection HTTP timeouts (single request per connection). */
constexpr uint32_t kPortalHttpIdleTimeoutMs = 3000;     // max gap between bytes
constexpr uint32_t kPortalHttpOverallTimeoutMs = 8000;  // max total per request
/** Cumulative bounded deadline for writing one HTTP response (non-blocking
 *  lwip_send with DNS/button pumped between attempts). */
constexpr uint32_t kPortalHttpWriteDeadlineMs = 3000;

// Enforced HTTP parser limits (mirror the core production maxima). Validated once
// at startup via core::httpLimitsValid before serving.
constexpr core::HttpLimits kPortalHttpLimits = core::kDefaultHttpLimits;

// --- Candidate credential trial ---
/** A submitted candidate must obtain a fresh generation-bound GOT_IP within this
 *  budget or the trial fails and the old credentials are restored untouched. */
constexpr uint32_t kCandidateConnectTimeoutMs = 30000;  // 30 s

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after going offline before a background reconnect (avoids churn on brief
 *  drops); then retry at most this often. Runtime OfflineIdle retries only. */
constexpr unsigned long kWifiDownGraceMs = 4000;
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT / provisioning button (ESP32-C3 Super Mini, GPIO 9, active LOW) ---
//
// The approved two-stage gesture policy (core::ProvisionButton). A tap cycles the
// range; a medium hold opens setup; a long hold ARMS erase but the SAME hold can
// never erase (release + a second confirming hold is required). There is NO
// 3-second immediate reset and NO power-on GPIO9 hold dependency.
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
constexpr core::ProvisionButtonPolicy kButtonPolicy =
    core::kDefaultProvisionButtonPolicy;
static_assert(kButtonPolicy.tap_min_ms == 40 && kButtonPolicy.tap_max_ms == 1000 &&
                  kButtonPolicy.configure_min_ms == 2000 &&
                  kButtonPolicy.arm_ms == 8000 &&
                  kButtonPolicy.confirm_window_ms == 10000 &&
                  kButtonPolicy.confirm_hold_ms == 3000,
              "button gesture policy drifted from approved core defaults");
/** Ignore BOOT taps shorter than this (debounce); mirrors the gesture tap floor. */
constexpr unsigned long kBootTapMinMs = core::kDefaultProvisionButtonPolicy.tap_min_ms;

// --- Display: GC9A01 1.28" round 240×240 (SPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_0;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_1;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_3;  // display SDA
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_4;  // display SCL

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 52.3676;
constexpr double kDefaultRadarLon = 4.9041;

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 3000;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- ADS-B bounded HTTPS transport (opendata.adsb.fi) ---
constexpr char kAdsbHost[] = "opendata.adsb.fi";
constexpr uint16_t kAdsbPort = 443;
/**
 * Cumulative DNS + TCP + TLS connect budget for one fetch. The timer starts
 * before DNS and only the remainder bounds the socket connect and TLS handshake
 * (each at the core's whole-second granularity). Exception: WiFi.hostByName()
 * exposes no timeout, so DNS alone may run up to the ESP-IDF resolver's ~15 s
 * core timeout that this budget cannot preempt; its elapsed time is still
 * charged here, so the socket/handshake get whatever is left (possibly nothing).
 */
constexpr uint32_t kAdsbConnectTimeoutMs = 8000;
/**
 * Cumulative request/response budget once connected: it spans BOTH the request
 * send and the response decode. The send draws from it first and only the
 * remainder is handed to the decoder -- send and response never each receive a
 * fresh budget.
 */
constexpr uint32_t kAdsbOverallTimeoutMs = 10000;
/** Response-local max idle gap between received bytes before a stall (Timeout). */
constexpr uint32_t kAdsbStallTimeoutMs = 5000;

// --- Phase 6 ADS-B poll backoff + radar-data freshness (ms) ---
// The canonical values live in core::kDefaultAdsbPollPolicy and
// core::kDefaultRadarFreshnessPolicy (Arduino-free and unit-tested). These
// firmware-facing names mirror them, and the static_asserts below keep the two
// in lock-step so a later main-loop integration can build the policies from
// config without drift.
constexpr uint32_t kAdsbSuccessIntervalMs = 3000;    // success, incl. empty ac[]
constexpr uint32_t kAdsbTransientInitialMs = 5000;   // first transient backoff
constexpr uint32_t kAdsbTransientCapMs = 60000;      // transient backoff ceiling
constexpr uint32_t kAdsbRateDefaultMs = 60000;       // 429 without Retry-After
constexpr uint32_t kAdsbRetryAfterMinMs = 5000;      // Retry-After clamp floor
constexpr uint32_t kAdsbRetryAfterMaxMs = 300000;    // Retry-After clamp ceiling
constexpr uint32_t kAdsbPermanentBackoffMs = 300000; // permanent/other errors
constexpr uint32_t kRadarStaleMs = 15000;            // Live -> Stale age
constexpr uint32_t kRadarOfflineMs = 60000;          // -> Offline / hide age

static_assert(kAdsbSuccessIntervalMs == kAdsbFetchIntervalMs,
              "success interval must equal the legacy fetch interval");
static_assert(kAdsbSuccessIntervalMs == core::kDefaultAdsbPollPolicy.success_ms,
              "config success interval drifted from core poll policy");
static_assert(
    kAdsbTransientInitialMs == core::kDefaultAdsbPollPolicy.transient_initial_ms,
    "config transient initial drifted from core poll policy");
static_assert(kAdsbTransientCapMs == core::kDefaultAdsbPollPolicy.transient_cap_ms,
              "config transient cap drifted from core poll policy");
static_assert(kAdsbRateDefaultMs == core::kDefaultAdsbPollPolicy.rate_default_ms,
              "config rate default drifted from core poll policy");
static_assert(
    kAdsbRetryAfterMinMs == core::kDefaultAdsbPollPolicy.retry_after_min_ms,
    "config Retry-After minimum drifted from core poll policy");
static_assert(
    kAdsbRetryAfterMaxMs == core::kDefaultAdsbPollPolicy.retry_after_max_ms,
    "config Retry-After maximum drifted from core poll policy");
static_assert(kAdsbPermanentBackoffMs == core::kDefaultAdsbPollPolicy.permanent_ms,
              "config permanent backoff drifted from core poll policy");
static_assert(kRadarStaleMs == core::kDefaultRadarFreshnessPolicy.stale_ms,
              "config stale threshold drifted from core freshness policy");
static_assert(kRadarOfflineMs == core::kDefaultRadarFreshnessPolicy.offline_ms,
              "config offline threshold drifted from core freshness policy");

// --- Phase 7: trusted UTC time + verified TLS --------------------------------
//
// ADS-B network connections are forbidden until trusted UTC is established after
// boot. The CA chain and hostname are verified by mbedTLS via the pinned CA
// bundle (services/adsb_ca_bundle.h); because the pinned ESP32-C3 SDK builds
// mbedTLS without CONFIG_MBEDTLS_HAVE_TIME_DATE, the app additionally checks the
// peer certificate's notBefore/notAfter against trusted UTC after the handshake.

// Deterministic release/build epoch floor (UTC seconds) -- NOT __DATE__/__TIME__.
// Lower bound on plausible "now": SNTP samples earlier than this (minus a small
// rollback tolerance) are rejected, and every boot must accept a fresh sample at
// or after this floor before the first ADS-B connection. Corresponds to the
// Phase 7 release date 2026-07-20T00:00:00Z. Reproducible update procedure:
//   PLANE_RADAR_RELEASE_EPOCH = 1784505600 = `date -u -d 2026-07-20 +%s`.
// For a new release bump this constant (or pass -DPLANE_RADAR_RELEASE_EPOCH).
#ifndef PLANE_RADAR_RELEASE_EPOCH
#define PLANE_RADAR_RELEASE_EPOCH 1784505600
#endif
constexpr int64_t kReleaseEpochFloorUnix = PLANE_RADAR_RELEASE_EPOCH;
static_assert(kReleaseEpochFloorUnix >= 1704067200,
              "release epoch floor looks unset/too old (expected >= 2024-01-01)");

// Trusted-time acceptance policy (mirrors core::TimeTrustPolicy). A sample is
// accepted only within [floor - rollback, floor + future_ceiling]; otherwise the
// service stays/returns untrusted and ADS-B fetches stay blocked.
//
// Rollback tolerance is deliberately small (5 min): a genuine SNTP resync from a
// disciplined stratum-1/2 source is accurate to well under a second, so the only
// reason to allow ANY backward slack is bounded jitter between resyncs -- not a
// clock that runs an hour slow. Anything further below the floor is a rollback or
// a hostile server and is rejected (fail closed).
//
// The future ceiling stays generous (~10 y) so a device that is first powered on
// long after its build still accepts today's genuine time. This is safe because
// forward time can no longer be persistently poisoned: the persisted floor
// advances ONLY to a CA-signed peer leaf certificate's notBefore epoch (never an
// SNTP-derived value), and only when that authenticated notBefore is at least
// kTimeFloorMinAdvanceSec (24 h) beyond the stored floor. An unauthenticated NTP
// attacker cannot choose a CA-signed notBefore, so a spoofed SNTP sample can
// cause only a non-persistent, in-session DoS -- it can never poison NVS. The
// CA-authenticated candidate -- not the ceiling -- is the security boundary
// (proven by the native ratchet tests).
constexpr int64_t kTimeRollbackToleranceSec = 300;                 // <= 5 min below floor
constexpr int64_t kTimeFutureCeilingSec = 10LL * 365 * 24 * 3600;   // <= ~10 y above floor
constexpr uint32_t kTimeSyncTimeoutMs = 30000;                      // SyncPending -> Retry
constexpr uint32_t kTimeRetryBackoffInitialMs = 15000;             // first Retry wait
constexpr uint32_t kTimeRetryBackoffMaxMs = 300000;                // Retry wait ceiling (5 min)
// Maximum age of an accepted sample before trust is revoked and SNTP re-armed.
// Chosen finite and well below the uint32 millis wrap (~49.7 d) so the derived
// clock's rollover-safe elapsed stays exact, and comfortably above the normal
// SNTP resync interval so a healthy device re-anchors long before this fires.
constexpr uint32_t kTimeTrustedSampleMaxAgeMs = 12UL * 60 * 60 * 1000;  // 12 h

// SNTP: default server is time.cloudflare.com ONLY (adsb.fi is already behind
// Cloudflare, so this adds no additional operator). Up to two optional
// compile-time fallbacks, empty by default. DHCP-provided NTP stays disabled.
constexpr char kSntpServerPrimary[] = "time.cloudflare.com";
constexpr char kSntpServerFallback1[] = "";  // optional; "" = unused
constexpr char kSntpServerFallback2[] = "";  // optional; "" = unused

// Persisted CA-authenticated time floor (NVS). Dedicated namespace/key so it
// never collides with the "radar" / "wifi" / "planeradar" Preferences users.
// Stored ONLY as a lower bound (never as current time) inside a single versioned,
// checksummed blob (core::PersistedFloorRecord: version + floor + FNV-1a). The
// floor value is the CA-signed peer leaf certificate's notBefore epoch from a
// COMPLETE verified ADS-B response -- an authenticated value an unauthenticated
// NTP attacker cannot choose. It is written only when that authenticated notBefore
// advances the stored floor by at least kTimeFloorMinAdvanceSec (24 h), so a
// same/older/concurrently-served alternate cert neither writes nor rolls back and
// a genuine cert rotation advances it once. An in-session flash-wear guard caps
// writes per session; there is NO attacker-controlled cross-reboot time throttle.
constexpr char kTimeFloorNvsNamespace[] = "timefloor";
constexpr char kTimeFloorNvsKey[] = "floorrec";                          // versioned blob
constexpr uint32_t kTimeFloorPersistIntervalMs = 24UL * 60 * 60 * 1000;  // in-session flash-wear guard
constexpr int64_t kTimeFloorMinAdvanceSec = 24LL * 60 * 60;              // >= 24 h authenticated advance per write

// --- Provisioning transaction marker (NVS; power-loss durability) ------------
//
// A tiny, dedicated NVS marker records whether a credential FLASH commit or a
// factory erase was in progress, so a power loss mid-transaction is detected
// fail-closed at the next boot instead of silently resuming a normal connect.
// Dedicated namespace/key so it never collides with the "radar" / "planeradar" /
// "timefloor" / wifi Preferences users. It stores ONLY a versioned, checksummed
// enum (core::TxnMarkerRecord) -- NEVER a password or any credential byte; the
// old/new NVS atomicity of the credential itself remains the actual guarantee.
constexpr char kProvisionMarkerNvsNamespace[] = "provtxn";
constexpr char kProvisionMarkerNvsKey[] = "txnrec";  // versioned, checksummed enum

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
