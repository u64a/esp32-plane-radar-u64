#include <unity.h>

#include <cstdint>

#include "core/button_gesture.h"
#include "core/coordinates.h"
#include "core/poll_policy.h"
#include "core/time_math.h"

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

void setUp() {}
void tearDown() {}

void test_elapsed_before_exact_after_threshold() {
  TEST_ASSERT_FALSE(core::elapsedAtLeast(1099U, 1000U, 100U));
  TEST_ASSERT_TRUE(core::elapsedAtLeast(1100U, 1000U, 100U));
  TEST_ASSERT_TRUE(core::elapsedAtLeast(1101U, 1000U, 100U));
}

void test_elapsed_before_exact_after_threshold_across_rollover() {
  constexpr uint32_t started = UINT32_MAX - 49U;
  TEST_ASSERT_FALSE(core::elapsedAtLeast(48U, started, 99U));
  TEST_ASSERT_TRUE(core::elapsedAtLeast(49U, started, 99U));
  TEST_ASSERT_TRUE(core::elapsedAtLeast(50U, started, 99U));
  TEST_ASSERT_EQUAL_UINT32(99U, core::elapsedMs(49U, started));
}

void test_remaining_budget_saturates_at_exact_expiry() {
  // No time elapsed yet: the whole budget remains.
  TEST_ASSERT_EQUAL_UINT32(8000U, core::remainingBudgetMs(1000U, 1000U, 8000U));
  // Partial elapsed leaves the remainder (3000 ms used of 8000).
  TEST_ASSERT_EQUAL_UINT32(5000U, core::remainingBudgetMs(4000U, 1000U, 8000U));
  // Exactly expired (8000 ms elapsed) yields 0, never a wrapped huge value.
  TEST_ASSERT_EQUAL_UINT32(0U, core::remainingBudgetMs(9000U, 1000U, 8000U));
  // Over-elapsed also saturates at 0.
  TEST_ASSERT_EQUAL_UINT32(0U, core::remainingBudgetMs(9500U, 1000U, 8000U));
  // A zero budget is always exhausted.
  TEST_ASSERT_EQUAL_UINT32(0U, core::remainingBudgetMs(1000U, 1000U, 0U));
}

void test_remaining_budget_is_rollover_safe() {
  constexpr uint32_t started = UINT32_MAX - 100U;  // 100 ms before the wrap
  constexpr uint32_t now = 49U;  // (UINT32_MAX - 100) + 150 wraps to 49
  TEST_ASSERT_EQUAL_UINT32(150U, core::elapsedMs(now, started));
  // 150 ms of a 200 ms budget elapsed across the rollover: 50 ms remains.
  TEST_ASSERT_EQUAL_UINT32(50U, core::remainingBudgetMs(now, started, 200U));
  // Exact expiry across the rollover still yields 0.
  TEST_ASSERT_EQUAL_UINT32(0U, core::remainingBudgetMs(now, started, 150U));
  // Over-elapsed across the rollover still saturates at 0 (no wrap).
  TEST_ASSERT_EQUAL_UINT32(0U, core::remainingBudgetMs(now, started, 100U));
}

void test_coordinates_reject_null_empty_and_whitespace_only() {
  double lat = 12.0;
  double lon = 34.0;
  TEST_ASSERT_FALSE(core::parseCoordinates(nullptr, "1", &lat, &lon));
  TEST_ASSERT_FALSE(core::parseCoordinates("1", nullptr, &lat, &lon));
  TEST_ASSERT_FALSE(core::parseCoordinates("", "1", &lat, &lon));
  TEST_ASSERT_FALSE(core::parseCoordinates("1", "", &lat, &lon));
  TEST_ASSERT_FALSE(core::parseCoordinates(" ", "1", &lat, &lon));
  TEST_ASSERT_TRUE(lat == 12.0);
  TEST_ASSERT_TRUE(lon == 34.0);
}

