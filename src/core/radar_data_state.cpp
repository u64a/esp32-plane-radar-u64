#include "core/radar_data_state.h"

#include "core/time_math.h"

namespace core {

namespace {

uint32_t referenceMs(const RadarDataState& state) {
  return state.has_success ? state.last_success_ms : state.revision_started_ms;
}

// Mode implied purely by elapsed time, ignoring the latch. radarDataAdvance()
// ratchets the stored mode up to this but never down.
RadarDataMode timeDerivedMode(const RadarDataState& state, uint32_t now_ms,
                              const RadarFreshnessPolicy& policy) {
  const uint32_t age_ms = elapsedMs(now_ms, referenceMs(state));
  if (!state.has_success) {
    // No current-revision success: Loading until the offline horizon.
    return age_ms >= policy.offline_ms ? RadarDataMode::Offline
                                       : RadarDataMode::Loading;
  }
  if (age_ms >= policy.offline_ms) {
    return RadarDataMode::Offline;
  }
  if (age_ms >= policy.stale_ms) {
    return RadarDataMode::Stale;
  }
  return RadarDataMode::Live;
}

}  // namespace

void radarDataStart(RadarDataState* state, uint32_t now_ms) {
  if (state == nullptr) {
    return;
  }
  state->settings_revision = 0;
  state->revision_started_ms = now_ms;
  state->last_success_ms = now_ms;
  state->has_success = false;
  state->mode = RadarDataMode::Loading;
}

void radarDataRevisionChanged(RadarDataState* state, uint32_t settings_revision,
                              uint32_t now_ms) {
  if (state == nullptr) {
    return;
  }
  state->settings_revision = settings_revision;
  state->revision_started_ms = now_ms;
  state->last_success_ms = now_ms;
  state->has_success = false;
  state->mode = RadarDataMode::Loading;
}

void radarDataSuccess(RadarDataState* state, uint32_t settings_revision,
                      uint32_t now_ms) {
  if (state == nullptr || settings_revision != state->settings_revision) {
    return;
  }
  state->last_success_ms = now_ms;
  state->has_success = true;
  state->mode = RadarDataMode::Live;
}

RadarDataMode radarDataAdvance(RadarDataState* state, uint32_t now_ms,
                               const RadarFreshnessPolicy& policy) {
  if (state == nullptr) {
    return RadarDataMode::Loading;
  }
  const RadarDataMode target = timeDerivedMode(*state, now_ms, policy);
  if (static_cast<uint8_t>(target) > static_cast<uint8_t>(state->mode)) {
    state->mode = target;
  }
  return state->mode;
}

RadarDataView radarDataView(const RadarDataState& state, uint32_t now_ms) {
  RadarDataView view{};
  view.mode = state.mode;
  view.age_seconds = elapsedMs(now_ms, referenceMs(state)) / 1000U;
  view.show_aircraft =
      state.mode == RadarDataMode::Live || state.mode == RadarDataMode::Stale;
  view.settings_revision = state.settings_revision;
  return view;
}

}  // namespace core
