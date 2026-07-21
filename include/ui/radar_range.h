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
/**
 * Cycle to the next preset and save to flash. Returns true when the preset
 * actually changed -- an effective query change that bumps the runtime settings
 * revision. (Distance-unit and runway toggles are visual-only and never do.)
 */
bool rangeNext();
const RangePreset& rangeCurrent();
uint8_t rangeIndex();
/** ADSB fetch radius (km): scaled to screen edge so beyond-ring dots have data. */
float fetchRadiusKm();

bool useMiles();
bool showRunways();
/** Set distance units (miles vs km). Persists + marks a visual-only change only
 *  on an effective change; never bumps the query revision. Returns true when the
 *  value is unchanged or the write persisted AND verified by read-back; false when
 *  a changed value could not be durably persisted. */
bool setUseMiles(bool use_miles);
/** Set the runway overlay on/off. Persists + marks a visual-only change only on
 *  an effective change; never bumps the query revision. Returns true when the
 *  value is unchanged or the write persisted AND verified by read-back; false when
 *  a changed value could not be durably persisted. */
bool setShowRunways(bool show_runways);
void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles);
void formatCurrentRing3Label(char* buf, size_t len);
/** Factory reset ALL persisted radar preferences (range preset, distance units,
 *  runway overlay) to defaults. Returns true only when the NVS keys are verifiably
 *  absent afterward (read back), so a factory erase never claims success on a
 *  failed clear. */
bool resetAll();

}  // namespace ui::radar
