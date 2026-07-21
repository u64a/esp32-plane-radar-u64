#pragma once

// Deterministic control seam for the native-gfx render harness. Lets each scene
// pin the firmware adapters the production UI object files depend on
// (radar center location, radar range preset, runway/units toggles) so scenes
// are reproducible without any NVS, Preferences, or network state.

#include <cstdint>

namespace nativegfx {

// Reset all adapter state to deterministic defaults (Amsterdam center, range
// preset index 1 = 10 km, runways ON, kilometres). Call at the start of every
// scene so ordering never leaks state between scenes.
void resetAdapters();

void setLocation(double lat, double lon);
void setRangeIndex(uint8_t index);
void setShowRunways(bool show);
void setUseMiles(bool use_miles);

}  // namespace nativegfx
