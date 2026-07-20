#pragma once

// Arduino-free seam for the per-object ADS-B decoder. The implementation is the
// only translation unit permitted to include ArduinoJson; callers stay free of
// it. Each object is decoded from a mutable NUL-terminated buffer using only a
// caller-owned arena, so decoding never touches the global heap.

#include <cstddef>
#include <cstdint>

#include "services/adsb_types.h"

namespace services::adsb {

enum class DecodeStatus : uint8_t {
  Accepted,  // valid aircraft; retain and rank by distance
  Skipped,   // schema-invalid or hidden ground; consumes no slot
  NoMemory,  // arena exhausted; fatal for the whole response
  Malformed  // object could not be decoded as JSON; fatal
};

struct DecodeResult {
  DecodeStatus status;
  Aircraft aircraft;  // populated only when status == Accepted
  double lat;         // decode-precision coordinates for distance ranking
  double lon;
};

struct ObjectDecoderConfig {
  bool show_ground;   // retain aircraft whose alt_baro == "ground"
  uint8_t max_depth;  // nesting limit handed to the JSON decoder
};

// Decode one aircraft object. object_json must be writable and NUL-terminated
// at object_json[length]; arena is a caller-owned buffer of arena_size bytes.
DecodeResult decodeAircraftObject(char* object_json, size_t length, void* arena,
                                  size_t arena_size,
                                  const ObjectDecoderConfig& config);

}  // namespace services::adsb
