#include <unity.h>

#include <cstdint>

#include "core/poll_policy.h"

using core::AdsbPollPolicy;
using core::AdsbPollState;
using core::kDefaultAdsbPollPolicy;
using core::PollOutcome;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

constexpr AdsbPollPolicy P = kDefaultAdsbPollPolicy;

AdsbPollState displayed() {
  AdsbPollState state = {};
  core::adsbRadarDisplayed(&state);
  return state;
}

void complete(AdsbPollState* state, uint32_t at_ms, PollOutcome outcome,
              bool present = false, uint32_t retry_ms = 0) {
  core::adsbFetchCompleted(state, at_ms, P, outcome, present, retry_ms);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_transient_backoff_table_and_cap() {
  // 5,10,20,40,60,60... seconds. Deep streaks saturate without a uint8 wrap.
  TEST_ASSERT_EQUAL_UINT32(5000, core::adsbTransientDelayMs(P, 1));
  TEST_ASSERT_EQUAL_UINT32(10000, core::adsbTransientDelayMs(P, 2));
  TEST_ASSERT_EQUAL_UINT32(20000, core::adsbTransientDelayMs(P, 3));
  TEST_ASSERT_EQUAL_UINT32(40000, core::adsbTransientDelayMs(P, 4));
  TEST_ASSERT_EQUAL_UINT32(60000, core::adsbTransientDelayMs(P, 5));
  TEST_ASSERT_EQUAL_UINT32(60000, core::adsbTransientDelayMs(P, 6));
  TEST_ASSERT_EQUAL_UINT32(60000, core::adsbTransientDelayMs(P, 200));
  TEST_ASSERT_EQUAL_UINT32(60000, core::adsbTransientDelayMs(P, 255));
}

void test_rate_limit_clamp_presence_zero_min_max() {
  // Absent/malformed Retry-After: fixed default regardless of any stale ms.
  TEST_ASSERT_EQUAL_UINT32(60000, core::adsbRateLimitDelayMs(P, false, 0));
  TEST_ASSERT_EQUAL_UINT32(60000, core::adsbRateLimitDelayMs(P, false, 999999));
  // Present: clamp to [5 s, 5 min]; a header of 0 differs from absence.
  TEST_ASSERT_EQUAL_UINT32(5000, core::adsbRateLimitDelayMs(P, true, 0));
  TEST_ASSERT_EQUAL_UINT32(5000, core::adsbRateLimitDelayMs(P, true, 4999));
  TEST_ASSERT_EQUAL_UINT32(5000, core::adsbRateLimitDelayMs(P, true, 5000));
  TEST_ASSERT_EQUAL_UINT32(12000, core::adsbRateLimitDelayMs(P, true, 12000));
  TEST_ASSERT_EQUAL_UINT32(300000, core::adsbRateLimitDelayMs(P, true, 300000));
  TEST_ASSERT_EQUAL_UINT32(300000, core::adsbRateLimitDelayMs(P, true, 300001));
  TEST_ASSERT_EQUAL_UINT32(300000,
                           core::adsbRateLimitDelayMs(P, true, 0xFFFFFFFFU));
}

void test_success_schedules_three_seconds_and_resets_streak() {
  AdsbPollState s = displayed();
  complete(&s, 0, PollOutcome::Transient);  // dirty the streak first
  complete(&s, 1000, PollOutcome::Success);
  TEST_ASSERT_EQUAL_UINT8(0, s.transient_streak);
  TEST_ASSERT_EQUAL_UINT32(3000, s.next_interval_ms);
  TEST_ASSERT_FALSE(core::adsbFetchDue(s, 3999));
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, 4000));  // completion(1000)+3000
}

void test_transient_streak_advances_and_only_success_resets() {
  AdsbPollState s = displayed();
  complete(&s, 0, PollOutcome::Transient);
  TEST_ASSERT_EQUAL_UINT8(1, s.transient_streak);
  TEST_ASSERT_EQUAL_UINT32(5000, s.next_interval_ms);
  complete(&s, 0, PollOutcome::Transient);
  complete(&s, 0, PollOutcome::Transient);
  complete(&s, 0, PollOutcome::Transient);
  TEST_ASSERT_EQUAL_UINT8(4, s.transient_streak);
  TEST_ASSERT_EQUAL_UINT32(40000, s.next_interval_ms);
  complete(&s, 0, PollOutcome::Transient);  // streak 5 -> cap
  TEST_ASSERT_EQUAL_UINT32(60000, s.next_interval_ms);
  complete(&s, 0, PollOutcome::Transient);  // streak 6 -> still cap
  TEST_ASSERT_EQUAL_UINT32(60000, s.next_interval_ms);
  complete(&s, 0, PollOutcome::Success);
  TEST_ASSERT_EQUAL_UINT8(0, s.transient_streak);
}

