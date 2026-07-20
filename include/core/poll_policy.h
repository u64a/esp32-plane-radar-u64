#pragma once

#include <cstdint>

namespace core {

struct ReconnectState {
  bool disconnected;
  bool has_attempt_completion;
  uint32_t disconnected_ms;
  uint32_t last_attempt_completed_ms;
};

void reconnectDisconnected(ReconnectState* state, uint32_t now_ms);
void reconnectConnected(ReconnectState* state);
bool reconnectAttemptDue(const ReconnectState& state, uint32_t now_ms,
                         uint32_t grace_ms, uint32_t retry_ms);
void reconnectAttemptCompleted(ReconnectState* state, uint32_t completed_ms,
                               bool connected);

// Outcome class that drives the completion-relative ADS-B poll schedule. A
// FetchOutcome (services layer) maps onto exactly one of these; keeping the
// enum Arduino- and services-free preserves the core's dependency direction.
enum class PollOutcome : uint8_t {
  Success,      // published success (incl. empty ac[]): 3 s, resets streak
  Transient,    // timeout/DNS/TLS/5xx: shared streak 5,10,20,40,60,60 s
  RateLimited,  // 429: Retry-After clamp, else default; streak untouched
  Permanent,    // HttpOther/TooLarge/ParseError/NoMemory: 5 min; streak untouched
  Obsolete,     // response discarded for a stale revision; streak untouched
};

// Completion-relative backoff/rate parameters. Supplied explicitly (never read
// from config.h) so the core stays Arduino-free and unit-testable. config.h
// mirrors these values and static_asserts them against kDefaultAdsbPollPolicy.
struct AdsbPollPolicy {
  uint32_t success_ms;            // normal interval after a success
  uint32_t transient_initial_ms;  // first transient wait
  uint32_t transient_cap_ms;      // transient wait ceiling
  uint32_t rate_default_ms;       // 429 without a usable Retry-After
  uint32_t retry_after_min_ms;    // Retry-After clamp floor
  uint32_t retry_after_max_ms;    // Retry-After clamp ceiling
  uint32_t permanent_ms;          // permanent/other error wait
};

inline constexpr AdsbPollPolicy kDefaultAdsbPollPolicy = {
    /*success_ms=*/3000,
    /*transient_initial_ms=*/5000,
    /*transient_cap_ms=*/60000,
    /*rate_default_ms=*/60000,
    /*retry_after_min_ms=*/5000,
    /*retry_after_max_ms=*/300000,
    /*permanent_ms=*/300000,
};

struct AdsbPollState {
  bool radar_visible;
  bool immediate_fetch_due;
  bool has_fetch_completion;
  uint32_t last_fetch_completed_ms;
  uint8_t transient_streak;   // consecutive transient failures, saturating
  uint32_t next_interval_ms;  // completion-relative wait from the last outcome
  // A forced-immediate fetch (radar shown, reconnect, or effective settings
  // change) must not be dropped when an *older* in-flight request completes --
  // above all a stale/obsolete completion. immediate_request_seq counts every
  // forced-immediate request; adsbFetchStarted() snapshots it into
  // inflight_request_seq. A completion clears the immediate latch only when no
  // newer request was raised after the in-flight fetch started.
  uint32_t immediate_request_seq;  // forced-immediate requests raised, monotonic
  uint32_t inflight_request_seq;   // seq captured at the in-flight fetch's start
  bool fetch_in_flight;            // an explicit fetch start awaits completion
};

void adsbRadarDisplayed(AdsbPollState* state);
void adsbRadarHidden(AdsbPollState* state);

// Legacy fixed-interval scheduling (retained for the current main loop and its
// tests): the caller supplies the interval each call.
bool adsbFetchDue(const AdsbPollState& state, uint32_t now_ms,
                  uint32_t interval_ms);
void adsbFetchCompleted(AdsbPollState* state, uint32_t completed_ms);

// Outcome-aware scheduling. next_interval_ms is computed from the outcome and
// the transient streak, and adsbFetchDue(state, now) reads it back. Both are
// completion-relative and uint32 rollover-safe.
bool adsbFetchDue(const AdsbPollState& state, uint32_t now_ms);

// Record that a fetch for the current request has started. Snapshots the current
// forced-immediate request count so a later completion can tell whether a newer
// immediate (e.g. a settings change or reconnect) was raised while this fetch was
// in flight; if so, that completion -- including an Obsolete or failure outcome
// for the older request -- must not clear the newer immediate latch. Optional:
// the legacy synchronous main loop never calls it and its completions clear the
// latch as before, so ordinary back-to-back completions never stay immediate.
void adsbFetchStarted(AdsbPollState* state);

void adsbFetchCompleted(AdsbPollState* state, uint32_t completed_ms,
                        const AdsbPollPolicy& policy, PollOutcome outcome,
                        bool retry_after_present, uint32_t retry_after_ms);

// Effective completion outcome given the Wi-Fi state at completion. A fetch that
// was aborted because Wi-Fi dropped mid-flight (Transient) must NOT count as an
// ADS-B failure: when wifi_connected is false it is downgraded to Obsolete
// (pause semantics, shared transient streak untouched). Every other outcome --
// and every outcome while connected -- passes through unchanged. Pure and
// Arduino-free so the "Wi-Fi drop is not a fetch failure" rule is unit-testable.
PollOutcome effectiveOutcomeAtCompletion(PollOutcome outcome, bool wifi_connected);

// Effective location/range change: force one immediate fetch without resetting
// the transient streak (a settings change is not a fetch failure or success).
void adsbSettingsChanged(AdsbPollState* state);

// True when the 32-bit Wi-Fi disconnect sequence advanced between two captures,
// i.e. at least one ARDUINO_EVENT_WIFI_STA_DISCONNECTED fired during a fetch --
// a "flap" -- even if the link auto-reconnected before/after so the level-based
// Wi-Fi status never showed the drop. A plain inequality is uint32 wrap-safe:
// any real advance (including a single increment across the 0xFFFFFFFF -> 0
// boundary) differs from the captured value; only an astronomically improbable
// exact 2^32 wrap during one fetch would alias, which cannot occur in practice.
// Pure and Arduino-free so the edge detection is unit-testable.
bool disconnectSeqChanged(uint32_t before_seq, uint32_t after_seq);

// Pure backoff helpers, exposed for direct testing.
uint32_t adsbTransientDelayMs(const AdsbPollPolicy& policy, uint8_t streak);
uint32_t adsbRateLimitDelayMs(const AdsbPollPolicy& policy,
                              bool retry_after_present, uint32_t retry_after_ms);

}  // namespace core
