#include <unity.h>

#include <cstdint>
#include <cstddef>
#include <type_traits>

#include "core/time_math.h"
#include "runtime_diagnostics.h"
#include "services/adsb_worker.h"

// Compile-time / runtime contract check for the Phase 10 diagnostics and logging
// headers under the DIAGNOSTICS-ON configuration (PLANE_RADAR_DIAGNOSTICS=1).
//
// This suite is run ONLY under [env:native-diag] which defines
// PLANE_RADAR_DIAGNOSTICS=1. It verifies:
//   * PLANE_RADAR_DIAGNOSTICS is exactly 1 in this environment.
//   * kDiagnosticsEnabled is true.
//   * The conditional WorkerResult field fetch_duration_ms exists, is uint32_t,
//     and is at a reachable offset.
//   * WorkerResult remains trivially copyable with the additional field.
//   * The WorkerResultMsg internal queue payload (which also gains the field)
//     keeps trivial copyability; this is proved by its internal static_assert
//     inside adsb_worker.cpp plus a successful worker-diag build — NOT by
//     indirect inference from the public WorkerResult.
//   * core::elapsedMicros remains rollover-safe.

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

// ---- environment sanity -----------------------------------------------------

// This suite must only be compiled with PLANE_RADAR_DIAGNOSTICS=1.
static_assert(PLANE_RADAR_DIAGNOSTICS == 1,
              "test_runtime_diagnostics_on must be built with "
              "PLANE_RADAR_DIAGNOSTICS=1 (use [env:native-diag])");

// ---- kDiagnosticsEnabled reflects the macro ---------------------------------

static_assert(plane_radar::kDiagnosticsEnabled == true,
              "kDiagnosticsEnabled must be true when PLANE_RADAR_DIAGNOSTICS==1");

// ---- WorkerResult conditional field -----------------------------------------

// Under DIAGNOSTICS=1, WorkerResult must have a fetch_duration_ms field of
// type uint32_t.
static_assert(
    std::is_same<decltype(services::adsb::WorkerResult::fetch_duration_ms),
                 uint32_t>::value,
    "WorkerResult::fetch_duration_ms must be uint32_t when PLANE_RADAR_DIAGNOSTICS=1");

// The struct must still be trivially copyable (a uint32_t field cannot change that).
static_assert(std::is_trivially_copyable<services::adsb::WorkerResult>::value,
              "WorkerResult must remain trivially copyable with fetch_duration_ms");

// ---- core::elapsedMicros still correct in diag builds ----------------------

static_assert(core::elapsedMicros(200U, 100U) == 100U,
              "elapsedMicros(200, 100) must be 100");
static_assert(core::elapsedMicros(10U, 0xFFFFFF00U) == 266U,
              "elapsedMicros must be rollover-safe in diag builds too");

// ============================================================
// Runtime tests
// ============================================================

void setUp() {}
void tearDown() {}

void test_diagnostics_is_one() {
  TEST_ASSERT_EQUAL_INT(1, PLANE_RADAR_DIAGNOSTICS);
  TEST_ASSERT_TRUE(plane_radar::kDiagnosticsEnabled);
}

void test_worker_result_has_fetch_duration_ms_field() {
  // Verify the field is present and the right type at runtime.
  services::adsb::WorkerResult r{};
  r.fetch_duration_ms = 12345U;
  TEST_ASSERT_EQUAL_UINT32(12345U, r.fetch_duration_ms);
}

void test_worker_result_trivially_copyable_diag() {
  TEST_ASSERT_TRUE(
      std::is_trivially_copyable<services::adsb::WorkerResult>::value);
}

void test_worker_result_fetch_duration_ms_is_uint32() {
  // Type check: sizeof(uint32_t) == 4.
  TEST_ASSERT_EQUAL_UINT32(4U,
      sizeof(services::adsb::WorkerResult::fetch_duration_ms));
}

void test_elapsed_micros_rollover_safe_in_diag_build() {
  // Same rollover check as the non-diag suite to confirm nothing changed.
  TEST_ASSERT_EQUAL_UINT32(266U, core::elapsedMicros(10U, 0xFFFFFF00U));
  TEST_ASSERT_EQUAL_UINT32(100U, core::elapsedMicros(200U, 100U));
  TEST_ASSERT_EQUAL_UINT32(0U,   core::elapsedMicros(77U, 77U));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_diagnostics_is_one);
  RUN_TEST(test_worker_result_has_fetch_duration_ms_field);
  RUN_TEST(test_worker_result_trivially_copyable_diag);
  RUN_TEST(test_worker_result_fetch_duration_ms_is_uint32);
  RUN_TEST(test_elapsed_micros_rollover_safe_in_diag_build);
  return UNITY_END();
}