void test_mixed_classes_do_not_touch_the_transient_streak() {
  AdsbPollState s = displayed();
  complete(&s, 0, PollOutcome::Transient);  // streak 1
  complete(&s, 0, PollOutcome::RateLimited);
  TEST_ASSERT_EQUAL_UINT8(1, s.transient_streak);
  TEST_ASSERT_EQUAL_UINT32(60000, s.next_interval_ms);  // 429 default
  complete(&s, 0, PollOutcome::Permanent);
  TEST_ASSERT_EQUAL_UINT8(1, s.transient_streak);
  TEST_ASSERT_EQUAL_UINT32(300000, s.next_interval_ms);
  complete(&s, 0, PollOutcome::Obsolete);
  TEST_ASSERT_EQUAL_UINT8(1, s.transient_streak);
  TEST_ASSERT_EQUAL_UINT32(3000, s.next_interval_ms);
  // The next transient resumes the shared streak at 2 (-> 10 s), not from 1.
  complete(&s, 0, PollOutcome::Transient);
  TEST_ASSERT_EQUAL_UINT8(2, s.transient_streak);
  TEST_ASSERT_EQUAL_UINT32(10000, s.next_interval_ms);
}

void test_rate_limited_completion_applies_retry_after_rules() {
  AdsbPollState s = displayed();
  complete(&s, 0, PollOutcome::RateLimited, true, 12000);
  TEST_ASSERT_EQUAL_UINT32(12000, s.next_interval_ms);
  complete(&s, 0, PollOutcome::RateLimited, true, 0);  // present 0 -> min
  TEST_ASSERT_EQUAL_UINT32(5000, s.next_interval_ms);
  complete(&s, 0, PollOutcome::RateLimited, false, 0);  // absent -> default
  TEST_ASSERT_EQUAL_UINT32(60000, s.next_interval_ms);
  complete(&s, 0, PollOutcome::RateLimited, true, 999999);  // over max
  TEST_ASSERT_EQUAL_UINT32(300000, s.next_interval_ms);
}

void test_permanent_waits_five_minutes() {
  AdsbPollState s = displayed();
  complete(&s, 1000, PollOutcome::Permanent);
  TEST_ASSERT_EQUAL_UINT32(300000, s.next_interval_ms);
  TEST_ASSERT_FALSE(core::adsbFetchDue(s, 300999));
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, 301000));
}

void test_next_due_is_completion_relative_for_long_requests() {
  AdsbPollState s = displayed();
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, 100));  // first fetch is immediate
  complete(&s, 20100, PollOutcome::Success);     // long op completes at 20100
  TEST_ASSERT_FALSE(core::adsbFetchDue(s, 23099));
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, 23100));  // 20100 + 3000, not start
}

void test_first_and_reconnect_force_immediate_preserving_streak() {
  AdsbPollState s = {};
  core::adsbRadarDisplayed(&s);
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, 0));  // first fetch immediate

  complete(&s, 0, PollOutcome::Transient);
  complete(&s, 0, PollOutcome::Transient);
  TEST_ASSERT_EQUAL_UINT8(2, s.transient_streak);

  core::adsbRadarHidden(&s);  // Wi-Fi drop pauses polling
  TEST_ASSERT_EQUAL_UINT8(2, s.transient_streak);  // streak survives the pause
  TEST_ASSERT_FALSE(core::adsbFetchDue(s, 500000));

  core::adsbRadarDisplayed(&s);  // reconnect
  TEST_ASSERT_EQUAL_UINT8(2, s.transient_streak);   // still not reset
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, 500000));  // forced immediate
}

void test_settings_change_forces_immediate_preserving_streak() {
  AdsbPollState s = displayed();
  complete(&s, 1000, PollOutcome::Transient);  // streak 1, next in 5 s
  TEST_ASSERT_FALSE(core::adsbFetchDue(s, 2000));
  core::adsbSettingsChanged(&s);
  TEST_ASSERT_EQUAL_UINT8(1, s.transient_streak);  // preserved
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, 2000));   // immediate now
  complete(&s, 2000, PollOutcome::Success);        // the forced fetch succeeds
  TEST_ASSERT_EQUAL_UINT8(0, s.transient_streak);
}

void test_wifi_pause_is_not_a_fetch_failure() {
  AdsbPollState s = displayed();
  core::adsbRadarHidden(&s);
  TEST_ASSERT_FALSE(core::adsbFetchDue(s, 0));
  TEST_ASSERT_FALSE(core::adsbFetchDue(s, 1000000));
  // Completing while hidden is ignored: no streak change, no completion latched.
  complete(&s, 5000, PollOutcome::Transient);
  TEST_ASSERT_FALSE(s.has_fetch_completion);
  TEST_ASSERT_EQUAL_UINT8(0, s.transient_streak);
}