void test_coordinates_preserve_leading_whitespace_but_reject_trailing_data() {
  double lat = 0.0;
  double lon = 0.0;
  TEST_ASSERT_TRUE(core::parseCoordinates(" 1.5", "\t-2.5", &lat, &lon));
  TEST_ASSERT_TRUE(lat == 1.5);
  TEST_ASSERT_TRUE(lon == -2.5);
  TEST_ASSERT_FALSE(core::parseCoordinates("1.5 ", "-2.5", &lat, &lon));
  TEST_ASSERT_FALSE(core::parseCoordinates("1.5x", "-2.5", &lat, &lon));
  TEST_ASSERT_FALSE(core::parseCoordinates("1.5", "-2.5x", &lat, &lon));
}

void test_coordinates_reject_non_finite_values_without_updating_outputs() {
  double lat = 12.0;
  double lon = 34.0;
  TEST_ASSERT_FALSE(core::parseCoordinates("nan", "1", &lat, &lon));
  TEST_ASSERT_FALSE(core::parseCoordinates("1", "inf", &lat, &lon));
  TEST_ASSERT_FALSE(core::parseCoordinates("-infinity", "1", &lat, &lon));
  TEST_ASSERT_TRUE(lat == 12.0);
  TEST_ASSERT_TRUE(lon == 34.0);
}

void test_coordinates_accept_exact_bounds_and_exponent_notation() {
  double lat = 0.0;
  double lon = 0.0;
  TEST_ASSERT_TRUE(core::parseCoordinates("-9e1", "1.8e2", &lat, &lon));
  TEST_ASSERT_TRUE(lat == -90.0);
  TEST_ASSERT_TRUE(lon == 180.0);
  TEST_ASSERT_TRUE(core::parseCoordinates("90", "-180", &lat, &lon));
  TEST_ASSERT_TRUE(lat == 90.0);
  TEST_ASSERT_TRUE(lon == -180.0);
}

void test_coordinates_reject_out_of_range_without_updating_outputs() {
  double lat = 12.0;
  double lon = 34.0;
  TEST_ASSERT_FALSE(core::parseCoordinates("90.000001", "0", &lat, &lon));
  TEST_ASSERT_FALSE(core::parseCoordinates("0", "-180.000001", &lat, &lon));
  TEST_ASSERT_TRUE(lat == 12.0);
  TEST_ASSERT_TRUE(lon == 34.0);
}

void test_released_button_boundaries_are_exact() {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::ButtonEvent::Ignored),
      static_cast<int>(core::classifyReleasedPress(39U, 40U, 3000U)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::ButtonEvent::Tap),
      static_cast<int>(core::classifyReleasedPress(40U, 40U, 3000U)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::ButtonEvent::Tap),
      static_cast<int>(core::classifyReleasedPress(2999U, 40U, 3000U)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::ButtonEvent::LongHold),
      static_cast<int>(core::classifyReleasedPress(3000U, 40U, 3000U)));
}

void test_release_after_missed_active_poll_is_long_hold() {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::ButtonEvent::LongHold),
      static_cast<int>(core::classifyReleasedPress(5000U, 40U, 3000U)));
}

void test_active_long_hold_is_rollover_safe_and_one_shot() {
  constexpr uint32_t pressed = UINT32_MAX - 9U;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::ButtonEvent::Ignored),
      static_cast<int>(
          core::classifyActiveHold(1989U, pressed, 2000U, false)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::ButtonEvent::LongHold),
      static_cast<int>(
          core::classifyActiveHold(1990U, pressed, 2000U, false)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::ButtonEvent::Ignored),
      static_cast<int>(
          core::classifyActiveHold(2500U, pressed, 2000U, true)));
}

void test_reconnect_first_attempt_follows_disconnect_grace_at_tick_zero() {
  core::ReconnectState state = {};
  core::reconnectDisconnected(&state, 0U);
  TEST_ASSERT_FALSE(core::reconnectAttemptDue(state, 3999U, 4000U, 15000U));
  TEST_ASSERT_TRUE(core::reconnectAttemptDue(state, 4000U, 4000U, 15000U));
}

void test_failed_reconnect_retry_is_spaced_from_attempt_completion() {
  core::ReconnectState state = {};
  core::reconnectDisconnected(&state, 100U);
  core::reconnectAttemptCompleted(&state, 20000U, false);
  TEST_ASSERT_FALSE(
      core::reconnectAttemptDue(state, 34999U, 4000U, 15000U));
  TEST_ASSERT_TRUE(
      core::reconnectAttemptDue(state, 35000U, 4000U, 15000U));
}

