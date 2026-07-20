#include "core/frame_render.h"

namespace core {

FrameRenderKey frameRenderKey(RadarDataMode mode, uint32_t age_seconds,
                              uint32_t data_revision, uint32_t snapshot_revision,
                              uint16_t snapshot_count, bool wifi_connected,
                              uint8_t activity_phase) {
  FrameRenderKey key{};
  key.mode = static_cast<uint8_t>(mode);
  key.wifi_connected = wifi_connected;
  key.data_revision = data_revision;
  key.snapshot_revision = snapshot_revision;
  key.snapshot_count = snapshot_count;
  // The activity phase animates only the Loading badge; the age is only shown by
  // the Stale badge. Pin the other cases so Live/Offline frames stay identical
  // frame-to-frame and do not redraw continuously.
  key.loading_phase =
      mode == RadarDataMode::Loading ? activity_phase : static_cast<uint8_t>(0);
  key.stale_age_seconds =
      mode == RadarDataMode::Stale ? age_seconds : static_cast<uint32_t>(0);
  return key;
}

bool frameRenderKeyEqual(const FrameRenderKey& a, const FrameRenderKey& b) {
  return a.mode == b.mode && a.wifi_connected == b.wifi_connected &&
         a.data_revision == b.data_revision &&
         a.snapshot_revision == b.snapshot_revision &&
         a.snapshot_count == b.snapshot_count &&
         a.stale_age_seconds == b.stale_age_seconds &&
         a.loading_phase == b.loading_phase;
}

}  // namespace core
