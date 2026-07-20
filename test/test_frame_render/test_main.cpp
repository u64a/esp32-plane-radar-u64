#include <unity.h>

#include <cstdint>

#include "core/frame_render.h"
#include "core/radar_data_state.h"

using core::FrameRenderKey;
using core::frameRenderKey;
using core::frameRenderKeyEqual;
using core::RadarDataMode;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

// Build a key with sensible defaults; individual tests vary one axis at a time.
FrameRenderKey key(RadarDataMode mode, uint32_t age = 0, uint32_t data_rev = 7,
                   uint32_t snap_rev = 7, uint16_t count = 3, bool wifi = true,
                   uint8_t phase = 0) {
  return frameRenderKey(mode, age, data_rev, snap_rev, count, wifi, phase);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_equal_keys_compare_equal() {
  TEST_ASSERT_TRUE(
      frameRenderKeyEqual(key(RadarDataMode::Live), key(RadarDataMode::Live)));
}

void test_live_ignores_phase_and_age() {
  // Live is steady: neither the 500 ms activity phase nor the age changes it, so
  // a Live frame never redraws continuously.
  const FrameRenderKey base = key(RadarDataMode::Live, /*age=*/4, 7, 7, 3, true,
                                  /*phase=*/0);
  TEST_ASSERT_TRUE(frameRenderKeyEqual(
      base, key(RadarDataMode::Live, 9, 7, 7, 3, true, 1)));
  TEST_ASSERT_TRUE(frameRenderKeyEqual(
      base, key(RadarDataMode::Live, 99, 7, 7, 3, true, 2)));
}

void test_offline_ignores_phase_and_age() {
  const FrameRenderKey base =
      key(RadarDataMode::Offline, 120, 7, 7, 0, false, 0);
  TEST_ASSERT_TRUE(frameRenderKeyEqual(
      base, key(RadarDataMode::Offline, 240, 7, 7, 0, false, 2)));
}

void test_loading_tracks_phase_but_not_age() {
  const FrameRenderKey p0 =
      key(RadarDataMode::Loading, /*age=*/10, 7, 7, 0, true, /*phase=*/0);
  // A different Loading phase must change the key (animated dots).
  TEST_ASSERT_FALSE(frameRenderKeyEqual(
      p0, key(RadarDataMode::Loading, 10, 7, 7, 0, true, 1)));
  // A different age while Loading must NOT change the key (age isn't shown).
  TEST_ASSERT_TRUE(frameRenderKeyEqual(
      p0, key(RadarDataMode::Loading, 42, 7, 7, 0, true, 0)));
}

void test_stale_tracks_age_but_not_phase() {
  const FrameRenderKey a22 =
      key(RadarDataMode::Stale, /*age=*/22, 7, 7, 3, true, /*phase=*/0);
  // A different displayed age must change the key ("STALE Ns" ticks).
  TEST_ASSERT_FALSE(frameRenderKeyEqual(
      a22, key(RadarDataMode::Stale, 23, 7, 7, 3, true, 0)));
  // A different activity phase while Stale must NOT change the key.
  TEST_ASSERT_TRUE(frameRenderKeyEqual(
      a22, key(RadarDataMode::Stale, 22, 7, 7, 3, true, 2)));
}

void test_mode_transition_changes_key() {
  TEST_ASSERT_FALSE(frameRenderKeyEqual(key(RadarDataMode::Live),
                                        key(RadarDataMode::Stale)));
  TEST_ASSERT_FALSE(frameRenderKeyEqual(key(RadarDataMode::Loading),
                                        key(RadarDataMode::Offline)));
}

void test_wifi_edge_changes_key() {
  TEST_ASSERT_FALSE(frameRenderKeyEqual(
      key(RadarDataMode::Live, 0, 7, 7, 3, true, 0),
      key(RadarDataMode::Live, 0, 7, 7, 3, false, 0)));
}

void test_revision_and_count_changes_change_key() {
  const FrameRenderKey base = key(RadarDataMode::Live, 0, 7, 7, 3, true, 0);
  // A revision switch (data side) that will hide stale targets changes the key.
  TEST_ASSERT_FALSE(frameRenderKeyEqual(
      base, key(RadarDataMode::Live, 0, 8, 7, 3, true, 0)));
  // A new publication (snapshot revision) changes the key.
  TEST_ASSERT_FALSE(frameRenderKeyEqual(
      base, key(RadarDataMode::Live, 0, 7, 8, 3, true, 0)));
  // A new aircraft count (same revision republish) changes the key.
  TEST_ASSERT_FALSE(frameRenderKeyEqual(
      base, key(RadarDataMode::Live, 0, 7, 7, 5, true, 0)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_equal_keys_compare_equal);
  RUN_TEST(test_live_ignores_phase_and_age);
  RUN_TEST(test_offline_ignores_phase_and_age);
  RUN_TEST(test_loading_tracks_phase_but_not_age);
  RUN_TEST(test_stale_tracks_age_but_not_phase);
  RUN_TEST(test_mode_transition_changes_key);
  RUN_TEST(test_wifi_edge_changes_key);
  RUN_TEST(test_revision_and_count_changes_change_key);
  return UNITY_END();
}
