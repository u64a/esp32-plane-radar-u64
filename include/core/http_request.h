#pragma once

#include <cstddef>
#include <cstdint>

// Incremental, byte-fed HTTP/1.x request parser for a single connection.
//
// Design goals (see the Phase 8 secure-portal plan):
//   * Arduino-free and allocation-free: no String, std::vector, new, or malloc.
//     All state lives in a caller-owned fixed struct so an ESP adapter can place
//     it in static storage and feed non-blocking WiFiClient bytes as they arrive.
//   * Fragmentation-independent: the parser consumes one byte at a time and never
//     depends on how the transport chunks the stream (a full request, a single
//     byte at a time, or anything between must parse identically).
//   * Fail closed: the grammar and every limit are enforced strictly. Anything
//     ambiguous, oversized, or non-canonical is rejected with an explicit error;
//     there are no silent defaults or lenient fallbacks. Untrusted values are
//     never reflected -- header values (Host, etc.) are interpreted, not stored.
//
// The parser owns grammar and limits only. Timeouts, socket lifetime, and the
// "connection closes after one request" policy belong to the transport adapter:
// a truncated request simply stays Incomplete until the adapter's read timeout
// fires, and bytes arriving after a request is Complete are ignored here because
// the adapter closes the socket.
//
// Adapter close-contract (normative, enforced by the transport, not this parser):
//   * The connection is single-request. On Complete, on any Error, or on a read
//     timeout, the adapter MUST close the client socket.
//   * On close it MUST discard any unread/trailing bytes still buffered on the
//     socket (a pipelined second request, body overrun, or garbage after the
//     first request) without feeding them anywhere. This parser latches Complete/
//     Error and refuses to consume trailing bytes (httpRequestFeed reports them
//     as not consumed), but discarding them and tearing down the socket is the
//     adapter's responsibility.

namespace core {

// Compile-time buffer capacities. These are the production maxima and size the
// fixed buffers inside HttpRequestParser. The *enforced* limits are supplied at
// runtime via HttpLimits (which must be <= these caps), so tests can drive
// smaller limits to exercise overflow boundaries without huge inputs.
inline constexpr uint16_t kHttpRequestLineCap = 256;  // request-line content bytes
inline constexpr uint16_t kHttpTargetCap = 128;       // request-target bytes
inline constexpr uint16_t kHttpHeaderLineCap = 256;   // one header line's content bytes
inline constexpr uint16_t kHttpHeaderCountCap = 24;   // number of header fields
inline constexpr uint16_t kHttpHeaderTotalCap = 2048;  // cumulative header bytes
inline constexpr uint16_t kHttpBodyCap = 512;          // request body bytes

// The working line buffer is reused for the request line and each header line;
// both share the same 256-byte content ceiling.
inline constexpr uint16_t kHttpLineCap =
    kHttpRequestLineCap > kHttpHeaderLineCap ? kHttpRequestLineCap
                                             : kHttpHeaderLineCap;

// Enforced limits. Every field must be <= the matching compile-time cap above.
// config.h will mirror the production values; tests may pass tighter policies.
// A policy whose fields exceed the caps would let the parser write past a fixed
// backing buffer, so it is rejected up front (see httpLimitsValid) and the parser
// fails closed with HttpError::InvalidLimits before consuming any byte.
struct HttpLimits {
  uint16_t request_line_max;  // max request-line content bytes (<= 256)
  uint16_t target_max;        // max request-target bytes (<= 128)
  uint16_t header_line_max;   // max single header-line content bytes (<= 256)
  uint16_t header_count_max;  // max header fields (<= 24)
  uint16_t header_total_max;  // max cumulative header bytes incl. CRLFs (<= 2048)
  uint16_t body_max;          // max body bytes / Content-Length (<= 512)
};

inline constexpr HttpLimits kDefaultHttpLimits = {
    /*request_line_max=*/kHttpRequestLineCap,
    /*target_max=*/kHttpTargetCap,
    /*header_line_max=*/kHttpHeaderLineCap,
    /*header_count_max=*/kHttpHeaderCountCap,
    /*header_total_max=*/kHttpHeaderTotalCap,
    /*body_max=*/kHttpBodyCap,
};

// The only methods the portal accepts. Any other method token is MethodNotAllowed.
enum class HttpMethod : uint8_t {
  None = 0,
  Get,
  Post,
};

// Feed result after consuming a byte (or a buffer).
enum class HttpParseStatus : uint8_t {
  Incomplete = 0,  // more bytes needed
  Complete,        // full request parsed; the value is valid
  Error,           // rejected; see HttpError
};

// Explicit rejection reasons. Each maps to a specific HTTP response class so the
// adapter can answer truthfully without re-deriving the cause. No value is a
// catch-all default -- every rejection path sets exactly one of these.
enum class HttpError : uint8_t {
  None = 0,
  BadRequest,                   // 400: malformed line/header syntax, bad CRLF,
                                //      control/obs-fold, bad target/version,
                                //      duplicate/ambiguous Content-Length,
                                //      Host duplicate/empty, HTTP/1.1 without a
                                //      Host, or GET with a body
  MethodNotAllowed,             // 405: method token is not GET or POST
  UriTooLong,                   // 414: request line or target over its limit
  HeaderLineTooLong,            // 431: a single header line over its limit
  TooManyHeaderFields,          // 431: header field count over its limit
  HeaderSectionTooLarge,        // 431: cumulative header bytes over its limit
  UnsupportedTransferEncoding,  // 501: any Transfer-Encoding header present
  LengthRequired,               // 411: POST without a Content-Length
  PayloadTooLarge,              // 413: Content-Length exceeds the body limit
  UnsupportedMediaType,         // 415: POST body is not form-urlencoded
  InvalidLimits,                // 500: supplied HttpLimits exceed a backing-buffer
                                //      cap; a configuration/programming error that
                                //      fails closed before any byte is consumed
};

// Parsed request. Buffers are fixed and NUL-terminated. The target is split into
// path and query at the first '?' with no normalization: no case folding, no
// percent-decoding, no dot-segment collapsing. Routing matches the raw path
// exactly (case-sensitive). Header values are interpreted into flags only and
// never copied out, so nothing untrusted is retained for reflection.
struct HttpRequest {
  HttpMethod method;
  bool http_1_1;  // true = HTTP/1.1, false = HTTP/1.0 (only these two accepted)

