#pragma once

// Arduino-free data model, ownership types, and bounded limits shared by the
// ADS-B transport, HTTP response decoder, and streaming JSON parser. This
// header must never include config.h so that the shared parser/decoder code
// stays free of firmware-only configuration.

#include <cstddef>
#include <cstdint>

namespace services::adsb {

// Fixed renderer/main compatibility record. Layout is frozen by the static
// assertions below because ui/radar_display.cpp reads these fields directly.
struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  char callsign[9];
  char type[5];
  char alt[12];
};

// Nearest aircraft retained and published to the renderer per fetch.
constexpr size_t kMaxAircraft = 64;

// Double-buffered publish target. Never returned or queued by value; callers
// fill an inactive instance by reference and publish with one index switch.
// settings_revision records the runtime settings revision (location/radar range)
// this snapshot was fetched for, so a stale in-flight response can be discarded
// instead of published against a newer revision.
struct AircraftSnapshot {
  Aircraft aircraft[kMaxAircraft];
  uint16_t count;
  uint32_t settings_revision;
};

// Every terminal result of a fetch. All error causes are explicit. Obsolete is
// not a fetch error: it marks a fetch that itself succeeded (Ok) but whose
// candidate was rejected at publish time because the settings revision advanced
// while the request was in flight. The compatibility wrapper returns it so a
// stale-but-successful fetch is reported as "nothing published" instead of being
// mislabeled Ok or ParseError; runFetch itself never returns it.
//
// TransportFailure is a network read/EOF failure below the HTTP grammar: a byte
// source error or a premature end of stream that framed no complete response
// (HttpOutcome::TransportError). It is kept distinct from ParseError so a link
// interruption (which is transient and should back off briefly) is never
// conflated with a genuinely malformed HTTP/JSON payload (which is permanent).
enum class FetchOutcome : uint8_t {
  Ok,
  Timeout,
  DnsFailure,
  TlsFailure,
  TransportFailure,
  Http429,
  Http5xx,
  HttpOther,
  ResponseTooLarge,
  ParseError,
  NoMemory,
  Obsolete,
};

struct FetchResult {
  FetchOutcome outcome;
  int http_status;
  uint32_t bytes_received;  // decoded body bytes, independent of framing
  bool retry_after_present;  // true only when a valid Retry-After delta parsed
  uint32_t retry_after_ms;   // meaningful only when retry_after_present is true
  uint16_t aircraft_count;
};

// Streaming JSON parser limits. Production values live in kDefaultParseLimits;
// tests override any field downward to exercise a boundary cheaply.
struct ParseLimits {
  uint16_t max_aircraft;      // retained nearest aircraft
  uint16_t max_ac_entries;    // counted array elements before ResponseTooLarge
  uint16_t max_object_bytes;  // per-object JSON bytes before ResponseTooLarge
  uint8_t max_depth;          // JSON nesting depth before ResponseTooLarge
};

struct ParseOptions {
  ParseLimits limits;
  bool show_ground;  // retain aircraft whose alt_baro == "ground"
};

// Bounded HTTP response decoder limits, mirroring the Phase 5 contract.
struct HttpLimits {
  uint16_t status_line_bytes;      // status-line content excluding CRLF
  uint16_t header_line_bytes;      // one header line excluding CRLF
  uint16_t total_header_bytes;     // all initial header bytes incl. delimiters
  uint16_t max_header_fields;      // initial header field count
  uint16_t chunk_size_line_bytes;  // chunk-size line incl. extensions
  uint16_t max_trailer_fields;     // trailer field count
  uint16_t trailer_bytes;          // all trailer bytes incl. delimiters
  uint32_t max_body_bytes;         // decoded body bytes
  uint32_t max_framing_bytes;      // plaintext/framing bytes on the wire
  uint32_t max_chunks;             // chunk count for chunked bodies
};

// Rollover-safe overall and inactivity budgets for a single fetch.
struct HttpDeadlines {
  uint32_t overall_ms;
  uint32_t inactivity_ms;
};

inline constexpr ParseLimits kDefaultParseLimits = {
    /*max_aircraft=*/64,
    /*max_ac_entries=*/1024,
    /*max_object_bytes=*/2048,
    /*max_depth=*/16,
};

inline constexpr HttpLimits kDefaultHttpLimits = {
    /*status_line_bytes=*/128,
    /*header_line_bytes=*/512,
    /*total_header_bytes=*/4096,
    /*max_header_fields=*/32,
    /*chunk_size_line_bytes=*/64,
    /*max_trailer_fields=*/8,
    /*trailer_bytes=*/1024,
    /*max_body_bytes=*/65536,
    /*max_framing_bytes=*/131072,
    /*max_chunks=*/8192,
};

// Caller-owned fixed workspace sizes. The object buffer holds one captured
// aircraft object plus a terminating NUL, so it is one byte larger than the
// default object-byte limit.
inline constexpr size_t kObjectBufferBytes = kDefaultParseLimits.max_object_bytes + 1;
inline constexpr size_t kJsonArenaBytes = 4096;
inline constexpr size_t kTransportScratchBytes = 512;

static_assert(sizeof(float) == 4, "Aircraft layout assumes 32-bit float");
static_assert(sizeof(Aircraft) == 48, "Aircraft layout is frozen at 48 bytes");
static_assert(alignof(Aircraft) == 4, "Aircraft alignment is frozen at 4 bytes");
static_assert(offsetof(Aircraft, lat) == 0, "Aircraft field layout changed");
static_assert(offsetof(Aircraft, lon) == 4, "Aircraft field layout changed");
static_assert(offsetof(Aircraft, nose_deg) == 8, "Aircraft field layout changed");
static_assert(offsetof(Aircraft, track_deg) == 12, "Aircraft field layout changed");
static_assert(offsetof(Aircraft, gs_knots) == 16, "Aircraft field layout changed");
static_assert(offsetof(Aircraft, callsign) == 20, "Aircraft field layout changed");
static_assert(offsetof(Aircraft, type) == 29, "Aircraft field layout changed");
static_assert(offsetof(Aircraft, alt) == 34, "Aircraft field layout changed");
static_assert(sizeof(AircraftSnapshot) == 3080,
              "AircraftSnapshot size is frozen for the RAM budget");
static_assert(kMaxAircraft <= UINT16_MAX, "count field must hold kMaxAircraft");

}  // namespace services::adsb
