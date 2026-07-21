#pragma once

// Main-side facade for the OPTIONAL, compile-time-gated ADS-B network worker.
//
// The worker runs one long-lived FreeRTOS task that performs the bounded HTTPS
// ADS-B fetch off the main loop, filling the shared two-slot SnapshotStore's
// inactive candidate while the main task keeps rendering the active snapshot.
// The strict single-producer/single-consumer hand-off between the two tasks is
// modelled by the pure, unit-tested core::WorkerProtocolState (see
// core/adsb_worker_protocol.h); this facade is the Arduino/FreeRTOS glue that
// binds that protocol to the real static task, the two depth-1 queues, and the
// controllable fetch seam (services::adsb::fetchCandidateControlled).
//
// The whole adapter is compiled in ONLY when PLANE_RADAR_ADSB_WORKER != 0
// (the [env:supermini-worker] firmware build). The default firmware
// ([env:supermini]) leaves it 0, so no worker task, no queues, and no 8 KB
// worker stack are linked.
//
// Callers MUST select the worker path with a COMPILE-TIME branch so the default
// firmware references zero facade symbols:
//   * `#if PLANE_RADAR_ADSB_WORKER` is the current CANONICAL firmware path (used
//     by src/main.cpp and src/services/wifi_setup.cpp). It is chosen because the
//     pinned Arduino build invocation does not currently make C++17
//     `if constexpr` warning-free / effective in the firmware translation units.
//   * `if constexpr (services::adsb::workerEnabled())` is an equally valid
//     alternative wherever the effective language mode supports it cleanly (e.g.
//     the native tests, which are compiled at C++17).
// Either way the default build must reference NO facade symbol. The inert
// definitions in adsb_worker.cpp are a link-safety net for any compile-time-
// guarded caller; with the canonical `#if` gating the default main never names
// them, so they are unreferenced and dead-stripped (the default ELF links ZERO
// worker symbols -- verified with nm). Do NOT invoke the facade unconditionally.
//
// Every entry point is NON-BLOCKING and lifecycle-explicit. FreeRTOS types are
// deliberately kept out of this header so the main-loop integration stays
// Arduino-free at the call site; the request/result payloads carry only small
// trivially-copyable metadata (never an AircraftSnapshot, credentials, strings,
// or pointers into a caller's stack).

#include <cstddef>
#include <cstdint>

#include "services/adsb_snapshot_store.h"  // CandidateResult / CandidateHandle
#include "services/adsb_types.h"           // FetchResult / FetchOutcome

// The optional worker is OFF unless the firmware environment defines the macro
// (only [env:supermini-worker] sets it to 1). An undefined macro therefore
// resolves to 0 here, so the default build is worker-free with no extra flags.
#ifndef PLANE_RADAR_ADSB_WORKER
#define PLANE_RADAR_ADSB_WORKER 0
#endif

