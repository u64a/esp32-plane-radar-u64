#pragma once

#include <cstdint>

// Pure, Arduino-free provisioning transaction marker record.
//
// A tiny durable NVS marker records whether a credential-touching FLASH
// operation (a candidate commit) or a factory erase was in progress, so a power
// loss mid-transaction is DETECTED at the next boot and handled fail-closed
// rather than silently resuming a normal connect/portal. The marker NEVER stores
// a password or any credential byte -- only a versioned, checksummed enum. The
// underlying old/new NVS atomicity is still the credential guarantee; this marker
// only tells the next boot that a transaction did not verifiably finish.
//
// Layout (little-endian, fixed 12 bytes) so it encodes identically on the
// ESP32-C3 and on the native test host, independent of the build host endianness:
//   [0..3]   version   (uint32, == kTxnMarkerVersion)
//   [4..7]   state     (uint32, a TxnMarkerState enumerator)
//   [8..11]  checksum  (uint32 FNV-1a over bytes [0..7])
// A corrupt / partial / wrong-version / unknown-state record fails validation.
// The caller treats a validation failure of a PRESENT record as a fault (never
// as "none"), because an unreadable marker after a credential transaction cannot
// prove the stored credential is safe.

namespace core {

enum class TxnMarkerState : uint32_t {
  None = 0,              // no transaction outstanding (normal boot)
  CommitInProgress = 1,  // a candidate credential FLASH commit was in progress
  ErasePending = 2,      // a factory erase was authorized and in progress
};

constexpr uint32_t kTxnMarkerVersion = 1;
constexpr uint32_t kTxnMarkerRecordBytes = 12;

struct TxnMarkerRecord {
  TxnMarkerState state;
};

// True iff v is a defined TxnMarkerState enumerator (guards against a corrupt or
// forward-version blob decoding to an unknown state).
bool txnMarkerStateKnown(uint32_t v);

// Serialize a record into exactly kTxnMarkerRecordBytes bytes (checksum
// included). Returns false if buf is null/too small or the state is not a known
// enumerator (an unknown state is never persisted).
bool encodeTxnMarker(const TxnMarkerRecord& rec, uint8_t* buf, uint32_t len);

// Parse + fully validate a persisted marker: the length must be exactly
// kTxnMarkerRecordBytes, the version and FNV-1a checksum must match, and the
// decoded state must be a known enumerator. On success sets *out and returns
// true; on ANY failure returns false. Pure and Arduino-free.
bool decodeAndValidateTxnMarker(const uint8_t* buf, uint32_t len,
                                TxnMarkerRecord* out);

}  // namespace core
