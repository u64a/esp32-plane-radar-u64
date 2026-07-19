#pragma once

#include <cstddef>
#include <cstdint>

#include "core/radar_range.h"

namespace ui::radar {

/**
 * Range presets (label on ring 3 = ¾ of outer radius).
 *
 * Recommended for ADS-B on a 1.28″ display:
 *   5 km  — pattern / very local (airfield vicinity)
 *  10 km  — default; neighborhood spotting
 *  15 km  — wider local area
 *  25 km  — metro / regional picture
 *
 * Outer radius (for aircraft math) is ring-3 distance ÷ 0.75.
 */
using core::range::kRangePresetCount;
using core::range::kRangePresets;
using core::range::kRing3ToOuterKm;
using core::range::RangePreset;

/** Load saved range and distance units from flash. Call once after boot. */
void rangeInit();
/** Cycle preset and save to flash. */
void rangeNext();
const RangePreset& rangeCurrent();
uint8_t rangeIndex();
/** ADSB fetch radius (km): scaled to screen edge so beyond-ring dots have data. */
float fetchRadiusKm();

bool useMiles();
bool showRunways();
/** WiFi portal checkbox: "T" = miles, otherwise km. */
void saveMilesFromPortal(const char* checkbox_value);
void saveRunwaysFromPortal(const char* checkbox_value);
void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles);
void formatCurrentRing3Label(char* buf, size_t len);
/** Reset distance units to km (e.g. with WiFi credential wipe). */
void unitsReset();

}  // namespace ui::radar
