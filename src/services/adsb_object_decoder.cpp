// The single translation unit permitted to include ArduinoJson. A small pool
// capacity keeps allocation granularity identical on 32-bit firmware and 64-bit
// hosts, and NaN/Infinity/comment extensions are disabled so only strict JSON
// numbers are accepted. All allocation is routed through a caller-owned arena.
#define ARDUINOJSON_POOL_CAPACITY 32
#define ARDUINOJSON_ENABLE_NAN 0
#define ARDUINOJSON_ENABLE_INFINITY 0
#define ARDUINOJSON_ENABLE_COMMENTS 0

#include "services/adsb_object_decoder.h"

#include <ArduinoJson.h>

#include <cmath>
#include <cstring>

#include "core/coordinates.h"
#include "services/adsb_arena.h"

namespace services::adsb {

namespace {

// Whitelisted keys copied out of each aircraft object. Unknown keys are dropped
// by the ArduinoJson filter so an adversarial object cannot inflate slot usage.
constexpr const char* kFilterKeys[] = {
    "lat",         "lon", "true_heading", "mag_heading", "track",
    "dir",         "gs",  "tas",          "ias",         "flight",
    "hex",         "t",   "alt_baro",     "alt_geom",
};

// Altitudes outside this magnitude are treated as not representable so the
// fixed tag buffer never overflows and int conversion never wraps.
constexpr double kMaxRepresentableAltFt = 999999.0;

class ArenaAllocator : public ArduinoJson::Allocator {
 public:
  explicit ArenaAllocator(BoundedArena* arena) : arena_(arena) {}

  void* allocate(size_t size) override { return arena_->allocate(size); }
  void deallocate(void* ptr) override { arena_->deallocate(ptr); }
  void* reallocate(void* ptr, size_t new_size) override {
    return arena_->reallocate(ptr, new_size);
  }

 private:
  BoundedArena* arena_;
};

bool readNumber(JsonVariantConst value, double* out) {
  // Only genuine JSON numbers qualify; strings, bools, and null fall through so
  // wrong-typed optional fields never poison the record.
  if (!value.is<double>() && !value.is<long long>() &&
      !value.is<unsigned long long>()) {
    return false;
  }
  const double d = value.as<double>();
  if (!std::isfinite(d)) {
    return false;
  }
  *out = d;
  return true;
}

// A finite double such as 1e300 is a valid JSON number but overflows float to
// +/-infinity, and the renderer feeds nose/track/speed through trig and integer
// conversion. Only accept optional values that survive the narrowing cast as a
// finite float; anything else must fall through to the next precedence key.
bool readFiniteFloat(JsonVariantConst value, float* out) {
  double d = 0.0;
  if (!readNumber(value, &d)) {
    return false;
  }
  const float f = static_cast<float>(d);
  if (!std::isfinite(f)) {
    return false;
  }
  *out = f;
  return true;
}

float pickByPrecedence(JsonObjectConst obj, const char* const* keys,
                       size_t key_count) {
  float value = 0.0f;
  for (size_t i = 0; i < key_count; ++i) {
    if (readFiniteFloat(obj[keys[i]], &value)) {
      return value;
    }
  }
  return 0.0f;  // absent or all-unrepresentable: finite zero, never infinity
}

bool isGround(JsonObjectConst obj) {
  JsonVariantConst alt = obj["alt_baro"];
  return alt.is<const char*>() && std::strcmp(alt.as<const char*>(), "ground") == 0;
}

void copyStringTrimmed(JsonVariantConst value, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !value.is<const char*>()) {
    return;
  }
  const char* s = value.as<const char*>();
  size_t n = std::strlen(s);
  if (n > out_len - 1) {
    n = out_len - 1;
  }
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  std::memcpy(out, s, n);
  out[n] = '\0';
}

bool readRepresentableAltitude(JsonVariantConst value, double* out) {
  double alt = 0.0;
  if (!readNumber(value, &alt)) {
    return false;
  }
  const double rounded = std::round(alt);
  if (rounded < -kMaxRepresentableAltFt || rounded > kMaxRepresentableAltFt) {
    return false;  // finite but not safely representable in the fixed tag
  }
  *out = rounded;
  return true;
}

void formatAltitude(JsonObjectConst obj, bool ground, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }
  if (ground) {
    std::strncpy(out, "GND", out_len - 1);
    out[out_len - 1] = '\0';
    return;
  }
  double rounded = 0.0;
  // Prefer a representable alt_baro; otherwise fall back to alt_geom.
  if (readRepresentableAltitude(obj["alt_baro"], &rounded) ||
      readRepresentableAltitude(obj["alt_geom"], &rounded)) {
    std::snprintf(out, out_len, "%d ft", static_cast<int>(rounded));
  }
}

