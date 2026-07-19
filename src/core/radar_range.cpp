#include "core/radar_range.h"

#include <cmath>
#include <cstdio>

namespace core::range {

namespace {

constexpr float kKmPerMile = 1.609344f;

}  // namespace

uint8_t sanitizeSavedIndex(uint8_t saved_index) {
  return saved_index < kRangePresetCount ? saved_index : kDefaultRangeIndex;
}

uint8_t nextIndex(uint8_t current_index) {
  return static_cast<uint8_t>((current_index + 1U) % kRangePresetCount);
}

float fetchRadiusKm(const RangePreset& preset, float screen_radius_px,
                    float grid_outer_radius_px) {
  if (grid_outer_radius_px <= 0.0f) {
    return 0.0f;
  }
  return preset.outer_km * (screen_radius_px / grid_outer_radius_px);
}

void formatRing3Label(char* buffer, size_t length, float ring3_km,
                      bool use_miles) {
  if (buffer == nullptr || length == 0) {
    return;
  }

  if (use_miles) {
    const int miles = static_cast<int>(lroundf(ring3_km / kKmPerMile));
    snprintf(buffer, length, "%dmi", miles);
  } else {
    const int kilometres = static_cast<int>(lroundf(ring3_km));
    snprintf(buffer, length, "%dkm", kilometres);
  }
}

}  // namespace core::range
