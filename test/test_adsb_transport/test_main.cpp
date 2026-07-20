#include <unity.h>

#include "core/connect_budget.h"
#include "services/adsb_transport.h"

using namespace services::adsb;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

// Value of MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY (-0x7880) in the pinned
// Arduino-ESP32 2.0.14 mbedTLS. Native tests cannot include <mbedtls/ssl.h>, so
// the seam takes the sentinel as an argument and the ESP adapter static_asserts
// it stays negative; using the concrete value keeps these tests self-checking.
static constexpr int kCloseNotify = -30848;
// A different negative mbedTLS receive code (stands in for e.g. a fatal alert or
// a NET_RECV failure). Any negative that is not the close_notify sentinel must
// classify as Error, never a clean EOF.
static constexpr int kOtherTlsError = -30592;

void setUp() {}
void tearDown() {}

void test_positive_available_reads_regardless_of_connected() {
  // available > 0: buffered bytes are ready and connected() is irrelevant.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(TlsReadAction::Read),
      static_cast<int>(classifyTlsAvailable(1, true, kCloseNotify)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(TlsReadAction::Read),
      static_cast<int>(classifyTlsAvailable(512, false, kCloseNotify)));
}

void test_zero_available_connected_would_block() {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(TlsReadAction::WouldBlock),
      static_cast<int>(classifyTlsAvailable(0, true, kCloseNotify)));
}

void test_zero_available_disconnected_is_end() {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(TlsReadAction::End),
      static_cast<int>(classifyTlsAvailable(0, false, kCloseNotify)));
}

// Regression for the Arduino-ESP32 2.0.14 quirk: a clean TLS shutdown surfaces
// MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY through available() and has already
// stop()ped the client (so connected() is false). It must classify as a clean
// End so a close-delimited body publishes its complete snapshot instead of
// failing as a truncated receive error.
void test_close_notify_available_is_clean_end() {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(TlsReadAction::End),
      static_cast<int>(classifyTlsAvailable(kCloseNotify, false, kCloseNotify)));
  // Even in the (unexpected) case the client still reports connected(), the
  // exact sentinel is still a clean End.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(TlsReadAction::End),
      static_cast<int>(classifyTlsAvailable(kCloseNotify, true, kCloseNotify)));
}

// Any other negative available() is a genuine receive failure, classified as
// Error strictly before connected() is consulted, even though the client has
// also already stop()ped itself (connected() == false).
void test_other_negative_available_is_error() {
  const int errors[] = {-1, -76, -128, kOtherTlsError};
  for (int e : errors) {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(TlsReadAction::Error),
        static_cast<int>(classifyTlsAvailable(e, false, kCloseNotify)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(TlsReadAction::Error),
        static_cast<int>(classifyTlsAvailable(e, true, kCloseNotify)));
  }
}

void test_read_result_classification() {
  // Positive read: bytes produced.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ReadStatus::Data),
      static_cast<int>(classifyTlsReadResult(3, kCloseNotify)));
  // Empty read: retry as WouldBlock.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ReadStatus::WouldBlock),
      static_cast<int>(classifyTlsReadResult(0, kCloseNotify)));
  // A negative read equal to the close_notify sentinel is the same clean End...
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ReadStatus::End),
      static_cast<int>(classifyTlsReadResult(kCloseNotify, kCloseNotify)));
  // ...every other negative read is a transport Error.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ReadStatus::Error),
      static_cast<int>(classifyTlsReadResult(-1, kCloseNotify)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ReadStatus::Error),
      static_cast<int>(classifyTlsReadResult(kOtherTlsError, kCloseNotify)));
}

// --- Post-DNS connect-budget partition -------------------------------------
//
// splitConnectBudget must divide the remaining millisecond budget into two
// non-overlapping whole-second slices whose combined ceiling never exceeds the
// remainder, favoring TLS, with a 1 s floor on each nonzero slice and no viable
// partition below 2 whole seconds.

// Assert the documented invariants of a viable split against remaining_ms.
static void assertViableSplit(uint32_t remaining_ms) {
  const core::ConnectBudgetSplit s = core::splitConnectBudget(remaining_ms);
  TEST_ASSERT_TRUE(s.viable);
  // Each nonzero slice is at least a whole second (never collapses to 0 s).
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1U, s.tcp_seconds);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1U, s.tls_seconds);
  // TLS is favored: it is never the smaller slice.
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(s.tcp_seconds, s.tls_seconds);
  // The two slices are non-overlapping and their sum fits the remaining budget.
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(remaining_ms,
                                   (s.tcp_seconds + s.tls_seconds) * 1000U);
}

void test_split_zero_budget_not_viable() {
  const core::ConnectBudgetSplit s = core::splitConnectBudget(0U);
  TEST_ASSERT_FALSE(s.viable);
  TEST_ASSERT_EQUAL_UINT32(0U, s.tcp_seconds);
  TEST_ASSERT_EQUAL_UINT32(0U, s.tls_seconds);
}

void test_split_below_two_seconds_not_viable() {
  // Anything under two whole seconds cannot yield two distinct >= 1 s slices.
  const uint32_t under_two[] = {1U, 500U, 999U, 1000U, 1500U, 1999U};
  for (uint32_t ms : under_two) {
    TEST_ASSERT_FALSE(core::splitConnectBudget(ms).viable);
  }
}

