#pragma once

#include <cstdint>

#include <driver/gpio.h>

#include "core/poll_policy.h"
#include "core/radar_data_state.h"

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT button (ESP32-C3 Super Mini, active LOW) ---
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;

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

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
