#include <unity.h>

#include <cstring>

#include "core/location_record.h"

// Record/validation tests for the pure radar-location blob. The record must
// round-trip a valid lat/lon pair, and MUST reject a corrupt checksum, a wrong
// version, an out-of-range coordinate, or a wrong-length blob -- so a torn NVS
// write can never decode to a bogus location and a half-updated pair is
// impossible (lat + lon are one atomic, checksummed record).

void setUp() {}
void tearDown() {}

namespace {

void encodeOk(double lat, double lon, uint8_t out[core::kLocationRecordBytes]) {
  const core::LocationRecord rec{lat, lon};
  TEST_ASSERT_TRUE(
      core::encodeLocationRecord(rec, out, core::kLocationRecordBytes));
}

void roundTrip(double lat, double lon) {
  uint8_t buf[core::kLocationRecordBytes];
  encodeOk(lat, lon, buf);
  core::LocationRecord got{};
  TEST_ASSERT_TRUE(core::decodeAndValidateLocationRecord(
      buf, core::kLocationRecordBytes, &got));
  TEST_ASSERT_TRUE(got.lat == lat);
  TEST_ASSERT_TRUE(got.lon == lon);
}

}  // namespace

void test_record_size_is_twenty_four_bytes() {
  TEST_ASSERT_EQUAL_UINT32(24, core::kLocationRecordBytes);
}

void test_round_trip_valid_pairs() {
  roundTrip(0.0, 0.0);
  roundTrip(52.379189, 4.899431);
  roundTrip(-33.868820, 151.209290);
  roundTrip(90.0, 180.0);
  roundTrip(-90.0, -180.0);
}

void test_reject_corrupt_checksum() {
  uint8_t buf[core::kLocationRecordBytes];
  encodeOk(52.5, 4.25, buf);
  buf[20] ^= 0xFF;  // flip a checksum byte
  core::LocationRecord got{};
  TEST_ASSERT_FALSE(core::decodeAndValidateLocationRecord(
      buf, core::kLocationRecordBytes, &got));
}

void test_reject_flipped_lat_byte() {
  // Flipping a latitude byte without recomputing the checksum must fail integrity.
  uint8_t buf[core::kLocationRecordBytes];
  encodeOk(10.0, 20.0, buf);
  buf[4] ^= 0x01;
  core::LocationRecord got{};
  TEST_ASSERT_FALSE(core::decodeAndValidateLocationRecord(
      buf, core::kLocationRecordBytes, &got));
}

void test_reject_wrong_version() {
  uint8_t buf[core::kLocationRecordBytes];
  encodeOk(1.0, 2.0, buf);
  buf[0] = 0xEE;  // corrupt the version field (checksum now mismatches too)
  core::LocationRecord got{};
  TEST_ASSERT_FALSE(core::decodeAndValidateLocationRecord(
      buf, core::kLocationRecordBytes, &got));
}

void test_reject_wrong_length() {
  uint8_t buf[core::kLocationRecordBytes];
  encodeOk(3.0, 4.0, buf);
  core::LocationRecord got{};
  TEST_ASSERT_FALSE(core::decodeAndValidateLocationRecord(
      buf, core::kLocationRecordBytes - 1, &got));  // short
  TEST_ASSERT_FALSE(core::decodeAndValidateLocationRecord(
      buf, core::kLocationRecordBytes + 1, &got));  // long
  TEST_ASSERT_FALSE(core::decodeAndValidateLocationRecord(nullptr, 0, &got));
}

void test_encode_rejects_out_of_range() {
  uint8_t buf[core::kLocationRecordBytes];
  const core::LocationRecord bad_lat{91.0, 0.0};
  TEST_ASSERT_FALSE(
      core::encodeLocationRecord(bad_lat, buf, core::kLocationRecordBytes));
  const core::LocationRecord bad_lon{0.0, 181.0};
  TEST_ASSERT_FALSE(
      core::encodeLocationRecord(bad_lon, buf, core::kLocationRecordBytes));
}

void test_decode_rejects_out_of_range_even_with_valid_checksum() {
  // Hand-build a record with an out-of-range latitude and a MATCHING checksum, so
  // only the coordinate-range guard can reject it. Uses the same LE + FNV-1a
  // layout as the encoder.
  uint8_t buf[core::kLocationRecordBytes];
  memset(buf, 0, sizeof(buf));
  buf[0] = 1;  // version = 1 (LE)
  // lat = 200.0 (IEEE-754 LE); lon = 0.0 (already zeroed).
  uint64_t lat_bits = 0;
  const double lat = 200.0;
  memcpy(&lat_bits, &lat, sizeof(lat_bits));
  for (int i = 0; i < 8; ++i) {
    buf[4 + i] = static_cast<uint8_t>((lat_bits >> (8 * i)) & 0xFFu);
  }
  uint32_t h = 2166136261u;
  for (int i = 0; i < 20; ++i) {
    h ^= buf[i];
    h *= 16777619u;
  }
  buf[20] = static_cast<uint8_t>(h & 0xFFu);
  buf[21] = static_cast<uint8_t>((h >> 8) & 0xFFu);
  buf[22] = static_cast<uint8_t>((h >> 16) & 0xFFu);
  buf[23] = static_cast<uint8_t>((h >> 24) & 0xFFu);
  core::LocationRecord got{};
  TEST_ASSERT_FALSE(core::decodeAndValidateLocationRecord(
      buf, core::kLocationRecordBytes, &got));
}

void test_encode_rejects_small_buffer() {
  uint8_t small[core::kLocationRecordBytes - 1];
  const core::LocationRecord rec{1.0, 2.0};
  TEST_ASSERT_FALSE(core::encodeLocationRecord(rec, small, sizeof(small)));
  TEST_ASSERT_FALSE(core::encodeLocationRecord(rec, nullptr, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_record_size_is_twenty_four_bytes);
  RUN_TEST(test_round_trip_valid_pairs);
  RUN_TEST(test_reject_corrupt_checksum);
  RUN_TEST(test_reject_flipped_lat_byte);
  RUN_TEST(test_reject_wrong_version);
  RUN_TEST(test_reject_wrong_length);
  RUN_TEST(test_encode_rejects_out_of_range);
  RUN_TEST(test_decode_rejects_out_of_range_even_with_valid_checksum);
  RUN_TEST(test_encode_rejects_small_buffer);
  return UNITY_END();
}
