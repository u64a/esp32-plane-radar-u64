#pragma once

#include <cstdint>

namespace core {

// Arduino-free partition of a post-DNS connect budget into two NON-OVERLAPPING
// whole-second slices: one for the TCP socket connect and one for the TLS
// handshake.
//
// Why this exists: WiFiClientSecure::connect(IP, port, host, ...) does NOT run
// the socket connect and handshake under one shared deadline. It first uses the
// Stream timeout (setTimeout) for the TCP select, and only THEN applies a
// SEPARATE handshake timeout (setHandshakeTimeout). Assigning the full remaining
// duration to BOTH lets a slow connect burn the whole budget on TCP and then a
// second full budget on TLS -- blocking nearly 2x the advertised ceiling. The
// two knobs are also seconds-granular, so rounding each UP independently would
// compound the overshoot further.
//
// The fix is a truthful, conservative split whose two whole-second slices SUM to
// no more than the remaining budget:
//   * Work in FLOORED whole seconds (remaining_ms / 1000) so the combined
//     ceiling can never exceed the remaining millisecond budget.
//   * Favor TLS with an approximate 40% TCP / 60% TLS split. The handshake is
//     the slower, CPU-bound phase, so it gets the larger share; the socket
//     connect is comparatively quick.
//   * Enforce a 1-second floor on each nonzero slice so a sub-second share never
//     collapses to the core's 0 s ("use the built-in default") sentinel.
//   * If fewer than 2 whole seconds remain there is no way to give each phase a
//     distinct >= 1 s slice whose sum still fits, so report the partition as not
//     viable and let the caller classify Timeout BEFORE starting connect().
struct ConnectBudgetSplit {
  bool viable;           // false: < 2 whole seconds remain; do not start connect
  uint32_t tcp_seconds;  // TCP socket connect ceiling, >= 1 when viable
  uint32_t tls_seconds;  // TLS handshake ceiling, >= tcp_seconds when viable
};

// Partition remaining_ms (the budget left AFTER DNS) as documented above.
// Guarantees when viable: tcp_seconds >= 1, tls_seconds >= tcp_seconds, and
// (tcp_seconds + tls_seconds) * 1000 <= remaining_ms.
inline ConnectBudgetSplit splitConnectBudget(uint32_t remaining_ms) {
  const uint32_t whole_seconds = remaining_ms / 1000U;  // floor, never rounds up
  if (whole_seconds < 2U) {
    return ConnectBudgetSplit{false, 0U, 0U};
  }
  // 40% to TCP (floored), the remainder to TLS so any rounding slack favors the
  // handshake. floor(0.4 * whole) <= whole - 1 for whole >= 2, so TLS stays >= 1.
  uint32_t tcp_seconds = (whole_seconds * 2U) / 5U;
  if (tcp_seconds < 1U) {
    tcp_seconds = 1U;  // 1 s floor (only reachable at whole_seconds == 2)
  }
  const uint32_t tls_seconds = whole_seconds - tcp_seconds;  // >= tcp_seconds
  return ConnectBudgetSplit{true, tcp_seconds, tls_seconds};
}

// Truthful classification of a connect() completion against the ORIGINAL
// absolute connect budget, kept pure so the late-success rejection is
// natively testable without a real WiFiClientSecure.
//
// A success reported only AFTER the absolute budget is already spent is a late
// success: the peer beat the deadline by luck, but honoring it would violate the
// advertised ceiling, so it is reported as Timeout (the caller must stop() the
// socket). A failure that consumed the whole budget is likewise Timeout; any
// earlier failure is a genuine TlsFailure. This is the strongest distinction the
// core exposes, since connect() itself reports only success/failure.
enum class ConnectCompletion : uint8_t {
  Connected,   // success within budget
  Timeout,     // budget exhausted (late success, or a failure that ran it out)
  TlsFailure,  // failure with budget still remaining
};

inline ConnectCompletion classifyConnectCompletion(bool connect_succeeded,
                                                    bool budget_exhausted) {
  if (budget_exhausted) {
    return ConnectCompletion::Timeout;  // late success and late failure alike
  }
  return connect_succeeded ? ConnectCompletion::Connected
                           : ConnectCompletion::TlsFailure;
}

}  // namespace core
