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

#if PLANE_RADAR_DIAGNOSTICS
/**
 * Diagnostics-only last-render state. Populated by radarDisplayDraw() during
 * the preceding frame and valid to read after radarDisplayDraw() returns (before
 * the next call). All timings are in microseconds (micros()/core::elapsedMicros).
 *
 * Interpretation notes (hardware-only):
 *   runway_us: includes SPI transfer overhead in sprite mode and differs between
 *     sprite path (grid composited off-screen) vs direct-draw path (rendered live
 *     to panel). Real-hardware data is required to characterise the difference.
 *   used_sprite: true = composited sprite + pushSprite; false = direct draw.
 *     Both paths render identical pixels; timing differs by SPI transfer overhead.
 *   runways_enabled: whether drawLargeAirportRunways was actually called this frame.
 * Runway caching decision: consider caching only after observing runway_us > 5 ms
 * AND runway_us >= 20% of total frame time AND RAM allows the cache without
 * threatening the single-frame sprite or TLS heap.
 */
struct RenderDiagnostics {
  uint32_t runway_us;    // drawLargeAirportRunways wall time (0 if not enabled)
  bool used_sprite;      // true = sprite+pushSprite path; false = direct draw
  bool runways_enabled;  // whether drawLargeAirportRunways was called
};

/** Return the diagnostics snapshot from the most recent radarDisplayDraw() call.
 *  Not thread-safe; call only from the main task after radarDisplayDraw returns. */
RenderDiagnostics radarDisplayLastDiagnostics();
#endif  // PLANE_RADAR_DIAGNOSTICS

}  // namespace ui
