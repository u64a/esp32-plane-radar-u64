#include "services/adsb_worker.h"

// The entire FreeRTOS adapter below is compiled ONLY for the worker-enabled
// firmware ([env:supermini-worker], which defines PLANE_RADAR_ADSB_WORKER=1).
// In the default build the macro resolves to 0 (see adsb_worker.h) and only the
// tiny inert stubs at the bottom are compiled -- no task, no queues, and no
// 8 KB worker stack are ever linked.
#if PLANE_RADAR_ADSB_WORKER

#include <Arduino.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <type_traits>

#include "core/adsb_worker_protocol.h"
#include "core/time_math.h"            // core::elapsedMs (fetch duration)
#include "services/adsb_client.h"     // fetchCandidateControlled / discardCandidate
#include "services/adsb_transport.h"  // FetchControl

namespace services::adsb {
namespace {

// --- fixed task/queue sizing -------------------------------------------------

// ESP-IDF FreeRTOS measures the task stack depth in BYTES (not words) and
// StackType_t is a single byte, so kWorkerStackBytes is both the depth argument
// and the exact size of the backing array. ~8 KB is generous headroom for the
// WiFiClientSecure + bounded HTTP/JSON decode frames the fetch runs on THIS task.
constexpr uint32_t kWorkerStackBytes = 8192;
constexpr UBaseType_t kWorkerPriority = 1;   // one low-priority background task
constexpr UBaseType_t kRequestQueueLen = 1;  // strict one-request-in-flight
constexpr UBaseType_t kResultQueueLen = 1;   // strict one-result-in-flight

static_assert(sizeof(StackType_t) == 1,
              "ESP-IDF FreeRTOS: StackType_t is 1 byte and the task stack depth "
              "is expressed in bytes");

// --- queued payloads (small trivially-copyable metadata only) ----------------

// Request (main -> worker): the request id plus the immutable query captured by
// main before the fetch. No snapshot, string, credential, or caller-stack
// pointer ever rides the queue -- the FreeRTOS queue copies it by value.
struct WorkerRequest {
  uint32_t generation;
  double lat;
  double lon;
  float radius_km;
  uint32_t settings_revision;
  uint32_t connectivity_epoch;
};

// Result (worker -> main): the request id, the query metadata echoed back, the
// CandidateResult (a FetchResult plus a CandidateHandle that merely references
// the shared store's inactive slot -- never an inline AircraftSnapshot), and the
// cooperative-cancel observation.
// Under PLANE_RADAR_DIAGNOSTICS, fetch_duration_ms carries the rollover-safe
// fetchCandidateControlled wall time measured on the worker task; absent in
// non-diagnostic builds (the queue backing storage is sizeof-derived and adjusts).
struct WorkerResultMsg {
  uint32_t generation;
  uint32_t settings_revision;
  uint32_t connectivity_epoch;
  CandidateResult candidate;
  bool worker_cancelled;
#if PLANE_RADAR_DIAGNOSTICS
  uint32_t fetch_duration_ms;
#endif
};

static_assert(std::is_trivially_copyable<WorkerRequest>::value,
              "WorkerRequest must be trivially copyable for the queue byte-copy");
static_assert(std::is_trivially_copyable<WorkerResultMsg>::value,
              "WorkerResultMsg must be trivially copyable for the queue byte-copy");

// --- statically allocated task, queues, and coordination state ---------------

// One dedicated spinlock guards every consistent read/mutation of s_proto. It is
// held ONLY around the pure core:: protocol calls -- never across a queue op, the
// fetch, a delay, logging, or any other I/O -- so the pure core stays lock-free
// and no critical section spans blocking work.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
core::WorkerProtocolState s_proto{};  // Idle / generation 0 until workerBegin()
bool s_started = false;

// The long-lived worker task. The stack is 16-byte aligned as the RISC-V ABI
// expects for a task stack base, and is exactly kWorkerStackBytes bytes.
alignas(16) StackType_t s_worker_stack[kWorkerStackBytes];
StaticTask_t s_worker_tcb;
TaskHandle_t s_task = nullptr;

// Two depth-1 queues, both statically backed (no dynamic queue allocation).
uint8_t s_request_storage[kRequestQueueLen * sizeof(WorkerRequest)];
StaticQueue_t s_request_queue_buf;
QueueHandle_t s_request_q = nullptr;

uint8_t s_result_storage[kResultQueueLen * sizeof(WorkerResultMsg)];
StaticQueue_t s_result_queue_buf;
QueueHandle_t s_result_q = nullptr;

// Copy the protocol phase out under the lock (never held across anything else).
core::WorkerPhase currentPhase() {
  core::WorkerPhase phase;
  portENTER_CRITICAL(&s_mux);
  phase = s_proto.phase;
  portEXIT_CRITICAL(&s_mux);
  return phase;
}

// --- FetchControl callbacks (run on the worker task, inside the fetch) --------

// Idle pump: only yield the scheduler so this low-priority task cannot busy-spin
// the core during network WouldBlock waits (which would starve the idle task and
// trip the watchdog). It NEVER calls wifiLoop, the UI, buttons, settings, the
// portal, SNTP, or NVS -- the main task owns all of those and pumps them itself.
void workerIdle(void* /*ctx*/) { vTaskDelay(1); }

// Cooperative cancellation predicate: read the protocol under the lock and report
// whether a pause was requested against exactly this in-flight generation. It
// performs NO Wi-Fi/client/queue/stop call -- cancellation is realized purely by
// fetchCandidateControlled() returning early.
bool workerCancel(void* ctx) {
  const uint32_t generation = *static_cast<const uint32_t*>(ctx);
  bool cancelled;
  portENTER_CRITICAL(&s_mux);
  cancelled = core::workerCancelRequested(s_proto, generation);
  portEXIT_CRITICAL(&s_mux);
  return cancelled;
}

// --- the worker task ---------------------------------------------------------

void workerTask(void* /*param*/) {
  for (;;) {
    WorkerRequest req{};
    if (xQueueReceive(s_request_q, &req, portMAX_DELAY) != pdTRUE) {
      continue;  // portMAX_DELAY blocks indefinitely; a spurious wake just re-waits
    }

    // Claim the exact dispatched generation (Dispatched -> Running) under the
    // lock. A failed claim can only mean a stale/impossible request, which the
    // one-request-in-flight invariant makes unreachable. Rather than silently
    // dropping it -- which could strand a Dispatched coordinator awaiting a result
    // that never comes -- fail CLOSED to Fault (still under the lock) so main
    // observes an explicit fault, then wait for the next request. Nothing was
    // fetched, so there is no candidate to unwind.
    bool claimed;
    portENTER_CRITICAL(&s_mux);
    claimed = core::workerClaim(&s_proto, req.generation);
    if (!claimed) {
      core::workerFault(&s_proto);
    }
    portEXIT_CRITICAL(&s_mux);
    if (!claimed) {
      continue;
    }

    // Drive the bounded, cooperatively-cancellable HTTPS fetch on THIS task. The
    // cancel predicate reads the protocol under the lock for this generation; the
    // idle pump only yields. The worker ALONE fills the store's inactive
    // candidate; main publishes/discards it later after taking the result.
    uint32_t generation = req.generation;
    FetchControl control{};
    control.idle = &workerIdle;
    control.idle_ctx = nullptr;
    control.cancel = &workerCancel;
    control.cancel_ctx = &generation;
#if PLANE_RADAR_DIAGNOSTICS
    const uint32_t fetch_start_ms = millis();
#endif
    const CandidateResult candidate = fetchCandidateControlled(
        req.lat, req.lon, req.radius_km, req.settings_revision, control);
#if PLANE_RADAR_DIAGNOSTICS
    const uint32_t fetch_duration_ms = core::elapsedMs(millis(), fetch_start_ms);
#endif

    // Snapshot the cancel flag and complete the exact generation atomically
    // (Running -> ResultReady). ALWAYS complete -- including a cancelled fetch --
    // so a Dispatched/Running phase is never abandoned.
    bool cancelled;
    bool completed;
    portENTER_CRITICAL(&s_mux);
    cancelled = core::workerCancelRequested(s_proto, generation);
    completed = core::workerComplete(&s_proto, generation);
    portEXIT_CRITICAL(&s_mux);
    if (!completed) {
      // Unreachable under the one-in-flight invariant (we hold Running for this
      // exact generation). Fail CLOSED: discard the candidate (outside the lock),
      // fault, and NEVER enqueue an unclaimed/mismatched result.
      discardCandidate(candidate.handle);
      portENTER_CRITICAL(&s_mux);
      core::workerFault(&s_proto);
      portEXIT_CRITICAL(&s_mux);
      continue;
    }

    WorkerResultMsg msg{};
    msg.generation = generation;
    msg.settings_revision = req.settings_revision;
    msg.connectivity_epoch = req.connectivity_epoch;
    msg.candidate = candidate;
    msg.worker_cancelled = cancelled;
#if PLANE_RADAR_DIAGNOSTICS
    msg.fetch_duration_ms = fetch_duration_ms;
#endif

    // Depth-1 result send. It succeeds under the one-in-flight invariant (main
    // consumed every prior result before dispatching again). On the never-expected
    // failure, fail CLOSED: main will never see this result, so explicitly resolve
    // the candidate's ownership (discard it) and fault -- never leak the candidate
    // and never fabricate quiescence.
    if (xQueueSend(s_result_q, &msg, 0) != pdTRUE) {
      discardCandidate(candidate.handle);
      portENTER_CRITICAL(&s_mux);
      core::workerFault(&s_proto);
      portEXIT_CRITICAL(&s_mux);
    }
  }
}

}  // namespace

// --- lifecycle ---------------------------------------------------------------

bool workerBegin() {
  if (s_started) {
    return true;  // never reset or recreate the long-lived task
  }
  core::workerProtocolInit(&s_proto);  // Idle, generation 0, cancellation cleared

  // Fresh empty queues: startup can never carry a stale request/result/generation.
  s_request_q = xQueueCreateStatic(kRequestQueueLen, sizeof(WorkerRequest),
                                   s_request_storage, &s_request_queue_buf);
  s_result_q = xQueueCreateStatic(kResultQueueLen, sizeof(WorkerResultMsg),
                                  s_result_storage, &s_result_queue_buf);
  if (s_request_q == nullptr || s_result_q == nullptr) {
    return false;  // static storage makes this unreachable; defensive only
  }

  s_task = xTaskCreateStatic(&workerTask, "adsb_worker", kWorkerStackBytes,
                             /*param=*/nullptr, kWorkerPriority, s_worker_stack,
                             &s_worker_tcb);
  if (s_task == nullptr) {
    return false;
  }
  s_started = true;
  return true;
}

bool workerStarted() { return s_started; }

bool workerFaulted() {
  return s_started && currentPhase() == core::WorkerPhase::Fault;
}

WorkerState workerState() {
  if (!s_started) {
    return WorkerState::Disabled;
  }
  switch (currentPhase()) {
    case core::WorkerPhase::Idle:
      return WorkerState::Idle;
    case core::WorkerPhase::Dispatched:
    case core::WorkerPhase::Running:
    case core::WorkerPhase::ResultReady:
      return WorkerState::Busy;
    case core::WorkerPhase::Paused:
      return WorkerState::Paused;
    case core::WorkerPhase::Fault:
      return WorkerState::Fault;
  }
  return WorkerState::Fault;  // unreachable: the switch is exhaustive
}

size_t workerStackHighWaterBytes() {
  if (!s_started || s_task == nullptr) {
    return 0;
  }
  // uxTaskGetStackHighWaterMark returns the minimum free stack in units of
  // StackType_t; scaling keeps this correct/portable even though that is 1 byte
  // on ESP-IDF.
  return static_cast<size_t>(uxTaskGetStackHighWaterMark(s_task)) *
         sizeof(StackType_t);
}

// --- dispatch (main -> worker) ----------------------------------------------

WorkerDispatchResult workerDispatch(const WorkerQuery& query) {
  if (!s_started) {
    return WorkerDispatchResult{WorkerDispatchStatus::Rejected, 0};
  }

  // Capture a nonzero generation under the lock (Idle -> Dispatched). Rejected
  // with no state change while busy, paused, or faulted.
  core::WorkerDispatch dispatched;
  portENTER_CRITICAL(&s_mux);
  dispatched = core::workerDispatch(&s_proto);
  portEXIT_CRITICAL(&s_mux);
  if (!dispatched.accepted) {
    return WorkerDispatchResult{WorkerDispatchStatus::Rejected,
                                dispatched.generation};
  }

  // Post the SAME generation with the immutable query (outside the lock).
  WorkerRequest req{};
  req.generation = dispatched.generation;
  req.lat = query.lat;
  req.lon = query.lon;
  req.radius_km = query.radius_km;
  req.settings_revision = query.settings_revision;
  req.connectivity_epoch = query.connectivity_epoch;
  if (xQueueSend(s_request_q, &req, 0) == pdTRUE) {
    return WorkerDispatchResult{WorkerDispatchStatus::Accepted,
                                dispatched.generation};
  }

  // The generation was advanced under the lock but the request could not be
  // posted (unreachable under the one-in-flight invariant). No safe rollback of
  // the advanced generation exists, so fail CLOSED to Fault -- never pretend idle,
  // which would strand main awaiting a result that can never arrive.
  portENTER_CRITICAL(&s_mux);
  core::workerFault(&s_proto);
  portEXIT_CRITICAL(&s_mux);
  return WorkerDispatchResult{WorkerDispatchStatus::Faulted,
                              dispatched.generation};
}

// --- result (worker -> main) ------------------------------------------------

bool workerTakeResult(WorkerResult* out) {
  if (out == nullptr) {
    return false;
  }
  *out = WorkerResult{};
  out->status = WorkerResultStatus::None;
  if (!s_started) {
    return false;
  }

  WorkerResultMsg msg{};
  if (xQueueReceive(s_result_q, &msg, 0) != pdTRUE) {
    return false;  // queue empty: leave the protocol ResultReady, report None
  }

  // Consume exactly once under the lock (ResultReady -> Idle on a live result, or
  // -> Paused on a cancelled one).
  core::WorkerConsume consumed;
  portENTER_CRITICAL(&s_mux);
  consumed = core::workerConsume(&s_proto, msg.generation);
  portEXIT_CRITICAL(&s_mux);

  switch (consumed) {
    case core::WorkerConsume::Completed:
    case core::WorkerConsume::Quiesced:
      out->status = (consumed == core::WorkerConsume::Completed)
                        ? WorkerResultStatus::Completed
                        : WorkerResultStatus::Quiesced;
      out->generation = msg.generation;
      out->settings_revision = msg.settings_revision;
      out->connectivity_epoch = msg.connectivity_epoch;
      out->candidate = msg.candidate;
      out->worker_cancelled = msg.worker_cancelled;
#if PLANE_RADAR_DIAGNOSTICS
      out->fetch_duration_ms = msg.fetch_duration_ms;
#endif
      return true;
    case core::WorkerConsume::Rejected:
      // Unreachable under the one-in-flight invariant (the dequeued generation is
      // exactly the ResultReady one). Fail CLOSED: discard the candidate (outside
      // the lock), fault, and report an EXPLICIT Faulted status -- never disguise a
      // dequeued-but-unconsumable message as an ordinary empty None. A message WAS
      // taken, so return true and let main act on the fault.
      discardCandidate(msg.candidate.handle);
      portENTER_CRITICAL(&s_mux);
      core::workerFault(&s_proto);
      portEXIT_CRITICAL(&s_mux);
      *out = WorkerResult{};
      out->status = WorkerResultStatus::Faulted;
      out->generation = msg.generation;
      return true;
  }
  return false;
}

// --- pause / resume ---------------------------------------------------------

void workerRequestPause() {
  if (!s_started) {
    return;
  }
  portENTER_CRITICAL(&s_mux);
  core::workerRequestPause(&s_proto);
  portEXIT_CRITICAL(&s_mux);
}

bool workerQuiesced() {
  if (!s_started) {
    return true;  // no worker task exists: nothing can own shared state
  }
  bool quiesced;
  portENTER_CRITICAL(&s_mux);
  quiesced = core::workerQuiesced(s_proto);
  portEXIT_CRITICAL(&s_mux);
  return quiesced;
}

bool workerResume() {
  if (!s_started) {
    return false;
  }
  bool resumed;
  portENTER_CRITICAL(&s_mux);
  resumed = core::workerResume(&s_proto);
  portEXIT_CRITICAL(&s_mux);
  return resumed;
}

}  // namespace services::adsb

