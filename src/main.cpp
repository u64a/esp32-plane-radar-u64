/**
 * Plane Radar — Wi-Fi setup, then a live ADS-B radar on the round GC9A01 display.
 *
 * Phase 6 runtime: every rendered frame is a deterministic RadarDisplayModel
 * built from the freshness view, the published snapshot, the actual Wi-Fi
 * status, and a 500 ms Loading activity phase. Rendering is edge/key driven,
 * ADS-B polling is completion-relative with backoff, and a Wi-Fi drop pauses
 * fetches without hiding the radar.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "core/frame_render.h"
#include "core/poll_policy.h"
#include "core/radar_data_state.h"
#include "core/time_math.h"
#include "core/time_trust.h"
#include "hardware/display.h"
#include "runtime_diagnostics.h"
#include "services/adsb_client.h"
#include "services/adsb_fetch.h"
#include "services/adsb_worker.h"
#include "services/radar_location.h"
#include "services/settings_events.h"
#include "services/timekeeper.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

// Canonical Arduino-free policies (config.h static_asserts its mirrors match).
constexpr core::AdsbPollPolicy kPollPolicy = core::kDefaultAdsbPollPolicy;
constexpr core::RadarFreshnessPolicy kFreshnessPolicy =
    core::kDefaultRadarFreshnessPolicy;

// Deterministic Loading activity phase: three 500 ms buckets, rollover-safe by
// construction (unsigned millis()).
uint8_t loadingPhase(uint32_t now_ms) {
  return static_cast<uint8_t>((now_ms / 500U) % 3U);
}

bool wifiConnected() { return wifiLinkUp(); }

// --- runtime state -----------------------------------------------------------

core::RadarDataState g_data = {};
core::AdsbPollState g_poll = {};

core::FrameRenderKey g_last_key = {};
bool g_has_last_key = false;
bool g_force_redraw = false;
bool g_link_up = false;
bool g_owns_display = false;

#if PLANE_RADAR_ADSB_WORKER
// Worker-only latch: an internal worker fault (impossible-state) halts all ADS-B
// dispatch. Compiled only into the worker firmware.
bool g_worker_faulted = false;
#endif

#if PLANE_RADAR_DIAGNOSTICS
// Diagnostics-only: last fetch wall-clock duration (sync path: measured around
// fetchCandidate; worker path: carried through the queue from the worker task).
// Zero-initialized; overwritten on every accepted/completed fetch. Stale state
// is harmless (overwritten before the next diagnostic log line).
uint32_t g_diag_last_fetch_ms = 0;
#endif

// --- rendering ---------------------------------------------------------------

// Advance freshness for `now`, then render only when an event forced it or the
// frame key changed. Exactly one full-frame draw per redraw (one framebuffer,
// no large copies): the model is a few machine words passed by value.
void renderIfNeeded(uint32_t now_ms) {
  core::radarDataAdvance(&g_data, now_ms, kFreshnessPolicy);
  const core::RadarDataView view = core::radarDataView(g_data, now_ms);
  const services::adsb::SnapshotView snapshot =
      services::adsb::publishedSnapshot();
  const bool wifi = wifiConnected();
  const uint8_t phase = loadingPhase(now_ms);

  const core::FrameRenderKey key = core::frameRenderKey(
      view.mode, view.age_seconds, view.settings_revision,
      snapshot.settings_revision, snapshot.count, wifi, phase);

  if (!g_force_redraw && g_has_last_key &&
      core::frameRenderKeyEqual(key, g_last_key)) {
    return;  // steady frame: skip the redraw (Live/Offline stay quiet)
  }

  const ui::RadarDisplayModel model{view, snapshot, wifi, phase};
#if PLANE_RADAR_DIAGNOSTICS
  const uint32_t render_start_us = micros();
#endif
  ui::radarDisplayDraw(model);
#if PLANE_RADAR_DIAGNOSTICS
  {
    const uint32_t render_us = core::elapsedMicros(micros(), render_start_us);
    const ui::RenderDiagnostics rdiag = ui::radarDisplayLastDiagnostics();
    Serial.printf(
        "diag: render_us=%lu runway_us=%lu mode=%u age=%lus "
        "runways=%d sprite=%d\n",
        static_cast<unsigned long>(render_us),
        static_cast<unsigned long>(rdiag.runway_us),
        static_cast<unsigned>(view.mode),
        static_cast<unsigned long>(view.age_seconds),
        static_cast<int>(rdiag.runways_enabled),
        static_cast<int>(rdiag.used_sprite));
  }
#endif
  g_last_key = key;
  g_has_last_key = true;
  g_force_redraw = false;
}

// --- settings events ---------------------------------------------------------

// Consume any pending effective-query and visual-only settings events and apply
// their side effects. A query change resets freshness to the new revision and
// forces one immediate fetch; a visual-only change forces a redraw only. Both
// are single-shot latches, so calling this repeatedly per loop is safe.
void applyPendingSettings(uint32_t now_ms) {
  if (services::settings::consumeQueryChange()) {
    const uint32_t revision = services::settings::revision();
    core::radarDataRevisionChanged(&g_data, revision, now_ms);
    core::adsbSettingsChanged(&g_poll);
    // The published snapshot still carries the previous revision, so the model's
    // revision mismatch hides its now-stale targets until a new publish.
    g_force_redraw = true;
  }
  if (services::settings::consumeVisualChange()) {
    // Units / runway overlay: redraw only — no revision, freshness, or backoff
    // change.
    g_force_redraw = true;
  }
}

// --- range tap ---------------------------------------------------------------

void onRangeTap() {
  ui::radar::rangeNext();  // marks an effective query change only on a real change
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  PLANE_RADAR_LOG_I("Range: %s (outer ~%.0f km)\n", range_label,
                    ui::radar::rangeCurrent().outer_km);
}

// --- ADS-B fetch flow --------------------------------------------------------

const char* outcomeName(core::PollOutcome outcome) {
  switch (outcome) {
    case core::PollOutcome::Success:
      return "success";
    case core::PollOutcome::Transient:
      return "transient";
    case core::PollOutcome::RateLimited:
      return "rate-limited";
    case core::PollOutcome::Permanent:
      return "permanent";
    case core::PollOutcome::Obsolete:
      return "obsolete";
  }
  return "unknown";
}

const char* publishName(services::adsb::PublishResult result) {
  switch (result) {
    case services::adsb::PublishResult::Published:
      return "published";
    case services::adsb::PublishResult::ObsoleteRevision:
      return "obsolete-rev";
    case services::adsb::PublishResult::InvalidHandle:
      return "invalid-handle";
    case services::adsb::PublishResult::NoCandidate:
      return "no-candidate";
  }
  return "none";
}

// Shared ADS-B completion: consume any settings/range change that landed during
// the fetch, then run the revision gate, authenticated cert-floor ratchet,
// publish classification, Wi-Fi-level + disconnect-flap handling, completion-
// relative backoff, payload-free logging, freshness success, and redraw. Used
// identically by the synchronous fetch and the async worker result so both share
// one behavior. `disconnect_seq_before` is the connectivity epoch captured at
// dispatch (echoed back on the worker result).
void finishAdsbFetch(const services::adsb::CandidateResult& candidate,
                     uint32_t disconnect_seq_before) {
  // Re-consume a range tap or portal save that landed during the fetch so the
  // revision gate below reflects the newest query.
  if (wifiConsumeRangeTap()) {
    onRangeTap();
  }
  applyPendingSettings(millis());

  const uint32_t completed_ms = millis();
  const bool wifi_up = wifiConnected();
  const uint32_t current_revision = services::settings::revision();
  // Re-read the disconnect sequence after all post-fetch pumps: a change means a
  // Wi-Fi flap happened at some point during the fetch (a "flap"), regardless of
  // the current level-based link state.
  const bool disconnect_flap = core::disconnectSeqChanged(
      disconnect_seq_before, wifiDisconnectSeq());

  services::adsb::PublishResult publish_result =
      services::adsb::PublishResult::NoCandidate;
  bool published_success = false;
  core::PollOutcome outcome;
  if (candidate.fetch.outcome == services::adsb::FetchOutcome::Ok) {
    // A complete, CA + hostname + date verified response arrived (runFetch only
    // returns Ok after a full 200 body decode). Offer to ratchet the persisted
    // floor using the CA-signed peer leaf notBefore stamped on the result -- an
    // authenticated value an unauthenticated NTP attacker cannot choose (NOT
    // SNTP-derived time). This counts even if publication is skipped for a stale
    // revision, because the fetch itself was fully verified. It is NEVER gated on
    // a mere TCP/TLS connect.
    services::timekeeper::noteVerifiedCertFloor(
        candidate.fetch.authenticated_cert_not_before_unix, completed_ms);
    publish_result =
        services::adsb::publishCandidate(candidate.handle, current_revision);
    outcome = services::adsb::pollOutcomeForPublish(publish_result);
    if (publish_result == services::adsb::PublishResult::Published) {
      // Only a real publication advances freshness / last-success.
      published_success = true;
      core::radarDataSuccess(&g_data, current_revision, completed_ms);
    }
  } else {
    outcome = services::adsb::pollOutcomeFor(candidate.fetch.outcome);
  }

  // A Wi-Fi drop still down at completion is a pause, not an ADS-B failure.
  outcome = core::effectiveOutcomeAtCompletion(outcome, wifi_up);

  // A Wi-Fi flap during the fetch (edge-detected, even if already reconnected)
  // that did not yield a published success and whose outcome is link-sensitive
  // is likewise a pause (Obsolete), so a spurious drop never advances backoff.
  outcome = services::adsb::effectiveOutcomeAfterFlap(
      candidate.fetch.outcome, outcome, published_success, disconnect_flap);
  if (disconnect_flap) {
    // Raise a newer forced-immediate request BEFORE adsbFetchCompleted so the
    // in-flight sequence logic preserves the immediate latch across this (older)
    // completion, forcing exactly one refresh even though Wi-Fi may already have
    // reconnected. If the link is still down, the latch simply survives until a
    // later loop reconnects and services the immediate fetch.
    core::adsbSettingsChanged(&g_poll);
  }

  core::adsbFetchCompleted(&g_poll, completed_ms, kPollPolicy, outcome,
                           candidate.fetch.retry_after_present,
                           candidate.fetch.retry_after_ms);

  // Concise, payload-free outcome line (no host/URL/body/secrets).
  PLANE_RADAR_LOG_I(
      "adsb: outcome=%s status=%d bytes=%lu count=%u rev=%lu publish=%s "
      "flap=%d next=%lums\n",
      outcomeName(outcome), candidate.fetch.http_status,
      static_cast<unsigned long>(candidate.fetch.bytes_received),
      candidate.fetch.aircraft_count,
      static_cast<unsigned long>(current_revision), publishName(publish_result),
      disconnect_flap ? 1 : 0,
      static_cast<unsigned long>(g_poll.next_interval_ms));

#if PLANE_RADAR_DIAGNOSTICS
  // Self-contained diagnostic line (emitted even when PLANE_RADAR_LOG_LEVEL=0).
  // Heap semantics: heap_min is since boot; largest_block is a post-fetch
  // snapshot that reflects current fragmentation, not the minimum since boot.
  // worker_hwm (when present) is meaningful only after representative fetch load.
  Serial.printf(
      "diag: fetch_ms=%lu outcome=%s bytes=%lu next_ms=%lu "
      "heap_free=%lu heap_min=%lu heap_max=%lu",
      static_cast<unsigned long>(g_diag_last_fetch_ms),
      outcomeName(outcome),
      static_cast<unsigned long>(candidate.fetch.bytes_received),
      static_cast<unsigned long>(g_poll.next_interval_ms),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getMinFreeHeap()),
      static_cast<unsigned long>(ESP.getMaxAllocHeap()));
#if PLANE_RADAR_ADSB_WORKER
  if (services::adsb::workerStarted()) {
    Serial.printf(" worker_hwm=%lu",
                  static_cast<unsigned long>(
                      services::adsb::workerStackHighWaterBytes()));
  }
#endif
  Serial.printf("\n");
#endif  // PLANE_RADAR_DIAGNOSTICS

  g_force_redraw = true;  // fetch completion / publication is a render trigger
}

// Synchronous ADS-B poll (default firmware, and the worker build's fallback when
// static task creation failed). Captures the query atomically, performs the
// blocking fetch on the main task, pumps the controllable-latency side effects,
// then runs the shared completion.
void serviceAdsbSync() {
  // Defense in depth: hold the (possibly immediate) fetch latch pending until
  // trusted UTC is established this boot. adsbFetchAllowed() does NOT mutate poll
  // state, so a pending immediate-fetch survives untrusted time and fires on the
  // first loop after trust. ADS-B connections are forbidden before trusted UTC.
  if (!core::adsbFetchAllowed(core::adsbFetchDue(g_poll, millis()),
                              services::timekeeper::trusted())) {
    return;
  }

  // Consume pending settings first so the capture below reflects the newest query.
  applyPendingSettings(millis());

  // Atomically capture the query parameters and the revision they belong to.
  const double lat = services::location::lat();
  const double lon = services::location::lon();
  const float fetch_km = ui::radar::fetchRadiusKm();
  const uint32_t query_revision = services::settings::revision();

  // Snapshot the Wi-Fi disconnect sequence before the (blocking) fetch so a drop
  // that occurs entirely inside uninterruptible DNS/TCP/TLS is observable
  // afterward even if the stack auto-reconnected in the meantime.
  const uint32_t disconnect_seq_before = wifiDisconnectSeq();

  core::adsbFetchStarted(&g_poll);
#if PLANE_RADAR_DIAGNOSTICS
  const uint32_t sync_fetch_start_ms = millis();
#endif
  const services::adsb::CandidateResult candidate =
      services::adsb::fetchCandidate(lat, lon, fetch_km, query_revision);
#if PLANE_RADAR_DIAGNOSTICS
  g_diag_last_fetch_ms = core::elapsedMs(millis(), sync_fetch_start_ms);
#endif

  // Before publication, run the controllable-latency side effects (the main task
  // was blocked in the fetch) so a range tap or portal save is visible.
  wifiLoop();
  finishAdsbFetch(candidate, disconnect_seq_before);
}

#if PLANE_RADAR_ADSB_WORKER
// Single fail-closed reaction to any observed worker fault: a Faulted dispatch, a
// Faulted result, or a result-LESS fault seen on an empty queue (claim reject,
// complete reject, or result-queue send failure -- adapter paths that fault the
// coordinator WITHOUT enqueuing a result). Clear any accepted in-flight poll
// WITHOUT a normal outcome (safe no-op if none), halt further dispatch, log once,
// and force a redraw. Never a completion; never a switch to synchronous while the
// long-lived task exists. Reused by every fault site so unwind/logging cannot
// diverge.
void handleWorkerFault() {
  core::adsbFetchAbortedForPause(&g_poll);
  if (!g_worker_faulted) {
    g_worker_faulted = true;
    PLANE_RADAR_LOG_E("adsb: worker faulted; halting worker fetches\n");
  }
  g_force_redraw = true;
}

// Asynchronous ADS-B dispatch (worker firmware). Non-blocking: it captures the
// immutable query and hands it to the worker, then returns. The result is
// consumed later by pumpWorkerResult(). Exactly one adsbFetchStarted() is
// recorded, and only for an Accepted dispatch; Rejected (busy/paused) and Faulted
// never advance poll state. A worker fault halts dispatch and never falls back to
// synchronous fetching while the long-lived task exists.
void serviceAdsbAsync() {
  if (g_worker_faulted) {
    return;
  }
  if (g_poll.fetch_in_flight) {
    return;  // a dispatch is outstanding; wait for its result before another
  }
  if (!core::adsbFetchAllowed(core::adsbFetchDue(g_poll, millis()),
                              services::timekeeper::trusted())) {
    return;
  }
  applyPendingSettings(millis());

  services::adsb::WorkerQuery query;
  query.lat = services::location::lat();
  query.lon = services::location::lon();
  query.radius_km = ui::radar::fetchRadiusKm();
  query.settings_revision = services::settings::revision();
  query.connectivity_epoch = wifiDisconnectSeq();

  const services::adsb::WorkerDispatchResult dispatched =
      services::adsb::workerDispatch(query);
  switch (dispatched.status) {
    case services::adsb::WorkerDispatchStatus::Accepted:
      core::adsbFetchStarted(&g_poll);  // exactly once, only for Accepted
      break;
    case services::adsb::WorkerDispatchStatus::Rejected:
      break;  // busy/paused: non-blocking, no duplicate fetch, no poll advance
    case services::adsb::WorkerDispatchStatus::Faulted:
      handleWorkerFault();  // halt dispatch; never switch to synchronous
      break;
  }
}

// Drain at most one worker result (worker firmware). Non-blocking. Main ALONE
// publishes/discards the candidate here. Called before/after the top-level
// wifiLoop() and while a status screen owns the panel so a deferred Configure/
// Erase can drive the worker to Paused.
void pumpWorkerResult() {
  services::adsb::WorkerResult result;
  if (!services::adsb::workerTakeResult(&result)) {
    // Empty queue. Some adapter fault paths (claim reject, complete reject,
    // result-queue send failure) fault the coordinator WITHOUT enqueuing a
    // result, so a result-less fault must still be observed here rather than
    // silently wedging an accepted in-flight poll. The pure classifier keeps this
    // decision native-testable.
    if (services::adsb::workerPollActionForNoResult(
            services::adsb::workerFaulted()) ==
        services::adsb::WorkerPollAction::FaultHalt) {
      handleWorkerFault();
    }
    return;
  }
  switch (services::adsb::workerPollActionFor(result.status)) {
    case services::adsb::WorkerPollAction::Publish:
      // Publish via the shared completion, using the connectivity epoch captured
      // at dispatch (echoed back on the result) for flap detection.
#if PLANE_RADAR_DIAGNOSTICS
      g_diag_last_fetch_ms = result.fetch_duration_ms;
#endif
      finishAdsbFetch(result.candidate, result.connectivity_epoch);
      break;
    case services::adsb::WorkerPollAction::DiscardAndAbort:
      // Cancelled for a pause: discard (idempotent) and unwind the in-flight poll
      // WITHOUT recording a completion, retaining backoff/immediate state. Never
      // publish or ratchet time/NVS on this path, even after a late-cancelled OK.
      services::adsb::discardCandidate(result.candidate.handle);
      core::adsbFetchAbortedForPause(&g_poll);
      g_force_redraw = true;
      break;
    case services::adsb::WorkerPollAction::FaultHalt:
      // Impossible-state fault surfaced on the take path: the same fail-closed
      // unwind as a result-less fault (candidate already discarded by the adapter).
      handleWorkerFault();
      break;
    case services::adsb::WorkerPollAction::Ignore:
      break;
  }
}

// Main-owned network-work hooks: the Wi-Fi controller calls these (all on the main
// task, non-blocking) to defer Configure/Erase until the worker is quiesced. The
// quiesced hook is a pure query -- main resolves any pending result in
// pumpWorkerResult() before the controller evaluates it (pumped before wifiLoop
// and while owning the panel), so worker Paused implies ownership fully resolved.
void adsbWorkerPauseHook(void*) { services::adsb::workerRequestPause(); }
bool adsbWorkerQuiescedHook(void*) { return services::adsb::workerQuiesced(); }
void adsbWorkerResumeHook(void*) { services::adsb::workerResume(); }
#endif  // PLANE_RADAR_ADSB_WORKER

}  // namespace

void setup() {
  PLANE_RADAR_SERIAL_BEGIN(115200);
  PLANE_RADAR_LOG_I("\n");
  PLANE_RADAR_LOG_I("Plane Radar\n");

  wifiControllerInit();
  displayInit();
  const bool frame_buffered = ui::radarDisplayPrepareFrame();
  PLANE_RADAR_LOG_I("radar: rendering mode: %s\n",
                    frame_buffered ? "frame sprite" : "direct draw");
  services::location::init();
  ui::radar::rangeInit();
  services::adsb::setPollFn(wifiLoop);
#if PLANE_RADAR_ADSB_WORKER
  // The single framebuffer is already allocated (radarDisplayPrepareFrame above).
  // Start the long-lived static worker and register the network-work hooks BEFORE
  // wifiBootConnect() so first-boot / open-session pause actions are honored. If
  // static task creation fails, retain the synchronous path: no hooks are
  // registered and the loop falls back on serviceAdsbSync() because
  // workerStarted() stays false (sync never runs concurrently with a started or
  // faulted worker).
  if (services::adsb::workerBegin()) {
    const WifiNetworkWorkHooks hooks{&adsbWorkerPauseHook,
                                     &adsbWorkerQuiescedHook,
                                     &adsbWorkerResumeHook, nullptr};
    wifiSetNetworkWorkHooks(hooks);
  } else {
    PLANE_RADAR_LOG_E("adsb: static worker start failed; using synchronous path\n");
  }
#endif
  // Seed the trusted-time service (reads/validates the persisted floor, registers
  // the SNTP callback). Starts UNTRUSTED; ADS-B is gated until a fresh sample.
  services::timekeeper::init();

  // Register the filtered Wi-Fi event callbacks (STA GOT_IP + DISCONNECTED) BEFORE
  // any network setup so a mid-fetch disconnect that auto-reconnects is counted
  // and every connect generation observes a fresh link-up.
  wifiRegisterEventHandlers();

  // Boot connect (blocks only for the connecting UI); opens secure setup
  // automatically on first boot. May mutate the settings revision.
  wifiBootConnect();

  // Always activate the radar frame, even if Wi-Fi is down.
  const uint32_t now_ms = millis();
  core::radarDataStart(&g_data, now_ms);
  core::radarDataRevisionChanged(&g_data, services::settings::revision(),
                                 now_ms);  // adopt the boot-time revision
  // Adopt boot-time settings latches without re-triggering them on loop 1.
  services::settings::consumeQueryChange();
  services::settings::consumeVisualChange();
  core::adsbRadarDisplayed(&g_poll);  // radar visible; forces the first fetch
  g_link_up = wifiLinkUp();
  g_owns_display = wifiOwnsDisplay();
  g_force_redraw = true;
  if (!g_owns_display) {
    renderIfNeeded(now_ms);  // render a real RadarDisplayModel (Loading)
  }
}

void loop() {
  // Worker firmware: pump results BEFORE the top-level wifiLoop so a pending
  // Quiesced result is taken + discarded (worker -> Paused) before the controller
  // evaluates a deferred Configure/Erase in this same iteration.
#if PLANE_RADAR_ADSB_WORKER
  pumpWorkerResult();
#endif

  // The controller owns the whole Wi-Fi lifecycle: boot connect, the secure setup
  // portal, the credential transaction, background reconnects, and the button.
  wifiLoop();

  // ... and AFTER the top-level wifiLoop, so a result that arrived during it is
  // resolved promptly.
#if PLANE_RADAR_ADSB_WORKER
  pumpWorkerResult();
#endif

  const uint32_t now_ms = millis();
  const bool owns = wifiOwnsDisplay();

  // A range tap latch survives blocking work; apply it regardless of ownership so
  // the query revision is current when the radar reclaims the panel.
  if (wifiConsumeRangeTap()) {
    onRangeTap();
  }
  applyPendingSettings(now_ms);

  if (owns) {
    // Provisioning / boot-connect / gesture prompts / a pending network-work wait
    // own the panel: skip ADS-B and the radar redraw so status screens are not
    // overdrawn. Keep pumping worker results so a deferred Configure/Erase can
    // still drive the worker to Paused while a status screen is up.
#if PLANE_RADAR_ADSB_WORKER
    pumpWorkerResult();
#endif
    g_link_up = wifiLinkUp();
    g_owns_display = true;
    delay(10);
    return;
  }

  if (g_owns_display) {
    g_force_redraw = true;  // reclaim the panel from a status screen
    g_owns_display = false;
  }

  // Trusted-time state machine (SNTP only while a real STA link is up).
  services::timekeeper::update(wifiLinkUp(), now_ms);

  // Controller-forced immediate fetch (restored / newly committed STA success).
  if (wifiConsumeImmediateFetch()) {
    core::adsbSettingsChanged(&g_poll);
  }

  // Link edge: on a plain reconnect force exactly one immediate fetch (streak
  // intact) and redraw; a drop keeps the radar with NO WIFI and ages normally.
  const bool up = wifiLinkUp();
  if (up != g_link_up) {
    g_force_redraw = true;
    if (up) {
      core::adsbSettingsChanged(&g_poll);
    }
    g_link_up = up;
  }

  if (up) {
#if PLANE_RADAR_ADSB_WORKER
    if (services::adsb::workerStarted()) {
      serviceAdsbAsync();  // dispatch when due; result pumped elsewhere
    } else {
      serviceAdsbSync();  // worker creation failed: safe synchronous fallback
    }
#else
    serviceAdsbSync();  // fetch when due; internally pumps wifiLoop
#endif
  }

  // Resolve any result the dispatch/fetch produced, then recheck ownership: the
  // sync fallback's internal wifiLoop() (or a button gesture drained this loop)
  // can acquire the panel (e.g. a configure gesture). Skip the final radar render
  // if it changed so a status screen is never overdrawn; the next loop forces a
  // redraw when ownership is released.
#if PLANE_RADAR_ADSB_WORKER
  pumpWorkerResult();
#endif
  if (wifiOwnsDisplay()) {
#if PLANE_RADAR_ADSB_WORKER
    pumpWorkerResult();
#endif
    g_link_up = wifiLinkUp();
    g_owns_display = true;
    delay(10);
    return;
  }

  renderIfNeeded(millis());
  delay(10);
}
