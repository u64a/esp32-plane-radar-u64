#pragma once

#include <cstdint>

#include "core/txn_marker.h"

// ESP-only durable provisioning-transaction marker (NVS-backed). Wraps the pure,
// native-tested core::TxnMarker record with a dedicated Preferences namespace. It
// records whether a credential FLASH commit or a factory erase was in progress so
// the next boot can fail closed on a power loss mid-transaction. It NEVER stores a
// password or any credential byte -- only a versioned, checksummed enum.

namespace services::provision_marker {

enum class ReadResult : uint8_t {
  Present,  // *out holds a valid decoded state (which may be None)
  Absent,   // no marker stored (verified) -- treat as None / normal boot
  Error,    // NVS open/read failure OR a corrupt record -- caller fails closed
};

// Read the persisted marker. Opens the namespace read-write so an absent
// namespace on a fresh device is reported as Absent (not Error) -- a genuine NVS
// open/read failure or a wrong-size / corrupt record is Error, which the caller
// treats fail-closed. On Present, *out holds the decoded state.
ReadResult read(core::TxnMarkerState* out);

// Persist `state` and verify it by reading the record straight back. Returns true
// ONLY when the written record reads back as exactly `state`. Writing None is how
// the marker is cleared after a verified commit / rollback / erase. Never stores
// credentials.
bool writeVerified(core::TxnMarkerState state);

}  // namespace services::provision_marker
