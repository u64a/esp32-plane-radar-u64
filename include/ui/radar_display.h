#pragma once

#include <cstdint>

#include "core/radar_data_state.h"
#include "services/adsb_snapshot_store.h"

namespace ui {

/**
 * Deterministic renderer input. Everything the frame needs is supplied by value
 * so drawing has no hidden global reads beyond the shared location/range state:
 *   data           freshness view (mode, age, aircraft visibility, revision)
 *   snapshot       copy-free view of the published nearest-aircraft snapshot
 *   wifi_connected whether the radio is currently associated
 *   activity_phase deterministic Loading-indicator phase (drives 1..3 dots)
 */
struct RadarDisplayModel {
  core::RadarDataView data;
  services::adsb::SnapshotView snapshot;
  bool wifi_connected;
  uint8_t activity_phase;
};

/**
 * Attempt the only full-screen sprite allocation once.
 * Returns false when rendering will use the direct-draw fallback.
 */
bool radarDisplayPrepareFrame();

/** Draw the full frame (grid + conditional aircraft + status) for `model`. */
void radarDisplayDraw(const RadarDisplayModel& model);

/** Redraw the full frame for `model` (blits the composited grid; no flicker). */
void radarDisplayRefreshAircraft(const RadarDisplayModel& model);

/**
 * Behavior-compatible wrappers for the not-yet-migrated main loop: they build a
 * Live/connected model from the currently published snapshot and forward to the
 * model overloads. Drawing logic is not duplicated here.
 */
void radarDisplayDraw();
void radarDisplayRefreshAircraft();

}  // namespace ui
