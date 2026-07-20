#include <unity.h>

#include <cstdint>

#include "core/coordinates.h"
#include "core/settings_events.h"

using core::CoordinateSaveResult;
using core::SettingsState;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

int result(CoordinateSaveResult r) { return static_cast<int>(r); }

}  // namespace

void setUp() {}
void tearDown() {}

void test_initial_state_has_no_revision_or_pending() {
  SettingsState s = {};
  TEST_ASSERT_EQUAL_UINT32(0, core::settingsRevision(s));
  TEST_ASSERT_FALSE(core::settingsConsumeQueryChange(&s));
}

void test_query_change_bumps_revision_and_latches_once() {
  SettingsState s = {};
  core::settingsQueryChanged(&s);
  TEST_ASSERT_EQUAL_UINT32(1, core::settingsRevision(s));
  TEST_ASSERT_TRUE(core::settingsConsumeQueryChange(&s));
  TEST_ASSERT_FALSE(core::settingsConsumeQueryChange(&s));  // consumed once
  TEST_ASSERT_EQUAL_UINT32(1, core::settingsRevision(s));   // revision persists
}

void test_multiple_changes_increment_but_consume_is_single_shot() {
  SettingsState s = {};
  core::settingsQueryChanged(&s);
  core::settingsQueryChanged(&s);
  core::settingsQueryChanged(&s);
  TEST_ASSERT_EQUAL_UINT32(3, core::settingsRevision(s));
  TEST_ASSERT_TRUE(core::settingsConsumeQueryChange(&s));
  TEST_ASSERT_FALSE(core::settingsConsumeQueryChange(&s));
}

void test_revision_is_rollover_safe() {
  SettingsState s = {};
  s.revision = UINT32_MAX;
  core::settingsQueryChanged(&s);
  TEST_ASSERT_EQUAL_UINT32(0, core::settingsRevision(s));  // wraps, equality-safe
  TEST_ASSERT_TRUE(core::settingsConsumeQueryChange(&s));
}

void test_visual_change_latches_once_without_touching_revision() {
  SettingsState s = {};
  core::settingsVisualChanged(&s);
  // Visual-only: the revision must not move and no query change is pending.
  TEST_ASSERT_EQUAL_UINT32(0, core::settingsRevision(s));
  TEST_ASSERT_FALSE(core::settingsConsumeQueryChange(&s));
  // The visual flag is a single-shot latch.
  TEST_ASSERT_TRUE(core::settingsConsumeVisualChange(&s));
  TEST_ASSERT_FALSE(core::settingsConsumeVisualChange(&s));
  TEST_ASSERT_EQUAL_UINT32(0, core::settingsRevision(s));
}

void test_visual_and_query_latches_are_independent() {
  SettingsState s = {};
  core::settingsQueryChanged(&s);   // revision 1, query pending
  core::settingsVisualChanged(&s);  // visual pending, revision unchanged
  TEST_ASSERT_EQUAL_UINT32(1, core::settingsRevision(s));
  // Consuming the query change must not clear the visual latch, and vice versa.
  TEST_ASSERT_TRUE(core::settingsConsumeQueryChange(&s));
  TEST_ASSERT_TRUE(core::settingsConsumeVisualChange(&s));
  TEST_ASSERT_FALSE(core::settingsConsumeQueryChange(&s));
  TEST_ASSERT_FALSE(core::settingsConsumeVisualChange(&s));
  TEST_ASSERT_EQUAL_UINT32(1, core::settingsRevision(s));
}

void test_repeated_visual_changes_do_not_revise_query() {
  SettingsState s = {};
  for (int i = 0; i < 5; ++i) {
    core::settingsVisualChanged(&s);
  }
  // No number of visual-only changes ever bumps the query revision.
  TEST_ASSERT_EQUAL_UINT32(0, core::settingsRevision(s));
  TEST_ASSERT_FALSE(core::settingsConsumeQueryChange(&s));
  TEST_ASSERT_TRUE(core::settingsConsumeVisualChange(&s));  // still just a latch
  TEST_ASSERT_FALSE(core::settingsConsumeVisualChange(&s));
}

void test_coordinate_save_changed_returns_parsed_values() {
  double lat = 0.0;
  double lon = 0.0;
  const CoordinateSaveResult r =
      core::classifyCoordinateSave("52.0", "4.0", 10.0, 20.0, &lat, &lon);
  TEST_ASSERT_EQUAL_INT(result(CoordinateSaveResult::Changed), result(r));
  TEST_ASSERT_TRUE(lat == 52.0);
  TEST_ASSERT_TRUE(lon == 4.0);
}

void test_coordinate_save_unchanged_when_identical() {
  double lat = -1.0;
  double lon = -1.0;
  const CoordinateSaveResult r =
      core::classifyCoordinateSave("52.5", "4.25", 52.5, 4.25, &lat, &lon);
  TEST_ASSERT_EQUAL_INT(result(CoordinateSaveResult::Unchanged), result(r));
  // Outputs still receive the parsed values on a valid parse.
  TEST_ASSERT_TRUE(lat == 52.5);
  TEST_ASSERT_TRUE(lon == 4.25);
}

void test_coordinate_save_changed_when_only_one_axis_differs() {
  double lat = 0.0;
  double lon = 0.0;
  TEST_ASSERT_EQUAL_INT(
      result(CoordinateSaveResult::Changed),
      result(core::classifyCoordinateSave("52.5", "5.0", 52.5, 4.25, &lat, &lon)));
}

void test_coordinate_save_invalid_leaves_outputs_untouched() {
  double lat = 12.0;
  double lon = 34.0;
  TEST_ASSERT_EQUAL_INT(
      result(CoordinateSaveResult::Invalid),
      result(core::classifyCoordinateSave("nan", "4.0", 1.0, 2.0, &lat, &lon)));
  TEST_ASSERT_EQUAL_INT(
      result(CoordinateSaveResult::Invalid),
      result(core::classifyCoordinateSave("200", "4.0", 1.0, 2.0, &lat, &lon)));
  TEST_ASSERT_EQUAL_INT(
      result(CoordinateSaveResult::Invalid),
      result(core::classifyCoordinateSave("", "4.0", 1.0, 2.0, &lat, &lon)));
  TEST_ASSERT_TRUE(lat == 12.0);  // untouched on invalid
  TEST_ASSERT_TRUE(lon == 34.0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_initial_state_has_no_revision_or_pending);
  RUN_TEST(test_query_change_bumps_revision_and_latches_once);
  RUN_TEST(test_multiple_changes_increment_but_consume_is_single_shot);
  RUN_TEST(test_revision_is_rollover_safe);
  RUN_TEST(test_visual_change_latches_once_without_touching_revision);
  RUN_TEST(test_visual_and_query_latches_are_independent);
  RUN_TEST(test_repeated_visual_changes_do_not_revise_query);
  RUN_TEST(test_coordinate_save_changed_returns_parsed_values);
  RUN_TEST(test_coordinate_save_unchanged_when_identical);
  RUN_TEST(test_coordinate_save_changed_when_only_one_axis_differs);
  RUN_TEST(test_coordinate_save_invalid_leaves_outputs_untouched);
  return UNITY_END();
}
