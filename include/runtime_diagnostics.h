#pragma once

// Compile-time logging and diagnostics contract for Plane Radar (Phase 10).
//
// Two orthogonal compile-time axes:
//
//   PLANE_RADAR_LOG_LEVEL  — controls Serial output verbosity:
//     0 = OFF   : all logging compiled out; Serial.begin also suppressed
//     1 = ERROR : fault/save-failure/fallback messages only
//     2 = INFO  : fault + ordinary startup/settings/fetch/range messages (default)
//
//   PLANE_RADAR_DIAGNOSTICS — enables optional performance metric instrumentation
//     (fetch timing, render/runway timing, heap snapshots, worker HWM):
//     0 = off (default): no metric state, strings, heap references, or conditional
//         result fields are added to the build. Diagnostic output is absent.
//     1 = on: timing and heap fields/symbols are included; diag output is always
//         produced regardless of PLANE_RADAR_LOG_LEVEL (self-contained).
//
// The default (both unset) preserves existing runtime behavior unchanged.
//
// Secret policy: no secret, Wi-Fi credential, provisioning token, CSRF token,
// SSID, full payload, callsign, ICAO hex, host/URL, or body data may ever
// appear in any log macro expansion or diagnostic output line.
//
// This header is Arduino-free (no Arduino.h, no Serial declaration): it
// defines the constexpr reflection constants and the logging macros. The macros
// expand to Serial calls only when included in Arduino translation units where
// Serial is already in scope; on native tests the macros evaluate to no-ops,
// matching the Arduino LOG_LEVEL=0 behaviour.
//
// Usage in firmware source:
//   #include "runtime_diagnostics.h"
//   PLANE_RADAR_LOG_E("...\n");           // ERROR-level message
//   PLANE_RADAR_LOG_I("...\n", arg);      // INFO-level message (printf-style)
//   PLANE_RADAR_SERIAL_BEGIN(115200);     // gate Serial.begin+delay(500)
//   #if PLANE_RADAR_DIAGNOSTICS
//   Serial.printf("diag: ...\n", ...);   // diagnostic output (always on)
//   #endif

#include <cstdint>

// ============================================================
// 1. Defaults
// ============================================================

#ifndef PLANE_RADAR_DIAGNOSTICS
#define PLANE_RADAR_DIAGNOSTICS 0
#endif

#ifndef PLANE_RADAR_LOG_LEVEL
// Default: 2 (INFO) — preserves all existing runtime Serial output.
#define PLANE_RADAR_LOG_LEVEL 2
#endif

// ============================================================
// 2. Compile-time validation
// ============================================================

static_assert(PLANE_RADAR_LOG_LEVEL == 0 ||
              PLANE_RADAR_LOG_LEVEL == 1 ||
              PLANE_RADAR_LOG_LEVEL == 2,
              "PLANE_RADAR_LOG_LEVEL must be 0 (OFF), 1 (ERROR), or 2 (INFO)");

static_assert(PLANE_RADAR_DIAGNOSTICS == 0 ||
              PLANE_RADAR_DIAGNOSTICS == 1,
              "PLANE_RADAR_DIAGNOSTICS must be 0 (off) or 1 (on)");

// ============================================================
// 3. Constexpr reflection (usable in static_assert and native tests)
// ============================================================

namespace plane_radar {

/// Symbolic log level constants.
constexpr int kLogLevelOff   = 0;
constexpr int kLogLevelError = 1;
constexpr int kLogLevelInfo  = 2;

/// Compile-time log level in effect (mirrors PLANE_RADAR_LOG_LEVEL).
inline constexpr int kLogLevel = PLANE_RADAR_LOG_LEVEL;

/// Whether diagnostics instrumentation is compiled in.
inline constexpr bool kDiagnosticsEnabled = (PLANE_RADAR_DIAGNOSTICS != 0);

}  // namespace plane_radar

// ============================================================
// 4. Logging macros
// ============================================================

// PLANE_RADAR_LOG_E(fmt, ...): ERROR-level — fault/save-failure/fallback.
// Compiled in when PLANE_RADAR_LOG_LEVEL >= 1. Uses Serial.printf with
// the supplied printf-style format string (include \n in fmt as needed).
// GCC/clang ##__VA_ARGS__ drops the comma when no extra args are supplied.
#if PLANE_RADAR_LOG_LEVEL >= 1
#define PLANE_RADAR_LOG_E(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
#define PLANE_RADAR_LOG_E(fmt, ...) ((void)0)
#endif

// PLANE_RADAR_LOG_I(fmt, ...): INFO-level — startup/settings/fetch/range.
// Compiled in when PLANE_RADAR_LOG_LEVEL >= 2.
#if PLANE_RADAR_LOG_LEVEL >= 2
#define PLANE_RADAR_LOG_I(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
#define PLANE_RADAR_LOG_I(fmt, ...) ((void)0)
#endif

// PLANE_RADAR_SERIAL_BEGIN(baud): gate Serial.begin + the USB CDC settle
// delay so both are compiled out when neither logging nor diagnostics is
// active (LOG_LEVEL=0 and DIAGNOSTICS=0).
#if PLANE_RADAR_LOG_LEVEL > 0 || PLANE_RADAR_DIAGNOSTICS
#define PLANE_RADAR_SERIAL_BEGIN(baud) \
  do {                                 \
    Serial.begin(baud);                \
    delay(500);                        \
  } while (0)
#else
#define PLANE_RADAR_SERIAL_BEGIN(baud) ((void)0)
#endif
