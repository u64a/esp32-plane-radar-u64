#pragma once

#include <cstdint>

#include "core/radar_data_state.h"

// Pure, Arduino-free redraw-decision seam. The runtime renders the radar frame
// only when an explicit event forces it (initial display, settings change,
// Wi-Fi edge / reconnect completion, fetch completion) OR when this compact
// frame key changes. Encoding the time-varying fields per mode lets the loop
// stay responsive without redrawing Live/Offline every iteration, while still
// animating Loading dots and ticking the Stale age. Keeping the comparison here
// makes the "when to redraw" policy unit-testable with no graphics dependency.

namespace core {

// Compact fingerprint of everything that determines the rendered radar frame.
// Time-varying fields are significant only in the mode that displays them: the
// loading activity phase only while Loading, and the displayed age only while
// Stale. In Live/Offline both are pinned to 0 so a steady frame never triggers a
// continuous redraw. Revision + count changes (a new publication or a revision
// switch that hides stale targets) always change the key.
struct FrameRenderKey {
  uint8_t mode;                // core::RadarDataMode
  bool wifi_connected;
  uint32_t data_revision;      // freshness view settings revision
  uint32_t snapshot_revision;  // published snapshot settings revision
  uint16_t snapshot_count;     // published aircraft count
  uint32_t stale_age_seconds;  // displayed age; significant only while Stale
  uint8_t loading_phase;       // 0..2 activity phase; significant only while Loading
};

// Build a render key. activity_phase is retained only while Loading and
// age_seconds only while Stale; both are pinned to 0 otherwise so Live/Offline
// frames keep a stable key.
FrameRenderKey frameRenderKey(RadarDataMode mode, uint32_t age_seconds,
                              uint32_t data_revision, uint32_t snapshot_revision,
                              uint16_t snapshot_count, bool wifi_connected,
                              uint8_t activity_phase);

// Field-wise equality of two keys (true == the frame is unchanged).
bool frameRenderKeyEqual(const FrameRenderKey& a, const FrameRenderKey& b);

}  // namespace core