bool buildFilter(JsonDocument& filter) {
  JsonObject root = filter.to<JsonObject>();
  for (const char* key : kFilterKeys) {
    root[key] = true;
  }
  return !filter.overflowed();
}

}  // namespace

DecodeResult decodeAircraftObject(char* object_json, size_t length, void* arena,
                                  size_t arena_size,
                                  const ObjectDecoderConfig& config) {
  DecodeResult result = {};
  result.status = DecodeStatus::Skipped;

  BoundedArena bounded(arena, arena_size);
  ArenaAllocator allocator(&bounded);

  // The filter shares the caller arena, bounding kept slots to the whitelist.
  JsonDocument filter(&allocator);
  if (!buildFilter(filter)) {
    result.status = DecodeStatus::NoMemory;
    return result;
  }

  JsonDocument doc(&allocator);
  const DeserializationError err = deserializeJson(
      doc, object_json, length, DeserializationOption::Filter(filter),
      DeserializationOption::NestingLimit(config.max_depth));

  if (err == DeserializationError::NoMemory) {
    result.status = DecodeStatus::NoMemory;
    return result;
  }
  if (err) {
    result.status = DecodeStatus::Malformed;
    return result;
  }

  JsonObjectConst obj = doc.as<JsonObjectConst>();
  if (obj.isNull()) {
    result.status = DecodeStatus::Skipped;
    return result;
  }

  double lat = 0.0;
  double lon = 0.0;
  if (!readNumber(obj["lat"], &lat) || !readNumber(obj["lon"], &lon) ||
      !core::coordinatesValid(lat, lon)) {
    result.status = DecodeStatus::Skipped;
    return result;
  }

  const bool ground = isGround(obj);
  if (ground && !config.show_ground) {
    result.status = DecodeStatus::Skipped;
    return result;
  }

  static constexpr const char* kNoseKeys[] = {"true_heading", "mag_heading",
                                              "track", "dir"};
  static constexpr const char* kTrackKeys[] = {"track", "true_heading",
                                               "mag_heading", "dir"};
  static constexpr const char* kSpeedKeys[] = {"gs", "tas", "ias"};

  Aircraft& ac = result.aircraft;
  ac.lat = static_cast<float>(lat);
  ac.lon = static_cast<float>(lon);
  ac.nose_deg = pickByPrecedence(obj, kNoseKeys, 4);
  ac.track_deg = pickByPrecedence(obj, kTrackKeys, 4);
  ac.gs_knots = pickByPrecedence(obj, kSpeedKeys, 3);

  copyStringTrimmed(obj["flight"], ac.callsign, sizeof(ac.callsign));
  if (ac.callsign[0] == '\0') {
    copyStringTrimmed(obj["hex"], ac.callsign, sizeof(ac.callsign));
  }
  copyStringTrimmed(obj["t"], ac.type, sizeof(ac.type));
  formatAltitude(obj, ground, ac.alt, sizeof(ac.alt));

  result.lat = lat;
  result.lon = lon;
  result.status = DecodeStatus::Accepted;
  return result;
}

}  // namespace services::adsb
