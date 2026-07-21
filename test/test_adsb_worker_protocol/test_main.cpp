#include <unity.h>

#include <cstdint>

#include "core/adsb_worker_protocol.h"

using core::WorkerConsume;
using core::WorkerDispatch;
using core::WorkerPhase;
using core::WorkerProtocolState;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

int phase(const WorkerProtocolState& s) { return static_cast<int>(s.phase); }
int PH(WorkerPhase p) { return static_cast<int>(p); }
int CO(WorkerConsume c) { return static_cast<int>(c); }

// Drive a full live (non-cancelled) request cycle and return the generation used.
uint32_t runOneLiveCycle(WorkerProtocolState* s) {
  const WorkerDispatch d = core::workerDispatch(s);
  TEST_ASSERT_TRUE(d.accepted);
  TEST_ASSERT_TRUE(core::workerClaim(s, d.generation));
  TEST_ASSERT_TRUE(core::workerComplete(s, d.generation));
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Completed),
                        CO(core::workerConsume(s, d.generation)));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Idle), phase(*s));
  return d.generation;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_init_is_idle_with_invalid_generation() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Idle), phase(s));
  TEST_ASSERT_EQUAL_UINT32(0, s.generation);
  TEST_ASSERT_FALSE(s.cancel_requested);
  TEST_ASSERT_TRUE(core::workerCanDispatch(s));
  TEST_ASSERT_FALSE(core::workerQuiesced(s));
  TEST_ASSERT_FALSE(core::workerFaulted(s));
}

void test_happy_path_dispatch_claim_complete_consume() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);

  const WorkerDispatch d = core::workerDispatch(&s);
  TEST_ASSERT_TRUE(d.accepted);
  TEST_ASSERT_NOT_EQUAL(0, d.generation);  // first issued generation is nonzero
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Dispatched), phase(s));
  TEST_ASSERT_FALSE(core::workerCanDispatch(s));  // busy

  TEST_ASSERT_TRUE(core::workerClaim(&s, d.generation));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Running), phase(s));

  TEST_ASSERT_TRUE(core::workerComplete(&s, d.generation));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::ResultReady), phase(s));

  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Completed),
                        CO(core::workerConsume(&s, d.generation)));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Idle), phase(s));
  TEST_ASSERT_TRUE(core::workerCanDispatch(s));
}

void test_dispatch_rejected_while_busy() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  const WorkerDispatch d1 = core::workerDispatch(&s);
  TEST_ASSERT_TRUE(d1.accepted);

  // Dispatched: a second dispatch is rejected and the generation is unchanged.
  WorkerDispatch d2 = core::workerDispatch(&s);
  TEST_ASSERT_FALSE(d2.accepted);
  TEST_ASSERT_EQUAL_UINT32(d1.generation, d2.generation);
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Dispatched), phase(s));

  // Running: still rejected.
  TEST_ASSERT_TRUE(core::workerClaim(&s, d1.generation));
  d2 = core::workerDispatch(&s);
  TEST_ASSERT_FALSE(d2.accepted);
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Running), phase(s));

  // ResultReady: still rejected until consumed.
  TEST_ASSERT_TRUE(core::workerComplete(&s, d1.generation));
  d2 = core::workerDispatch(&s);
  TEST_ASSERT_FALSE(d2.accepted);
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::ResultReady), phase(s));
}

void test_generation_wrap_skips_zero_and_saturation() {
  // Seed just below the uint32 max so the next dispatch lands on the boundary,
  // then wraps: MAX-1 -> MAX -> 1 (skips 0) -> 2. A zero-initialized or stale
  // token can never collide with an issued generation.
  WorkerProtocolState s;
  core::workerProtocolInit(&s, 0xFFFFFFFEU);

  uint32_t g = runOneLiveCycle(&s);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFU, g);  // saturates at the max value first
  g = runOneLiveCycle(&s);
  TEST_ASSERT_EQUAL_UINT32(1U, g);  // wraps past 0xFFFFFFFF, SKIPPING 0
  g = runOneLiveCycle(&s);
  TEST_ASSERT_EQUAL_UINT32(2U, g);
}

