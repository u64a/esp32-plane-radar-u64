#include <unity.h>

#include <cstdint>
#include <type_traits>

#include "core/time_math.h"
#include "runtime_diagnostics.h"
#include "services/adsb_worker.h"

// Compile-time / runtime contract check for the Phase 10 diagnostics and logging
// headers under the DEFAULT (diagnostics-OFF) configuration.
//
// This suite verifies, without any FreeRTOS/Arduino harness:
//   * PLANE_RADAR_DIAGNOSTICS defaults to 0 when nothing defines it.
//   * PLANE_RADAR_LOG_LEVEL defaults to 2 (INFO) when nothing defines it.
//   * The symbolic level constants (OFF=0, ERROR=1, INFO=2) are correct.
//   * constexpr reflection (kLogLevel, kDiagnosticsEnabled) is usable in
//     constant expressions and returns the expected values.
//   * The existing WorkerResult is trivially copyable without the conditional
//     fetch_duration_ms field (DIAGNOSTICS=0 layout).
//   * core::elapsedMicros is present and computes rollover-safe elapsed time.
//
// These checks run under [env:native] (PLANE_RADAR_DIAGNOSTICS not defined →
// defaults to 0). The complementary suite test_runtime_diagnostics_on runs
// under [env:native-diag] (PLANE_RADAR_DIAGNOSTICS=1) to verify the ON state.

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

// ---- defaults ---------------------------------------------------------------

// The header must default PLANE_RADAR_DIAGNOSTICS to 0 when not defined.
static_assert(PLANE_RADAR_DIAGNOSTICS == 0,
              "runtime_diagnostics.h must default PLANE_RADAR_DIAGNOSTICS to 0");

// The header must default PLANE_RADAR_LOG_LEVEL to 2 (INFO) when not defined.
static_assert(PLANE_RADAR_LOG_LEVEL == 2,
              "runtime_diagnostics.h must default PLANE_RADAR_LOG_LEVEL to 2 (INFO)");

// ---- constexpr reflection ---------------------------------------------------

static_assert(plane_radar::kLogLevelOff   == 0, "kLogLevelOff must be 0");
static_assert(plane_radar::kLogLevelError == 1, "kLogLevelError must be 1");
static_assert(plane_radar::kLogLevelInfo  == 2, "kLogLevelInfo must be 2");

static_assert(plane_radar::kLogLevel == 2,
              "kLogLevel must reflect the default PLANE_RADAR_LOG_LEVEL (2)");

static_assert(plane_radar::kDiagnosticsEnabled == false,
              "kDiagnosticsEnabled must be false when PLANE_RADAR_DIAGNOSTICS==0");

// Usable in constant expressions (the static_asserts above already prove this,
// but make it explicit for clarity).
static_assert(plane_radar::kLogLevel == PLANE_RADAR_LOG_LEVEL,
              "kLogLevel must track PLANE_RADAR_LOG_LEVEL exactly");
static_assert(plane_radar::kDiagnosticsEnabled == (PLANE_RADAR_DIAGNOSTICS != 0),
              "kDiagnosticsEnabled must track PLANE_RADAR_DIAGNOSTICS != 0");

// ---- WorkerResult layout (DIAGNOSTICS=0: no fetch_duration_ms field) --------

// C++17 detection idiom: prove WorkerResult has NO fetch_duration_ms member
// when DIAGNOSTICS=0. std::void_t<> combined with decltype selects the partial
// specialisation only when the member exists; the primary template (false_type)
// fires when it is absent.
template <typename T, typename = void>
struct HasFetchDurationMs : std::false_type {};

template <typename T>
struct HasFetchDurationMs<T, std::void_t<decltype(T::fetch_duration_ms)>>
    : std::true_type {};

static_assert(!HasFetchDurationMs<services::adsb::WorkerResult>::value,
              "WorkerResult must NOT have fetch_duration_ms when DIAGNOSTICS=0");

// The non-diagnostic WorkerResult must remain trivially copyable.
static_assert(std::is_trivially_copyable<services::adsb::WorkerResult>::value,
              "WorkerResult must be trivially copyable (non-diag layout)");

// ---- core::elapsedMicros ----------------------------------------------------

// The function must exist and perform rollover-safe uint32 subtraction.
// (compile-time check only — no Arduino hardware needed)
static_assert(core::elapsedMicros(100U, 50U) == 50U,
              "elapsedMicros(100, 50) must be 50");
static_assert(core::elapsedMicros(10U, 0xFFFFFF00U) == 266U,
              "elapsedMicros must be rollover-safe (wrap past UINT32_MAX)");

// ============================================================
// Runtime tests
// ============================================================

void setUp() {}
void tearDown() {}

void test_diagnostics_defaults_to_zero() {
  TEST_ASSERT_EQUAL_INT(0, PLANE_RADAR_DIAGNOSTICS);
  TEST_ASSERT_FALSE(plane_radar::kDiagnosticsEnabled);
}

void test_log_level_defaults_to_info() {
  TEST_ASSERT_EQUAL_INT(2, PLANE_RADAR_LOG_LEVEL);
  TEST_ASSERT_EQUAL_INT(plane_radar::kLogLevelInfo, plane_radar::kLogLevel);
}

void test_log_level_constants_are_ordered() {
  TEST_ASSERT_EQUAL_INT(0, plane_radar::kLogLevelOff);
  TEST_ASSERT_EQUAL_INT(1, plane_radar::kLogLevelError);
  TEST_ASSERT_EQUAL_INT(2, plane_radar::kLogLevelInfo);
  TEST_ASSERT_TRUE(plane_radar::kLogLevelOff < plane_radar::kLogLevelError);
  TEST_ASSERT_TRUE(plane_radar::kLogLevelError < plane_radar::kLogLevelInfo);
}

void test_constexpr_reflection_matches_macros() {
  constexpr int level = plane_radar::kLogLevel;
  constexpr bool diag = plane_radar::kDiagnosticsEnabled;
  TEST_ASSERT_EQUAL_INT(PLANE_RADAR_LOG_LEVEL, level);
  TEST_ASSERT_EQUAL_INT((PLANE_RADAR_DIAGNOSTICS != 0) ? 1 : 0,
                         diag ? 1 : 0);
}

void test_elapsed_micros_basic() {
  TEST_ASSERT_EQUAL_UINT32(50U, core::elapsedMicros(100U, 50U));
  TEST_ASSERT_EQUAL_UINT32(0U,  core::elapsedMicros(50U, 50U));
  // Rollover: 10 - UINT32_MAX + something
  TEST_ASSERT_EQUAL_UINT32(266U, core::elapsedMicros(10U, 0xFFFFFF00U));
}

void test_worker_result_trivially_copyable_nondiag() {
  // Runtime echo of the static_assert above.
  TEST_ASSERT_TRUE(
      std::is_trivially_copyable<services::adsb::WorkerResult>::value);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_diagnostics_defaults_to_zero);
  RUN_TEST(test_log_level_defaults_to_info);
  RUN_TEST(test_log_level_constants_are_ordered);
  RUN_TEST(test_constexpr_reflection_matches_macros);
  RUN_TEST(test_elapsed_micros_basic);
  RUN_TEST(test_worker_result_trivially_copyable_nondiag);
  return UNITY_END();
}