void test_reconnect_success_resets_state_and_new_disconnect_gets_new_grace() {
  core::ReconnectState state = {};
  core::reconnectDisconnected(&state, 100U);
  core::reconnectAttemptCompleted(&state, 5000U, true);
  TEST_ASSERT_FALSE(core::reconnectAttemptDue(state, 5000U, 4000U, 15000U));
  core::reconnectDisconnected(&state, 6000U);
  TEST_ASSERT_FALSE(core::reconnectAttemptDue(state, 9999U, 4000U, 15000U));
  TEST_ASSERT_TRUE(core::reconnectAttemptDue(state, 10000U, 4000U, 15000U));
}

void test_first_adsb_fetch_is_immediate_at_tick_zero() {
  core::AdsbPollState state = {};
  core::adsbRadarDisplayed(&state);
  TEST_ASSERT_TRUE(core::adsbFetchDue(state, 0U, 3000U));
}

void test_later_adsb_fetch_is_spaced_from_fetch_completion_even_after_failure() {
  core::AdsbPollState state = {};
  core::adsbRadarDisplayed(&state);
  core::adsbFetchCompleted(&state, 10000U);
  TEST_ASSERT_FALSE(core::adsbFetchDue(state, 12999U, 3000U));
  TEST_ASSERT_TRUE(core::adsbFetchDue(state, 13000U, 3000U));
}

void test_long_adsb_operation_uses_completion_not_start_for_next_due_time() {
  core::AdsbPollState state = {};
  core::adsbRadarDisplayed(&state);
  TEST_ASSERT_TRUE(core::adsbFetchDue(state, 100U, 3000U));
  core::adsbFetchCompleted(&state, 20100U);
  TEST_ASSERT_FALSE(core::adsbFetchDue(state, 23099U, 3000U));
  TEST_ASSERT_TRUE(core::adsbFetchDue(state, 23100U, 3000U));
}

void test_reconnected_radar_makes_adsb_fetch_immediate_again() {
  core::AdsbPollState state = {};
  core::adsbRadarDisplayed(&state);
  core::adsbFetchCompleted(&state, 1000U);
  core::adsbRadarHidden(&state);
  TEST_ASSERT_FALSE(core::adsbFetchDue(state, 10000U, 3000U));
  core::adsbRadarDisplayed(&state);
  TEST_ASSERT_TRUE(core::adsbFetchDue(state, 10000U, 3000U));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_elapsed_before_exact_after_threshold);
  RUN_TEST(test_elapsed_before_exact_after_threshold_across_rollover);
  RUN_TEST(test_remaining_budget_saturates_at_exact_expiry);
  RUN_TEST(test_remaining_budget_is_rollover_safe);
  RUN_TEST(test_coordinates_reject_null_empty_and_whitespace_only);
  RUN_TEST(
      test_coordinates_preserve_leading_whitespace_but_reject_trailing_data);
  RUN_TEST(test_coordinates_reject_non_finite_values_without_updating_outputs);
  RUN_TEST(test_coordinates_accept_exact_bounds_and_exponent_notation);
  RUN_TEST(test_coordinates_reject_out_of_range_without_updating_outputs);
  RUN_TEST(test_released_button_boundaries_are_exact);
  RUN_TEST(test_release_after_missed_active_poll_is_long_hold);
  RUN_TEST(test_active_long_hold_is_rollover_safe_and_one_shot);
  RUN_TEST(
      test_reconnect_first_attempt_follows_disconnect_grace_at_tick_zero);
  RUN_TEST(test_failed_reconnect_retry_is_spaced_from_attempt_completion);
  RUN_TEST(
      test_reconnect_success_resets_state_and_new_disconnect_gets_new_grace);
  RUN_TEST(test_first_adsb_fetch_is_immediate_at_tick_zero);
  RUN_TEST(
      test_later_adsb_fetch_is_spaced_from_fetch_completion_even_after_failure);
  RUN_TEST(test_long_adsb_operation_uses_completion_not_start_for_next_due_time);
  RUN_TEST(test_reconnected_radar_makes_adsb_fetch_immediate_again);
  return UNITY_END();
}