void test_generation_wrap_from_max_directly() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s, 0xFFFFFFFFU);
  const uint32_t g = runOneLiveCycle(&s);
  TEST_ASSERT_EQUAL_UINT32(1U, g);  // MAX + 1 wraps to 0, skipped -> 1
  TEST_ASSERT_NOT_EQUAL(0, g);
}

void test_claim_requires_exact_generation() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  const WorkerDispatch d = core::workerDispatch(&s);

  // Wrong generation: rejected, phase unchanged (still Dispatched).
  TEST_ASSERT_FALSE(core::workerClaim(&s, d.generation + 7U));
  TEST_ASSERT_FALSE(core::workerClaim(&s, 0U));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Dispatched), phase(s));

  // Exact generation still works after the rejected attempts (no corruption).
  TEST_ASSERT_TRUE(core::workerClaim(&s, d.generation));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Running), phase(s));
}

void test_complete_requires_running_and_exact_generation() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  const WorkerDispatch d = core::workerDispatch(&s);

  // Complete before claim (phase Dispatched): rejected.
  TEST_ASSERT_FALSE(core::workerComplete(&s, d.generation));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Dispatched), phase(s));

  TEST_ASSERT_TRUE(core::workerClaim(&s, d.generation));
  // Wrong generation while Running: rejected, still Running.
  TEST_ASSERT_FALSE(core::workerComplete(&s, d.generation + 1U));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Running), phase(s));

  TEST_ASSERT_TRUE(core::workerComplete(&s, d.generation));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::ResultReady), phase(s));
}

void test_duplicate_and_stale_completion_are_rejected() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  const WorkerDispatch d1 = core::workerDispatch(&s);
  TEST_ASSERT_TRUE(core::workerClaim(&s, d1.generation));
  TEST_ASSERT_TRUE(core::workerComplete(&s, d1.generation));

  // Duplicate completion for the same generation: rejected (phase ResultReady).
  TEST_ASSERT_FALSE(core::workerComplete(&s, d1.generation));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::ResultReady), phase(s));

  // Consume, dispatch again: a completion carrying the OLD generation is stale
  // and rejected, and the current generation's completion path is unharmed.
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Completed),
                        CO(core::workerConsume(&s, d1.generation)));
  const WorkerDispatch d2 = core::workerDispatch(&s);
  TEST_ASSERT_TRUE(core::workerClaim(&s, d2.generation));
  TEST_ASSERT_FALSE(core::workerComplete(&s, d1.generation));  // stale generation
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Running), phase(s));
  TEST_ASSERT_TRUE(core::workerComplete(&s, d2.generation));  // current still ok
}

void test_consume_requires_result_ready_and_exact_generation() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  const WorkerDispatch d = core::workerDispatch(&s);

  // Consume before a result is ready: rejected, no change.
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Rejected),
                        CO(core::workerConsume(&s, d.generation)));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Dispatched), phase(s));

  TEST_ASSERT_TRUE(core::workerClaim(&s, d.generation));
  TEST_ASSERT_TRUE(core::workerComplete(&s, d.generation));

  // Wrong generation at consume: rejected, still ResultReady.
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Rejected),
                        CO(core::workerConsume(&s, d.generation + 3U)));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::ResultReady), phase(s));

  // Duplicate consume after a successful consume: rejected (already Idle).
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Completed),
                        CO(core::workerConsume(&s, d.generation)));
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Rejected),
                        CO(core::workerConsume(&s, d.generation)));
}

void test_pause_when_idle_quiesces_immediately() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  core::workerRequestPause(&s);
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Paused), phase(s));
  TEST_ASSERT_TRUE(core::workerQuiesced(s));
  TEST_ASSERT_FALSE(core::workerCanDispatch(s));  // dispatch disabled while paused
  const WorkerDispatch d = core::workerDispatch(&s);
  TEST_ASSERT_FALSE(d.accepted);
}

