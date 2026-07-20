#pragma once

// Arduino-free bounded HTTP/1.x response decoder. It validates the status line,
// header block, and body framing (Content-Length, single final chunked, or
// close-delimited) against explicit byte/count limits, and streams decoded body
// bytes to a caller-supplied sink. It never allocates a response-sized buffer.

#include <cstddef>
#include <cstdint>

#include "services/adsb_transport.h"
#include "services/adsb_types.h"

namespace services::adsb {

// Framing-level outcome. Content interpretation (status mapping, JSON parsing)
// is left to the caller via the sink and the returned status.
enum class HttpOutcome : uint8_t {
  Ok,                // status line, headers, and body framing all complete
  Timeout,           // overall or inactivity deadline elapsed
  TransportError,    // byte source error or premature end of stream
  ResponseTooLarge,  // a size/count limit was exceeded
  ParseError,        // status/header/framing grammar violation
  BodyRejected,      // the sink aborted (its own error is authoritative)
};

// Body consumer. begin() is invoked once with the parsed status before any body
// byte, letting the sink parse (200) or drain (non-200). write() returns false
// to abort. finish() validates any streaming parser once the body completes.
class BodySink {
 public:
  virtual ~BodySink() = default;
  virtual void begin(int status) = 0;
  virtual bool write(const uint8_t* data, size_t length) = 0;
  virtual bool finish() = 0;
};

struct HttpResponseResult {
  HttpOutcome outcome;
  int status;                  // parsed status code, or -1 if unavailable
  uint32_t decoded_body_bytes;  // decoded body bytes, published as bytes_received
  bool retry_after_present;
  uint32_t retry_after_ms;
};

// Caller-owned scratch buffers so the decoder keeps almost no state on the
// stack. scratch backs transport reads (budget 512 bytes); line assembles one
// status/header/chunk/trailer line and must hold the largest configured line
// limit plus a terminating NUL.
struct HttpWorkspace {
  uint8_t* scratch;
  size_t scratch_size;
  char* line;
  size_t line_size;
};

// Decode one response. All buffers are caller-owned; no dynamic allocation
// occurs.
HttpResponseResult decodeHttpResponse(ByteSource& source, Clock& clock,
                                      IdleHandler& idle, const HttpLimits& limits,
                                      const HttpDeadlines& deadlines,
                                      const HttpWorkspace& workspace,
                                      BodySink& sink);

}  // namespace services::adsb
