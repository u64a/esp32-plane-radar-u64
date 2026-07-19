#pragma once

#include <cstddef>
#include <cstdint>

namespace core::range {

struct RangePreset {
  float ring3_km;
  float outer_km;
};

constexpr float kRing3ToOuterKm = 4.0f / 3.0f;

inline constexpr RangePreset kRangePresets[] = {
    {5.0f, 5.0f * kRing3ToOuterKm},
    {10.0f, 10.0f * kRing3ToOuterKm},
    {15.0f, 15.0f * kRing3ToOuterKm},
    {25.0f, 25.0f * kRing3ToOuterKm},
};
inline constexpr size_t kRangePresetCount =
    sizeof(kRangePresets) / sizeof(kRangePresets[0]);
constexpr uint8_t kDefaultRangeIndex = 1;

uint8_t sanitizeSavedIndex(uint8_t saved_index);
uint8_t nextIndex(uint8_t current_index);
float fetchRadiusKm(const RangePreset& preset, float screen_radius_px,
                    float grid_outer_radius_px);
void formatRing3Label(char* buffer, size_t length, float ring3_km,
                      bool use_miles);

}  // namespace core::range
