#include "core/poll_policy.h"

#include "core/time_math.h"

namespace core {

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
  state->has_fetch_completion = false;
}

void adsbRadarHidden(AdsbPollState* state) {
  if (state == nullptr) {
    return;
  }
  *state = {};
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

void adsbFetchCompleted(AdsbPollState* state, uint32_t completed_ms) {
  if (state == nullptr || !state->radar_visible) {
    return;
  }
  state->immediate_fetch_due = false;
  state->has_fetch_completion = true;
  state->last_fetch_completed_ms = completed_ms;
}

}  // namespace core
