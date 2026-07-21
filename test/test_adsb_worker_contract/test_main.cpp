#include <unity.h>

#include <cstdint>
#include <type_traits>

#include "services/adsb_worker.h"

// PURE compile-time / source-policy contract check for the optional network-worker
// FACADE HEADER. It links NONE of the FreeRTOS adapter -- src/services/
// adsb_worker.cpp is excluded from [env:native]'s build_src_filter and this suite
// calls no facade function -- so it only proves that:
//   * the public header is Arduino/FreeRTOS-free (it compiles natively at all),
//   * workerEnabled() is a genuine constexpr feature gate that is FALSE whenever
//     PLANE_RADAR_ADSB_WORKER is undefined (the default firmware configuration) --
//     the language-level companion to the canonical `#if PLANE_RADAR_ADSB_WORKER`
//     firmware gate, and what keeps the default build free of facade references,
//   * the hardened result status exposes an explicit Faulted outcome distinct
//     from an empty None, and the payloads stay trivially copyable.
// It deliberately does NOT stand up a fake FreeRTOS harness.

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

// The header must default the gate macro to 0 when nothing defines it.
static_assert(PLANE_RADAR_ADSB_WORKER == 0,
              "adsb_worker.h must default PLANE_RADAR_ADSB_WORKER to 0");

// workerEnabled() is usable in a constant expression AND is false in this
// (worker-undefined) build -- the property the default [env:supermini] relies on
// (via its compile-time `#if` gate) to keep every facade reference out.
static_assert(services::adsb::workerEnabled() == false,
              "workerEnabled() must be constexpr false when the worker macro is "
              "undefined (default firmware links no worker symbols)");
static_assert(services::adsb::workerEnabled() ==
                  (PLANE_RADAR_ADSB_WORKER != 0),
              "workerEnabled() must track PLANE_RADAR_ADSB_WORKER exactly");

// The result status carries an explicit fault distinct from None so main can tell
// an internal fault from an empty queue.
static_assert(services::adsb::WorkerResultStatus::Faulted !=
                  services::adsb::WorkerResultStatus::None,
              "Faulted must be distinct from None");
static_assert(sizeof(services::adsb::WorkerResultStatus) == 1,
              "WorkerResultStatus is a fixed 1-byte enum");

// The queued/returned payloads remain trivially copyable and bounded.
static_assert(std::is_trivially_copyable<services::adsb::WorkerResult>::value,
              "WorkerResult must be trivially copyable");
static_assert(std::is_trivially_copyable<services::adsb::WorkerQuery>::value,
              "WorkerQuery must be trivially copyable");
static_assert(
    std::is_trivially_copyable<services::adsb::WorkerDispatchResult>::value,
    "WorkerDispatchResult must be trivially copyable");

void setUp() {}
void tearDown() {}

void test_worker_enabled_is_constexpr_false_in_default_build() {
  // Usable as a constant expression (compiles ONLY because it is constexpr) and
  // false in the default configuration.
  constexpr bool enabled = services::adsb::workerEnabled();
  TEST_ASSERT_FALSE(enabled);
  TEST_ASSERT_FALSE(services::adsb::workerEnabled());
}

void test_result_status_faulted_is_explicit_and_distinct() {
  using services::adsb::WorkerResultStatus;
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(WorkerResultStatus::None),
                        static_cast<int>(WorkerResultStatus::Faulted));
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(WorkerResultStatus::Completed),
                        static_cast<int>(WorkerResultStatus::Faulted));
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(WorkerResultStatus::Quiesced),
                        static_cast<int>(WorkerResultStatus::Faulted));
}

void test_worker_poll_action_maps_cancellation_to_abort_not_completion() {
  using services::adsb::WorkerPollAction;
  using services::adsb::workerPollActionFor;
  using services::adsb::WorkerResultStatus;
  // A completed result publishes.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(WorkerPollAction::Publish),
      static_cast<int>(workerPollActionFor(WorkerResultStatus::Completed)));
  // A cancellation is an ABORT, never a completion/publish.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(WorkerPollAction::DiscardAndAbort),
      static_cast<int>(workerPollActionFor(WorkerResultStatus::Quiesced)));
  TEST_ASSERT_NOT_EQUAL(
      static_cast<int>(WorkerPollAction::Publish),
      static_cast<int>(workerPollActionFor(WorkerResultStatus::Quiesced)));
  // A fault halts dispatch (never publishes / completes normally).
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(WorkerPollAction::FaultHalt),
      static_cast<int>(workerPollActionFor(WorkerResultStatus::Faulted)));
  // An empty queue is ignored.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(WorkerPollAction::Ignore),
      static_cast<int>(workerPollActionFor(WorkerResultStatus::None)));
}

void test_worker_poll_action_no_result_observes_faulted() {
  using services::adsb::WorkerPollAction;
  using services::adsb::workerPollActionForNoResult;
  // A result-LESS fault (claim/complete reject, result-queue send failure) must
  // still map to FaultHalt so it is never missed on an empty queue.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WorkerPollAction::FaultHalt),
                        static_cast<int>(workerPollActionForNoResult(true)));
  // An empty queue with no fault is simply ignored.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WorkerPollAction::Ignore),
                        static_cast<int>(workerPollActionForNoResult(false)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_worker_enabled_is_constexpr_false_in_default_build);
  RUN_TEST(test_result_status_faulted_is_explicit_and_distinct);
  RUN_TEST(test_worker_poll_action_maps_cancellation_to_abort_not_completion);
  RUN_TEST(test_worker_poll_action_no_result_observes_faulted);
  return UNITY_END();
}
