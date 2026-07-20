#include "services/adsb_client.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "config.h"
#include "core/coordinates.h"
#include "core/time_math.h"
#include "services/adsb_fetch.h"
#include "services/adsb_transport_esp.h"
#include "services/settings_events.h"

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

// Arduino-side fetch seam handed to SnapshotStore::fetchCandidate. It performs
// the bounded HTTPS request into the caller-owned inactive snapshot `out` and
// returns the FetchResult; it never publishes. `ctx` is unused (workspace and
// poll hook are file-scope statics).
FetchResult realFetch(const FetchRequest& request, AircraftSnapshot& out,
                      void* /*ctx*/) {
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

  WiFiClientSecure client;
  // Phase 5 temporary policy: Phase 7 replaces this with CA validation + SNTP.
  client.setInsecure();

  const ConnectOutcome connected = espTlsConnect(
      client, config::kAdsbHost, config::kAdsbPort, config::kAdsbConnectTimeoutMs);
  if (connected != ConnectOutcome::Connected) {
    client.stop();
    return makeFailure(connectFailure(connected));
  }

  EspMillisClock clock;
  EspPollIdle idle(s_poll_fn);
  // One cumulative request/response budget covers BOTH the request send and the
  // HTTP response decode. It starts before the send; the send may consume part
  // of it, and only what remains is handed to the decoder -- send and response
  // do NOT each get a fresh kAdsbOverallTimeoutMs.
  const uint32_t overall_started = clock.nowMs();
  if (!espSendAll(client, reinterpret_cast<const uint8_t*>(http_request),
                  static_cast<size_t>(req_len), clock, idle,
                  config::kAdsbOverallTimeoutMs)) {
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

  const FetchResult result = runFetch(
      source, clock, idle, request.lat, request.lon, parse_options,
      kDefaultHttpLimits, deadlines, workspace, out, request.settings_revision);
  client.stop();
  return result;
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

PublishResult publishCandidate(const CandidateHandle& handle,
                               uint32_t current_settings_revision) {
  return s_store.publishCandidate(handle, current_settings_revision);
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