#else  // PLANE_RADAR_ADSB_WORKER

// Default (worker-free) firmware: inert definitions that are a link-safety net
// for any compile-time-guarded caller. With the canonical `#if PLANE_RADAR_ADSB_
// WORKER` gating (see adsb_worker.h), the default main/wifi_setup never name these
// symbols, so they are unreferenced and dead-stripped under -Os + --gc-sections --
// no task, queue, or 8 KB stack, and ZERO worker symbols in the default ELF.
// (workerEnabled() itself is the inline constexpr in the header and is
// intentionally NOT defined here.)
namespace services::adsb {

bool workerBegin() { return false; }
bool workerStarted() { return false; }
bool workerFaulted() { return false; }
WorkerState workerState() { return WorkerState::Disabled; }
size_t workerStackHighWaterBytes() { return 0; }

WorkerDispatchResult workerDispatch(const WorkerQuery& /*query*/) {
  return WorkerDispatchResult{WorkerDispatchStatus::Rejected, 0};
}

bool workerTakeResult(WorkerResult* out) {
  if (out != nullptr) {
    *out = WorkerResult{};
    out->status = WorkerResultStatus::None;
  }
  return false;
}

void workerRequestPause() {}

bool workerQuiesced() { return true; }  // no worker: trivially quiescent

bool workerResume() { return false; }

}  // namespace services::adsb

#endif  // PLANE_RADAR_ADSB_WORKER
