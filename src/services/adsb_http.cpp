#include "services/adsb_http.h"

#include <cstring>

namespace services::adsb {

namespace {

char asciiLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoreCase(const char* a, const char* b) {
  while (*a && *b) {
    if (asciiLower(*a) != asciiLower(*b)) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

bool isTokenChar(char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9')) {
    return true;
  }
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Retry-After delta-seconds are clamped to seven days so the millisecond
// conversion never overflows uint32.
constexpr uint32_t kMaxRetryAfterSeconds = 604800;

enum class ReadResult { Byte, End, Error, Timeout };
enum class LineResult { Ok, Eof, Error, Timeout, TooLong, Bad };

class HttpDecoder {
 public:
  HttpDecoder(ByteSource& source, Clock& clock, IdleHandler& idle,
              const HttpLimits& limits, const HttpDeadlines& deadlines,
              const HttpWorkspace& workspace, BodySink& sink)
      : source_(source),
        clock_(clock),
        idle_(idle),
        limits_(limits),
        deadlines_(deadlines),
        ws_(workspace),
        sink_(sink) {}

  HttpResponseResult run();

 private:
  ReadResult nextByte(uint8_t* out);
  LineResult readLine(size_t content_limit, size_t* out_len,
                      uint32_t* out_consumed);
  HttpResponseResult finish(HttpOutcome outcome);
  HttpOutcome mapLineError(LineResult lr) const;

  bool parseStatusLine(size_t len);
  bool parseHeader(size_t len);
  void parseContentLength(const char* value);
  void parseTransferEncoding(const char* value);
  void parseRetryAfter(const char* value);
  enum class ChunkSizeStatus { Ok, Bad, Overflow };
  ChunkSizeStatus parseChunkSize(size_t len, uint64_t* out_size) const;
  bool validTrailerField(const char* line, size_t len) const;
  bool isFramingTrailerName(const char* line) const;
  bool validChunkExtensions(const char* line, size_t start, size_t len) const;

  HttpOutcome decodeFixedBody(uint64_t length);
  HttpOutcome decodeCloseDelimitedBody();
  HttpOutcome decodeChunkedBody();
  HttpOutcome decodeTrailers();
  ReadResult readExactCrlf();
  bool emitByte(uint8_t byte, HttpOutcome* fail);

  ByteSource& source_;
  Clock& clock_;
  IdleHandler& idle_;
  HttpLimits limits_;
  HttpDeadlines deadlines_;
  HttpWorkspace ws_;
  BodySink& sink_;

  size_t pos_ = 0;
  size_t len_ = 0;
  uint32_t start_ms_ = 0;
  uint32_t last_data_ms_ = 0;

  uint32_t header_bytes_ = 0;
  uint32_t framing_bytes_ = 0;
  uint32_t decoded_body_bytes_ = 0;
  uint32_t chunk_count_ = 0;

  int status_ = -1;
  int version_minor_ = 1;

  bool cl_present_ = false;
  bool cl_conflict_ = false;
  bool cl_overflow_ = false;
  uint64_t cl_value_ = 0;
  bool te_present_ = false;
  bool te_chunked_ = false;
  bool te_invalid_ = false;
  bool retry_present_ = false;
  bool retry_seen_ = false;
  bool retry_disabled_ = false;
  uint32_t retry_ms_ = 0;
};

ReadResult HttpDecoder::nextByte(uint8_t* out) {
  for (;;) {
    if (pos_ < len_) {
      *out = ws_.scratch[pos_++];
      return ReadResult::Byte;
    }
    const uint32_t now = clock_.nowMs();
    if (deadlines_.overall_ms != 0 &&
        static_cast<uint32_t>(now - start_ms_) >= deadlines_.overall_ms) {
      return ReadResult::Timeout;
    }
    if (deadlines_.inactivity_ms != 0 &&
        static_cast<uint32_t>(now - last_data_ms_) >= deadlines_.inactivity_ms) {
      return ReadResult::Timeout;
    }
    idle_.onIdle();  // poll/yield at least once per scratch refill
    size_t n = 0;
    const ReadStatus st = source_.read(ws_.scratch, ws_.scratch_size, &n);
    if (st == ReadStatus::Data) {
      if (n == 0) {
        continue;  // treat an empty Data read as WouldBlock
      }
      len_ = n;
      pos_ = 0;
      last_data_ms_ = clock_.nowMs();
    } else if (st == ReadStatus::WouldBlock) {
      continue;
    } else if (st == ReadStatus::End) {
      return ReadResult::End;
    } else {
      return ReadResult::Error;
    }
  }
}

LineResult HttpDecoder::readLine(size_t content_limit, size_t* out_len,
                                 uint32_t* out_consumed) {
  size_t len = 0;
  uint32_t consumed = 0;
  const size_t cap =
      content_limit + 1 < ws_.line_size ? content_limit + 1 : ws_.line_size;
  for (;;) {
    uint8_t b = 0;
    const ReadResult r = nextByte(&b);
    if (r == ReadResult::End) {
      return LineResult::Eof;
    }
    if (r == ReadResult::Error) {
      return LineResult::Error;
    }
    if (r == ReadResult::Timeout) {
      return LineResult::Timeout;
    }
    ++consumed;
    if (b == '\r') {
      uint8_t lf = 0;
      const ReadResult r2 = nextByte(&lf);
      if (r2 == ReadResult::End) return LineResult::Eof;
      if (r2 == ReadResult::Error) return LineResult::Error;
      if (r2 == ReadResult::Timeout) return LineResult::Timeout;
      ++consumed;
      if (lf != '\n') {
        return LineResult::Bad;  // lone CR
      }
      ws_.line[len] = '\0';
      *out_len = len;
      *out_consumed = consumed;
      return LineResult::Ok;
    }
    if (b == '\n') {
      return LineResult::Bad;  // bare LF
    }
    if ((b < 0x20 && b != '\t') || b == 0x7F) {
      return LineResult::Bad;  // control character
    }
    if (len + 1 >= cap) {
      return LineResult::TooLong;
    }
    ws_.line[len++] = static_cast<char>(b);
  }
}

HttpOutcome HttpDecoder::mapLineError(LineResult lr) const {
  switch (lr) {
    case LineResult::Timeout:
      return HttpOutcome::Timeout;
    case LineResult::TooLong:
      return HttpOutcome::ResponseTooLarge;
    case LineResult::Bad:
      return HttpOutcome::ParseError;
    case LineResult::Eof:
    case LineResult::Error:
    default:
      return HttpOutcome::TransportError;
  }
}

HttpResponseResult HttpDecoder::finish(HttpOutcome outcome) {
  HttpResponseResult result{};
  result.outcome = outcome;
  result.status = status_;
  result.decoded_body_bytes = decoded_body_bytes_;
  result.retry_after_present = retry_present_;
  result.retry_after_ms = retry_ms_;
  return result;
}

bool HttpDecoder::parseStatusLine(size_t len) {
  const char* line = ws_.line;
  if (len < 12 || std::strncmp(line, "HTTP/1.", 7) != 0) {
    return false;
  }
  if (line[7] == '0') {
    version_minor_ = 0;
  } else if (line[7] == '1') {
    version_minor_ = 1;
  } else {
    return false;
  }
  if (line[8] != ' ') {
    return false;
  }
  for (int i = 9; i < 12; ++i) {
    if (line[i] < '0' || line[i] > '9') {
      return false;
    }
  }
  const int code =
      (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
  // Constrain to real final status codes. Values outside 100..599 are not
  // valid HTTP status codes for this client.
  if (code < 100 || code > 599) {
    return false;
  }
  // 1xx informational responses are unsupported framing here: this decoder does
  // not chain past an interim response, so the first status line must be final.
  // Accepting a 1xx line would misreport an informational preamble as a
  // complete HttpOther response.
  if (code < 200) {
    return false;
  }
  if (len != 12 && line[12] != ' ') {
    return false;  // reason phrase must be separated by a single space
  }
  status_ = code;
  return true;
}

bool HttpDecoder::parseHeader(size_t len) {
  char* line = ws_.line;
  char* colon = static_cast<char*>(std::memchr(line, ':', len));
  if (colon == nullptr || colon == line) {
    return false;  // missing name
  }
  for (char* p = line; p < colon; ++p) {
    if (!isTokenChar(*p)) {
      return false;  // invalid header name
    }
  }
  *colon = '\0';
  char* value = colon + 1;
  while (*value == ' ' || *value == '\t') {
    ++value;
  }
  size_t vlen = std::strlen(value);
  while (vlen > 0 && (value[vlen - 1] == ' ' || value[vlen - 1] == '\t')) {
    value[--vlen] = '\0';
  }

  if (equalsIgnoreCase(line, "content-length")) {
    parseContentLength(value);
  } else if (equalsIgnoreCase(line, "transfer-encoding")) {
    parseTransferEncoding(value);
  } else if (equalsIgnoreCase(line, "retry-after")) {
    parseRetryAfter(value);
  }
  return true;
}

void HttpDecoder::parseContentLength(const char* value) {
  // Content-Length is a 1#DIGIT list: one or more equal decimal values in a
  // comma-separated list. Empty members (leading, trailing, repeated, or
  // OWS-only) are a grammar violation, while a syntactically numeric value that
  // overflows the counter is an oversized response, not bad grammar.
  uint64_t merged = 0;
  bool have = false;
  const char* p = value;
  for (;;) {
    while (*p == ' ' || *p == '\t') ++p;
    if (*p < '0' || *p > '9') {
      cl_conflict_ = true;  // empty member or non-numeric token
      return;
    }
    uint64_t token = 0;
    while (*p >= '0' && *p <= '9') {
      const unsigned d = static_cast<unsigned>(*p - '0');
      if (token > (UINT64_MAX - d) / 10) {
        cl_overflow_ = true;  // numeric but exceeds the counter -> too large
        return;
      }
      token = token * 10 + d;
      ++p;
    }
    while (*p == ' ' || *p == '\t') ++p;
    if (have && token != merged) {
      cl_conflict_ = true;
      return;
    }
    merged = token;
    have = true;
    if (*p == '\0') {
      break;
    }
    if (*p != ',') {
      cl_conflict_ = true;
      return;
    }
    ++p;  // consume the separator; a following member is now required
  }
  if (cl_present_ && merged != cl_value_) {
    cl_conflict_ = true;
    return;
  }
  cl_present_ = true;
  cl_value_ = merged;
}

void HttpDecoder::parseTransferEncoding(const char* value) {
  if (te_present_) {
    te_invalid_ = true;  // repeated Transfer-Encoding
    return;
  }
  te_present_ = true;
  if (equalsIgnoreCase(value, "chunked")) {
    te_chunked_ = true;
  } else {
    te_invalid_ = true;  // unsupported/non-final/parameterized/list coding
  }
}

void HttpDecoder::parseRetryAfter(const char* value) {
  // Parse one Retry-After value as delta-seconds. An empty value or an
  // HTTP-date (any non-digit) is "not a usable delta" rather than a hard error.
  bool valid_delta = false;
  uint32_t ms = 0;
  if (*value != '\0') {
    uint32_t seconds = 0;
    valid_delta = true;
    for (const char* p = value; *p != '\0'; ++p) {
      if (*p < '0' || *p > '9') {
        valid_delta = false;  // HTTP-date or malformed: not a delta
        break;
      }
      const uint32_t d = static_cast<uint32_t>(*p - '0');
      if (seconds > (kMaxRetryAfterSeconds - d) / 10) {
        seconds = kMaxRetryAfterSeconds;  // clamp without overflow
        continue;
      }
      seconds = seconds * 10 + d;
      if (seconds > kMaxRetryAfterSeconds) {
        seconds = kMaxRetryAfterSeconds;
      }
    }
    if (valid_delta) {
      ms = seconds * 1000;
    }
  }

  if (retry_disabled_) {
    return;  // a prior conflicting/malformed duplicate already disabled it
  }
  if (!retry_seen_) {
    retry_seen_ = true;
    if (valid_delta) {
      retry_present_ = true;
      retry_ms_ = ms;
    }
    return;  // first Retry-After: a lone date is simply ignored
  }
  // Duplicate Retry-After. Only an identical valid delta stays usable; anything
  // else (conflicting delta, or a malformed/date value) disables Retry-After
  // deterministically without failing the HTTP response.
  if (valid_delta && retry_present_ && ms == retry_ms_) {
    return;
  }
  retry_present_ = false;
  retry_ms_ = 0;
  retry_disabled_ = true;
}

HttpDecoder::ChunkSizeStatus HttpDecoder::parseChunkSize(size_t len,
                                                        uint64_t* out_size) const {
  const char* line = ws_.line;
  size_t i = 0;
  uint64_t size = 0;
  int digits = 0;
  while (i < len) {
    const int v = hexValue(line[i]);
    if (v < 0) {
      break;
    }
    if (size > (UINT32_MAX - static_cast<uint32_t>(v)) / 16) {
      return ChunkSizeStatus::Overflow;  // numeric but exceeds the counter
    }
    size = size * 16 + static_cast<uint64_t>(v);
    ++digits;
    ++i;
  }
  if (digits == 0) {
    return ChunkSizeStatus::Bad;  // no hex digits
  }
  if (i < len && !validChunkExtensions(line, i, len)) {
    return ChunkSizeStatus::Bad;  // malformed chunk extension list
  }
  *out_size = size;
  return ChunkSizeStatus::Ok;
}

// Validate the chunk-extension list that may follow the chunk size, per the
// bounded RFC 9112 subset:
//   chunk-ext = *( BWS ";" BWS ext-name [ BWS "=" BWS ( token / quoted-string ) ] )
// `start` is the first byte after the hex size and `len` the line length. The
// line already passed readLine, so it holds no CR/LF or control bytes; only
// grammar (empty names, bad separators, unterminated/empty quoted values, and
// stray junk) has to be rejected here.
bool HttpDecoder::validChunkExtensions(const char* line, size_t start,
                                       size_t len) const {
  size_t i = start;
  bool any = false;
  for (;;) {
    while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;  // BWS
    if (i >= len) {
      return any;  // clean end is valid only after >=1 extension, not bare BWS
    }
    if (line[i] != ';') {
      return false;  // junk where a ';' separator was required
    }
    ++i;
    any = true;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;  // BWS after ';'
    const size_t name_start = i;
    while (i < len && isTokenChar(line[i])) ++i;
    if (i == name_start) {
      return false;  // empty extension name
    }
    size_t j = i;
    while (j < len && (line[j] == ' ' || line[j] == '\t')) ++j;  // BWS before '='
    if (j < len && line[j] == '=') {
      i = j + 1;
      while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;  // BWS after '='
      if (i >= len) {
        return false;  // '=' with no value
      }
      if (line[i] == '"') {
        ++i;  // opening DQUOTE
        for (;;) {
          if (i >= len) {
            return false;  // unterminated quoted-string
          }
          const char c = line[i];
          if (c == '\\') {
            if (i + 1 >= len) {
              return false;  // dangling quoted-pair escape
            }
            ++i;  // skip the escaped char (readLine already excluded controls)
          } else if (c == '"') {
            ++i;  // closing DQUOTE
            break;
          }
          ++i;
        }
      } else {
        const size_t val_start = i;
        while (i < len && isTokenChar(line[i])) ++i;
        if (i == val_start) {
          return false;  // empty / non-token extension value
        }
      }
    }
    // No '=': the name stands alone; BWS before the next ';' is handled above.
  }
}

// A trailer must be a normal field: a nonempty field-name token, a colon, then
// a legal field value (readLine already guaranteed no CR/LF/control bytes).
bool HttpDecoder::validTrailerField(const char* line, size_t len) const {
  const void* colon_ptr = std::memchr(line, ':', len);
  if (colon_ptr == nullptr) {
    return false;  // missing colon
  }
  const char* colon = static_cast<const char*>(colon_ptr);
  if (colon == line) {
    return false;  // empty field name
  }
  for (const char* p = line; p < colon; ++p) {
    if (!isTokenChar(*p)) {
      return false;  // invalid field-name token
    }
  }
  return true;
}

bool HttpDecoder::isFramingTrailerName(const char* line) const {
  const char* colon = std::strchr(line, ':');
  if (colon == nullptr) {
    return false;
  }
  const size_t name_len = static_cast<size_t>(colon - line);
  static const char* kBanned[] = {"content-length", "transfer-encoding"};
  for (const char* banned : kBanned) {
    if (std::strlen(banned) == name_len) {
      bool match = true;
      for (size_t i = 0; i < name_len; ++i) {
        if (asciiLower(line[i]) != banned[i]) {
          match = false;
          break;
        }
      }
      if (match) {
        return true;
      }
    }
  }
  return false;
}

bool HttpDecoder::emitByte(uint8_t byte, HttpOutcome* fail) {
  ++decoded_body_bytes_;
  ++framing_bytes_;
  if (decoded_body_bytes_ > limits_.max_body_bytes ||
      framing_bytes_ > limits_.max_framing_bytes) {
    *fail = HttpOutcome::ResponseTooLarge;
    return false;
  }
  if (!sink_.write(&byte, 1)) {
    *fail = HttpOutcome::BodyRejected;
    return false;
  }
  return true;
}

ReadResult HttpDecoder::readExactCrlf() {
  uint8_t b = 0;
  ReadResult r = nextByte(&b);
  if (r != ReadResult::Byte) return r;
  if (b != '\r') return ReadResult::Error;  // signalled to caller as ParseError
  r = nextByte(&b);
  if (r != ReadResult::Byte) return r;
  if (b != '\n') return ReadResult::Error;
  return ReadResult::Byte;
}

HttpOutcome HttpDecoder::decodeFixedBody(uint64_t length) {
  uint64_t remaining = length;
  while (remaining > 0) {
    uint8_t b = 0;
    const ReadResult r = nextByte(&b);
    if (r == ReadResult::Timeout) return HttpOutcome::Timeout;
    if (r == ReadResult::End || r == ReadResult::Error) {
      return HttpOutcome::TransportError;  // premature EOF or error
    }
    HttpOutcome fail = HttpOutcome::Ok;
    if (!emitByte(b, &fail)) {
      return fail;
    }
    --remaining;
  }
  return sink_.finish() ? HttpOutcome::Ok : HttpOutcome::BodyRejected;
}

HttpOutcome HttpDecoder::decodeCloseDelimitedBody() {
  for (;;) {
    uint8_t b = 0;
    const ReadResult r = nextByte(&b);
    if (r == ReadResult::Timeout) return HttpOutcome::Timeout;
    if (r == ReadResult::Error) return HttpOutcome::TransportError;
    if (r == ReadResult::End) {
      return sink_.finish() ? HttpOutcome::Ok : HttpOutcome::BodyRejected;
    }
    HttpOutcome fail = HttpOutcome::Ok;
    if (!emitByte(b, &fail)) {
      return fail;
    }
  }
}

HttpOutcome HttpDecoder::decodeChunkedBody() {
  for (;;) {
    size_t len = 0;
    uint32_t consumed = 0;
    const LineResult lr = readLine(limits_.chunk_size_line_bytes, &len, &consumed);
    if (lr != LineResult::Ok) {
      return mapLineError(lr);
    }
    framing_bytes_ += consumed;
    if (framing_bytes_ > limits_.max_framing_bytes) {
      return HttpOutcome::ResponseTooLarge;
    }
    uint64_t chunk_size = 0;
    switch (parseChunkSize(len, &chunk_size)) {
      case ChunkSizeStatus::Ok:
        break;
      case ChunkSizeStatus::Overflow:
        return HttpOutcome::ResponseTooLarge;  // numeric size beyond the counter
      case ChunkSizeStatus::Bad:
        return HttpOutcome::ParseError;  // nonnumeric / bad extension grammar
    }
    ++chunk_count_;
    if (chunk_count_ > limits_.max_chunks) {
      return HttpOutcome::ResponseTooLarge;
    }
    if (chunk_size == 0) {
      return decodeTrailers();
    }
    uint64_t remaining = chunk_size;
    while (remaining > 0) {
      uint8_t b = 0;
      const ReadResult r = nextByte(&b);
      if (r == ReadResult::Timeout) return HttpOutcome::Timeout;
      if (r == ReadResult::End || r == ReadResult::Error) {
        return HttpOutcome::TransportError;
      }
      HttpOutcome fail = HttpOutcome::Ok;
      if (!emitByte(b, &fail)) {
        return fail;
      }
      --remaining;
    }
    const ReadResult crlf = readExactCrlf();
    if (crlf == ReadResult::Timeout) return HttpOutcome::Timeout;
    if (crlf == ReadResult::Error) return HttpOutcome::ParseError;  // bad CRLF
    if (crlf == ReadResult::End) return HttpOutcome::TransportError;
    framing_bytes_ += 2;
    if (framing_bytes_ > limits_.max_framing_bytes) {
      return HttpOutcome::ResponseTooLarge;
    }
  }
}

HttpOutcome HttpDecoder::decodeTrailers() {
  uint32_t trailer_bytes = 0;
  uint16_t trailer_fields = 0;
  for (;;) {
    size_t len = 0;
    uint32_t consumed = 0;
    const LineResult lr = readLine(limits_.header_line_bytes, &len, &consumed);
    if (lr != LineResult::Ok) {
      return mapLineError(lr);
    }
    trailer_bytes += consumed;
    if (trailer_bytes > limits_.trailer_bytes) {
      return HttpOutcome::ResponseTooLarge;
    }
    if (len == 0) {
      break;
    }
    ++trailer_fields;
    if (trailer_fields > limits_.max_trailer_fields) {
      return HttpOutcome::ResponseTooLarge;
    }
    if (ws_.line[0] == ' ' || ws_.line[0] == '\t') {
      return HttpOutcome::ParseError;  // obs-fold
    }
    if (!validTrailerField(ws_.line, len)) {
      return HttpOutcome::ParseError;  // malformed trailer (name token + colon)
    }
    if (isFramingTrailerName(ws_.line)) {
      return HttpOutcome::ParseError;  // framing headers may not appear in trailers
    }
  }
  return sink_.finish() ? HttpOutcome::Ok : HttpOutcome::BodyRejected;
}

HttpResponseResult HttpDecoder::run() {
  start_ms_ = clock_.nowMs();
  last_data_ms_ = start_ms_;

  size_t len = 0;
  uint32_t consumed = 0;
  LineResult lr = readLine(limits_.status_line_bytes, &len, &consumed);
  if (lr != LineResult::Ok) {
    return finish(mapLineError(lr));
  }
  if (!parseStatusLine(len)) {
    return finish(HttpOutcome::ParseError);
  }

  uint16_t field_count = 0;
  for (;;) {
    lr = readLine(limits_.header_line_bytes, &len, &consumed);
    if (lr != LineResult::Ok) {
      return finish(mapLineError(lr));
    }
    header_bytes_ += consumed;
    if (header_bytes_ > limits_.total_header_bytes) {
      return finish(HttpOutcome::ResponseTooLarge);
    }
    if (len == 0) {
      break;  // end of header block
    }
    if (ws_.line[0] == ' ' || ws_.line[0] == '\t') {
      return finish(HttpOutcome::ParseError);  // obs-fold continuation
    }
    ++field_count;
    if (field_count > limits_.max_header_fields) {
      return finish(HttpOutcome::ResponseTooLarge);
    }
    if (!parseHeader(len)) {
      return finish(HttpOutcome::ParseError);
    }
  }

  if (te_invalid_ || cl_conflict_) {
    return finish(HttpOutcome::ParseError);
  }
  if (cl_overflow_) {
    // A syntactically numeric Content-Length that exceeds the counter is an
    // oversized response, not a grammar violation.
    return finish(HttpOutcome::ResponseTooLarge);
  }
  const bool chunked = te_present_ && te_chunked_;
  if (te_present_ && cl_present_) {
    return finish(HttpOutcome::ParseError);  // TE + CL
  }
  if (te_present_ && version_minor_ == 0) {
    return finish(HttpOutcome::ParseError);  // HTTP/1.0 must not use TE
  }
  if (cl_present_ && cl_value_ > limits_.max_body_bytes) {
    return finish(HttpOutcome::ResponseTooLarge);  // reject before body bytes
  }

  const bool no_body = status_ == 204 || status_ == 304;

  sink_.begin(status_);

  HttpOutcome body_outcome;
  if (no_body) {
    body_outcome = sink_.finish() ? HttpOutcome::Ok : HttpOutcome::BodyRejected;
  } else if (chunked) {
    body_outcome = decodeChunkedBody();
  } else if (cl_present_) {
    body_outcome = decodeFixedBody(cl_value_);
  } else {
    body_outcome = decodeCloseDelimitedBody();
  }
  return finish(body_outcome);
}

}  // namespace

HttpResponseResult decodeHttpResponse(ByteSource& source, Clock& clock,
                                      IdleHandler& idle, const HttpLimits& limits,
                                      const HttpDeadlines& deadlines,
                                      const HttpWorkspace& workspace,
                                      BodySink& sink) {
  HttpDecoder decoder(source, clock, idle, limits, deadlines, workspace, sink);
  return decoder.run();
}

}  // namespace services::adsb
