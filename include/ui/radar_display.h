#pragma once

namespace ui {

/**
 * Attempt the only full-screen sprite allocation once.
 * Returns false when rendering will use the direct-draw fallback.
 */
bool radarDisplayPrepareFrame();

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

}  // namespace ui
