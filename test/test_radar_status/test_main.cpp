#include <unity.h>

#include <cstdint>
#include <cstring>

#include "core/radar_status.h"

using core::RadarDataMode;
using core::RadarStatusBadge;
using core::RadarStatusPlan;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

int badge(RadarStatusBadge b) { return static_cast<int>(b); }

// Convenience wrapper with the common "revisions match, connected" defaults.
RadarStatusPlan plan(RadarDataMode mode, uint32_t age, bool show_aircraft,
                     bool wifi = true, uint8_t phase = 0,
                     uint32_t data_rev = 7, uint32_t snap_rev = 7) {
  return core::radarStatusPlan(mode, age, show_aircraft, data_rev, snap_rev,
                               wifi, phase);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_live_connected_has_no_chrome_and_draws_aircraft() {
  const RadarStatusPlan p = plan(RadarDataMode::Live, 0, true);
  TEST_ASSERT_TRUE(p.draw_aircraft);
  TEST_ASSERT_EQUAL_INT(badge(RadarStatusBadge::None), badge(p.badge));
  TEST_ASSERT_EQUAL_STRING("", p.text);
}

void test_live_zero_count_is_still_live_no_chrome() {
  // count-zero is decided by the renderer via the snapshot; the plan only cares
  // that a matching revision + show_aircraft keeps drawing enabled.
  const RadarStatusPlan p = plan(RadarDataMode::Live, 3, true);
  TEST_ASSERT_TRUE(p.draw_aircraft);
  TEST_ASSERT_EQUAL_INT(badge(RadarStatusBadge::None), badge(p.badge));
}

void test_revision_mismatch_hides_aircraft_defensively() {
  const RadarStatusPlan p =
      core::radarStatusPlan(RadarDataMode::Live, 0, true, 7, 8, true, 0);
  TEST_ASSERT_FALSE(p.draw_aircraft);
  // Still Live + connected, so no chrome even though targets are hidden.
  TEST_ASSERT_EQUAL_INT(badge(RadarStatusBadge::None), badge(p.badge));
}

void test_loading_connected_shows_loading_with_cycling_dots() {
  const RadarStatusPlan p0 = plan(RadarDataMode::Loading, 0, false, true, 0);
  TEST_ASSERT_FALSE(p0.draw_aircraft);
  TEST_ASSERT_EQUAL_INT(badge(RadarStatusBadge::Loading), badge(p0.badge));
  TEST_ASSERT_EQUAL_STRING("LOADING.", p0.text);

  TEST_ASSERT_EQUAL_STRING("LOADING..",
                           plan(RadarDataMode::Loading, 0, false, true, 1).text);
  TEST_ASSERT_EQUAL_STRING("LOADING...",
                           plan(RadarDataMode::Loading, 0, false, true, 2).text);
  // Deterministic cycle: phase 3 wraps back to a single dot.
  TEST_ASSERT_EQUAL_STRING("LOADING.",
                           plan(RadarDataMode::Loading, 0, false, true, 3).text);
  TEST_ASSERT_EQUAL_STRING(
      "LOADING..", plan(RadarDataMode::Loading, 0, false, true, 253).text);
}

void test_loading_disconnected_shows_no_wifi() {
  const RadarStatusPlan p = plan(RadarDataMode::Loading, 0, false, false, 1);
  TEST_ASSERT_FALSE(p.draw_aircraft);
  TEST_ASSERT_EQUAL_INT(badge(RadarStatusBadge::NoWifi), badge(p.badge));
  TEST_ASSERT_EQUAL_STRING("NO WIFI", p.text);
}

void test_live_disconnected_keeps_targets_and_shows_no_wifi() {
  const RadarStatusPlan p = plan(RadarDataMode::Live, 4, true, false);
  TEST_ASSERT_TRUE(p.draw_aircraft);
  TEST_ASSERT_EQUAL_INT(badge(RadarStatusBadge::NoWifi), badge(p.badge));
  TEST_ASSERT_EQUAL_STRING("NO WIFI", p.text);
}

void test_stale_shows_age_and_retains_targets_even_when_disconnected() {
  const RadarStatusPlan connected = plan(RadarDataMode::Stale, 22, true, true);
  TEST_ASSERT_TRUE(connected.draw_aircraft);
  TEST_ASSERT_EQUAL_INT(badge(RadarStatusBadge::Stale), badge(connected.badge));
  TEST_ASSERT_EQUAL_STRING("STALE 22s", connected.text);

  // Freshness takes precedence over a Wi-Fi drop once Stale.
  const RadarStatusPlan dropped = plan(RadarDataMode::Stale, 22, true, false);
  TEST_ASSERT_EQUAL_INT(badge(RadarStatusBadge::Stale), badge(dropped.badge));
  TEST_ASSERT_EQUAL_STRING("STALE 22s", dropped.text);
}

void test_stale_age_is_clamped_for_bounded_text() {
  TEST_ASSERT_EQUAL_STRING(
      "STALE 999s", plan(RadarDataMode::Stale, 999, true).text);
  TEST_ASSERT_EQUAL_STRING(
      "STALE 999s", plan(RadarDataMode::Stale, 1000, true).text);
  TEST_ASSERT_EQUAL_STRING(
      "STALE 999s", plan(RadarDataMode::Stale, 4000000000u, true).text);
}

void test_offline_hides_targets_and_takes_precedence_over_wifi() {
  const RadarStatusPlan p = plan(RadarDataMode::Offline, 120, false, false);
  TEST_ASSERT_FALSE(p.draw_aircraft);
  TEST_ASSERT_EQUAL_INT(badge(RadarStatusBadge::Offline), badge(p.badge));
  TEST_ASSERT_EQUAL_STRING("OFFLINE", p.text);
}

void test_all_text_fits_the_fixed_buffer() {
  // The longest strings must stay NUL-terminated within text[16].
  const RadarStatusPlan stale = plan(RadarDataMode::Stale, 999, true);
  TEST_ASSERT_TRUE(std::strlen(stale.text) < sizeof(stale.text));
  const RadarStatusPlan loading =
      plan(RadarDataMode::Loading, 0, false, true, 2);
  TEST_ASSERT_TRUE(std::strlen(loading.text) < sizeof(loading.text));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_live_connected_has_no_chrome_and_draws_aircraft);
  RUN_TEST(test_live_zero_count_is_still_live_no_chrome);
  RUN_TEST(test_revision_mismatch_hides_aircraft_defensively);
  RUN_TEST(test_loading_connected_shows_loading_with_cycling_dots);
  RUN_TEST(test_loading_disconnected_shows_no_wifi);
  RUN_TEST(test_live_disconnected_keeps_targets_and_shows_no_wifi);
  RUN_TEST(test_stale_shows_age_and_retains_targets_even_when_disconnected);
  RUN_TEST(test_stale_age_is_clamped_for_bounded_text);
  RUN_TEST(test_offline_hides_targets_and_takes_precedence_over_wifi);
  RUN_TEST(test_all_text_fits_the_fixed_buffer);
  return UNITY_END();
}
