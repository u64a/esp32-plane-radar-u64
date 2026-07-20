#pragma once

#include <cstdint>

namespace services::settings {

// Firmware-wide accessor for the runtime settings revision that identifies the
// current radar query (center location + radar range). Effective query changes
// mark it; a later main-loop iteration consumes the pending flag to force one
// immediate fetch and reset freshness. Backed by a single core::SettingsState,
// whose effective/visual/invalid logic is unit-tested in core.
//
// Callers must only mark on an *effective* change (location or radar range);
// visual-only settings (miles, runways) must never call markQueryChanged() --
// they call markVisualChanged() instead.

// Record an effective query change: bump the revision and latch the pending flag.
void markQueryChanged();

// Main loop: returns true exactly once per pending effective change, then clears
// the flag. The revision is not modified.
bool consumeQueryChange();

// Record a visual-only change (distance units / runway overlay): latch a
// redraw-only flag WITHOUT bumping the revision. Consuming it must only force a
// redraw -- never a fetch, freshness reset, or backoff change.
void markVisualChanged();

// Main loop: returns true exactly once per pending visual-only change, then
// clears the flag. The revision is not modified.
bool consumeVisualChange();

// Current settings revision (0 at boot).
uint32_t revision();

}  // namespace services::settings
