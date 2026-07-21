#include "services/adsb_client.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "config.h"
#include "core/cert_time.h"
#include "core/coordinates.h"
#include "core/time_math.h"
#include "services/adsb_ca_bundle.h"
#include "services/adsb_fetch.h"
#include "services/adsb_transport_esp.h"
#include "services/settings_events.h"
#include "services/timekeeper.h"

namespace services::adsb {

namespace {

constexpr float kKmPerNm = 1.852f;

// Two-slot double-buffered publication store. aircraftList() reads the active
// snapshot while a fetch fills the inactive one; publication is a single index
// switch. Replaces the former hand-rolled s_snapshots/s_active/s_revision trio.
SnapshotStore s_store;

// Fixed per-fetch workspace (documented in the README RAM budget).
uint8_t s_http_scratch[kTransportScratchBytes];
char s_http_line[kDefaultHttpLimits.header_line_bytes + 1];
char s_object_buffer[kObjectBufferBytes];
alignas(alignof(std::max_align_t)) uint8_t s_json_arena[kJsonArenaBytes];
float s_distances[kMaxAircraft];
uint16_t s_ordinals[kMaxAircraft];

PollFn s_poll_fn = nullptr;

FetchResult makeFailure(FetchOutcome outcome) {
  FetchResult result{};
  result.outcome = outcome;
  result.http_status = -1;
  result.bytes_received = 0;
  result.retry_after_present = false;
  result.retry_after_ms = 0;
  result.aircraft_count = 0;
  result.authenticated_cert_not_before_unix = 0;  // no verified cert on a failure
  return result;
}

FetchOutcome connectFailure(ConnectOutcome outcome) {
  switch (outcome) {
    case ConnectOutcome::DnsFailure:
      return FetchOutcome::DnsFailure;
    case ConnectOutcome::Timeout:
      return FetchOutcome::Timeout;
    case ConnectOutcome::TlsFailure:
    default:
      return FetchOutcome::TlsFailure;
  }
}

// Synchronous idle adapter: routes the FetchControl idle pump back to the
// file-scope poll hook (wifiLoop) so the synchronous fetch behaves exactly as it
// did before the control seam existed.
void syncPollAdapter(void* /*ctx*/) {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

// Shared bounded-HTTPS fetch used by both the synchronous path and the future
// worker. It fills the caller-owned inactive snapshot `out` and returns the
// FetchResult; it never publishes. Idle waits and cooperative cancellation are
// driven through `control`: a synchronous caller passes a control whose `cancel`
// is null (never cancels), so all cancellation checkpoints below are no-ops and
// the behavior is byte-for-byte identical to the pre-control fetch. A worker
// caller passes a control whose predicate aborts the fetch on requestPause.
// Cancellation is checked before DNS/connect, immediately after connect,
// before/after the peer-certificate date check, before the HTTP send, in the send
// retries and the response-decode idle/refill loops, and before returning
// success. The client is ALWAYS stop()ped from this executing task.
FetchResult realFetchImpl(const FetchRequest& request, AircraftSnapshot& out,
                          const FetchControl& control) {
  if (!core::coordinatesValid(request.lat, request.lon) ||
      !std::isfinite(request.radius_km) || request.radius_km <= 0.0f) {
    return makeFailure(FetchOutcome::ParseError);
  }
  const float dist_nm = request.radius_km / kKmPerNm;

  char http_request[256];
  const int req_len = std::snprintf(
      http_request, sizeof(http_request),
      "GET /api/v3/lat/%.6f/lon/%.6f/dist/%.1f HTTP/1.1\r\n"
      "Host: %s\r\n"
      "Accept: application/json\r\n"
      "Accept-Encoding: identity\r\n"
      "Connection: close\r\n\r\n",
      request.lat, request.lon, static_cast<double>(dist_nm), config::kAdsbHost);
  if (req_len <= 0 || req_len >= static_cast<int>(sizeof(http_request))) {
    return makeFailure(FetchOutcome::ParseError);
  }

  // Cancellation checkpoint: before any DNS/connect. Nothing is open yet, so a
  // cancelled fetch simply returns without touching the network.
  if (fetchControlCancelled(control)) {
    return makeFailure(FetchOutcome::TransportFailure);
  }

  WiFiClientSecure client;
  // Defense in depth: never open a connection until trusted UTC is established
  // this boot. serviceAdsb() already gates on trusted time, but guarding the
  // transport too means no future call site can bypass the time requirement.
  if (!services::timekeeper::trusted()) {
    return makeFailure(FetchOutcome::TimeUnavailable);
  }

  // No setInsecure(): the pinned CA bundle is passed straight into the verified
  // IP+host connect overload so mbedTLS enforces the CA chain and the hostname.
  const ConnectOutcome connected =
      espTlsConnect(client, config::kAdsbHost, config::kAdsbPort,
                    config::kAdsbConnectTimeoutMs, kAdsbCaBundle);
  if (connected != ConnectOutcome::Connected) {
    client.stop();
    return makeFailure(connectFailure(connected));
  }

  // Cancellation checkpoint: immediately after connect, before any further work.
  if (fetchControlCancelled(control)) {
    client.stop();
    return makeFailure(FetchOutcome::TransportFailure);
  }

  // Close the callback TOCTOU / pre-handshake staleness: obtain the DERIVED
  // trusted timestamp AGAIN here -- AFTER DNS + TCP + the TLS handshake and
  // immediately before the peer notBefore/notAfter check -- never a value
  // captured before the connection. If trust expired or was revoked while the
  // (blocking) handshake ran, or no derived time is available, stop now and
  // return TimeUnavailable BEFORE any HTTP is sent. No certificate date decision
  // may use a timestamp captured before the connection existed.
  const int64_t trusted_now_unix = services::timekeeper::nowUnix();
  if (trusted_now_unix <= 0) {
    client.stop();
    return makeFailure(FetchOutcome::TimeUnavailable);
  }

  // Cancellation checkpoint: before the peer-certificate date validation.
  if (fetchControlCancelled(control)) {
    client.stop();
    return makeFailure(FetchOutcome::TransportFailure);
  }

  // The handshake verified the CA chain + hostname, but the pinned SDK builds
  // mbedTLS without notBefore/notAfter enforcement for ANY node. Explicitly walk
  // the full retained peer chain (leaf + intermediates/cross-certs) and check
  // every node's validity against the just-derived trusted UTC, failing closed
  // (missing / not yet valid / expired / malformed node, or an over-long chain)
  // BEFORE sending any HTTP request, so a wrong date anywhere in the chain never
  // reaches the parser or the send path. The same seam returns the CA-signed leaf
  // notBefore epoch ONLY when the whole chain is valid, captured here BEFORE the
  // send as the authenticated persisted-floor candidate (an unauthenticated NTP
  // attacker cannot choose it, and an invalid intermediate forces it to 0); it is
  // only stamped onto a complete Ok below.
  const core::CertVerification peer =
      espVerifyPeerCertValidity(client, trusted_now_unix);
  if (core::certValidityBlocksFetch(peer.validity)) {
    client.stop();
    return makeFailure(FetchOutcome::CertInvalid);
  }
  const int64_t authenticated_leaf_not_before_unix = peer.not_before_unix;

  // Cancellation checkpoint: after cert validation, before the HTTP send.
  if (fetchControlCancelled(control)) {
    client.stop();
    return makeFailure(FetchOutcome::TransportFailure);
  }

  EspMillisClock clock;
  FetchControlIdle idle(control);
  // One cumulative request/response budget covers BOTH the request send and the
  // HTTP response decode. It starts before the send; the send may consume part
  // of it, and only what remains is handed to the decoder -- send and response
  // do NOT each get a fresh kAdsbOverallTimeoutMs.
  const uint32_t overall_started = clock.nowMs();
  if (!espSendAll(client, reinterpret_cast<const uint8_t*>(http_request),
                  static_cast<size_t>(req_len), clock, idle,
                  config::kAdsbOverallTimeoutMs)) {
    // espSendAll also returns false on a cooperative cancel in its retry loop.
    client.stop();
    return makeFailure(FetchOutcome::Timeout);
  }

  // Subtract the time the send consumed; if the budget is already spent, the
  // response has no time left.
  const uint32_t response_budget_ms = core::remainingBudgetMs(
      clock.nowMs(), overall_started, config::kAdsbOverallTimeoutMs);
  if (response_budget_ms == 0) {
    client.stop();
    return makeFailure(FetchOutcome::Timeout);
  }

  EspTlsByteSource source(client);
  const ParseOptions parse_options{kDefaultParseLimits,
                                   config::kAdsbShowGroundAircraft};
  // Overall budget is the send-adjusted remainder; the inactivity (stall) budget
  // stays response-local (reset per received byte inside the decoder).
  const HttpDeadlines deadlines{response_budget_ms,
                                config::kAdsbStallTimeoutMs};

  FetchWorkspace workspace{};
  workspace.http = HttpWorkspace{s_http_scratch, sizeof(s_http_scratch),
                                 s_http_line, sizeof(s_http_line)};
  workspace.object_buffer = s_object_buffer;
  workspace.object_capacity = sizeof(s_object_buffer);
  workspace.arena = s_json_arena;
  workspace.arena_size = sizeof(s_json_arena);
  workspace.distances = s_distances;
  workspace.ordinals = s_ordinals;

  FetchResult result = runFetch(
      source, clock, idle, request.lat, request.lon, parse_options,
      kDefaultHttpLimits, deadlines, workspace, out, request.settings_revision);

  // Cancellation checkpoint: before returning/posting success. A cancel observed
  // only now (e.g. a fully buffered body that never idled) must not post a
  // success; the good bytes stay in the unpublished inactive slot.
  if (fetchControlCancelled(control)) {
    client.stop();
    return makeFailure(FetchOutcome::TransportFailure);
  }

  // Stamp the authenticated floor candidate ONLY on a complete Ok (a partial or
  // failed response, even over a verified connection, must ratchet nothing). The
  // value is the CA-signed leaf notBefore captured before the send -- never an
  // SNTP-derived timestamp.
  result.authenticated_cert_not_before_unix = authenticatedNotBeforeForResult(
      result.outcome, authenticated_leaf_not_before_unix);
  client.stop();
  return result;
}

// Synchronous fetch seam handed to SnapshotStore::fetchCandidate. It builds a
// control that pumps the file-scope poll hook and NEVER cancels, so it is
// byte-for-byte identical to the pre-control synchronous fetch. `ctx` is unused.
FetchResult realFetch(const FetchRequest& request, AircraftSnapshot& out,
                      void* /*ctx*/) {
  FetchControl control{};
  control.idle = &syncPollAdapter;
  // control.cancel stays null: the synchronous path never cancels.
  return realFetchImpl(request, out, control);
}

// Controlled fetch seam handed to SnapshotStore::fetchCandidate by the worker
// path. `ctx` is the caller's FetchControl (idle pump + cancellation predicate).
FetchResult realFetchControlled(const FetchRequest& request,
                                AircraftSnapshot& out, void* ctx) {
  if (ctx == nullptr) {
    FetchControl empty{};  // no idle, no cancel
    return realFetchImpl(request, out, empty);
  }
  return realFetchImpl(request, out, *static_cast<const FetchControl*>(ctx));
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_store.aircraftCount(); }

const Aircraft* aircraftList() { return s_store.aircraftList(); }

SnapshotView publishedSnapshot() { return s_store.view(); }

CandidateResult fetchCandidate(double center_lat, double center_lon,
                               float fetch_radius_km,
                               uint32_t settings_revision) {
  const FetchRequest request{center_lat, center_lon, fetch_radius_km,
                             settings_revision};
  return s_store.fetchCandidate(request, &realFetch, nullptr);
}

CandidateResult fetchCandidateControlled(double center_lat, double center_lon,
                                         float fetch_radius_km,
                                         uint32_t settings_revision,
                                         const FetchControl& control) {
  const FetchRequest request{center_lat, center_lon, fetch_radius_km,
                             settings_revision};
  // The fetch runs synchronously within this call, so `control` outlives it; the
  // const_cast only satisfies the C-style void* ctx seam (realFetchControlled
  // treats it as const FetchControl* and never mutates it).
  return s_store.fetchCandidate(request, &realFetchControlled,
                                const_cast<FetchControl*>(&control));
}

PublishResult publishCandidate(const CandidateHandle& handle,
                               uint32_t current_settings_revision) {
  return s_store.publishCandidate(handle, current_settings_revision);
}

void discardCandidate(const CandidateHandle& handle) {
  s_store.discardCandidate(handle);  // idempotent; preserves the active snapshot
}

FetchResult fetchLatest(double center_lat, double center_lon,
                        float fetch_radius_km) {
  // Compatibility path for the current main loop. Bind the fetch to the settings
  // revision in effect *before* the (potentially slow) request instead of a
  // hardcoded zero, so a location/range change mid-fetch is detected.
  const uint32_t query_revision = services::settings::revision();
  const CandidateResult candidate =
      fetchCandidate(center_lat, center_lon, fetch_radius_km, query_revision);
  FetchResult result = candidate.fetch;
  if (result.outcome != FetchOutcome::Ok) {
    return result;  // transport/parse failure: nothing to publish; cause is kept
  }

  // Re-read the revision after the fetch and publish only against the current
  // one. A slow fetch may span a settings change; publishing then would overwrite
  // fresh data with a stale query, so it is rejected instead.
  const uint32_t current_revision = services::settings::revision();
  switch (publishCandidate(candidate.handle, current_revision)) {
    case PublishResult::Published:
      break;  // active snapshot switched; result stays Ok
    case PublishResult::ObsoleteRevision:
      // Successful fetch, but the query is stale: report it explicitly as
      // Obsolete (a "nothing published" result) rather than Ok or ParseError.
      result.outcome = FetchOutcome::Obsolete;
      break;
    case PublishResult::InvalidHandle:
    case PublishResult::NoCandidate:
      // Unreachable in this synchronous wrapper (the handle we just issued is the
      // sole outstanding candidate). Kept explicit and non-publishing: never
      // report Ok for a publish that did not happen.
      result.outcome = FetchOutcome::ParseError;
      break;
  }
  return result;
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  // True only when a fresh snapshot was actually published for the current
  // revision; an Obsolete (stale) or failed fetch returns false so the caller
  // does not redraw stale data.
  return fetchLatest(center_lat, center_lon, fetch_radius_km).outcome ==
         FetchOutcome::Ok;
}

}  // namespace services::adsb
