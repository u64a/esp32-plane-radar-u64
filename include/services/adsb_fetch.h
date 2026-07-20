#pragma once

// Arduino-free glue that drives the bounded HTTP decoder and streaming parser
// for one fetch and maps their results onto FetchOutcome. It fills a caller-
// owned inactive snapshot by reference and never publishes by value; the caller
// performs the double-buffer index switch only when the outcome is Ok.

#include <cstddef>
#include <cstdint>

#include "core/poll_policy.h"
#include "services/adsb_http.h"
#include "services/adsb_snapshot_store.h"
#include "services/adsb_transport.h"
#include "services/adsb_types.h"

namespace services::adsb {

// Caller-owned fixed buffers for a single fetch.
struct FetchWorkspace {
  HttpWorkspace http;       // transport scratch + line assembly
  char* object_buffer;      // captured aircraft object (>= max_object_bytes + 1)
  size_t object_capacity;
  void* arena;              // ArduinoJson arena
  size_t arena_size;
  float* distances;         // >= kMaxAircraft
  uint16_t* ordinals;       // >= kMaxAircraft
};

// Run one fetch. Parses into `out` (an inactive snapshot). On FetchOutcome::Ok
// it stamps out.count and out.settings_revision = revision; on every other
// outcome `out` may be partially written but must not be published. The caller
// switches buffers only when the outcome is Ok.
FetchResult runFetch(ByteSource& source, Clock& clock, IdleHandler& idle,
                     double center_lat, double center_lon,
                     const ParseOptions& parse_options,
                     const HttpLimits& http_limits,
                     const HttpDeadlines& deadlines,
                     const FetchWorkspace& workspace, AircraftSnapshot& out,
                     uint32_t revision);

// Map a fetch outcome onto the poll-policy class that schedules the next fetch.
// An Ok fetch maps to PollOutcome::Success; Obsolete (a successful response the
// compatibility wrapper discarded for a stale revision at publish time) maps to
// PollOutcome::Obsolete. Transient shares one streak across timeout/DNS/TLS/5xx;
// 429 is RateLimited; all remaining errors are Permanent.
core::PollOutcome pollOutcomeFor(FetchOutcome outcome);

// Map the explicit result of a two-step publishCandidate() onto the poll-policy
// class. Only Published is a real success; ObsoleteRevision is a successful
// fetch discarded for a stale revision (Obsolete, streak untouched, prior
// snapshot preserved); InvalidHandle/NoCandidate are internal publish-path
// faults that never published, treated as Permanent so a broken step-2 backs off
// hard rather than hot-looping.
core::PollOutcome pollOutcomeForPublish(PublishResult result);

// True when `outcome` is sensitive to a mid-fetch Wi-Fi link interruption, i.e.
// a spurious disconnect/reconnect flap can plausibly cause it. This covers the
// network read/abort family (Timeout, DnsFailure, TlsFailure, TransportFailure)
// and ParseError -- a truncated body from a dropped link is indistinguishable
// from genuinely malformed JSON, so ParseError counts ONLY where a flap is known
// to have occurred (this predicate is consulted exclusively under that guard).
// Server-attributable and content-limit outcomes (429/5xx/other HTTP,
// ResponseTooLarge, NoMemory) prove the link stayed up long enough to frame a
// full response, so they are not link-sensitive. Ok/Obsolete are not failures.
// Arduino-free and pure so the classification is unit-testable.
bool fetchOutcomeNetworkAbortSensitive(FetchOutcome outcome);

// Resolve the effective poll outcome for a completed fetch when a Wi-Fi
// disconnect flap was observed during it (edge-detected via the disconnect
// sequence, so it fires even if the link already auto-reconnected and the
// level-based Wi-Fi check reads "connected"). `base_outcome` is the poll class
// already computed from the fetch/publish result (after
// effectiveOutcomeAtCompletion). Rules:
//   * No flap, or a fully published success: pass `base_outcome` through (a
//     published success stays Success; the caller still forces one immediate
//     refresh because the link flapped).
//   * Flap without a published success and a link-interruption-sensitive fetch
//     outcome: downgrade to Obsolete (pause semantics) so the spurious drop does
//     not advance the transient or permanent backoff streak.
//   * Any other flapped failure (server/content error): pass through unchanged.
// Arduino-free and pure so the decision is unit-testable.
core::PollOutcome effectiveOutcomeAfterFlap(FetchOutcome fetch_outcome,
                                            core::PollOutcome base_outcome,
                                            bool published_success,
                                            bool disconnect_flap);

}  // namespace services::adsb
