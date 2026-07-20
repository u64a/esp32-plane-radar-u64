#pragma once

// Arduino-free glue that drives the bounded HTTP decoder and streaming parser
// for one fetch and maps their results onto FetchOutcome. It fills a caller-
// owned inactive snapshot by reference and never publishes by value; the caller
// performs the double-buffer index switch only when the outcome is Ok.

#include <cstddef>
#include <cstdint>

#include "services/adsb_http.h"
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
// it stamps out.count and out.source_revision = revision; on every other
// outcome `out` may be partially written but must not be published. The caller
// switches buffers only when the outcome is Ok.
FetchResult runFetch(ByteSource& source, Clock& clock, IdleHandler& idle,
                     double center_lat, double center_lon,
                     const ParseOptions& parse_options,
                     const HttpLimits& http_limits,
                     const HttpDeadlines& deadlines,
                     const FetchWorkspace& workspace, AircraftSnapshot& out,
                     uint32_t revision);

}  // namespace services::adsb
