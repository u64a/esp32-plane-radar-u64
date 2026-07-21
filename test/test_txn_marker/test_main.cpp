#include <unity.h>

#include <cstring>

#include "core/txn_marker.h"

// Record/validation tests for the pure provisioning transaction marker. The
// marker must round-trip every known state, and MUST reject a corrupt checksum,
// a wrong version, an unknown state, or a wrong-length blob -- so a torn NVS
// write after a credential commit / erase can never decode to a bogus "none".

void setUp() {}
void tearDown() {}

namespace {

// Encode helper that asserts the encode succeeded and returns the buffer filled.
void encodeOk(core::TxnMarkerState state, uint8_t out[core::kTxnMarkerRecordBytes]) {
  const core::TxnMarkerRecord rec{state};
  TEST_ASSERT_TRUE(
      core::encodeTxnMarker(rec, out, core::kTxnMarkerRecordBytes));
}

void roundTrip(core::TxnMarkerState state) {
  uint8_t buf[core::kTxnMarkerRecordBytes];
  encodeOk(state, buf);
  core::TxnMarkerRecord got{};
  TEST_ASSERT_TRUE(core::decodeAndValidateTxnMarker(
      buf, core::kTxnMarkerRecordBytes, &got));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(state),
                           static_cast<uint32_t>(got.state));
}

}  // namespace

void test_record_size_is_twelve_bytes() {
  TEST_ASSERT_EQUAL_UINT32(12, core::kTxnMarkerRecordBytes);
}

void test_round_trip_every_known_state() {
  roundTrip(core::TxnMarkerState::None);
  roundTrip(core::TxnMarkerState::CommitInProgress);
  roundTrip(core::TxnMarkerState::ErasePending);
}

void test_reject_corrupt_checksum() {
  uint8_t buf[core::kTxnMarkerRecordBytes];
  encodeOk(core::TxnMarkerState::CommitInProgress, buf);
  buf[8] ^= 0xFF;  // flip a checksum byte
  core::TxnMarkerRecord got{};
  TEST_ASSERT_FALSE(core::decodeAndValidateTxnMarker(
      buf, core::kTxnMarkerRecordBytes, &got));
}

void test_reject_flipped_state_byte() {
  // Flipping a state byte without recomputing the checksum must fail integrity.
  uint8_t buf[core::kTxnMarkerRecordBytes];
  encodeOk(core::TxnMarkerState::ErasePending, buf);
  buf[4] ^= 0x01;
  core::TxnMarkerRecord got{};
  TEST_ASSERT_FALSE(core::decodeAndValidateTxnMarker(
      buf, core::kTxnMarkerRecordBytes, &got));
}

void test_reject_wrong_version() {
  uint8_t buf[core::kTxnMarkerRecordBytes];
  encodeOk(core::TxnMarkerState::None, buf);
  buf[0] = 0xEE;  // corrupt the version field (checksum now mismatches too)
  core::TxnMarkerRecord got{};
  TEST_ASSERT_FALSE(core::decodeAndValidateTxnMarker(
      buf, core::kTxnMarkerRecordBytes, &got));
}

void test_reject_unknown_state_even_with_valid_checksum() {
  // Hand-build a record with an unknown state (3) and a MATCHING checksum, so
  // only the state-known guard can reject it.
  uint8_t buf[core::kTxnMarkerRecordBytes];
  memset(buf, 0, sizeof(buf));
  buf[0] = 1;  // version = 1 (LE)
  buf[4] = 3;  // state = 3 (unknown), LE
  // FNV-1a over bytes [0..7] computed the same way as the encoder.
  uint32_t h = 2166136261u;
  for (int i = 0; i < 8; ++i) {
    h ^= buf[i];
    h *= 16777619u;
  }
  buf[8] = static_cast<uint8_t>(h & 0xFFu);
  buf[9] = static_cast<uint8_t>((h >> 8) & 0xFFu);
  buf[10] = static_cast<uint8_t>((h >> 16) & 0xFFu);
  buf[11] = static_cast<uint8_t>((h >> 24) & 0xFFu);
  core::TxnMarkerRecord got{};
  TEST_ASSERT_FALSE(core::decodeAndValidateTxnMarker(
      buf, core::kTxnMarkerRecordBytes, &got));
  // The encoder likewise refuses to write an unknown state.
  core::TxnMarkerRecord bad{static_cast<core::TxnMarkerState>(7)};
  TEST_ASSERT_FALSE(
      core::encodeTxnMarker(bad, buf, core::kTxnMarkerRecordBytes));
}

void test_reject_wrong_length() {
  uint8_t buf[core::kTxnMarkerRecordBytes];
  encodeOk(core::TxnMarkerState::CommitInProgress, buf);
  core::TxnMarkerRecord got{};
  TEST_ASSERT_FALSE(core::decodeAndValidateTxnMarker(
      buf, core::kTxnMarkerRecordBytes - 1, &got));  // short
  TEST_ASSERT_FALSE(core::decodeAndValidateTxnMarker(
      buf, core::kTxnMarkerRecordBytes + 1, &got));  // long
  TEST_ASSERT_FALSE(core::decodeAndValidateTxnMarker(nullptr, 0, &got));
}

void test_encode_rejects_small_buffer() {
  uint8_t small[core::kTxnMarkerRecordBytes - 1];
  const core::TxnMarkerRecord rec{core::TxnMarkerState::None};
  TEST_ASSERT_FALSE(core::encodeTxnMarker(rec, small, sizeof(small)));
  TEST_ASSERT_FALSE(core::encodeTxnMarker(rec, nullptr, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_record_size_is_twelve_bytes);
  RUN_TEST(test_round_trip_every_known_state);
  RUN_TEST(test_reject_corrupt_checksum);
  RUN_TEST(test_reject_flipped_state_byte);
  RUN_TEST(test_reject_wrong_version);
  RUN_TEST(test_reject_unknown_state_even_with_valid_checksum);
  RUN_TEST(test_reject_wrong_length);
  RUN_TEST(test_encode_rejects_small_buffer);
  return UNITY_END();
}
