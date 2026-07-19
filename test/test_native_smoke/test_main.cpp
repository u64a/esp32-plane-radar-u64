#include <unity.h>

#include <cstdint>

#include "../support/captured_output.h"
#include "../support/manual_clock.h"
#include "../support/sequence_rng.h"

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

void setUp() {}
void tearDown() {}

void test_manual_clock_advances_with_uint32_rollover() {
  test_support::ManualClock clock(UINT32_MAX - 1U);

  clock.advance(3U);

  TEST_ASSERT_EQUAL_UINT32(1U, clock.nowMs());
}

void test_sequence_rng_returns_scripted_values() {
  test_support::SequenceRng rng({17U, 23U});

  TEST_ASSERT_EQUAL_UINT32(17U, rng.next());
  TEST_ASSERT_EQUAL_UINT32(23U, rng.next());
  TEST_ASSERT_EQUAL_UINT32(0U, rng.remaining());
}

void test_captured_output_preserves_exact_text() {
  test_support::CapturedOutput output;

  output.write("adsb: ");
  output.writeln("ready");

  TEST_ASSERT_EQUAL_STRING("adsb: ready\n", output.text().c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_manual_clock_advances_with_uint32_rollover);
  RUN_TEST(test_sequence_rng_returns_scripted_values);
  RUN_TEST(test_captured_output_preserves_exact_text);
  return UNITY_END();
}
