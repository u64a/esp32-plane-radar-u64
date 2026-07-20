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

namespace services::adsb {

namespace {

constexpr float kKmPerNm = 1.852f;

// Double-buffered snapshots: aircraftList() reads the active one while a fetch
// fills the inactive one. Publishing is a single index switch.
AircraftSnapshot s_snapshots[2] = {};
uint8_t s_active = 0;
uint32_t s_revision = 0;

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

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_snapshots[s_active].count; }

const Aircraft* aircraftList() { return s_snapshots[s_active].aircraft; }

FetchResult fetchLatest(double center_lat, double center_lon,
                        float fetch_radius_km) {
  if (!core::coordinatesValid(center_lat, center_lon) ||
      !std::isfinite(fetch_radius_km) || fetch_radius_km <= 0.0f) {
    return makeFailure(FetchOutcome::ParseError);
  }
  const float dist_nm = fetch_radius_km / kKmPerNm;

  char request[256];
  const int req_len = std::snprintf(
      request, sizeof(request),
      "GET /api/v3/lat/%.6f/lon/%.6f/dist/%.1f HTTP/1.1\r\n"
      "Host: %s\r\n"
      "Accept: application/json\r\n"
      "Accept-Encoding: identity\r\n"
      "Connection: close\r\n\r\n",
      center_lat, center_lon, static_cast<double>(dist_nm), config::kAdsbHost);
  if (req_len <= 0 || req_len >= static_cast<int>(sizeof(request))) {
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
  if (!espSendAll(client, reinterpret_cast<const uint8_t*>(request),
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

  const uint8_t inactive = s_active ^ 1;
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

  const FetchResult result =
      runFetch(source, clock, idle, center_lat, center_lon, parse_options,
               kDefaultHttpLimits, deadlines, workspace, s_snapshots[inactive],
               s_revision + 1);
  client.stop();

  if (result.outcome == FetchOutcome::Ok) {
    s_revision += 1;
    s_active = inactive;  // publish with one index switch
  }
  return result;
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  return fetchLatest(center_lat, center_lon, fetch_radius_km).outcome ==
         FetchOutcome::Ok;
}

}  // namespace services::adsb
