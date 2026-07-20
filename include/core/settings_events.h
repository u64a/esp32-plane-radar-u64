#pragma once

#include <cstdint>

namespace core {

// Runtime tracking for the "settings revision" that identifies the current
// radar query (center location + radar range). An effective query change bumps
// the revision and latches a pending flag; a later main-loop iteration consumes
// the flag to force one immediate fetch and reset freshness. Visual-only changes
// (miles/runways) never touch the revision: they latch a separate visual flag
// that only forces a redraw. The logic is Arduino-free so the
// effective/visual/invalid distinction is unit-testable.
struct SettingsState {
  uint32_t revision;           // starts at 0; increments per effective change
  bool query_change_pending;   // latched for the main loop; consumed once
  bool visual_change_pending;  // latched redraw-only flag; consumed once
};

// Record an effective query change: increment the revision (uint32 wrap-safe;
// equality-only comparisons elsewhere stay correct across the wrap) and latch
// the pending flag. Never call this for visual-only settings.
void settingsQueryChanged(SettingsState* state);

// Consume the latched query-change flag exactly once. Returns true iff a change
// was pending, then clears it. The revision itself is not modified.
bool settingsConsumeQueryChange(SettingsState* state);

// Record a visual-only change (distance units / runway overlay): latch the
// redraw flag WITHOUT touching the revision or the query-change flag. Consuming
// it must only force a redraw -- never a fetch, freshness reset, or backoff.
void settingsVisualChanged(SettingsState* state);

// Consume the latched visual-change flag exactly once. Returns true iff a visual
// change was pending, then clears it. The revision is not modified.
bool settingsConsumeVisualChange(SettingsState* state);

// Current revision (0 for a default/zero-initialized state).
uint32_t settingsRevision(const SettingsState& state);

}  // namespace core
