#include "ui/radar_range.h"

#include "ui/radar_theme.h"

#include <Preferences.h>
#include <cstring>

#include "services/settings_events.h"

namespace ui::radar {

namespace {

constexpr char kPrefsNamespace[] = "planeradar";
constexpr char kPrefsRangeKey[] = "rangeIdx";
constexpr char kPrefsMilesKey[] = "useMiles";
constexpr char kPrefsRunwaysKey[] = "showRwys";

Preferences s_prefs;
uint8_t s_range_index = core::range::kDefaultRangeIndex;
bool s_use_miles = false;
bool s_show_runways = true;

void saveRangeIndex() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsRangeKey, s_range_index);
  s_prefs.end();
}

void saveUseMiles() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsMilesKey, s_use_miles);
  s_prefs.end();
}

void saveShowRunways() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsRunwaysKey, s_show_runways);
  s_prefs.end();
}

bool portalCheckboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  // WiFiManager checkbox submits its value= attribute ("T", or "F" if we prefilled F).
  if ((value[0] == 'T' || value[0] == 't' || value[0] == 'F' || value[0] == 'f') &&
      value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

}  // namespace

void rangeInit() {
  if (!s_prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const uint8_t saved =
      s_prefs.getUChar(kPrefsRangeKey, core::range::kDefaultRangeIndex);
  s_range_index = core::range::sanitizeSavedIndex(saved);
  s_use_miles = s_prefs.getBool(kPrefsMilesKey, false);
  s_show_runways = s_prefs.getBool(kPrefsRunwaysKey, true);
  s_prefs.end();
}

bool rangeNext() {
  const uint8_t previous = s_range_index;
  s_range_index = core::range::nextIndex(s_range_index);
  saveRangeIndex();
  if (s_range_index == previous) {
    return false;
  }
  // Radar range is part of the query: mark an effective settings change so the
  // main loop forces one immediate fetch and resets freshness for the new revision.
  services::settings::markQueryChanged();
  return true;
}

const RangePreset& rangeCurrent() { return kRangePresets[s_range_index]; }

uint8_t rangeIndex() { return s_range_index; }

float fetchRadiusKm() {
  const float screen_r_px =
      static_cast<float>(kCenterX - kBeyondRingScreenMarginPx);
  return core::range::fetchRadiusKm(
      rangeCurrent(), screen_r_px, static_cast<float>(kGridOuterRadius));
}

bool useMiles() { return s_use_miles; }

bool showRunways() { return s_show_runways; }

void saveMilesFromPortal(const char* checkbox_value) {
  const bool next = portalCheckboxChecked(checkbox_value);
  if (next == s_use_miles) {
    return;  // no effective change: persist nothing, latch no redraw
  }
  s_use_miles = next;
  saveUseMiles();
  // Distance units are visual-only: mark a redraw-only change (never a query
  // revision bump, freshness reset, or backoff change).
  services::settings::markVisualChanged();
  Serial.printf("Distance units: %s\n", s_use_miles ? "miles" : "km");
}

void saveRunwaysFromPortal(const char* checkbox_value) {
  const bool next = portalCheckboxChecked(checkbox_value);
  if (next == s_show_runways) {
    return;  // no effective change: persist nothing, latch no redraw
  }
  s_show_runways = next;
  saveShowRunways();
  // Runway overlay is visual-only: redraw only, no query revision change.
  services::settings::markVisualChanged();
  Serial.printf("Runway overlay: %s\n", s_show_runways ? "on" : "off");
}

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  core::range::formatRing3Label(buf, len, ring3_km, use_miles);
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

void unitsReset() {
  s_use_miles = false;
  s_show_runways = true;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.remove(kPrefsMilesKey);
    s_prefs.remove(kPrefsRunwaysKey);
    s_prefs.end();
  }
}

}  // namespace ui::radar
