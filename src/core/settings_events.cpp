#include "core/settings_events.h"

namespace core {

void settingsQueryChanged(SettingsState* state) {
  if (state == nullptr) {
    return;
  }
  state->revision += 1U;  // uint32 wrap is intentional and equality-safe
  state->query_change_pending = true;
}

bool settingsConsumeQueryChange(SettingsState* state) {
  if (state == nullptr) {
    return false;
  }
  const bool pending = state->query_change_pending;
  state->query_change_pending = false;
  return pending;
}

void settingsVisualChanged(SettingsState* state) {
  if (state == nullptr) {
    return;
  }
  // Visual-only: latch a redraw flag but leave the revision and the query-change
  // flag untouched, so a units/runway toggle never forces a fetch or resets
  // freshness/backoff.
  state->visual_change_pending = true;
}

bool settingsConsumeVisualChange(SettingsState* state) {
  if (state == nullptr) {
    return false;
  }
  const bool pending = state->visual_change_pending;
  state->visual_change_pending = false;
  return pending;
}

uint32_t settingsRevision(const SettingsState& state) { return state.revision; }

}  // namespace core