void test_pause_before_claim_quiesces_after_completion_and_consume() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  const WorkerDispatch d = core::workerDispatch(&s);

  core::workerRequestPause(&s);  // pause while Dispatched (before the claim)
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Dispatched), phase(s));  // not yet quiesced
  TEST_ASSERT_FALSE(core::workerQuiesced(s));
  TEST_ASSERT_TRUE(core::workerCancelRequested(s, d.generation));

  // Cooperative worker: it still claims, sees the cancel, and completes promptly.
  TEST_ASSERT_TRUE(core::workerClaim(&s, d.generation));
  TEST_ASSERT_TRUE(core::workerCancelRequested(s, d.generation));
  TEST_ASSERT_TRUE(core::workerComplete(&s, d.generation));

  // Consume of a cancelled result quiesces to Paused.
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Quiesced),
                        CO(core::workerConsume(&s, d.generation)));
  TEST_ASSERT_TRUE(core::workerQuiesced(s));
  TEST_ASSERT_FALSE(s.cancel_requested);
}

void test_pause_during_run_quiesces_after_completion_and_consume() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  const WorkerDispatch d = core::workerDispatch(&s);
  TEST_ASSERT_TRUE(core::workerClaim(&s, d.generation));

  core::workerRequestPause(&s);  // pause while Running
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Running), phase(s));
  TEST_ASSERT_TRUE(core::workerCancelRequested(s, d.generation));
  TEST_ASSERT_FALSE(core::workerQuiesced(s));

  TEST_ASSERT_TRUE(core::workerComplete(&s, d.generation));
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Quiesced),
                        CO(core::workerConsume(&s, d.generation)));
  TEST_ASSERT_TRUE(core::workerQuiesced(s));
}

void test_pause_after_completion_quiesces_on_consume() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  const WorkerDispatch d = core::workerDispatch(&s);
  TEST_ASSERT_TRUE(core::workerClaim(&s, d.generation));
  TEST_ASSERT_TRUE(core::workerComplete(&s, d.generation));  // ResultReady first

  core::workerRequestPause(&s);  // pause AFTER completion, before consume
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::ResultReady), phase(s));
  TEST_ASSERT_FALSE(core::workerQuiesced(s));

  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Quiesced),
                        CO(core::workerConsume(&s, d.generation)));
  TEST_ASSERT_TRUE(core::workerQuiesced(s));
}

void test_cancel_requested_only_for_current_generation() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  const WorkerDispatch d = core::workerDispatch(&s);
  core::workerRequestPause(&s);
  TEST_ASSERT_TRUE(core::workerCancelRequested(s, d.generation));
  TEST_ASSERT_FALSE(core::workerCancelRequested(s, d.generation + 1U));
  TEST_ASSERT_FALSE(core::workerCancelRequested(s, 0U));
}

void test_resume_only_returns_paused_to_idle() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);

  // resume from Idle: no-op, returns false.
  TEST_ASSERT_FALSE(core::workerResume(&s));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Idle), phase(s));

  const WorkerDispatch d = core::workerDispatch(&s);
  // resume while Dispatched/Running/ResultReady: rejected, no change.
  TEST_ASSERT_FALSE(core::workerResume(&s));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Dispatched), phase(s));
  TEST_ASSERT_TRUE(core::workerClaim(&s, d.generation));
  TEST_ASSERT_FALSE(core::workerResume(&s));
  TEST_ASSERT_TRUE(core::workerComplete(&s, d.generation));
  TEST_ASSERT_FALSE(core::workerResume(&s));

  // Quiesce, then resume: Paused -> Idle, dispatch enabled again.
  core::workerRequestPause(&s);
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Quiesced),
                        CO(core::workerConsume(&s, d.generation)));
  TEST_ASSERT_TRUE(core::workerResume(&s));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Idle), phase(s));
  TEST_ASSERT_FALSE(s.cancel_requested);
  TEST_ASSERT_TRUE(core::workerCanDispatch(s));
  TEST_ASSERT_TRUE(core::workerDispatch(&s).accepted);
}

void test_resume_after_idle_pause_returns_to_idle() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  core::workerRequestPause(&s);  // Idle -> Paused directly
  TEST_ASSERT_TRUE(core::workerQuiesced(s));
  TEST_ASSERT_TRUE(core::workerResume(&s));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Idle), phase(s));
}

