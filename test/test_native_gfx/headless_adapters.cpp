// Headless native-gfx firmware-adapter fakes.
//
// The production UI object files (radar_display.cpp, runway_overlay.cpp) call a
// handful of firmware adapters that, in the real device, read NVS/Preferences,
// the published ADS-B snapshot, and the saved radar location. Those adapters pull
// in Arduino/Preferences/WiFi and are NOT the UI drawing code under test. This
// file supplies minimal, deterministic stand-ins for exactly the symbols the
// compiled UI references -- reusing the pure core::range math so range labels and
// fetch radius match the firmware -- and a small control API (headless_adapters.h)
// so each scene pins location/range/toggles reproducibly.
//
// Only the referenced adapters are defined; the other declared range/location
// functions are intentionally absent because no compiled UI code links them.

#include "headless_adapters.h"

#include "config.h"
#include "core/radar_range.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace {

double s_lat = config::kDefaultRadarLat;
double s_lon = config::kDefaultRadarLon;
uint8_t s_range_index = core::range::kDefaultRangeIndex;
bool s_show_runways = true;
bool s_use_miles = false;

}  // namespace

namespace nativegfx {

void resetAdapters() {
  s_lat = config::kDefaultRadarLat;
  s_lon = config::kDefaultRadarLon;
  s_range_index = core::range::kDefaultRangeIndex;
  s_show_runways = true;
  s_use_miles = false;
}

void setLocation(double lat, double lon) {
  s_lat = lat;
  s_lon = lon;
}

void setRangeIndex(uint8_t index) {
  s_range_index = core::range::sanitizeSavedIndex(index);
}

void setShowRunways(bool show) { s_show_runways = show; }

void setUseMiles(bool use_miles) { s_use_miles = use_miles; }

}  // namespace nativegfx

namespace services::location {

double lat() { return s_lat; }
double lon() { return s_lon; }

}  // namespace services::location

namespace services::adsb {

// The compiled UI references these only from the not-under-test no-arg
// radarDisplayDraw() convenience wrapper; scenes always use the explicit
// RadarDisplayModel overload, so an empty published snapshot is sufficient and
// deterministic.
size_t aircraftCount() { return 0; }
const Aircraft* aircraftList() { return nullptr; }

}  // namespace services::adsb

namespace ui::radar {

const RangePreset& rangeCurrent() {
  return core::range::kRangePresets[s_range_index];
}

float fetchRadiusKm() {
  const float screen_r_px =
      static_cast<float>(kCenterX - kBeyondRingScreenMarginPx);
  return core::range::fetchRadiusKm(rangeCurrent(), screen_r_px,
                                    static_cast<float>(kGridOuterRadius));
}

bool showRunways() { return s_show_runways; }

bool useMiles() { return s_use_miles; }

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  core::range::formatRing3Label(buf, len, ring3_km, use_miles);
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

}  // namespace ui::radar
