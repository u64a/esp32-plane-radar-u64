#include "core/adsb_worker_protocol.h"

namespace core {

namespace {

// 0 is the reserved invalid generation: a zero-initialized or stale token can
// never match a live request. The sequence advances before use and skips 0 on
// wrap so an issued generation is always nonzero.
constexpr uint32_t kInvalidGeneration = 0;

uint32_t nextGeneration(uint32_t generation) {
  const uint32_t advanced = generation + 1U;  // wrap is intentional and safe
  return advanced == kInvalidGeneration ? advanced + 1U : advanced;
}

}  // namespace

void workerProtocolInit(WorkerProtocolState* state,
                        uint32_t initial_generation) {
  if (state == nullptr) {
    return;
  }
  state->phase = WorkerPhase::Idle;
  state->generation = initial_generation;
  state->cancel_requested = false;
}

bool workerCanDispatch(const WorkerProtocolState& state) {
  return state.phase == WorkerPhase::Idle;
}

WorkerDispatch workerDispatch(WorkerProtocolState* state) {
  if (state == nullptr) {
    return WorkerDispatch{false, kInvalidGeneration};
  }
  if (state->phase != WorkerPhase::Idle) {
    // Rejected while busy, paused, or faulted: the current generation stands.
    return WorkerDispatch{false, state->generation};
  }
  state->generation = nextGeneration(state->generation);
  state->cancel_requested = false;
  state->phase = WorkerPhase::Dispatched;
  return WorkerDispatch{true, state->generation};
}

WorkerConsume workerConsume(WorkerProtocolState* state, uint32_t generation) {
  if (state == nullptr || state->phase != WorkerPhase::ResultReady ||
      generation != state->generation) {
    return WorkerConsume::Rejected;  // stale / duplicate / wrong phase: no change
  }
  if (state->cancel_requested) {
    state->phase = WorkerPhase::Paused;
    state->cancel_requested = false;  // the pending pause is now satisfied
    return WorkerConsume::Quiesced;
  }
  state->phase = WorkerPhase::Idle;
  return WorkerConsume::Completed;
}

void workerRequestPause(WorkerProtocolState* state) {
  if (state == nullptr) {
    return;
  }
  switch (state->phase) {
    case WorkerPhase::Idle:
      // Nothing is in flight: quiesce immediately.
      state->phase = WorkerPhase::Paused;
      state->cancel_requested = false;
      break;
    case WorkerPhase::Dispatched:
    case WorkerPhase::Running:
    case WorkerPhase::ResultReady:
      // Mark the current generation cancelled; quiescence follows the worker's
      // completion and main's consume (cooperative cancellation only).
      state->cancel_requested = true;
      break;
    case WorkerPhase::Paused:
    case WorkerPhase::Fault:
      break;  // idempotent / no-op; a caller is never blocked
  }
}

bool workerResume(WorkerProtocolState* state) {
  if (state == nullptr || state->phase != WorkerPhase::Paused) {
    return false;  // resume ONLY returns a quiesced coordinator to Idle
  }
  state->phase = WorkerPhase::Idle;
  state->cancel_requested = false;
  return true;
}

void workerFault(WorkerProtocolState* state) {
  if (state == nullptr) {
    return;
  }
  state->phase = WorkerPhase::Fault;  // dispatch disabled; callers never block
}

bool workerClaim(WorkerProtocolState* state, uint32_t generation) {
  if (state == nullptr || state->phase != WorkerPhase::Dispatched ||
      generation != state->generation) {
    return false;  // stale / duplicate / wrong-generation: no state change
  }
  state->phase = WorkerPhase::Running;
  return true;
}

bool workerComplete(WorkerProtocolState* state, uint32_t generation) {
  if (state == nullptr || state->phase != WorkerPhase::Running ||
      generation != state->generation) {
    return false;  // stale / duplicate / wrong-generation: no state change
  }
  state->phase = WorkerPhase::ResultReady;
  return true;
}

bool workerCancelRequested(const WorkerProtocolState& state,
                           uint32_t generation) {
  return state.cancel_requested && generation == state.generation;
}

bool workerQuiesced(const WorkerProtocolState& state) {
  return state.phase == WorkerPhase::Paused;
}

bool workerFaulted(const WorkerProtocolState& state) {
  return state.phase == WorkerPhase::Fault;
}

}  // namespace core