void test_fault_disables_dispatch_but_never_blocks_callers() {
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  const WorkerDispatch d = core::workerDispatch(&s);
  TEST_ASSERT_TRUE(core::workerClaim(&s, d.generation));

  core::workerFault(&s);  // internal fault while Running
  TEST_ASSERT_TRUE(core::workerFaulted(s));
  TEST_ASSERT_FALSE(core::workerCanDispatch(s));
  TEST_ASSERT_FALSE(core::workerQuiesced(s));

  // Every entry point returns immediately and leaves the fault standing.
  TEST_ASSERT_FALSE(core::workerDispatch(&s).accepted);
  TEST_ASSERT_FALSE(core::workerClaim(&s, d.generation));
  TEST_ASSERT_FALSE(core::workerComplete(&s, d.generation));
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Rejected),
                        CO(core::workerConsume(&s, d.generation)));
  core::workerRequestPause(&s);  // no-op, does not throw or hang
  TEST_ASSERT_TRUE(core::workerFaulted(s));
  TEST_ASSERT_FALSE(core::workerResume(&s));  // resume never recovers a fault
  TEST_ASSERT_TRUE(core::workerFaulted(s));

  // Recovery is only via a fresh init.
  core::workerProtocolInit(&s);
  TEST_ASSERT_FALSE(core::workerFaulted(s));
  TEST_ASSERT_TRUE(core::workerCanDispatch(s));
}

void test_fault_from_dispatched_is_fail_closed() {
  // The network-worker adapter faults from Dispatched when the request-queue send
  // fails right after workerDispatch captured the generation. Dispatch must then
  // be disabled and no caller may block; recovery is only via a fresh init.
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  const WorkerDispatch d = core::workerDispatch(&s);
  TEST_ASSERT_TRUE(d.accepted);
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Dispatched), phase(s));

  core::workerFault(&s);  // request-queue send failure: fail closed from Dispatched
  TEST_ASSERT_TRUE(core::workerFaulted(s));
  TEST_ASSERT_FALSE(core::workerCanDispatch(s));
  TEST_ASSERT_FALSE(core::workerQuiesced(s));

  // Every entry point returns immediately and leaves the fault standing.
  TEST_ASSERT_FALSE(core::workerDispatch(&s).accepted);
  TEST_ASSERT_FALSE(core::workerClaim(&s, d.generation));
  TEST_ASSERT_FALSE(core::workerComplete(&s, d.generation));
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Rejected),
                        CO(core::workerConsume(&s, d.generation)));
  core::workerRequestPause(&s);
  TEST_ASSERT_TRUE(core::workerFaulted(s));
  TEST_ASSERT_FALSE(core::workerResume(&s));  // resume never recovers a fault

  core::workerProtocolInit(&s);  // recovery only via a fresh coordinator
  TEST_ASSERT_TRUE(core::workerCanDispatch(s));
}

void test_fault_from_result_ready_is_fail_closed() {
  // The adapter faults from ResultReady when the result-queue send fails after the
  // worker completed. The completed result is never delivered: dispatch stays
  // disabled and a consume of that generation is rejected, so main observes Fault
  // rather than a fabricated completion or a leaked candidate.
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  const WorkerDispatch d = core::workerDispatch(&s);
  TEST_ASSERT_TRUE(core::workerClaim(&s, d.generation));
  TEST_ASSERT_TRUE(core::workerComplete(&s, d.generation));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::ResultReady), phase(s));

  core::workerFault(&s);  // result-queue send failure: fail closed from ResultReady
  TEST_ASSERT_TRUE(core::workerFaulted(s));
  TEST_ASSERT_FALSE(core::workerCanDispatch(s));
  TEST_ASSERT_FALSE(core::workerQuiesced(s));
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Rejected),
                        CO(core::workerConsume(&s, d.generation)));
  TEST_ASSERT_FALSE(core::workerResume(&s));
  TEST_ASSERT_FALSE(core::workerDispatch(&s).accepted);

  core::workerProtocolInit(&s);  // recovery only via a fresh coordinator
  TEST_ASSERT_TRUE(core::workerCanDispatch(s));
}