void test_outcome_aware_due_is_rollover_safe() {
  AdsbPollState s = displayed();
  const uint32_t completed = UINT32_MAX - 1000U;  // 1000 ms before the wrap
  complete(&s, completed, PollOutcome::Success);  // 3 s interval
  TEST_ASSERT_FALSE(
      core::adsbFetchDue(s, static_cast<uint32_t>(completed + 2999U)));
  TEST_ASSERT_TRUE(
      core::adsbFetchDue(s, static_cast<uint32_t>(completed + 3000U)));
}

void test_pending_immediate_survives_obsolete_completion_in_flight() {
  // The exact blocker sequence: start a request, force an immediate via a
  // settings change while it is in flight, then complete the older request as
  // Obsolete. The immediate must be due at the same completion time -- the stale
  // completion must not clear the newer latch.
  AdsbPollState s = displayed();
  core::adsbFetchStarted(&s);
  complete(&s, 1000, PollOutcome::Success);  // consumes the initial immediate
  TEST_ASSERT_FALSE(core::adsbFetchDue(s, 1000));

  core::adsbFetchStarted(&s);                 // a request is now in flight
  core::adsbSettingsChanged(&s);              // settings change mid-flight
  complete(&s, 5000, PollOutcome::Obsolete);  // the older request completes
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, 5000));           // immediate at completion
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, 5000U + 999999U));  // still immediate
  TEST_ASSERT_EQUAL_UINT8(0, s.transient_streak);          // obsolete leaves streak

  // Servicing the new immediate returns to the normal cadence -- a started
  // completion with no newer request does not stay permanently immediate.
  core::adsbFetchStarted(&s);
  complete(&s, 6000, PollOutcome::Success);
  TEST_ASSERT_FALSE(core::adsbFetchDue(s, 6000));
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, 9000));  // 6000 + 3000
}

void test_pending_immediate_survives_failure_completion_in_flight() {
  // Failure-completion variant: the older in-flight request fails (Transient).
  // The immediate raised mid-flight survives while the failure is still recorded.
  AdsbPollState s = displayed();
  core::adsbFetchStarted(&s);
  complete(&s, 1000, PollOutcome::Success);  // consume the initial immediate

  core::adsbFetchStarted(&s);                  // request in flight
  core::adsbSettingsChanged(&s);               // settings change mid-flight
  complete(&s, 5000, PollOutcome::Transient);  // the older request fails
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, 5000));    // immediate survives the failure
  TEST_ASSERT_EQUAL_UINT8(1, s.transient_streak);   // failure still recorded
  TEST_ASSERT_EQUAL_UINT32(5000, s.next_interval_ms);
}

void test_pending_immediate_from_reconnect_survives_in_flight_completion() {
  // Reconnect variant: the immediate is forced by re-showing the radar while a
  // request is in flight; the older completion must not clear it either.
  AdsbPollState s = displayed();
  core::adsbFetchStarted(&s);
  complete(&s, 1000, PollOutcome::Success);

  core::adsbFetchStarted(&s);                 // request in flight
  core::adsbRadarDisplayed(&s);               // reconnect re-shows radar mid-flight
  complete(&s, 5000, PollOutcome::Obsolete);
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, 5000));
}

void test_pending_immediate_survives_completion_across_rollover() {
  // Rollover variant: the older completion time wraps past UINT32_MAX.
  AdsbPollState s = displayed();
  core::adsbFetchStarted(&s);
  const uint32_t t0 = UINT32_MAX - 500U;
  complete(&s, t0, PollOutcome::Success);  // consume the initial immediate
  TEST_ASSERT_FALSE(core::adsbFetchDue(s, t0));

  core::adsbFetchStarted(&s);
  core::adsbSettingsChanged(&s);
  const uint32_t t1 = static_cast<uint32_t>(t0 + 1000U);  // wraps past UINT32_MAX
  complete(&s, t1, PollOutcome::Obsolete);
  TEST_ASSERT_TRUE(core::adsbFetchDue(s, t1));  // immediate regardless of the wrap
}

void test_started_ordinary_completions_do_not_stay_immediate() {
  // Without a mid-flight forced immediate, repeated started completions must
  // clear the latch every time (no permanently-immediate regression).
  AdsbPollState s = displayed();
  for (uint32_t i = 1; i <= 3; ++i) {
    core::adsbFetchStarted(&s);
    complete(&s, 1000U * i, PollOutcome::Success);
    TEST_ASSERT_FALSE(core::adsbFetchDue(s, 1000U * i));
    TEST_ASSERT_TRUE(core::adsbFetchDue(s, 1000U * i + 3000U));
  }
}