namespace services::adsb {

// Compile-time feature gate: true only in the worker-enabled firmware. It is an
// inline constexpr reflection of the PLANE_RADAR_ADSB_WORKER macro for C++
// constant-expression contexts (static_assert, `if constexpr` where the language
// mode supports it, and native tests). The preprocessor `#if PLANE_RADAR_ADSB_
// WORKER` remains the canonical firmware gate (see the file header); this is its
// language-level companion. It is deliberately NOT a runtime function -- that
// would force the default build to link the facade.
inline constexpr bool workerEnabled() { return PLANE_RADAR_ADSB_WORKER != 0; }

// Lock the gate to the macro AND force it to be a genuine constant expression: a
// future edit that decoupled it from PLANE_RADAR_ADSB_WORKER, or that made it a
// non-constexpr function, fails the build here rather than silently linking
// worker code into the default firmware.
static_assert(workerEnabled() == (PLANE_RADAR_ADSB_WORKER != 0),
              "workerEnabled() must be a compile-time constant tracking "
              "PLANE_RADAR_ADSB_WORKER");

// Coarse lifecycle/health of the worker, as observed from the main task. This is
// the projection of the internal core::WorkerPhase that main actually needs.
enum class WorkerState : uint8_t {
  Disabled,  // compiled out (PLANE_RADAR_ADSB_WORKER == 0) or begin() not called
  Idle,      // running; nothing outstanding; a dispatch is allowed
  Busy,      // a request/result is outstanding (dispatched, running, or ready)
  Paused,    // quiesced: nothing outstanding and dispatch disabled until resume
  Fault,     // internal fault: dispatch disabled; callers still never block
};

// Immutable query inputs for one dispatch. All fields are captured by the main
// task BEFORE the (blocking) fetch begins and copied by value into the request
// queue, so the worker never reads live main-side state. connectivity_epoch is
// the caller's snapshot of wifiDisconnectSeq() at dispatch time; it rides the
// request and comes back on the result so the integration layer can detect a
// mid-fetch Wi-Fi flap without the worker ever touching a Wi-Fi API.
struct WorkerQuery {
  double lat;
  double lon;
  float radius_km;
  uint32_t settings_revision;   // services::settings::revision() at dispatch
  uint32_t connectivity_epoch;  // wifiDisconnectSeq() at dispatch
};

// Outcome of a dispatch attempt.
enum class WorkerDispatchStatus : uint8_t {
  Accepted,  // request posted (Idle -> busy); generation is nonzero
  Rejected,  // not Idle (busy / paused / faulted / disabled): no state change
  Faulted,   // protocol accepted but the queue send failed; failed CLOSED to
             // Fault (never silently pretended idle)
};

struct WorkerDispatchResult {
  WorkerDispatchStatus status;
  uint32_t generation;  // the accepted request id (nonzero) only on Accepted
};

// Outcome of taking a result. Completed/Quiesced are the two authoritative
// consume verdicts; None means the result queue was empty this poll; Faulted is
// the fail-closed outcome of an impossible dequeued-but-unconsumable message.
enum class WorkerResultStatus : uint8_t {
  None,       // no result was available (queue empty); *out left inert
  Completed,  // live result consumed: candidate is PUBLISHABLE; worker now Idle
  Quiesced,   // cancelled result consumed: candidate must be DISCARDED; Paused
  Faulted,    // a dequeued result could not be consumed (impossible under the
              // one-in-flight invariant): its candidate was discarded and the
              // coordinator is now Fault -- explicitly distinct from None
};

// One taken result. It carries everything the integration layer needs to either
// publish (Completed) or discard/unwind (Quiesced) without re-reading worker
// internals. `candidate` embeds the FetchResult and the CandidateHandle produced
// by the worker's fetchCandidateControlled(); the handle is invalid whenever the
// fetch did not fully succeed (including a cooperative abort), so a discard on it
// is a safe no-op. It never contains an AircraftSnapshot -- only the small handle
// that references the store's inactive slot.
//
// When PLANE_RADAR_DIAGNOSTICS is enabled, `fetch_duration_ms` carries the
// rollover-safe duration of fetchCandidateControlled on the worker task (measured
// with millis() + core::elapsedMs and written by the worker into WorkerResultMsg
// before queue-send, then copied here by workerTakeResult). The field is ABSENT
// from the non-diagnostic struct layout (compiled out entirely when
// PLANE_RADAR_DIAGNOSTICS == 0) — it is not zero-initialised in non-diagnostic
// builds; it simply does not exist. The queue backing storage is
// sizeof(WorkerResultMsg)-derived so it auto-adjusts to either layout.
struct WorkerResult {
  WorkerResultStatus status;
  uint32_t generation;
  uint32_t settings_revision;   // revision the candidate was fetched for
  uint32_t connectivity_epoch;  // wifiDisconnectSeq() captured at dispatch
  CandidateResult candidate;    // FetchResult + handle (handle invalid on abort)
  bool worker_cancelled;        // the worker cooperatively aborted the fetch
#if PLANE_RADAR_DIAGNOSTICS
  uint32_t fetch_duration_ms;   // fetchCandidateControlled wall time on worker task
#endif
};

// How the main integration must react to a taken WorkerResult. Extracting this as
// a pure classifier keeps the "cancellation is an abort, not a completion" and
// "a fault halts dispatch" contracts unit-testable without a FreeRTOS harness.
enum class WorkerPollAction : uint8_t {
  Ignore,           // None: the result queue was empty; do nothing
  Publish,          // Completed: publish the candidate via the shared completion
  DiscardAndAbort,  // Quiesced: discard the candidate and unwind the in-flight
                    // poll WITHOUT a completion (retain backoff/immediate); never
                    // publish or ratchet time/NVS, even after a late-cancelled OK
  FaultHalt,        // Faulted: clear the in-flight poll without a normal outcome
                    // and halt further worker dispatch (never fall back to sync)
};

inline WorkerPollAction workerPollActionFor(WorkerResultStatus status) {
  switch (status) {
    case WorkerResultStatus::Completed:
      return WorkerPollAction::Publish;
    case WorkerResultStatus::Quiesced:
      return WorkerPollAction::DiscardAndAbort;
    case WorkerResultStatus::Faulted:
      return WorkerPollAction::FaultHalt;
    case WorkerResultStatus::None:
      break;
  }
  return WorkerPollAction::Ignore;
}

// Action when workerTakeResult() reported NO message (empty queue). Some adapter
// fault paths (claim reject, complete reject, result-queue send failure) fault
// the coordinator WITHOUT enqueuing a result, so an empty queue must still be
// checked for a fault: faulted -> FaultHalt (unwind + halt dispatch), otherwise
// Ignore. Pairs with workerFaulted() to make the result-less fault observation
// native-testable without a FreeRTOS harness.
inline WorkerPollAction workerPollActionForNoResult(bool faulted) {
  return faulted ? WorkerPollAction::FaultHalt : WorkerPollAction::Ignore;
}

// --- lifecycle --------------------------------------------------------------

// Start the long-lived static worker task and its two depth-1 queues exactly
// once. Call from the main task during setup, before the first dispatch.
// Idempotent: a second call after a successful start is a no-op that returns
// true (the running task is NEVER torn down or recreated). Returns false when the
// worker is compiled out or when static task creation failed.
bool workerBegin();

// True once workerBegin() has created the task. False when compiled out.
bool workerStarted();

// True while the coordinator is in the internal-fault state (dispatch disabled).
bool workerFaulted();

// Coarse lifecycle/health snapshot for the renderer / integration layer.
WorkerState workerState();

// Minimum free worker-task stack observed so far, in BYTES (uxTaskGetStack-
// HighWaterMark scaled by sizeof(StackType_t)). 0 when not started / compiled
// out. Poll-on-demand only -- there is no periodic logging.
size_t workerStackHighWaterBytes();

// --- dispatch (main -> worker) ----------------------------------------------

// Post a new fetch request. Allowed ONLY from Idle: rejected while a prior
// request/result is still outstanding, while Paused, while Faulted, and when
// compiled out. On success the generation is advanced (nonzero, rollover-safe)
// and the immutable query is queued for the worker. If the request queue send
// fails (never expected under the one-in-flight invariant) the adapter fails
// CLOSED to Fault rather than leaving a phantom-idle coordinator.
WorkerDispatchResult workerDispatch(const WorkerQuery& query);

// --- result (worker -> main) ------------------------------------------------

// Non-blocking: dequeue at most one result and consume it exactly once
// (ResultReady -> Idle on a live result, or -> Paused on a cancelled one). Fills
// *out and returns true when a message was taken (status Completed, Quiesced, or
// Faulted); returns false and sets out->status = None when the queue was empty.
// A dequeued message whose generation cannot be consumed (impossible under the
// one-in-flight invariant) discards its candidate, faults the coordinator, and is
// reported as status Faulted -- never as an ordinary empty None. On Completed the
// caller MUST publish out->candidate; on Quiesced it MUST discard it; in both
// cases before the next workerDispatch(), since a new dispatch supersedes any
// still-outstanding candidate in the shared store.
bool workerTakeResult(WorkerResult* out);

// --- pause / resume ---------------------------------------------------------

// Request a cooperative pause. If nothing is outstanding this quiesces
// immediately (-> Paused); otherwise it only marks the in-flight generation
// cancelled, and quiescence follows once the worker completes and the result is
// taken. Cancellation is cooperative through fetchCandidateControlled(): the
// task is NEVER killed, suspended, or deleted. Idempotent while Paused, a no-op
// while Faulted, and always returns immediately.
void workerRequestPause();

// True only when the worker is PROVABLY quiesced (Paused: nothing outstanding
// and dispatch disabled) -- the condition the future Configure/Erase gate waits
// on before touching the radio or NVS. Returns true when compiled out or not yet
// started (there is no worker that could own shared state).
bool workerQuiesced();

// Resume a quiesced worker (Paused -> Idle). Returns false (no change) from any
// other state, including Fault; there is no auto-resume.
bool workerResume();

}  // namespace services::adsb
