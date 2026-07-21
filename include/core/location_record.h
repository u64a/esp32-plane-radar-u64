#pragma once

#include <cstdint>

// Pure, Arduino-free radar-location record (NVS blob).
//
// The radar center latitude+longitude is persisted as ONE atomic, versioned,
// integrity-checked blob so a torn write can never leave a half-updated pair
// (e.g. a new latitude with a stale longitude). The adapter writes the whole
// record in a single NVS putBytes and reads it back before trusting it; the
// legacy separate `lat`/`lon` keys are kept only for READ compatibility and are
// removed after a verified blob write.
//
// Layout (little-endian, fixed 24 bytes) so it encodes identically on the
// ESP32-C3 and on the native test host, independent of the build host endianness:
//   [0..3]    version   (uint32, == kLocationRecordVersion)
//   [4..11]   lat       (IEEE-754 binary64, little-endian)
//   [12..19]  lon       (IEEE-754 binary64, little-endian)
//   [20..23]  checksum  (uint32 FNV-1a over bytes [0..19])
// A corrupt / partial / wrong-version record, or one whose coordinates are out of
// range, fails validation and the caller falls back to legacy keys / defaults.

namespace core {

constexpr uint32_t kLocationRecordVersion = 1;
constexpr uint32_t kLocationRecordBytes = 24;

struct LocationRecord {
  double lat;
  double lon;
};

// Serialize a record into exactly kLocationRecordBytes bytes (checksum included).
// Returns false if buf is null/too small or the coordinates are not finite and in
// range (an invalid location is never persisted).
bool encodeLocationRecord(const LocationRecord& rec, uint8_t* buf, uint32_t len);

// Parse + fully validate a persisted record: the length must be exactly
// kLocationRecordBytes, the version and FNV-1a checksum must match, and the
// decoded coordinates must be finite and in range. On success sets *out and
// returns true; on ANY failure returns false. Pure and Arduino-free.
bool decodeAndValidateLocationRecord(const uint8_t* buf, uint32_t len,
                                     LocationRecord* out);

}  // namespace core
