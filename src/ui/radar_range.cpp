#include "ui/radar_range.h"

#include "ui/radar_theme.h"

#include <Preferences.h>

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

// Persist + verify a bool preference by read-back (a default distinct from the
// value being written) so a silently-failed NVS write is reported, not trusted.
// The PROPOSED value is written; the caller updates its runtime copy only on a
// verified success.
bool saveBoolVerified(const char* key, bool value) {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return false;
  }
  const bool wrote = s_prefs.putBool(key, value) > 0;
  const bool verified = wrote && s_prefs.getBool(key, !value) == value;
  s_prefs.end();
  return verified;
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

bool setUseMiles(bool use_miles) {
  if (use_miles == s_use_miles) {
    return true;  // no effective change: persist nothing, already saved
  }
  // Persist + read back the PROPOSED value FIRST; update the runtime value and
  // mark the visual-only change ONLY on a verified write, so a failed write leaves
  // the prior runtime value (a later identical retry is not falsely "already
  // saved"). Distance units are visual-only: never a query revision bump.
  if (!saveBoolVerified(kPrefsMilesKey, use_miles)) {
    Serial.printf("Distance units: %s (SAVE FAILED)\n",
                  use_miles ? "miles" : "km");
    return false;
  }
  s_use_miles = use_miles;
  services::settings::markVisualChanged();
  Serial.printf("Distance units: %s (saved)\n", s_use_miles ? "miles" : "km");
  return true;
}

bool setShowRunways(bool show_runways) {
  if (show_runways == s_show_runways) {
    return true;  // no effective change: persist nothing, already saved
  }
  // Persist + read back the PROPOSED value FIRST; update the runtime value and
  // mark the visual-only change ONLY on a verified write. Runway overlay is
  // visual-only: redraw only, no query revision change.
  if (!saveBoolVerified(kPrefsRunwaysKey, show_runways)) {
    Serial.printf("Runway overlay: %s (SAVE FAILED)\n",
                  show_runways ? "on" : "off");
    return false;
  }
  s_show_runways = show_runways;
  services::settings::markVisualChanged();
  Serial.printf("Runway overlay: %s (saved)\n", s_show_runways ? "on" : "off");
  return true;
}

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  core::range::formatRing3Label(buf, len, ring3_km, use_miles);
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

bool resetAll() {
  s_range_index = core::range::kDefaultRangeIndex;
  s_use_miles = false;
  s_show_runways = true;
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return false;
  }
  s_prefs.remove(kPrefsRangeKey);
  s_prefs.remove(kPrefsMilesKey);
  s_prefs.remove(kPrefsRunwaysKey);
  // remove() returns false for an already-absent key, so confirm the final state
  // by read-back rather than trusting the remove() return values.
  const bool ok = !s_prefs.isKey(kPrefsRangeKey) &&
                  !s_prefs.isKey(kPrefsMilesKey) &&
                  !s_prefs.isKey(kPrefsRunwaysKey);
  s_prefs.end();
  return ok;
}

}  // namespace ui::radar
