#pragma once

// Pure, Arduino/FreeRTOS-free coordination protocol for the OPTIONAL ADS-B
// network worker (compile-time default OFF; see README "network worker"). The
// worker fills the SnapshotStore's inactive candidate on its own task while the
// main task renders the active snapshot; this state machine models the strict
// SPSC hand-off between them so it can be unit-tested without any threading:
//
//   * main  dispatches a request and later consumes the result.
//   * worker claims the dispatched request and completes it.
//   * exactly one request/result is ever outstanding (no third snapshot, no
//     queue): main may not dispatch again until it has consumed the result.
//   * cancellation is COOPERATIVE only -- requestPause marks the current
//     generation cancelled and the worker completes promptly; nothing ever
//     cross-task stops or deletes the in-flight transport.
//
// Every entry point is non-blocking and mutates only the fixed POD state below;
// the real FreeRTOS queues/notifications and the FetchResult envelope live in the
// integration layer, not here.

#include <cstdint>
#include <type_traits>

namespace core {

// Explicit coordinator phases. Dispatched and Running are kept distinct so a
// pause can be reasoned about precisely before the worker claims, while it runs,
// and after it completes.
enum class WorkerPhase : uint8_t {
  Idle,         // no request outstanding; a dispatch is allowed
  Dispatched,   // main posted a request; the worker has not claimed it yet
  Running,      // the worker claimed the current request and is fetching
  ResultReady,  // the worker completed; the result awaits main's consume
  Paused,       // quiesced: no work outstanding and dispatch disabled until resume
  Fault,        // internal protocol fault; dispatch disabled, callers never block
};

// Outcome of main consuming a ResultReady result.
enum class WorkerConsume : uint8_t {
  Rejected,   // not ResultReady, or generation mismatch: no state change
  Completed,  // consumed a live result; coordinator returned to Idle (publishable)
  Quiesced,   // consumed a cancelled result; coordinator is now Paused (discard)
};

// Fixed POD coordination state. Trivially copyable so the integration layer can
// snapshot it across a lock. generation is the current request id: 0 before the
// first dispatch (the reserved invalid sentinel), nonzero and rollover-safe
// afterward, so a zero-initialized or stale generation never matches a live one.
struct WorkerProtocolState {
  WorkerPhase phase;
  uint32_t generation;
  bool cancel_requested;  // a pause was requested against the current generation
};

// Seed a fresh coordinator: Idle, no outstanding work, cancellation cleared, and
// generation = initial_generation (0 in production; a test may seed near the
// uint32 wrap boundary to exercise generation rollover cheaply).
void workerProtocolInit(WorkerProtocolState* state,
                        uint32_t initial_generation = 0);

// --- main task ---------------------------------------------------------------

// A dispatch is allowed ONLY from Idle. It is rejected while busy (Dispatched,
// Running, or ResultReady), while Paused, and while Fault.
bool workerCanDispatch(const WorkerProtocolState& state);

// Result of a dispatch attempt.
struct WorkerDispatch {
  bool accepted;       // true only when the request was posted (Idle -> Dispatched)
  uint32_t generation;  // the new nonzero generation on success; the unchanged
                        // current generation when rejected
};

// Post a new request. On success advances the generation (nonzero, rollover-safe,
// skipping the 0 sentinel on wrap), clears cancel_requested, and moves
// Idle -> Dispatched. Rejected with no state change when !workerCanDispatch.
WorkerDispatch workerDispatch(WorkerProtocolState* state);

// Consume a completed result. Requires phase == ResultReady AND generation ==
// state.generation. A live result returns to Idle (Completed, publishable); a
// cancelled result quiesces to Paused (Quiesced, the candidate must be
// discarded). A wrong generation, a duplicate consume, or any other phase is
// Rejected with no state change.
WorkerConsume workerConsume(WorkerProtocolState* state, uint32_t generation);

// Request a cooperative pause. Idle quiesces IMMEDIATELY (-> Paused, nothing is
// in flight). While work is outstanding (Dispatched / Running / ResultReady) it
// only marks the current generation cancelled; quiescence then follows the
// worker's completion and main's consume. Idempotent while Paused and a no-op
// while Fault -- like every entry point it returns immediately (no deadlock).
void workerRequestPause(WorkerProtocolState* state);

// Resume returns ONLY a quiesced coordinator to Idle (Paused -> Idle, clearing
// cancel_requested) and returns true. Any other phase -- including Fault -- is
// left unchanged and returns false.
bool workerResume(WorkerProtocolState* state);

// Force the internal-fault state from any phase. Dispatch is then permanently
// disabled (workerCanDispatch is false) but every caller entry point still
// returns immediately, so a faulted coordinator can never deadlock main.
// Recovery is only via workerProtocolInit (a fresh coordinator), never resume.
void workerFault(WorkerProtocolState* state);

// --- worker task -------------------------------------------------------------

// Claim the dispatched request. Requires phase == Dispatched AND generation ==
// state.generation; moves Dispatched -> Running. A stale, duplicate, or
// wrong-generation claim is rejected with no state change (returns false).
bool workerClaim(WorkerProtocolState* state, uint32_t generation);

// Report completion of the claimed request. Requires phase == Running AND
// generation == state.generation; moves Running -> ResultReady. A stale,
// duplicate, or wrong-generation completion is rejected with no state change
// (returns false) so a late/re-posted event can never corrupt the coordinator.
bool workerComplete(WorkerProtocolState* state, uint32_t generation);

// Cooperative cancellation query for the worker: true only when a pause was
// requested against exactly this in-flight generation. The worker polls it and,
// when true, performs no further network I/O and completes promptly with a
// cancelled result. A mismatched generation always reads false.
bool workerCancelRequested(const WorkerProtocolState& state, uint32_t generation);

// --- predicates --------------------------------------------------------------

// Provably quiesced: Paused, with no outstanding work and dispatch disabled. This
// is the truth the future Configure/Erase gate waits on before touching NVS or
// the radio. Only WorkerPhase::Paused is quiesced -- Idle is active (a dispatch
// may begin at any instant) and Dispatched / Running / ResultReady / Fault all
// leave the worker's ownership of shared state unresolved.
bool workerQuiesced(const WorkerProtocolState& state);

// True while the coordinator is in the internal-fault state.
bool workerFaulted(const WorkerProtocolState& state);

// The protocol relies on a fixed, trivially-copyable POD so the integration layer
// can copy it under a short lock; pin that here.
static_assert(std::is_trivially_copyable<WorkerProtocolState>::value,
              "WorkerProtocolState must be trivially copyable for lock-snapshot");
static_assert(std::is_standard_layout<WorkerProtocolState>::value,
              "WorkerProtocolState must be standard-layout POD");
static_assert(sizeof(WorkerPhase) == 1, "WorkerPhase is a fixed 1-byte enum");
static_assert(sizeof(WorkerConsume) == 1, "WorkerConsume is a fixed 1-byte enum");

}  // namespace core