void test_split_exactly_two_seconds() {
  const core::ConnectBudgetSplit s = core::splitConnectBudget(2000U);
  TEST_ASSERT_TRUE(s.viable);
  // The only split for 2 whole seconds is 1 s + 1 s.
  TEST_ASSERT_EQUAL_UINT32(1U, s.tcp_seconds);
  TEST_ASSERT_EQUAL_UINT32(1U, s.tls_seconds);
  assertViableSplit(2000U);
}

void test_split_odd_whole_seconds_favor_tls() {
  // 5 s -> 40% floor = 2 s TCP, 3 s TLS.
  const core::ConnectBudgetSplit s = core::splitConnectBudget(5000U);
  TEST_ASSERT_EQUAL_UINT32(2U, s.tcp_seconds);
  TEST_ASSERT_EQUAL_UINT32(3U, s.tls_seconds);
  assertViableSplit(5000U);
}

void test_split_even_whole_seconds_favor_tls() {
  // 4 s -> 40% floor = 1 s TCP, 3 s TLS.
  const core::ConnectBudgetSplit s = core::splitConnectBudget(4000U);
  TEST_ASSERT_EQUAL_UINT32(1U, s.tcp_seconds);
  TEST_ASSERT_EQUAL_UINT32(3U, s.tls_seconds);
  assertViableSplit(4000U);
}

void test_split_eight_seconds_default_budget() {
  // 8 s (the kAdsbConnectTimeoutMs default with instant DNS) -> 3 s TCP, 5 s TLS.
  const core::ConnectBudgetSplit s = core::splitConnectBudget(8000U);
  TEST_ASSERT_EQUAL_UINT32(3U, s.tcp_seconds);
  TEST_ASSERT_EQUAL_UINT32(5U, s.tls_seconds);
  assertViableSplit(8000U);
}

void test_split_fractional_remainder_floors_and_fits() {
  // A sub-second remainder is dropped (floored), never rounded up, so the sum
  // stays strictly within the remaining budget.
  const core::ConnectBudgetSplit a = core::splitConnectBudget(2500U);
  TEST_ASSERT_EQUAL_UINT32(1U, a.tcp_seconds);
  TEST_ASSERT_EQUAL_UINT32(1U, a.tls_seconds);
  assertViableSplit(2500U);  // 2000 ms <= 2500 ms
  const core::ConnectBudgetSplit b = core::splitConnectBudget(8999U);
  TEST_ASSERT_EQUAL_UINT32(3U, b.tcp_seconds);
  TEST_ASSERT_EQUAL_UINT32(5U, b.tls_seconds);
  assertViableSplit(8999U);  // 8000 ms <= 8999 ms
}

void test_split_invariants_hold_across_range() {
  // Sweep every millisecond value from 2 s to ~30 s and assert the invariants.
  for (uint32_t ms = 2000U; ms <= 30000U; ms += 137U) {
    assertViableSplit(ms);
  }
}

// --- Completion classification (late-success rejection) --------------------
//
// classifyConnectCompletion is the pure model behind espTlsConnect's post-
// connect() decision: it maps (success, budget-exhausted) to the truthful
// outcome. A success reported only after the absolute budget is spent is a late
// success and must be rejected as Timeout, not accepted as Connected.

void test_completion_success_within_budget_is_connected() {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::ConnectCompletion::Connected),
      static_cast<int>(core::classifyConnectCompletion(true, false)));
}

void test_completion_late_success_is_rejected_as_timeout() {
  // connect() returned success, but the absolute budget was already exhausted:
  // honoring it would exceed the advertised ceiling, so it is a Timeout.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::ConnectCompletion::Timeout),
      static_cast<int>(core::classifyConnectCompletion(true, true)));
}

void test_completion_failure_that_exhausts_budget_is_timeout() {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::ConnectCompletion::Timeout),
      static_cast<int>(core::classifyConnectCompletion(false, true)));
}

void test_completion_early_failure_is_tls_failure() {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::ConnectCompletion::TlsFailure),
      static_cast<int>(core::classifyConnectCompletion(false, false)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_positive_available_reads_regardless_of_connected);
  RUN_TEST(test_zero_available_connected_would_block);
  RUN_TEST(test_zero_available_disconnected_is_end);
  RUN_TEST(test_close_notify_available_is_clean_end);
  RUN_TEST(test_other_negative_available_is_error);
  RUN_TEST(test_read_result_classification);
  RUN_TEST(test_split_zero_budget_not_viable);
  RUN_TEST(test_split_below_two_seconds_not_viable);
  RUN_TEST(test_split_exactly_two_seconds);
  RUN_TEST(test_split_odd_whole_seconds_favor_tls);
  RUN_TEST(test_split_even_whole_seconds_favor_tls);
  RUN_TEST(test_split_eight_seconds_default_budget);
  RUN_TEST(test_split_fractional_remainder_floors_and_fits);
  RUN_TEST(test_split_invariants_hold_across_range);
  RUN_TEST(test_completion_success_within_budget_is_connected);
  RUN_TEST(test_completion_late_success_is_rejected_as_timeout);
  RUN_TEST(test_completion_failure_that_exhausts_budget_is_timeout);
  RUN_TEST(test_completion_early_failure_is_tls_failure);
  return UNITY_END();
}
