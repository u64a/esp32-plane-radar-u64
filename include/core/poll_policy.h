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

struct AdsbPollState {
  bool radar_visible;
  bool immediate_fetch_due;
  bool has_fetch_completion;
  uint32_t last_fetch_completed_ms;
};

void adsbRadarDisplayed(AdsbPollState* state);
void adsbRadarHidden(AdsbPollState* state);
bool adsbFetchDue(const AdsbPollState& state, uint32_t now_ms,
                  uint32_t interval_ms);
void adsbFetchCompleted(AdsbPollState* state, uint32_t completed_ms);

}  // namespace core
