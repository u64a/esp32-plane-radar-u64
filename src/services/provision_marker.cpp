#include "services/provision_marker.h"

#include <Preferences.h>

#include "config.h"

namespace services::provision_marker {

ReadResult read(core::TxnMarkerState* out) {
  if (out == nullptr) {
    return ReadResult::Error;
  }
  Preferences prefs;
  // Read-write so an absent namespace on a fresh device is created and reported
  // as Absent, rather than being indistinguishable from a genuine NVS error (a
  // read-only begin fails for both). A real NVS failure still returns false here.
  if (!prefs.begin(config::kProvisionMarkerNvsNamespace, /*readOnly=*/false)) {
    return ReadResult::Error;  // genuine NVS failure: caller fails closed
  }
  if (!prefs.isKey(config::kProvisionMarkerNvsKey)) {
    prefs.end();
    return ReadResult::Absent;  // never written / already cleared to absent
  }
  if (prefs.getBytesLength(config::kProvisionMarkerNvsKey) !=
      core::kTxnMarkerRecordBytes) {
    prefs.end();
    return ReadResult::Error;  // wrong-size blob: corrupt / torn write
  }
  uint8_t buf[core::kTxnMarkerRecordBytes];
  const size_t n =
      prefs.getBytes(config::kProvisionMarkerNvsKey, buf, sizeof(buf));
  prefs.end();
  core::TxnMarkerRecord rec{};
  if (n != core::kTxnMarkerRecordBytes ||
      !core::decodeAndValidateTxnMarker(buf, static_cast<uint32_t>(n), &rec)) {
    return ReadResult::Error;  // corrupt: fail closed (never decode to "none")
  }
  *out = rec.state;
  return ReadResult::Present;
}

bool writeVerified(core::TxnMarkerState state) {
  uint8_t buf[core::kTxnMarkerRecordBytes];
  const core::TxnMarkerRecord rec{state};
  if (!core::encodeTxnMarker(rec, buf, sizeof(buf))) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(config::kProvisionMarkerNvsNamespace, /*readOnly=*/false)) {
    return false;
  }
  const bool wrote =
      prefs.putBytes(config::kProvisionMarkerNvsKey, buf, sizeof(buf)) ==
      sizeof(buf);
  prefs.end();
  if (!wrote) {
    return false;
  }
  // Verify by read-back: only a record that decodes to exactly `state` counts as
  // durably persisted. A torn write reads back Error/other and fails here.
  core::TxnMarkerState rb = core::TxnMarkerState::None;
  return read(&rb) == ReadResult::Present && rb == state;
}

}  // namespace services::provision_marker
