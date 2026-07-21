#include <unity.h>

#include "core/factory_erase.h"

// Truthful-aggregation tests for core::factoryEraseAllCleared: the device may
// claim a full wipe ONLY when every subsystem verifiably cleared; any single
// failure must report incomplete so a partial erase is never presented as clean.

void setUp() {}
void tearDown() {}

void test_all_cleared_reports_true() {
  const core::FactoryEraseOutcome o{true, true, true, true};
  TEST_ASSERT_TRUE(core::factoryEraseAllCleared(o));
}

void test_any_single_failure_reports_false() {
  TEST_ASSERT_FALSE(
      core::factoryEraseAllCleared({false, true, true, true}));   // STA
  TEST_ASSERT_FALSE(
      core::factoryEraseAllCleared({true, false, true, true}));   // location
  TEST_ASSERT_FALSE(
      core::factoryEraseAllCleared({true, true, false, true}));   // radar
  TEST_ASSERT_FALSE(
      core::factoryEraseAllCleared({true, true, true, false}));   // time-floor
}

void test_all_failed_reports_false() {
  const core::FactoryEraseOutcome o{false, false, false, false};
  TEST_ASSERT_FALSE(core::factoryEraseAllCleared(o));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_all_cleared_reports_true);
  RUN_TEST(test_any_single_failure_reports_false);
  RUN_TEST(test_all_failed_reports_false);
  return UNITY_END();
}
