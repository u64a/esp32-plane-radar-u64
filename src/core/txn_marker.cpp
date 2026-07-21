#include "core/txn_marker.h"

namespace core {

namespace {

// Little-endian (de)serialization helpers so the persisted marker encodes
// identically on the ESP32-C3 and on the native test host, mirroring the
// core::PersistedFloorRecord helpers.
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

// FNV-1a 32-bit: a small deterministic integrity check that detects a corrupt or
// partially-written NVS blob. It is NOT a cryptographic MAC (the marker is not a
// trust anchor -- it carries only a small enum re-validated on read).
uint32_t fnv1a32(const uint8_t* data, uint32_t len) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

}  // namespace

bool txnMarkerStateKnown(uint32_t v) {
  return v == static_cast<uint32_t>(TxnMarkerState::None) ||
         v == static_cast<uint32_t>(TxnMarkerState::CommitInProgress) ||
         v == static_cast<uint32_t>(TxnMarkerState::ErasePending);
}

bool encodeTxnMarker(const TxnMarkerRecord& rec, uint8_t* buf, uint32_t len) {
  if (buf == nullptr || len < kTxnMarkerRecordBytes) {
    return false;
  }
  const uint32_t state = static_cast<uint32_t>(rec.state);
  if (!txnMarkerStateKnown(state)) {
    return false;  // never persist an unknown state
  }
  putU32(buf + 0, kTxnMarkerVersion);
  putU32(buf + 4, state);
  putU32(buf + 8, fnv1a32(buf, 8));  // checksum over version + state
  return true;
}

bool decodeAndValidateTxnMarker(const uint8_t* buf, uint32_t len,
                                TxnMarkerRecord* out) {
  // Exact length only: a partial or oversized blob is corrupt.
  if (buf == nullptr || out == nullptr || len != kTxnMarkerRecordBytes) {
    return false;
  }
  if (getU32(buf + 0) != kTxnMarkerVersion) {
    return false;
  }
  if (getU32(buf + 8) != fnv1a32(buf, 8)) {
    return false;  // integrity failure (corrupt / torn write)
  }
  const uint32_t state = getU32(buf + 4);
  if (!txnMarkerStateKnown(state)) {
    return false;  // unknown state -> treat as invalid, never "none"
  }
  out->state = static_cast<TxnMarkerState>(state);
  return true;
}

}  // namespace core
