#include "core/poll_policy.h"

#include "core/time_math.h"

namespace core {

namespace {

// Resolve the immediate-fetch latch when a request completes. A forced-immediate
// request raised *after* the in-flight fetch started (e.g. a settings change or
// reconnect during a slow request) must survive this completion, even when the
// completing request is Obsolete or a failure. When no explicit start was
// recorded (the legacy synchronous main loop), the completion consumes the
// immediate that triggered it, so ordinary completions never stay immediate.
void resolveImmediateOnCompletion(AdsbPollState* state) {
  if (state->fetch_in_flight) {
    if (state->immediate_request_seq == state->inflight_request_seq) {
      state->immediate_fetch_due = false;
    }
    state->fetch_in_flight = false;
  } else {
    state->immediate_fetch_due = false;
  }
}

}  // namespace

void reconnectDisconnected(ReconnectState* state, uint32_t now_ms) {
  if (state == nullptr || state->disconnected) {
    return;
  }
  state->disconnected = true;
  state->has_attempt_completion = false;
  state->disconnected_ms = now_ms;
}

void reconnectConnected(ReconnectState* state) {
  if (state == nullptr) {
    return;
  }
  *state = {};
}

bool reconnectAttemptDue(const ReconnectState& state, uint32_t now_ms,
                         uint32_t grace_ms, uint32_t retry_ms) {
  if (!state.disconnected) {
    return false;
  }
  if (!state.has_attempt_completion) {
    return elapsedAtLeast(now_ms, state.disconnected_ms, grace_ms);
  }
  return elapsedAtLeast(now_ms, state.last_attempt_completed_ms, retry_ms);
}

void reconnectAttemptCompleted(ReconnectState* state, uint32_t completed_ms,
                               bool connected) {
  if (state == nullptr) {
    return;
  }
  if (connected) {
    reconnectConnected(state);
    return;
  }
  state->has_attempt_completion = true;
  state->last_attempt_completed_ms = completed_ms;
}

void adsbRadarDisplayed(AdsbPollState* state) {
  if (state == nullptr) {
    return;
  }
  state->radar_visible = true;
  state->immediate_fetch_due = true;
  state->immediate_request_seq += 1U;  // a new forced-immediate request
  state->has_fetch_completion = false;
  // transient_streak and next_interval_ms are preserved: a reconnect forces one
  // immediate fetch but must not reset the transient backoff.
}

void adsbRadarHidden(AdsbPollState* state) {
  if (state == nullptr) {
    return;
  }
  state->radar_visible = false;
  state->immediate_fetch_due = false;
  state->has_fetch_completion = false;
  state->last_fetch_completed_ms = 0;
  // transient_streak and next_interval_ms are preserved: a Wi-Fi pause is not a
  // fetch failure and must not reset the transient backoff.
}

bool adsbFetchDue(const AdsbPollState& state, uint32_t now_ms,
                  uint32_t interval_ms) {
  if (!state.radar_visible) {
    return false;
  }
  if (state.immediate_fetch_due) {
    return true;
  }
  return state.has_fetch_completion &&
         elapsedAtLeast(now_ms, state.last_fetch_completed_ms, interval_ms);
}

bool adsbFetchDue(const AdsbPollState& state, uint32_t now_ms) {
  if (!state.radar_visible) {
    return false;
  }
  if (state.immediate_fetch_due) {
    return true;
  }
  return state.has_fetch_completion &&
         elapsedAtLeast(now_ms, state.last_fetch_completed_ms,
                        state.next_interval_ms);
}

void adsbFetchStarted(AdsbPollState* state) {
  if (state == nullptr || !state->radar_visible) {
    return;
  }
  state->fetch_in_flight = true;
  state->inflight_request_seq = state->immediate_request_seq;
}

void adsbFetchCompleted(AdsbPollState* state, uint32_t completed_ms) {
  if (state == nullptr || !state->radar_visible) {
    return;
  }
  resolveImmediateOnCompletion(state);
  state->has_fetch_completion = true;
  state->last_fetch_completed_ms = completed_ms;
}

namespace {

// Saturating ceiling for the transient streak: far beyond where the schedule
// reaches transient_cap_ms, so the counter never wraps past 255 back to 0 (which
// would restart the backoff and violate the "only success resets" contract).
constexpr uint8_t kMaxTransientStreak = 32;

}  // namespace

uint32_t adsbTransientDelayMs(const AdsbPollPolicy& policy, uint8_t streak) {
  // streak is 1-based (1 = first transient failure). Doubling from the initial
  // wait and clamping at the cap yields 5,10,20,40,60,60... for the defaults.
  const uint8_t n = streak == 0 ? 1 : streak;
  uint32_t delay = policy.transient_initial_ms;
  for (uint8_t i = 1; i < n; ++i) {
    if (delay >= policy.transient_cap_ms) {
      delay = policy.transient_cap_ms;
      break;
    }
    const uint64_t doubled = static_cast<uint64_t>(delay) * 2U;
    delay = doubled > policy.transient_cap_ms
                ? policy.transient_cap_ms
                : static_cast<uint32_t>(doubled);
  }
  if (delay > policy.transient_cap_ms) {
    delay = policy.transient_cap_ms;
  }
  return delay;
}

uint32_t adsbRateLimitDelayMs(const AdsbPollPolicy& policy,
                              bool retry_after_present, uint32_t retry_after_ms) {
  if (!retry_after_present) {
    return policy.rate_default_ms;
  }
  if (retry_after_ms < policy.retry_after_min_ms) {
    return policy.retry_after_min_ms;
  }
  if (retry_after_ms > policy.retry_after_max_ms) {
    return policy.retry_after_max_ms;
  }
  return retry_after_ms;
}

void adsbFetchCompleted(AdsbPollState* state, uint32_t completed_ms,
                        const AdsbPollPolicy& policy, PollOutcome outcome,
                        bool retry_after_present, uint32_t retry_after_ms) {
  if (state == nullptr || !state->radar_visible) {
    return;
  }
  resolveImmediateOnCompletion(state);
  state->has_fetch_completion = true;
  state->last_fetch_completed_ms = completed_ms;
  switch (outcome) {
    case PollOutcome::Success:
      state->transient_streak = 0;
      state->next_interval_ms = policy.success_ms;
      break;
    case PollOutcome::Transient:
      if (state->transient_streak < kMaxTransientStreak) {
        state->transient_streak += 1;
      }
      state->next_interval_ms =
          adsbTransientDelayMs(policy, state->transient_streak);
      break;
    case PollOutcome::RateLimited:
      state->next_interval_ms =
          adsbRateLimitDelayMs(policy, retry_after_present, retry_after_ms);
      break;
    case PollOutcome::Permanent:
      state->next_interval_ms = policy.permanent_ms;
      break;
    case PollOutcome::Obsolete:
      state->next_interval_ms = policy.success_ms;
      break;
  }
}

void adsbSettingsChanged(AdsbPollState* state) {
  if (state == nullptr) {
    return;
  }
  state->immediate_fetch_due = true;
  state->immediate_request_seq += 1U;  // a new forced-immediate request
  // transient_streak and next_interval_ms are preserved: an effective settings
  // change forces one immediate fetch but is neither a failure nor a success.
}

PollOutcome effectiveOutcomeAtCompletion(PollOutcome outcome,
                                         bool wifi_connected) {
  // A network-aborted attempt (Transient) while Wi-Fi is down is a pause, not an
  // ADS-B failure: report Obsolete so the shared transient streak is untouched.
  if (!wifi_connected && outcome == PollOutcome::Transient) {
    return PollOutcome::Obsolete;
  }
  return outcome;
}

bool disconnectSeqChanged(uint32_t before_seq, uint32_t after_seq) {
  // Any advance signals a disconnect flap. Inequality is inherently uint32
  // wrap-safe: an increment past 0xFFFFFFFF to 0 still differs from the capture.
  return before_seq != after_seq;
}

}  // namespace core
