#include <unity.h>

#include <cstdint>

#include "core/radar_range.h"

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

void setUp() {}
void tearDown() {}

void test_saved_index_sanitization_preserves_valid_and_falls_back_invalid() {
  for (uint8_t index = 0; index < core::range::kRangePresetCount; ++index) {
    TEST_ASSERT_EQUAL_UINT8(index, core::range::sanitizeSavedIndex(index));
  }
  TEST_ASSERT_EQUAL_UINT8(core::range::kDefaultRangeIndex,
                          core::range::sanitizeSavedIndex(4U));
  TEST_ASSERT_EQUAL_UINT8(core::range::kDefaultRangeIndex,
                          core::range::sanitizeSavedIndex(UINT8_MAX));
}

void test_next_index_wraps_last_preset_to_first() {
  TEST_ASSERT_EQUAL_UINT8(
      0U, core::range::nextIndex(core::range::kRangePresetCount - 1U));
}

void test_every_preset_fetch_radius_matches_screen_edge_scale() {
  constexpr float kScreenRadiusPx = 118.0f;
  constexpr float kGridOuterRadiusPx = 107.0f;
  constexpr float kExpectedRing3Km[] = {5.0f, 10.0f, 15.0f, 25.0f};

  for (size_t i = 0; i < core::range::kRangePresetCount; ++i) {
    const float expected_outer =
        kExpectedRing3Km[i] * core::range::kRing3ToOuterKm;
    const float expected =
        expected_outer * (kScreenRadiusPx / kGridOuterRadiusPx);
    TEST_ASSERT_FLOAT_WITHIN(
        0.0001f, expected,
        core::range::fetchRadiusKm(core::range::kRangePresets[i],
                                   kScreenRadiusPx, kGridOuterRadiusPx));
  }
}

void test_ring_labels_format_kilometres_and_miles() {
  char label[12] = {};

  core::range::formatRing3Label(label, sizeof(label), 10.0f, false);
  TEST_ASSERT_EQUAL_STRING("10km", label);

  core::range::formatRing3Label(label, sizeof(label), 10.0f, true);
  TEST_ASSERT_EQUAL_STRING("6mi", label);
}

void test_ring_label_rounding_matches_legacy_behavior() {
  char label[12] = {};

  core::range::formatRing3Label(label, sizeof(label), 5.49f, false);
  TEST_ASSERT_EQUAL_STRING("5km", label);
  core::range::formatRing3Label(label, sizeof(label), 5.5f, false);
  TEST_ASSERT_EQUAL_STRING("6km", label);
  core::range::formatRing3Label(label, sizeof(label), 4.1f, true);
  TEST_ASSERT_EQUAL_STRING("3mi", label);
}

void test_ring_label_handles_zero_and_small_buffers() {
  char untouched = 'X';
  core::range::formatRing3Label(nullptr, 0, 5.0f, false);
  core::range::formatRing3Label(&untouched, 0, 5.0f, false);
  TEST_ASSERT_EQUAL_CHAR('X', untouched);

  char one_byte[1] = {'X'};
  core::range::formatRing3Label(one_byte, sizeof(one_byte), 5.0f, false);
  TEST_ASSERT_EQUAL_CHAR('\0', one_byte[0]);

  char two_bytes[2] = {};
  core::range::formatRing3Label(two_bytes, sizeof(two_bytes), 5.0f, false);
  TEST_ASSERT_EQUAL_STRING("5", two_bytes);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(
      test_saved_index_sanitization_preserves_valid_and_falls_back_invalid);
  RUN_TEST(test_next_index_wraps_last_preset_to_first);
  RUN_TEST(test_every_preset_fetch_radius_matches_screen_edge_scale);
  RUN_TEST(test_ring_labels_format_kilometres_and_miles);
  RUN_TEST(test_ring_label_rounding_matches_legacy_behavior);
  RUN_TEST(test_ring_label_handles_zero_and_small_buffers);
  return UNITY_END();
}
