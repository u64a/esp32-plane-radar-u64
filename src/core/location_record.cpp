#include "core/location_record.h"

#include <cstring>

#include "core/coordinates.h"  // core::coordinatesValid

namespace core {

namespace {

static_assert(sizeof(double) == 8, "LocationRecord assumes IEEE-754 binary64");

// Little-endian (de)serialization helpers so the persisted record encodes
// identically on the ESP32-C3 and on the native test host, mirroring the
// core::PersistedFloorRecord / core::TxnMarker helpers.
void putU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void putF64(uint8_t* p, double v) {
  uint64_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFFu);
  }
}

double getF64(const uint8_t* p) {
  uint64_t bits = 0;
  for (int i = 0; i < 8; ++i) {
    bits |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  double v = 0.0;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

// FNV-1a 32-bit: a small deterministic integrity check that detects a corrupt or
// partially-written NVS blob. It is NOT a cryptographic MAC (the record is not a
// trust anchor -- it carries only a location pair, re-validated against the
// coordinate range on read).
uint32_t fnv1a32(const uint8_t* data, uint32_t len) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

}  // namespace

bool encodeLocationRecord(const LocationRecord& rec, uint8_t* buf, uint32_t len) {
  if (buf == nullptr || len < kLocationRecordBytes) {
    return false;
  }
  if (!coordinatesValid(rec.lat, rec.lon)) {
    return false;  // never persist an out-of-range / non-finite location
  }
  putU32(buf + 0, kLocationRecordVersion);
  putF64(buf + 4, rec.lat);
  putF64(buf + 12, rec.lon);
  putU32(buf + 20, fnv1a32(buf, 20));  // checksum over version + lat + lon
  return true;
}

bool decodeAndValidateLocationRecord(const uint8_t* buf, uint32_t len,
                                     LocationRecord* out) {
  // Exact length only: a partial or oversized blob is corrupt.
  if (buf == nullptr || out == nullptr || len != kLocationRecordBytes) {
    return false;
  }
  if (getU32(buf + 0) != kLocationRecordVersion) {
    return false;
  }
  if (getU32(buf + 20) != fnv1a32(buf, 20)) {
    return false;  // integrity failure (corrupt / torn write)
  }
  const double lat = getF64(buf + 4);
  const double lon = getF64(buf + 12);
  if (!coordinatesValid(lat, lon)) {
    return false;  // corrupt-but-checksum-valid or out-of-range: reject
  }
  out->lat = lat;
  out->lon = lon;
  return true;
}

}  // namespace core