void test_quiescence_truth_table_across_all_phases() {
  // Only Paused is quiesced; every other phase is not.
  WorkerProtocolState s;

  core::workerProtocolInit(&s);  // Idle
  TEST_ASSERT_FALSE(core::workerQuiesced(s));

  const WorkerDispatch d = core::workerDispatch(&s);  // Dispatched
  TEST_ASSERT_FALSE(core::workerQuiesced(s));

  core::workerClaim(&s, d.generation);  // Running
  TEST_ASSERT_FALSE(core::workerQuiesced(s));

  core::workerComplete(&s, d.generation);  // ResultReady
  TEST_ASSERT_FALSE(core::workerQuiesced(s));

  core::workerRequestPause(&s);
  core::workerConsume(&s, d.generation);  // Paused
  TEST_ASSERT_TRUE(core::workerQuiesced(s));

  core::workerProtocolInit(&s);
  core::workerFault(&s);  // Fault
  TEST_ASSERT_FALSE(core::workerQuiesced(s));
}

void test_rejected_events_do_not_corrupt_state() {
  // A barrage of illegal events from Idle must leave the coordinator dispatchable
  // and untouched.
  WorkerProtocolState s;
  core::workerProtocolInit(&s);
  TEST_ASSERT_FALSE(core::workerClaim(&s, 0U));
  TEST_ASSERT_FALSE(core::workerClaim(&s, 12345U));
  TEST_ASSERT_FALSE(core::workerComplete(&s, 0U));
  TEST_ASSERT_FALSE(core::workerComplete(&s, 999U));
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Rejected),
                        CO(core::workerConsume(&s, 0U)));
  TEST_ASSERT_EQUAL_INT(PH(WorkerPhase::Idle), phase(s));
  TEST_ASSERT_EQUAL_UINT32(0, s.generation);
  TEST_ASSERT_TRUE(core::workerDispatch(&s).accepted);  // still healthy
}

void test_null_state_is_safe() {
  // Null pointers must not crash; dispatch reports rejected with generation 0.
  core::workerProtocolInit(nullptr);
  const WorkerDispatch d = core::workerDispatch(nullptr);
  TEST_ASSERT_FALSE(d.accepted);
  TEST_ASSERT_EQUAL_UINT32(0, d.generation);
  TEST_ASSERT_FALSE(core::workerClaim(nullptr, 1U));
  TEST_ASSERT_FALSE(core::workerComplete(nullptr, 1U));
  TEST_ASSERT_EQUAL_INT(CO(WorkerConsume::Rejected),
                        CO(core::workerConsume(nullptr, 1U)));
  core::workerRequestPause(nullptr);
  TEST_ASSERT_FALSE(core::workerResume(nullptr));
  core::workerFault(nullptr);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_init_is_idle_with_invalid_generation);
  RUN_TEST(test_happy_path_dispatch_claim_complete_consume);
  RUN_TEST(test_dispatch_rejected_while_busy);
  RUN_TEST(test_generation_wrap_skips_zero_and_saturation);
  RUN_TEST(test_generation_wrap_from_max_directly);
  RUN_TEST(test_claim_requires_exact_generation);
  RUN_TEST(test_complete_requires_running_and_exact_generation);
  RUN_TEST(test_duplicate_and_stale_completion_are_rejected);
  RUN_TEST(test_consume_requires_result_ready_and_exact_generation);
  RUN_TEST(test_pause_when_idle_quiesces_immediately);
  RUN_TEST(test_pause_before_claim_quiesces_after_completion_and_consume);
  RUN_TEST(test_pause_during_run_quiesces_after_completion_and_consume);
  RUN_TEST(test_pause_after_completion_quiesces_on_consume);
  RUN_TEST(test_cancel_requested_only_for_current_generation);
  RUN_TEST(test_resume_only_returns_paused_to_idle);
  RUN_TEST(test_resume_after_idle_pause_returns_to_idle);
  RUN_TEST(test_fault_disables_dispatch_but_never_blocks_callers);
  RUN_TEST(test_fault_from_dispatched_is_fail_closed);
  RUN_TEST(test_fault_from_result_ready_is_fail_closed);
  RUN_TEST(test_quiescence_truth_table_across_all_phases);
  RUN_TEST(test_rejected_events_do_not_corrupt_state);
  RUN_TEST(test_null_state_is_safe);
  return UNITY_END();
}