void test_effective_outcome_downgrades_only_transient_when_disconnected() {
  using core::effectiveOutcomeAtCompletion;
  // Wi-Fi down: a network-aborted (Transient) attempt becomes Obsolete (pause).
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Obsolete),
      static_cast<int>(
          effectiveOutcomeAtCompletion(PollOutcome::Transient, false)));
  // Every other outcome passes through unchanged even while disconnected.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Success),
      static_cast<int>(
          effectiveOutcomeAtCompletion(PollOutcome::Success, false)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::RateLimited),
      static_cast<int>(
          effectiveOutcomeAtCompletion(PollOutcome::RateLimited, false)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Permanent),
      static_cast<int>(
          effectiveOutcomeAtCompletion(PollOutcome::Permanent, false)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Obsolete),
      static_cast<int>(
          effectiveOutcomeAtCompletion(PollOutcome::Obsolete, false)));
  // Connected: everything passes through unchanged, including Transient.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Transient),
      static_cast<int>(
          effectiveOutcomeAtCompletion(PollOutcome::Transient, true)));
}

void test_wifi_down_transient_does_not_increment_streak() {
  // End-to-end of the runtime rule: a Transient completion while Wi-Fi is down
  // routes through effectiveOutcomeAtCompletion() and must not touch the streak.
  AdsbPollState s = displayed();
  complete(&s, 1000, PollOutcome::Transient);  // one genuine failure first
  TEST_ASSERT_EQUAL_UINT8(1, s.transient_streak);
  const PollOutcome effective =
      core::effectiveOutcomeAtCompletion(PollOutcome::Transient, false);
  complete(&s, 2000, effective);  // network-aborted while disconnected
  TEST_ASSERT_EQUAL_UINT8(1, s.transient_streak);  // unchanged: not a failure
  TEST_ASSERT_EQUAL_UINT32(3000, s.next_interval_ms);  // Obsolete -> success cadence
}

void test_disconnect_seq_change_detection_including_wrap() {
  using core::disconnectSeqChanged;
  // Equal captures: no ARDUINO_EVENT_WIFI_STA_DISCONNECTED fired -> no flap.
  TEST_ASSERT_FALSE(disconnectSeqChanged(0U, 0U));
  TEST_ASSERT_FALSE(disconnectSeqChanged(7U, 7U));
  TEST_ASSERT_FALSE(disconnectSeqChanged(UINT32_MAX, UINT32_MAX));
  // Any advance is a flap.
  TEST_ASSERT_TRUE(disconnectSeqChanged(0U, 1U));
  TEST_ASSERT_TRUE(disconnectSeqChanged(7U, 9U));
  // uint32 wrap: 0xFFFFFFFF incremented once wraps to 0, still detected as a
  // change (a plain inequality is inherently wrap-safe).
  TEST_ASSERT_TRUE(disconnectSeqChanged(UINT32_MAX, 0U));
  TEST_ASSERT_TRUE(disconnectSeqChanged(UINT32_MAX - 1U, 2U));  // several across wrap
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_transient_backoff_table_and_cap);
  RUN_TEST(test_rate_limit_clamp_presence_zero_min_max);
  RUN_TEST(test_success_schedules_three_seconds_and_resets_streak);
  RUN_TEST(test_transient_streak_advances_and_only_success_resets);
  RUN_TEST(test_mixed_classes_do_not_touch_the_transient_streak);
  RUN_TEST(test_rate_limited_completion_applies_retry_after_rules);
  RUN_TEST(test_permanent_waits_five_minutes);
  RUN_TEST(test_next_due_is_completion_relative_for_long_requests);
  RUN_TEST(test_first_and_reconnect_force_immediate_preserving_streak);
  RUN_TEST(test_settings_change_forces_immediate_preserving_streak);
  RUN_TEST(test_wifi_pause_is_not_a_fetch_failure);
  RUN_TEST(test_outcome_aware_due_is_rollover_safe);
  RUN_TEST(test_pending_immediate_survives_obsolete_completion_in_flight);
  RUN_TEST(test_pending_immediate_survives_failure_completion_in_flight);
  RUN_TEST(test_pending_immediate_from_reconnect_survives_in_flight_completion);
  RUN_TEST(test_pending_immediate_survives_completion_across_rollover);
  RUN_TEST(test_started_ordinary_completions_do_not_stay_immediate);
  RUN_TEST(test_effective_outcome_downgrades_only_transient_when_disconnected);
  RUN_TEST(test_wifi_down_transient_does_not_increment_streak);
  RUN_TEST(test_disconnect_seq_change_detection_including_wrap);
  return UNITY_END();
}
