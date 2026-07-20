#include <unity.h>

#include <cstdint>

#include "core/radar_data_state.h"

using core::kDefaultRadarFreshnessPolicy;
using core::RadarDataMode;
using core::RadarDataState;
using core::RadarDataView;
using core::RadarFreshnessPolicy;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

constexpr RadarFreshnessPolicy F = kDefaultRadarFreshnessPolicy;

int mode(RadarDataMode m) { return static_cast<int>(m); }

}  // namespace

void setUp() {}
void tearDown() {}

void test_starts_in_loading_with_no_aircraft() {
  RadarDataState s = {};
  core::radarDataStart(&s, 1000);
  const RadarDataView v = core::radarDataView(s, 1000);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Loading), mode(v.mode));
  TEST_ASSERT_FALSE(v.show_aircraft);
  TEST_ASSERT_EQUAL_UINT32(0, v.age_seconds);
  TEST_ASSERT_EQUAL_UINT32(0, v.settings_revision);
}

void test_live_then_stale_at_15s_then_offline_at_60s_exact_boundaries() {
  RadarDataState s = {};
  core::radarDataStart(&s, 1000);
  core::radarDataSuccess(&s, 0, 5000);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Live),
                        mode(core::radarDataAdvance(&s, 5000, F)));
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Live),
                        mode(core::radarDataAdvance(&s, 5000 + 14999, F)));
  // Exactly 15 s -> Stale, aircraft retained.
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Stale),
                        mode(core::radarDataAdvance(&s, 5000 + 15000, F)));
  RadarDataView stale = core::radarDataView(s, 5000 + 15000);
  TEST_ASSERT_TRUE(stale.show_aircraft);
  TEST_ASSERT_EQUAL_UINT32(15, stale.age_seconds);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Stale),
                        mode(core::radarDataAdvance(&s, 5000 + 59999, F)));
  // Exactly 60 s -> Offline, aircraft hidden.
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Offline),
                        mode(core::radarDataAdvance(&s, 5000 + 60000, F)));
  RadarDataView offline = core::radarDataView(s, 5000 + 60000);
  TEST_ASSERT_FALSE(offline.show_aircraft);
  TEST_ASSERT_EQUAL_UINT32(60, offline.age_seconds);
}

void test_loading_becomes_offline_at_60s_without_a_success() {
  RadarDataState s = {};
  core::radarDataStart(&s, 1000);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Loading),
                        mode(core::radarDataAdvance(&s, 1000 + 59999, F)));
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Offline),
                        mode(core::radarDataAdvance(&s, 1000 + 60000, F)));
  const RadarDataView v = core::radarDataView(s, 1000 + 60000);
  TEST_ASSERT_FALSE(v.show_aircraft);
  TEST_ASSERT_EQUAL_UINT32(60, v.age_seconds);
}

void test_new_success_returns_to_live_and_resets_age() {
  RadarDataState s = {};
  core::radarDataStart(&s, 0);
  core::radarDataSuccess(&s, 0, 0);
  core::radarDataAdvance(&s, 20000, F);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Stale), mode(s.mode));
  core::radarDataSuccess(&s, 0, 20000);  // fresh success resets the reference
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Live), mode(s.mode));
  RadarDataView v = core::radarDataView(s, 20000);
  TEST_ASSERT_EQUAL_UINT32(0, v.age_seconds);
  TEST_ASSERT_TRUE(v.show_aircraft);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Live),
                        mode(core::radarDataAdvance(&s, 20000 + 14999, F)));
}

void test_revision_change_resets_to_loading_and_gates_success() {
  RadarDataState s = {};
  core::radarDataStart(&s, 0);
  core::radarDataSuccess(&s, 0, 0);
  core::radarDataAdvance(&s, 70000, F);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Offline), mode(s.mode));

  core::radarDataRevisionChanged(&s, 1, 70000);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Loading), mode(s.mode));
  RadarDataView v = core::radarDataView(s, 70000);
  TEST_ASSERT_EQUAL_UINT32(1, v.settings_revision);
  TEST_ASSERT_FALSE(v.show_aircraft);

  // A success carrying the obsolete revision 0 is ignored.
  core::radarDataSuccess(&s, 0, 71000);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Loading), mode(s.mode));
  // A success for the current revision 1 makes it Live.
  core::radarDataSuccess(&s, 1, 71000);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Live), mode(s.mode));
}

void test_monotonic_latch_never_regresses_within_a_revision() {
  RadarDataState s = {};
  core::radarDataStart(&s, 0);
  core::radarDataSuccess(&s, 0, 1000000);
  core::radarDataAdvance(&s, 1000000 + 20000, F);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Stale), mode(s.mode));
  // A later advance whose elapsed appears small (as if the clock wrapped past the
  // reference again) must not regress a latched Stale back to Live.
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Stale),
                        mode(core::radarDataAdvance(&s, 1000000 + 5000, F)));
  // Offline likewise never regresses.
  core::radarDataAdvance(&s, 1000000 + 70000, F);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Offline),
                        mode(core::radarDataAdvance(&s, 1000000 + 16000, F)));
}

void test_age_and_modes_are_rollover_safe() {
  RadarDataState s = {};
  core::radarDataStart(&s, 0);
  const uint32_t success = UINT32_MAX - 5000U;  // 5 s before the millis wrap
  core::radarDataSuccess(&s, 0, success);
  TEST_ASSERT_EQUAL_INT(
      mode(RadarDataMode::Live),
      mode(core::radarDataAdvance(&s, static_cast<uint32_t>(success + 14999U), F)));
  TEST_ASSERT_EQUAL_INT(
      mode(RadarDataMode::Stale),
      mode(core::radarDataAdvance(&s, static_cast<uint32_t>(success + 15000U), F)));
  RadarDataView v =
      core::radarDataView(s, static_cast<uint32_t>(success + 15000U));
  TEST_ASSERT_EQUAL_UINT32(15, v.age_seconds);
  TEST_ASSERT_EQUAL_INT(
      mode(RadarDataMode::Offline),
      mode(core::radarDataAdvance(&s, static_cast<uint32_t>(success + 60000U), F)));
}

void test_loading_view_age_counts_from_revision_start() {
  RadarDataState s = {};
  core::radarDataStart(&s, 1000);
  const RadarDataView v = core::radarDataView(s, 1000 + 42000);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Loading), mode(v.mode));
  TEST_ASSERT_EQUAL_UINT32(42, v.age_seconds);
  TEST_ASSERT_FALSE(v.show_aircraft);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_in_loading_with_no_aircraft);
  RUN_TEST(test_live_then_stale_at_15s_then_offline_at_60s_exact_boundaries);
  RUN_TEST(test_loading_becomes_offline_at_60s_without_a_success);
  RUN_TEST(test_new_success_returns_to_live_and_resets_age);
  RUN_TEST(test_revision_change_resets_to_loading_and_gates_success);
  RUN_TEST(test_monotonic_latch_never_regresses_within_a_revision);
  RUN_TEST(test_age_and_modes_are_rollover_safe);
  RUN_TEST(test_loading_view_age_counts_from_revision_start);
  return UNITY_END();
}