  char path[kHttpTargetCap + 1];   // begins with '/', validated, NUL-terminated
  uint16_t path_len;
  char query[kHttpTargetCap + 1];  // raw query (may be empty), NUL-terminated
  uint16_t query_len;
  bool has_query;  // true iff the target contained a '?'

  bool has_host;  // a non-empty Host header was present (value not stored). For
                  // HTTP/1.1 this is required; for HTTP/1.0 it may be absent.
  bool host_is_portal;  // the Host value equalled a canonical portal authority
                        // (192.168.4.1 or 192.168.4.1:80). Classified at parse
                        // time via core::httpHostIsCanonicalPortal; the raw
                        // authority is interpreted and discarded, never stored.
                        // false when Host is absent or any other authority, so
                        // the router can gate the sensitive form/POST on it.

  bool has_content_length;
  uint32_t content_length;  // valid only when has_content_length

  bool has_content_type;
  bool content_type_form;  // Content-Type is application/x-www-form-urlencoded
                           // (an optional "; charset=utf-8" parameter is allowed)

  char body[kHttpBodyCap + 1];  // raw body bytes (form-encoded), NUL-terminated
  uint16_t body_len;
};

// Internal parser phases. Exposed only so the whole struct is a POD the caller
// can own; treat it as opaque.
enum class HttpParsePhase : uint8_t {
  RequestLine = 0,
  HeaderLine,
  Body,
  Complete,
  Failed,
};

// Caller-owned parser state. Zero-initialize with httpRequestInit before use.
struct HttpRequestParser {
  HttpParsePhase phase;
  HttpError error;

  char line[kHttpLineCap + 1];  // current line accumulator (request or header)
  uint16_t line_len;
  bool last_was_cr;  // previous byte was CR; the next must be LF

  uint16_t header_count;   // header fields seen so far
  uint32_t header_bytes;   // cumulative header-section bytes (incl. CRLFs)
  uint32_t body_needed;    // Content-Length bytes still to read

  HttpRequest req;
};

// Reset a parser to begin a fresh request.
void httpRequestInit(HttpRequestParser* p);

// True iff every field of `limits` is within its compile-time backing-buffer cap
// (kHttpRequestLineCap/kHttpTargetCap/kHttpHeaderLineCap/kHttpHeaderCountCap/
// kHttpHeaderTotalCap/kHttpBodyCap). A policy that fails this check would permit
// a write past a fixed buffer, so the feed functions reject it before consuming
// any byte (see below). Exposed so callers can validate a configured policy once
// at startup and refuse to serve.
bool httpLimitsValid(const HttpLimits& limits);

// Feed exactly one byte. Returns the status after consuming it. Once Complete or
// Error is reached the phase latches and further bytes are ignored (the adapter
// closes the connection), always returning the latched status.
//
// If `limits` is not httpLimitsValid, the parser fails closed with
// HttpError::InvalidLimits and consumes/stores nothing.
HttpParseStatus httpRequestFeedByte(HttpRequestParser* p, const HttpLimits& limits,
                                    uint8_t byte);

// Feed a buffer, stopping early as soon as the request is Complete or an Error is
// hit. *consumed (when non-null) receives the number of bytes actually consumed
// from data (the remainder, if any, belongs to a closed/ignored connection).
//
// If `limits` is not httpLimitsValid, the parser fails closed with
// HttpError::InvalidLimits, *consumed is set to 0, and nothing is stored.
HttpParseStatus httpRequestFeed(HttpRequestParser* p, const HttpLimits& limits,
                                const uint8_t* data, uint16_t len,
                                uint16_t* consumed);

// Current status / error / parsed value accessors.
HttpParseStatus httpRequestStatus(const HttpRequestParser& p);
HttpError httpRequestError(const HttpRequestParser& p);
const HttpRequest& httpRequestValue(const HttpRequestParser& p);

}  // namespace core
