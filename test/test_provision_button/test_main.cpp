#include <unity.h>

#include <cstdint>

#include "core/provision_button.h"

using core::kDefaultProvisionButtonPolicy;
using core::ProvisionButton;
using core::ProvisionButtonEvent;
using core::ProvisionButtonOutput;
using core::ProvisionButtonPrompt;
using core::ProvisionButtonState;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

int EV(ProvisionButtonEvent e) { return static_cast<int>(e); }
int PR(ProvisionButtonPrompt p) { return static_cast<int>(p); }
int ST(ProvisionButtonState s) { return static_cast<int>(s); }

ProvisionButtonOutput step(ProvisionButton* b, uint32_t t, bool down) {
  return provisionButtonUpdate(b, kDefaultProvisionButtonPolicy, t, down);
}

// Press at t=0 then release after `held` ms; return the release event.
ProvisionButtonEvent pressRelease(uint32_t held) {
  ProvisionButton b;
  provisionButtonInit(&b);
  step(&b, 0, true);
  return step(&b, held, false).event;
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- tap / dead band / configure boundaries ---------------------------------

void test_tap_window_boundaries() {
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::None), EV(pressRelease(39)));
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::Tap), EV(pressRelease(40)));
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::Tap), EV(pressRelease(999)));
  // 1000 is the exclusive top of the tap window -> dead band, no event.
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::None), EV(pressRelease(1000)));
}

void test_dead_band_boundaries() {
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::None), EV(pressRelease(1000)));
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::None), EV(pressRelease(1999)));
}

void test_configure_window_boundaries() {
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::ConfigureRequest),
                        EV(pressRelease(2000)));
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::ConfigureRequest),
                        EV(pressRelease(7999)));
  // 8000 is the arm threshold: a release there is NOT a configure request.
  TEST_ASSERT_NOT_EQUAL(EV(ProvisionButtonEvent::ConfigureRequest),
                        EV(pressRelease(8000)));
}

void test_configure_prompt_shown_while_held() {
  ProvisionButton b;
  provisionButtonInit(&b);
  step(&b, 0, true);
  TEST_ASSERT_EQUAL_INT(PR(ProvisionButtonPrompt::None),
                        PR(step(&b, 1500, true).prompt));  // dead band
  TEST_ASSERT_EQUAL_INT(PR(ProvisionButtonPrompt::ReleaseToConfigure),
                        PR(step(&b, 2000, true).prompt));
  TEST_ASSERT_EQUAL_INT(PR(ProvisionButtonPrompt::ReleaseToConfigure),
                        PR(step(&b, 7000, true).prompt));
}

// --- arming -----------------------------------------------------------------

void test_hold_arms_erase_and_prompts() {
  ProvisionButton b;
  provisionButtonInit(&b);
  step(&b, 0, true);
  ProvisionButtonOutput o = step(&b, 8000, true);  // reach arm threshold, held
  TEST_ASSERT_EQUAL_INT(ST(ProvisionButtonState::EraseArmed), ST(b.state));
  TEST_ASSERT_EQUAL_INT(PR(ProvisionButtonPrompt::EraseArmedRelease),
                        PR(o.prompt));
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::None), EV(o.event));
}

void test_same_hold_never_erases_stuck_button() {
  ProvisionButton b;
  provisionButtonInit(&b);
  step(&b, 0, true);
  // Hold continuously far past every threshold: it must stay armed, never erase.
  const uint32_t times[] = {8000, 12000, 30000, 120000, 600000, 3000000};
  for (uint32_t t : times) {
    ProvisionButtonOutput o = step(&b, t, true);
    TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::None), EV(o.event));
    TEST_ASSERT_EQUAL_INT(ST(ProvisionButtonState::EraseArmed), ST(b.state));
  }
}

// --- full erase confirmation ------------------------------------------------

void driveToConfirmWait(ProvisionButton* b, uint32_t press_ms, uint32_t arm_ms,
                        uint32_t release_ms) {
  provisionButtonInit(b);
  step(b, press_ms, true);
  step(b, arm_ms, true);       // arm (still held)
  step(b, release_ms, false);  // release -> ConfirmWait, window opens here
}

void test_full_erase_confirm() {
  ProvisionButton b;
  driveToConfirmWait(&b, 0, 8000, 9000);
  TEST_ASSERT_EQUAL_INT(ST(ProvisionButtonState::ConfirmWait), ST(b.state));
  TEST_ASSERT_EQUAL_INT(PR(ProvisionButtonPrompt::ConfirmHoldToErase),
                        PR(step(&b, 9500, false).prompt));
  step(&b, 10000, true);  // second press starts (1000ms into the window)
  TEST_ASSERT_EQUAL_INT(PR(ProvisionButtonPrompt::KeepHoldingToErase),
                        PR(step(&b, 12000, true).prompt));  // held2 = 2000
  ProvisionButtonOutput o = step(&b, 13000, true);          // held2 = 3000
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::EraseConfirmed), EV(o.event));
  TEST_ASSERT_EQUAL_INT(ST(ProvisionButtonState::Idle), ST(b.state));
}

