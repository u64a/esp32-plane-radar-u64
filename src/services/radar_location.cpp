#include "services/radar_location.h"

#include <Preferences.h>

#include "config.h"
#include "core/coordinates.h"
#include "core/location_record.h"

namespace services::location {

namespace {

constexpr char kPrefsNamespace[] = "radar";
// Atomic versioned+checksummed lat+lon blob (the authoritative record). The
// legacy separate keys are kept ONLY for read compatibility and are removed after
// a verified blob write.
constexpr char kKeyBlob[] = "loc";
constexpr char kKeyLat[] = "lat";  // legacy (read-compat / migration source)
constexpr char kKeyLon[] = "lon";  // legacy (read-compat / migration source)

double s_lat = config::kDefaultRadarLat;
double s_lon = config::kDefaultRadarLon;

// Read the authoritative blob (if present + valid) into out_lat/out_lon.
bool readBlob(Preferences& prefs, double* out_lat, double* out_lon) {
  if (!prefs.isKey(kKeyBlob) ||
      prefs.getBytesLength(kKeyBlob) != core::kLocationRecordBytes) {
    return false;
  }
  uint8_t buf[core::kLocationRecordBytes];
  core::LocationRecord rec{};
  if (prefs.getBytes(kKeyBlob, buf, sizeof(buf)) != sizeof(buf) ||
      !core::decodeAndValidateLocationRecord(buf, sizeof(buf), &rec)) {
    return false;
  }
  *out_lat = rec.lat;
  *out_lon = rec.lon;
  return true;
}

bool persist(double lat, double lon) {
  // Store lat+lon as ONE atomic, versioned, checksummed record so a torn write can
  // never leave a half-updated pair. The runtime values are updated ONLY after the
  // write verifies by read-back+decode, so a failed write leaves the prior runtime
  // value and a later identical retry is still classified as Changed (not falsely
  // "already saved").
  uint8_t buf[core::kLocationRecordBytes];
  const core::LocationRecord rec{lat, lon};
  if (!core::encodeLocationRecord(rec, buf, sizeof(buf))) {
    return false;  // out-of-range/non-finite: never persist
  }
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return false;
  }
  const bool wrote =
      prefs.putBytes(kKeyBlob, buf, sizeof(buf)) == sizeof(buf);
  // Verify by read-back + decode before trusting the write: NVS put can silently
  // fail on a full/worn partition.
  double rb_lat = 0.0;
  double rb_lon = 0.0;
  const bool verified =
      wrote && readBlob(prefs, &rb_lat, &rb_lon) && rb_lat == lat && rb_lon == lon;
  if (verified) {
    // Migrate: remove the legacy keys ONLY after a verified blob write (the blob
    // is authoritative on read, so a best-effort removal is safe).
    prefs.remove(kKeyLat);
    prefs.remove(kKeyLon);
  }
  prefs.end();
  if (verified) {
    s_lat = lat;
    s_lon = lon;
  }
  return verified;
}

}  // namespace

void init() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  double lat = 0.0;
  double lon = 0.0;
  if (readBlob(prefs, &lat, &lon)) {
    // The authoritative blob wins (already range-validated by decode).
    s_lat = lat;
    s_lon = lon;
  } else if (prefs.isKey(kKeyLat) && prefs.isKey(kKeyLon)) {
    // Read compatibility with the pre-blob layout. Migration to the blob happens
    // on the next verified save (init is read-only).
    lat = prefs.getDouble(kKeyLat, config::kDefaultRadarLat);
    lon = prefs.getDouble(kKeyLon, config::kDefaultRadarLon);
    if (core::coordinatesValid(lat, lon)) {
      s_lat = lat;
      s_lon = lon;
    }
  }
  prefs.end();
}

double lat() { return s_lat; }

double lon() { return s_lon; }

core::CoordinateSaveResult saveFromStrings(const char* lat_str,
                                           const char* lon_str,
                                           bool* persist_ok) {
  double lat = 0.0;
  double lon = 0.0;
  const core::CoordinateSaveResult result =
      core::classifyCoordinateSave(lat_str, lon_str, s_lat, s_lon, &lat, &lon);
  bool ok = true;  // Unchanged: nothing to persist, already durably saved
  if (result == core::CoordinateSaveResult::Changed) {
    ok = persist(lat, lon);
    Serial.printf("Radar location saved: %.6f, %.6f (%s)\n", lat, lon,
                  ok ? "ok" : "FAILED");
  } else if (result == core::CoordinateSaveResult::Invalid) {
    ok = false;  // could not apply the intended coordinates
  }
  if (persist_ok != nullptr) {
    *persist_ok = ok;
  }
  return result;
}

bool clear() {
  Preferences prefs;
  bool ok = prefs.begin(kPrefsNamespace, false);
  if (ok) {
    prefs.remove(kKeyBlob);
    prefs.remove(kKeyLat);
    prefs.remove(kKeyLon);
    // remove() returns false when a key was already absent, so verify the final
    // state directly instead of trusting the remove() return value. Both the blob
    // and the legacy keys must be verifiably absent.
    ok = !prefs.isKey(kKeyBlob) && !prefs.isKey(kKeyLat) && !prefs.isKey(kKeyLon);
    prefs.end();
  }
  s_lat = config::kDefaultRadarLat;
  s_lon = config::kDefaultRadarLon;
  return ok;
}

}  // namespace services::location
