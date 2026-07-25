#include "core/http_request.h"

#include <cstring>

#include "core/http_host.h"

namespace core {

namespace {

// --- small ASCII helpers (locale-independent, no <cctype>) -------------------

bool isDigit(uint8_t c) { return c >= '0' && c <= '9'; }

uint8_t toLowerAscii(uint8_t c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c - 'A' + 'a') : c;
}

// RFC 7230 token characters (valid in a header field name).
bool isTokenChar(uint8_t c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9')) {
    return true;
  }
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

// Case-insensitive compare of a (data,len) span against a NUL-terminated literal.
bool ciEqualLit(const char* data, uint16_t len, const char* lit) {
  uint16_t i = 0;
  for (; i < len; ++i) {
    if (lit[i] == '\0') {
      return false;  // literal ended first
    }
    if (toLowerAscii(static_cast<uint8_t>(data[i])) !=
        toLowerAscii(static_cast<uint8_t>(lit[i]))) {
      return false;
    }
  }
  return lit[i] == '\0';  // both ended together
}

// Trim leading/trailing spaces (OWS is SP only here; HTAB is rejected upstream
// as a control byte). Updates *start and *len in place.
void trimSpaces(const char* buf, uint16_t* start, uint16_t* len) {
  uint16_t s = *start;
  uint16_t e = static_cast<uint16_t>(*start + *len);
  while (s < e && buf[s] == ' ') {
    ++s;
  }
  while (e > s && buf[e - 1] == ' ') {
    --e;
  }
  *start = s;
  *len = static_cast<uint16_t>(e - s);
}

HttpParseStatus fail(HttpRequestParser* p, HttpError err) {
  p->phase = HttpParsePhase::Failed;
  p->error = err;
  return HttpParseStatus::Error;
}

// --- target validation -------------------------------------------------------

// The path must be an origin-form absolute path that is safe to match exactly.
// Rejects anything that could alias a route through normalization: backslash,
// percent-encoding (removes %2f/%2e slash/dot ambiguity), empty segments
// (repeated or trailing slash), and "." / ".." traversal segments. Only a tight
// unreserved character set plus '/' is permitted.
bool validatePath(const char* path, uint16_t len) {
  if (len == 0 || path[0] != '/') {
    return false;
  }
  for (uint16_t i = 0; i < len; ++i) {
    const char c = path[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~' || c == '/';
    if (!ok) {
      return false;  // rejects '\\', '%', ':', '@', and every other byte
    }
  }
  // Segment checks: split on '/', reject empty and dot/dot-dot segments.
  uint16_t i = 1;  // skip the leading '/'
  while (i <= len) {
    uint16_t j = i;
    while (j < len && path[j] != '/') {
      ++j;
    }
    const uint16_t seg_len = static_cast<uint16_t>(j - i);
    if (seg_len == 0) {
      if (len == 1) {
        break;  // path == "/" (root) is the sole legal empty case
      }
      return false;  // "//" or a trailing '/'
    }
    if (seg_len == 1 && path[i] == '.') {
      return false;  // "." segment
    }
    if (seg_len == 2 && path[i] == '.' && path[i + 1] == '.') {
      return false;  // ".." traversal
    }
    i = static_cast<uint16_t>(j + 1);
  }
  return true;
}

// The query is not used for routing or decoding, but must still be bounded and
// free of controls. Line accumulation already guarantees printable ASCII with no
// spaces, so this only re-asserts the safe range.
bool validateQuery(const char* query, uint16_t len) {
  for (uint16_t i = 0; i < len; ++i) {
    const uint8_t c = static_cast<uint8_t>(query[i]);
    if (c < 0x21 || c > 0x7e) {
      return false;
    }
  }
  return true;
}

// Validate the request-target and split it into path + query. Returns HttpError
// (None on success).
HttpError parseTarget(HttpRequestParser* p, const HttpLimits& limits,
                      const char* target, uint16_t len) {
  if (len == 0 || target[0] != '/') {
    return HttpError::BadRequest;  // only origin-form ("/...") accepted
  }
  if (len > limits.target_max) {
    return HttpError::UriTooLong;
  }
  // Split at the first '?'.
  uint16_t q = 0;
  bool has_q = false;
  for (; q < len; ++q) {
    if (target[q] == '?') {
      has_q = true;
      break;
    }
  }
  const uint16_t path_len = has_q ? q : len;
  const uint16_t query_len = has_q ? static_cast<uint16_t>(len - q - 1) : 0;
  if (!validatePath(target, path_len)) {
    return HttpError::BadRequest;
  }
  if (!validateQuery(target + (has_q ? q + 1 : len), query_len)) {
    return HttpError::BadRequest;
  }
  std::memcpy(p->req.path, target, path_len);
  p->req.path[path_len] = '\0';
  p->req.path_len = path_len;
  if (has_q) {
    std::memcpy(p->req.query, target + q + 1, query_len);
  }
  p->req.query[query_len] = '\0';
  p->req.query_len = query_len;
  p->req.has_query = has_q;
  return HttpError::None;
}

// --- request line ------------------------------------------------------------

HttpParseStatus finishRequestLine(HttpRequestParser* p,
                                  const HttpLimits& limits) {
  const char* line = p->line;
  const uint16_t len = p->line_len;
  if (len == 0) {
    return fail(p, HttpError::BadRequest);  // empty request line
  }
  // Exactly two spaces splitting three non-empty tokens.
  uint16_t sp1 = 0;
  bool have_sp1 = false;
  for (; sp1 < len; ++sp1) {
    if (line[sp1] == ' ') {
      have_sp1 = true;
      break;
    }
  }
  if (!have_sp1) {
    return fail(p, HttpError::BadRequest);
  }
  uint16_t sp2 = static_cast<uint16_t>(sp1 + 1);
  bool have_sp2 = false;
  for (; sp2 < len; ++sp2) {
    if (line[sp2] == ' ') {
      have_sp2 = true;
      break;
    }
  }
  if (!have_sp2) {
    return fail(p, HttpError::BadRequest);
  }
  for (uint16_t k = static_cast<uint16_t>(sp2 + 1); k < len; ++k) {
    if (line[k] == ' ') {
      return fail(p, HttpError::BadRequest);  // a third space -> malformed
    }
  }
  const uint16_t method_len = sp1;
  const uint16_t target_off = static_cast<uint16_t>(sp1 + 1);
  const uint16_t target_len = static_cast<uint16_t>(sp2 - target_off);
  const uint16_t ver_off = static_cast<uint16_t>(sp2 + 1);
  const uint16_t ver_len = static_cast<uint16_t>(len - ver_off);
  if (method_len == 0 || target_len == 0 || ver_len == 0) {
    return fail(p, HttpError::BadRequest);
  }
  // Method (case-sensitive per RFC): only GET/POST; anything else is 405.
  if (method_len == 3 && std::memcmp(line, "GET", 3) == 0) {
    p->req.method = HttpMethod::Get;
  } else if (method_len == 4 && std::memcmp(line, "POST", 4) == 0) {
    p->req.method = HttpMethod::Post;
  } else {
    return fail(p, HttpError::MethodNotAllowed);
  }
  // Target.
  const HttpError terr = parseTarget(p, limits, line + target_off, target_len);
  if (terr != HttpError::None) {
    return fail(p, terr);
  }
  // Version: exactly HTTP/1.0 or HTTP/1.1.
  const char* ver = line + ver_off;
  if (ver_len == 8 && std::memcmp(ver, "HTTP/1.1", 8) == 0) {
    p->req.http_1_1 = true;
  } else if (ver_len == 8 && std::memcmp(ver, "HTTP/1.0", 8) == 0) {
    p->req.http_1_1 = false;
  } else {
    return fail(p, HttpError::BadRequest);
  }
  p->line_len = 0;
  p->phase = HttpParsePhase::HeaderLine;
  return HttpParseStatus::Incomplete;
}

// --- headers -----------------------------------------------------------------

// Parse a strict non-negative decimal Content-Length. Returns false on any
// non-digit, sign, leading zero (except a lone "0"), or empty value. The value
// is saturated to UINT32_MAX; the magnitude-vs-body_max check happens later.
bool parseContentLength(const char* v, uint16_t len, uint32_t* out) {
  if (len == 0) {
    return false;
  }
  if (v[0] == '0' && len > 1) {
    return false;  // ambiguous leading zero
  }
  uint64_t value = 0;
  for (uint16_t i = 0; i < len; ++i) {
    if (!isDigit(static_cast<uint8_t>(v[i]))) {
      return false;
    }
    value = value * 10U + static_cast<uint64_t>(v[i] - '0');
    if (value > 0xFFFFFFFFULL) {
      value = 0xFFFFFFFFULL;  // saturate; still definitively over body_max
    }
  }
  *out = static_cast<uint32_t>(value);
  return true;
}

// Is a (trimmed) Content-Type value application/x-www-form-urlencoded, with at
// most an optional "charset=utf-8" parameter? Media type and parameter names are
// case-insensitive; any other parameter, a second parameter, or a non-utf-8
// charset is rejected.
bool isFormUrlEncoded(const char* v, uint16_t len) {
  uint16_t semi = 0;
  bool has_semi = false;
  for (; semi < len; ++semi) {
    if (v[semi] == ';') {
      has_semi = true;
      break;
    }
  }
  uint16_t media_start = 0;
  uint16_t media_len = has_semi ? semi : len;
  trimSpaces(v, &media_start, &media_len);
  if (!ciEqualLit(v + media_start, media_len,
                  "application/x-www-form-urlencoded")) {
    return false;
  }
  if (!has_semi) {
    return true;
  }
  // Exactly one parameter: charset=utf-8 (a second ';' is rejected).
  uint16_t param_start = static_cast<uint16_t>(semi + 1);
  uint16_t param_len = static_cast<uint16_t>(len - semi - 1);
  for (uint16_t i = param_start; i < len; ++i) {
    if (v[i] == ';') {
      return false;  // multiple parameters not allowed
    }
  }
  trimSpaces(v, &param_start, &param_len);
  // Find '=' inside the parameter.
  const uint16_t param_end =
      static_cast<uint16_t>(param_start + param_len);
  uint16_t eq = param_start;
  bool has_eq = false;
  for (; eq < param_end; ++eq) {
    if (v[eq] == '=') {
      has_eq = true;
      break;
    }
  }
  if (!has_eq) {
    return false;
  }
  uint16_t name_start = param_start;
  uint16_t name_len = static_cast<uint16_t>(eq - param_start);
  uint16_t val_start = static_cast<uint16_t>(eq + 1);
  uint16_t val_len = static_cast<uint16_t>(param_end - val_start);
  trimSpaces(v, &name_start, &name_len);
  trimSpaces(v, &val_start, &val_len);
  if (!ciEqualLit(v + name_start, name_len, "charset")) {
    return false;
  }
  return ciEqualLit(v + val_start, val_len, "utf-8");
}

HttpParseStatus parseHeaderField(HttpRequestParser* p) {
  const char* line = p->line;
  const uint16_t len = p->line_len;
  // Locate the field-name/value colon.
  uint16_t colon = 0;
  bool have_colon = false;
  for (; colon < len; ++colon) {
    if (line[colon] == ':') {
      have_colon = true;
      break;
    }
  }
  if (!have_colon || colon == 0) {
    return fail(p, HttpError::BadRequest);  // no colon or empty name
  }
  // Field name must be a valid token (this also rejects a space before the
  // colon, a classic request-smuggling vector).
  for (uint16_t i = 0; i < colon; ++i) {
    if (!isTokenChar(static_cast<uint8_t>(line[i]))) {
      return fail(p, HttpError::BadRequest);
    }
  }
  const char* name = line;
  const uint16_t name_len = colon;
  uint16_t val_start = static_cast<uint16_t>(colon + 1);
  uint16_t val_len = static_cast<uint16_t>(len - val_start);
  trimSpaces(line, &val_start, &val_len);
  const char* value = line + val_start;

  if (ciEqualLit(name, name_len, "host")) {
    if (p->req.has_host) {
      return fail(p, HttpError::BadRequest);  // duplicate Host
    }
    if (val_len == 0) {
      // A present-but-empty Host is malformed (captive probes still send a real
      // authority). The value itself is never stored or reflected.
      return fail(p, HttpError::BadRequest);
    }
    p->req.has_host = true;  // presence only; value not stored
    // Classify the authority against the canonical portal Host once, here, while
    // the bytes are still in the line buffer. Only the boolean verdict is kept --
    // the untrusted value is interpreted and discarded, never retained for
    // reflection -- so the router can host-gate the sensitive form/POST.
    p->req.host_is_portal = httpHostIsCanonicalPortal(value, val_len);
  } else if (ciEqualLit(name, name_len, "content-length")) {
    if (p->req.has_content_length) {
      return fail(p, HttpError::BadRequest);  // duplicate Content-Length
    }
    uint32_t cl = 0;
    if (!parseContentLength(value, val_len, &cl)) {
      return fail(p, HttpError::BadRequest);
    }
    p->req.has_content_length = true;
    p->req.content_length = cl;
  } else if (ciEqualLit(name, name_len, "content-type")) {
    if (p->req.has_content_type) {
      return fail(p, HttpError::BadRequest);  // duplicate Content-Type
    }
    p->req.has_content_type = true;
    p->req.content_type_form = isFormUrlEncoded(value, val_len);
  } else if (ciEqualLit(name, name_len, "transfer-encoding")) {
    return fail(p, HttpError::UnsupportedTransferEncoding);  // any TE rejected
  }
  // All other headers are ignored (already length/count/total bounded).
  return HttpParseStatus::Incomplete;
}

HttpParseStatus endOfHeaders(HttpRequestParser* p, const HttpLimits& limits) {
  // HTTP/1.1 requires exactly one non-empty Host header (RFC 7230 5.4). The
  // value is intentionally not restricted to the AP address -- captive-portal
  // probes deliberately send external hostnames, and this AP-only server must
  // still answer them -- but a 1.1 request that omits Host is malformed. HTTP/1.0
  // may omit it. Duplicate/empty Host was already rejected during header parsing.
  if (p->req.http_1_1 && !p->req.has_host) {
    return fail(p, HttpError::BadRequest);
  }
  if (p->req.method == HttpMethod::Post) {
    if (!p->req.has_content_length) {
      return fail(p, HttpError::LengthRequired);
    }
    if (!p->req.has_content_type || !p->req.content_type_form) {
      return fail(p, HttpError::UnsupportedMediaType);
    }
    if (p->req.content_length > limits.body_max) {
      return fail(p, HttpError::PayloadTooLarge);
    }
    p->body_needed = p->req.content_length;
    if (p->body_needed == 0) {
      p->phase = HttpParsePhase::Complete;
      return HttpParseStatus::Complete;
    }
    p->phase = HttpParsePhase::Body;
    return HttpParseStatus::Incomplete;
  }
  // GET: a body is not allowed. A positive Content-Length is a protocol error.
  if (p->req.has_content_length && p->req.content_length > 0) {
    return fail(p, HttpError::BadRequest);
  }
  p->phase = HttpParsePhase::Complete;
  return HttpParseStatus::Complete;
}

HttpParseStatus finishHeaderLine(HttpRequestParser* p,
                                 const HttpLimits& limits) {
  // Account the header-section bytes (line content + CRLF), including the blank
  // terminator line.
  p->header_bytes += static_cast<uint32_t>(p->line_len) + 2U;
  if (p->header_bytes > limits.header_total_max) {
    return fail(p, HttpError::HeaderSectionTooLarge);
  }
  if (p->line_len == 0) {
    return endOfHeaders(p, limits);  // blank line ends the header section
  }
  if (p->line[0] == ' ') {
    return fail(p, HttpError::BadRequest);  // obsolete line folding
  }
  if (p->header_count >= limits.header_count_max) {
    return fail(p, HttpError::TooManyHeaderFields);
  }
  p->header_count = static_cast<uint16_t>(p->header_count + 1);
  const HttpParseStatus st = parseHeaderField(p);
  if (st == HttpParseStatus::Error) {
    return st;
  }
  p->line_len = 0;
  return HttpParseStatus::Incomplete;
}

// --- byte feeding ------------------------------------------------------------

HttpParseStatus feedLineByte(HttpRequestParser* p, const HttpLimits& limits,
                             uint8_t b, bool is_request_line) {
  if (p->last_was_cr) {
    p->last_was_cr = false;
    if (b != '\n') {
      return fail(p, HttpError::BadRequest);  // bare CR
    }
    return is_request_line ? finishRequestLine(p, limits)
                           : finishHeaderLine(p, limits);
  }
  if (b == '\r') {
    p->last_was_cr = true;
    return HttpParseStatus::Incomplete;
  }
  if (b == '\n') {
    return fail(p, HttpError::BadRequest);  // bare LF (strict CRLF only)
  }
  // Content byte: only printable ASCII and SP are allowed. This rejects every
  // control (incl. HTAB), DEL, and non-ASCII byte.
  if (b < 0x20 || b >= 0x7f) {
    return fail(p, HttpError::BadRequest);
  }
  const uint16_t cap =
      is_request_line ? limits.request_line_max : limits.header_line_max;
  if (p->line_len >= cap) {
    return fail(p, is_request_line ? HttpError::UriTooLong
                                   : HttpError::HeaderLineTooLong);
  }
  p->line[p->line_len++] = static_cast<char>(b);
  return HttpParseStatus::Incomplete;
}

HttpParseStatus feedBodyByte(HttpRequestParser* p, uint8_t b) {
  p->req.body[p->req.body_len++] = static_cast<char>(b);
  if (--p->body_needed == 0) {
    p->req.body[p->req.body_len] = '\0';
    p->phase = HttpParsePhase::Complete;
    return HttpParseStatus::Complete;
  }
  return HttpParseStatus::Incomplete;
}

}  // namespace

void httpRequestInit(HttpRequestParser* p) {
  if (p == nullptr) {
    return;
  }
  std::memset(p, 0, sizeof(*p));
  p->phase = HttpParsePhase::RequestLine;
  p->error = HttpError::None;
  p->req.method = HttpMethod::None;
}

bool httpLimitsValid(const HttpLimits& limits) {
  return limits.request_line_max <= kHttpRequestLineCap &&
         limits.target_max <= kHttpTargetCap &&
         limits.header_line_max <= kHttpHeaderLineCap &&
         limits.header_count_max <= kHttpHeaderCountCap &&
         limits.header_total_max <= kHttpHeaderTotalCap &&
         limits.body_max <= kHttpBodyCap;
}

HttpParseStatus httpRequestFeedByte(HttpRequestParser* p,
                                    const HttpLimits& limits, uint8_t byte) {
  if (p == nullptr) {
    return HttpParseStatus::Error;
  }
  // Fail closed on a misconfigured policy before touching any backing buffer.
  if (p->phase != HttpParsePhase::Complete &&
      p->phase != HttpParsePhase::Failed && !httpLimitsValid(limits)) {
    return fail(p, HttpError::InvalidLimits);
  }
  switch (p->phase) {
    case HttpParsePhase::RequestLine:
      return feedLineByte(p, limits, byte, /*is_request_line=*/true);
    case HttpParsePhase::HeaderLine:
      return feedLineByte(p, limits, byte, /*is_request_line=*/false);
    case HttpParsePhase::Body:
      return feedBodyByte(p, byte);
    case HttpParsePhase::Complete:
      return HttpParseStatus::Complete;  // extra bytes ignored (socket closes)
    case HttpParsePhase::Failed:
    default:
      return HttpParseStatus::Error;
  }
}

HttpParseStatus httpRequestFeed(HttpRequestParser* p, const HttpLimits& limits,
                                const uint8_t* data, uint16_t len,
                                uint16_t* consumed) {
  HttpParseStatus st = httpRequestStatus(*p);
  if (st != HttpParseStatus::Incomplete) {
    if (consumed != nullptr) {
      *consumed = 0;  // already terminal; nothing is consumed
    }
    return st;
  }
  // Fail closed on a misconfigured policy before consuming any byte.
  if (!httpLimitsValid(limits)) {
    if (consumed != nullptr) {
      *consumed = 0;
    }
    return fail(p, HttpError::InvalidLimits);
  }
  uint16_t i = 0;
  for (; i < len; ++i) {
    st = httpRequestFeedByte(p, limits, data[i]);
    if (st != HttpParseStatus::Incomplete) {
      ++i;  // this byte was consumed
      break;
    }
  }
  if (consumed != nullptr) {
    *consumed = i;
  }
  return st;
}

HttpParseStatus httpRequestStatus(const HttpRequestParser& p) {
  switch (p.phase) {
    case HttpParsePhase::Complete:
      return HttpParseStatus::Complete;
    case HttpParsePhase::Failed:
      return HttpParseStatus::Error;
    default:
      return HttpParseStatus::Incomplete;
  }
}

HttpError httpRequestError(const HttpRequestParser& p) { return p.error; }

const HttpRequest& httpRequestValue(const HttpRequestParser& p) { return p.req; }

}  // namespace core