void test_confirm_hold_boundary() {
  ProvisionButton b;
  driveToConfirmWait(&b, 0, 8000, 9000);
  step(&b, 10000, true);  // second press starts
  // 2999 ms held: not yet confirmed.
  ProvisionButtonOutput o = step(&b, 12999, true);
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::None), EV(o.event));
  // Exactly 3000 ms held: confirmed.
  o = step(&b, 13000, true);
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::EraseConfirmed), EV(o.event));
}

void test_early_second_release_cancels() {
  ProvisionButton b;
  driveToConfirmWait(&b, 0, 8000, 9000);
  step(&b, 10000, true);                            // second press starts
  ProvisionButtonOutput o = step(&b, 12500, false);  // held2 = 2500 < 3000
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::EraseCancelled), EV(o.event));
  TEST_ASSERT_EQUAL_INT(PR(ProvisionButtonPrompt::Cancelled), PR(o.prompt));
  TEST_ASSERT_EQUAL_INT(ST(ProvisionButtonState::Idle), ST(b.state));
}

void test_confirm_window_timeout_boundary() {
  ProvisionButton b;
  driveToConfirmWait(&b, 0, 8000, 9000);  // window opens at 9000
  // 9999 ms into the window: still waiting, no event.
  ProvisionButtonOutput o = step(&b, 18999, false);
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::None), EV(o.event));
  TEST_ASSERT_EQUAL_INT(ST(ProvisionButtonState::ConfirmWait), ST(b.state));
  // Exactly 10000 ms: timed out.
  o = step(&b, 19000, false);
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::EraseCancelled), EV(o.event));
  TEST_ASSERT_EQUAL_INT(PR(ProvisionButtonPrompt::Cancelled), PR(o.prompt));
}

void test_second_press_at_window_edge_then_hold_confirms() {
  ProvisionButton b;
  driveToConfirmWait(&b, 0, 8000, 9000);  // window opens at 9000
  // Start the second press 9999 ms in (just inside the window)...
  step(&b, 18999, true);
  // ...and hold >= 3 s; completion may cross the original window end (19000).
  ProvisionButtonOutput o = step(&b, 21999, true);
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::EraseConfirmed), EV(o.event));
}

void test_no_second_press_exactly_at_deadline_times_out() {
  ProvisionButton b;
  driveToConfirmWait(&b, 0, 8000, 9000);
  // A press arriving exactly at the deadline loses to the timeout.
  ProvisionButtonOutput o = step(&b, 19000, true);
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::EraseCancelled), EV(o.event));
}

// --- rollover ---------------------------------------------------------------

void test_tap_across_millis_rollover() {
  ProvisionButton b;
  provisionButtonInit(&b);
  const uint32_t base = UINT32_MAX - 20U;
  step(&b, base, true);
  ProvisionButtonOutput o = step(&b, base + 40U, false);  // wraps; held = 40
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::Tap), EV(o.event));
}

void test_erase_confirm_across_rollover() {
  ProvisionButton b;
  provisionButtonInit(&b);
  const uint32_t base = UINT32_MAX - 5000U;
  step(&b, base, true);
  step(&b, base + 8000U, true);        // arm (wraps)
  step(&b, base + 9000U, false);       // ConfirmWait
  step(&b, base + 10000U, true);       // second press
  ProvisionButtonOutput o = step(&b, base + 13000U, true);  // held2 = 3000
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::EraseConfirmed), EV(o.event));
}

// --- reuse ------------------------------------------------------------------

void test_tap_then_configure_reuses_machine() {
  ProvisionButton b;
  provisionButtonInit(&b);
  step(&b, 0, true);
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::Tap),
                        EV(step(&b, 100, false).event));
  // A fresh gesture after returning to Idle is classified independently.
  step(&b, 200, true);
  TEST_ASSERT_EQUAL_INT(EV(ProvisionButtonEvent::ConfigureRequest),
                        EV(step(&b, 200 + 3000, false).event));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_tap_window_boundaries);
  RUN_TEST(test_dead_band_boundaries);
  RUN_TEST(test_configure_window_boundaries);
  RUN_TEST(test_configure_prompt_shown_while_held);
  RUN_TEST(test_hold_arms_erase_and_prompts);
  RUN_TEST(test_same_hold_never_erases_stuck_button);
  RUN_TEST(test_full_erase_confirm);
  RUN_TEST(test_confirm_hold_boundary);
  RUN_TEST(test_early_second_release_cancels);
  RUN_TEST(test_confirm_window_timeout_boundary);
  RUN_TEST(test_second_press_at_window_edge_then_hold_confirms);
  RUN_TEST(test_no_second_press_exactly_at_deadline_times_out);
  RUN_TEST(test_tap_across_millis_rollover);
  RUN_TEST(test_erase_confirm_across_rollover);
  RUN_TEST(test_tap_then_configure_reuses_machine);
  return UNITY_END();
}
