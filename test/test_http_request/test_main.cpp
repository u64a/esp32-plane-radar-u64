#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/http_request.h"

using core::HttpError;
using core::HttpLimits;
using core::HttpMethod;
using core::HttpParseStatus;
using core::HttpRequest;
using core::HttpRequestParser;
using core::kDefaultHttpLimits;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

int I(HttpParseStatus s) { return static_cast<int>(s); }
int E(HttpError e) { return static_cast<int>(e); }
int M(HttpMethod m) { return static_cast<int>(m); }

// Feed exactly n bytes, one at a time (the canonical 1-byte fragmentation path).
HttpParseStatus feedN(HttpRequestParser* p, const HttpLimits& lim, const char* s,
                      size_t n) {
  HttpParseStatus st = HttpParseStatus::Incomplete;
  for (size_t i = 0; i < n; ++i) {
    st = httpRequestFeedByte(p, lim, static_cast<uint8_t>(s[i]));
  }
  return st;
}

// Parse a NUL-terminated request text one byte at a time with the given limits.
HttpParseStatus parse(HttpRequestParser* p, const char* s,
                      const HttpLimits& lim = kDefaultHttpLimits) {
  httpRequestInit(p);
  return feedN(p, lim, s, std::strlen(s));
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- valid requests ----------------------------------------------------------

void test_simple_get_root() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete),
                        I(parse(&p, "GET / HTTP/1.1\r\nHost: x\r\n\r\n")));
  const HttpRequest& r = httpRequestValue(p);
  TEST_ASSERT_EQUAL_INT(M(HttpMethod::Get), M(r.method));
  TEST_ASSERT_EQUAL_STRING("/", r.path);
  TEST_ASSERT_TRUE(r.http_1_1);
  TEST_ASSERT_TRUE(r.has_host);
  TEST_ASSERT_FALSE(r.has_query);
  TEST_ASSERT_FALSE(r.has_content_length);
}

void test_get_http_1_0_no_host_ok() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete),
                        I(parse(&p, "GET /generate_204 HTTP/1.0\r\n\r\n")));
  const HttpRequest& r = httpRequestValue(p);
  TEST_ASSERT_FALSE(r.http_1_1);
  TEST_ASSERT_FALSE(r.has_host);
  TEST_ASSERT_EQUAL_STRING("/generate_204", r.path);
}

void test_get_splits_path_and_query_without_normalizing() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(
      I(HttpParseStatus::Complete),
      I(parse(&p, "GET /gen_204?foo=bar&x=1 HTTP/1.1\r\nHost: x\r\n\r\n")));
  const HttpRequest& r = httpRequestValue(p);
  TEST_ASSERT_EQUAL_STRING("/gen_204", r.path);
  TEST_ASSERT_TRUE(r.has_query);
  TEST_ASSERT_EQUAL_STRING("foo=bar&x=1", r.query);
}

void test_empty_query_after_question_mark() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete),
                        I(parse(&p, "GET /ncsi.txt? HTTP/1.1\r\nHost: x\r\n\r\n")));
  const HttpRequest& r = httpRequestValue(p);
  TEST_ASSERT_TRUE(r.has_query);
  TEST_ASSERT_EQUAL_UINT16(0, r.query_len);
  TEST_ASSERT_EQUAL_STRING("/ncsi.txt", r.path);
}

void test_post_form_body() {
  HttpRequestParser p;
  const char* req =
      "POST /save HTTP/1.1\r\n"
      "Host: 192.168.4.1\r\n"
      "Content-Type: application/x-www-form-urlencoded\r\n"
      "Content-Length: 11\r\n"
      "\r\n"
      "ssid=Home&x";
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete), I(parse(&p, req)));
  const HttpRequest& r = httpRequestValue(p);
  TEST_ASSERT_EQUAL_INT(M(HttpMethod::Post), M(r.method));
  TEST_ASSERT_TRUE(r.content_type_form);
  TEST_ASSERT_EQUAL_UINT32(11, r.content_length);
  TEST_ASSERT_EQUAL_UINT16(11, r.body_len);
  TEST_ASSERT_EQUAL_STRING("ssid=Home&x", r.body);
}

void test_post_content_type_with_charset_ok() {
  HttpRequestParser p;
  const char* req =
      "POST /save HTTP/1.1\r\n"
      "Host: x\r\n"
      "Content-Type: application/x-www-form-urlencoded; charset=UTF-8\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete), I(parse(&p, req)));
  TEST_ASSERT_TRUE(httpRequestValue(p).content_type_form);
  TEST_ASSERT_EQUAL_UINT16(0, httpRequestValue(p).body_len);
}

void test_header_names_case_insensitive() {
  HttpRequestParser p;
  const char* req =
      "POST /save HTTP/1.1\r\n"
      "Host: x\r\n"
      "CONTENT-type: application/x-www-form-urlencoded\r\n"
      "content-LENGTH: 3\r\n"
      "\r\n"
      "a=b";
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete), I(parse(&p, req)));
  TEST_ASSERT_EQUAL_UINT32(3, httpRequestValue(p).content_length);
}

void test_get_with_zero_content_length_ok() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(
      I(HttpParseStatus::Complete),
      I(parse(&p, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")));
}

// --- fragmentation independence ---------------------------------------------

void test_fragmentation_one_byte_vs_chunks_identical() {
  const char* req =
      "POST /save HTTP/1.1\r\n"
      "Host: h\r\n"
      "Content-Type: application/x-www-form-urlencoded\r\n"
      "Content-Length: 7\r\n"
      "\r\n"
      "csrf=ab";
  const size_t n = std::strlen(req);

  HttpRequestParser one;
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete),
                        I(parse(&one, req)));  // 1-byte feeds

  // Feed as a single bulk buffer.
  HttpRequestParser bulk;
  httpRequestInit(&bulk);
  uint16_t consumed = 0;
  HttpParseStatus st = httpRequestFeed(&bulk, kDefaultHttpLimits,
                                       reinterpret_cast<const uint8_t*>(req),
                                       static_cast<uint16_t>(n), &consumed);
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete), I(st));
  TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(n), consumed);

  // Feed in awkward 3-byte chunks.
  HttpRequestParser chunk;
  httpRequestInit(&chunk);
  for (size_t off = 0; off < n;) {
    size_t len = (n - off < 3) ? (n - off) : 3;
    uint16_t c = 0;
    st = httpRequestFeed(&chunk, kDefaultHttpLimits,
                         reinterpret_cast<const uint8_t*>(req + off),
                         static_cast<uint16_t>(len), &c);
    off += len;
  }
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete), I(st));

  // All three must agree on the parsed result.
  TEST_ASSERT_EQUAL_STRING("/save", httpRequestValue(one).path);
  TEST_ASSERT_EQUAL_STRING("/save", httpRequestValue(bulk).path);
  TEST_ASSERT_EQUAL_STRING("/save", httpRequestValue(chunk).path);
  TEST_ASSERT_EQUAL_STRING("csrf=ab", httpRequestValue(bulk).body);
  TEST_ASSERT_EQUAL_STRING("csrf=ab", httpRequestValue(chunk).body);
}

void test_truncated_request_stays_incomplete() {
  HttpRequestParser p;
  httpRequestInit(&p);
  const char* partial = "GET / HTTP/1.1\r\nHost: x\r\n";  // headers unterminated
  TEST_ASSERT_EQUAL_INT(
      I(HttpParseStatus::Incomplete),
      I(feedN(&p, kDefaultHttpLimits, partial, std::strlen(partial))));
}

void test_truncated_body_stays_incomplete() {
  HttpRequestParser p;
  httpRequestInit(&p);
  const char* req =
      "POST /save HTTP/1.1\r\n"
      "Host: x\r\n"
      "Content-Type: application/x-www-form-urlencoded\r\n"
      "Content-Length: 10\r\n"
      "\r\n"
      "abc";  // only 3 of 10 body bytes
  TEST_ASSERT_EQUAL_INT(
      I(HttpParseStatus::Incomplete),
      I(feedN(&p, kDefaultHttpLimits, req, std::strlen(req))));
}

void test_extra_bytes_after_complete_are_ignored() {
  HttpRequestParser p;
  httpRequestInit(&p);
  const char* req = "GET / HTTP/1.1\r\nHost: x\r\n\r\nGARBAGE";
  uint16_t consumed = 0;
  HttpParseStatus st = httpRequestFeed(
      &p, kDefaultHttpLimits, reinterpret_cast<const uint8_t*>(req),
      static_cast<uint16_t>(std::strlen(req)), &consumed);
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete), I(st));
  TEST_ASSERT_EQUAL_UINT16(std::strlen("GET / HTTP/1.1\r\nHost: x\r\n\r\n"),
                           consumed);
  // Feeding still more returns the latched Complete and consumes nothing.
  uint16_t c2 = 99;
  st = httpRequestFeed(&p, kDefaultHttpLimits,
                       reinterpret_cast<const uint8_t*>("X"), 1, &c2);
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete), I(st));
  TEST_ASSERT_EQUAL_UINT16(0, c2);
}

void test_body_extra_bytes_ignored() {
  HttpRequestParser p;
  httpRequestInit(&p);
  const char* req =
      "POST /save HTTP/1.1\r\n"
      "Host: x\r\n"
      "Content-Type: application/x-www-form-urlencoded\r\n"
      "Content-Length: 5\r\n"
      "\r\n"
      "helloEXTRA";
  uint16_t consumed = 0;
  HttpParseStatus st = httpRequestFeed(
      &p, kDefaultHttpLimits, reinterpret_cast<const uint8_t*>(req),
      static_cast<uint16_t>(std::strlen(req)), &consumed);
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete), I(st));
  TEST_ASSERT_EQUAL_STRING("hello", httpRequestValue(p).body);
  TEST_ASSERT_EQUAL_UINT16(std::strlen(req) - std::strlen("EXTRA"), consumed);
}

// --- strict line grammar -----------------------------------------------------

void test_bare_lf_rejected() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Error),
                        I(parse(&p, "GET / HTTP/1.1\n\n")));
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

void test_bare_cr_rejected() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Error),
                        I(parse(&p, "GET / HTTP/1.1\rX")));
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

void test_control_byte_rejected() {
  HttpRequestParser p;
  httpRequestInit(&p);
  const char req[] = {'G', 'E', 'T', ' ', '/', '\t', ' ', 'H'};  // HTAB in line
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Error),
                        I(feedN(&p, kDefaultHttpLimits, req, sizeof(req))));
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

void test_non_ascii_byte_rejected() {
  HttpRequestParser p;
  httpRequestInit(&p);
  const char req[] = {'G', 'E', 'T', ' ', '/', static_cast<char>(0xC3)};
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Error),
                        I(feedN(&p, kDefaultHttpLimits, req, sizeof(req))));
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

void test_obs_fold_rejected() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(
      I(HttpParseStatus::Error),
      I(parse(&p, "GET / HTTP/1.1\r\nHost: x\r\n obsfold\r\n\r\n")));
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

void test_empty_request_line_rejected() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Error), I(parse(&p, "\r\n")));
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

void test_space_before_colon_rejected() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Error),
                        I(parse(&p, "GET / HTTP/1.1\r\nHost : x\r\n\r\n")));
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

void test_header_without_colon_rejected() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Error),
                        I(parse(&p, "GET / HTTP/1.1\r\nBadHeader\r\n\r\n")));
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

// --- method / version / target ----------------------------------------------

void test_unknown_method_405() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Error),
                        I(parse(&p, "PUT / HTTP/1.1\r\n\r\n")));
  TEST_ASSERT_EQUAL_INT(E(HttpError::MethodNotAllowed), E(httpRequestError(p)));
}

void test_lowercase_method_405() {
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Error),
                        I(parse(&p, "get / HTTP/1.1\r\n\r\n")));
  TEST_ASSERT_EQUAL_INT(E(HttpError::MethodNotAllowed), E(httpRequestError(p)));
}

void test_head_and_delete_405() {
  HttpRequestParser p;
  parse(&p, "HEAD / HTTP/1.1\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::MethodNotAllowed), E(httpRequestError(p)));
  parse(&p, "DELETE / HTTP/1.1\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::MethodNotAllowed), E(httpRequestError(p)));
}

void test_malformed_request_line_spacing() {
  HttpRequestParser p;
  parse(&p, "GET /\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
  parse(&p, "GET  / HTTP/1.1\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
  parse(&p, "GET / HTTP/1.1 x\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

void test_bad_versions_400() {
  HttpRequestParser p;
  const char* bad[] = {"GET / HTTP/2.0\r\n\r\n", "GET / HTTP/1.2\r\n\r\n",
                       "GET / FTP/1.1\r\n\r\n", "GET / HTTP/1.1x\r\n\r\n"};
  for (const char* b : bad) {
    parse(&p, b);
    TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
  }
}

void test_non_origin_form_targets_400() {
  HttpRequestParser p;
  const char* bad[] = {"GET http://evil/ HTTP/1.1\r\n\r\n",
                       "GET * HTTP/1.1\r\n\r\n",
                       "GET example.com HTTP/1.1\r\n\r\n"};
  for (const char* b : bad) {
    parse(&p, b);
    TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
  }
}

void test_traversal_and_alias_targets_rejected() {
  HttpRequestParser p;
  const char* bad[] = {
      "GET /../etc HTTP/1.1\r\n\r\n",   "GET /a/../b HTTP/1.1\r\n\r\n",
      "GET /./a HTTP/1.1\r\n\r\n",      "GET //a HTTP/1.1\r\n\r\n",
      "GET /a/ HTTP/1.1\r\n\r\n",       "GET /a%2fb HTTP/1.1\r\n\r\n",
      "GET /%2e%2e HTTP/1.1\r\n\r\n",
  };
  for (const char* b : bad) {
    parse(&p, b);
    TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
  }
}

void test_backslash_target_rejected() {
  HttpRequestParser p;
  parse(&p, "GET /a\\b HTTP/1.1\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

// --- Content-Length / Transfer-Encoding / Content-Type ----------------------

void test_duplicate_content_length_rejected() {
  HttpRequestParser p;
  const char* req =
      "POST /save HTTP/1.1\r\n"
      "Content-Type: application/x-www-form-urlencoded\r\n"
      "Content-Length: 3\r\n"
      "Content-Length: 3\r\n"
      "\r\n"
      "abc";
  parse(&p, req);
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

void test_bad_content_length_syntax_rejected() {
  HttpRequestParser p;
  const char* bads[] = {"+5", "-5", "0x10", "5.0", "abc", "", "05", "00", "5 5"};
  for (const char* v : bads) {
    char req[128];
    std::snprintf(req, sizeof(req),
                  "POST /save HTTP/1.1\r\n"
                  "Content-Type: application/x-www-form-urlencoded\r\n"
                  "Content-Length: %s\r\n\r\n",
                  v);
    parse(&p, req);
    TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
  }
}

void test_content_length_zero_is_valid() {
  HttpRequestParser p;
  const char* req =
      "POST /save HTTP/1.1\r\n"
      "Host: x\r\n"
      "Content-Type: application/x-www-form-urlencoded\r\n"
      "Content-Length: 0\r\n\r\n";
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete), I(parse(&p, req)));
}

void test_any_transfer_encoding_rejected_501() {
  HttpRequestParser p;
  const char* tes[] = {"chunked", "identity", "gzip, chunked"};
  for (const char* te : tes) {
    char req[160];
    std::snprintf(req, sizeof(req),
                  "POST /save HTTP/1.1\r\nTransfer-Encoding: %s\r\n\r\n", te);
    parse(&p, req);
    TEST_ASSERT_EQUAL_INT(E(HttpError::UnsupportedTransferEncoding),
                          E(httpRequestError(p)));
  }
}

void test_post_without_content_length_411() {
  HttpRequestParser p;
  parse(&p,
        "POST /save HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::LengthRequired), E(httpRequestError(p)));
}

void test_post_wrong_content_type_415() {
  HttpRequestParser p;
  const char* cts[] = {"text/plain", "application/json",
                       "application/x-www-form-urlencoded; charset=iso-8859-1",
                       "application/x-www-form-urlencoded; boundary=z",
                       "multipart/form-data"};
  for (const char* ct : cts) {
    char req[200];
    std::snprintf(req, sizeof(req),
                  "POST /save HTTP/1.1\r\nHost: x\r\nContent-Type: %s\r\n"
                  "Content-Length: 0\r\n\r\n",
                  ct);
    parse(&p, req);
    TEST_ASSERT_EQUAL_INT(E(HttpError::UnsupportedMediaType),
                          E(httpRequestError(p)));
  }
}

void test_post_missing_content_type_415() {
  HttpRequestParser p;
  parse(&p, "POST /save HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::UnsupportedMediaType),
                        E(httpRequestError(p)));
}

void test_get_with_positive_content_length_rejected() {
  HttpRequestParser p;
  parse(&p, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

void test_duplicate_host_and_content_type_rejected() {
  HttpRequestParser p;
  parse(&p, "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
  parse(&p,
        "POST /save HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 0\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

// --- HTTP/1.1 Host protocol requirement (fix 8) -----------------------------

void test_http_1_1_missing_host_rejected() {
  // An HTTP/1.1 request MUST carry a Host (RFC 7230 5.4). Missing -> 400.
  HttpRequestParser p;
  parse(&p, "GET / HTTP/1.1\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
  // Even a POST with a valid body section is rejected when Host is absent.
  parse(&p,
        "POST /save HTTP/1.1\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 0\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

void test_http_1_0_may_omit_host() {
  // HTTP/1.0 is allowed to omit Host (captive checks, legacy probes).
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete),
                        I(parse(&p, "GET / HTTP/1.0\r\n\r\n")));
  TEST_ASSERT_FALSE(httpRequestValue(p).has_host);
}

void test_empty_host_rejected() {
  // A present-but-empty Host is malformed for either version.
  HttpRequestParser p;
  parse(&p, "GET / HTTP/1.1\r\nHost:\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
  parse(&p, "GET / HTTP/1.1\r\nHost: \r\n\r\n");  // whitespace-only value
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
  parse(&p, "GET / HTTP/1.0\r\nHost:\r\n\r\n");
  TEST_ASSERT_EQUAL_INT(E(HttpError::BadRequest), E(httpRequestError(p)));
}

void test_external_host_value_accepted_and_unreflected() {
  // Captive-portal probes send external hostnames; the AP-only server must not
  // restrict the Host value, only require its presence. The value is not stored.
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(
      I(HttpParseStatus::Complete),
      I(parse(&p,
              "GET /generate_204 HTTP/1.1\r\nHost: connectivitycheck.gstatic.com\r\n\r\n")));
  TEST_ASSERT_TRUE(httpRequestValue(p).has_host);
}

void test_host_classification_matches_canonical_portal_only() {
  // The parser interprets the Host into a single boolean verdict (never storing
  // the bytes) so the router can host-gate the form/POST. Only the two canonical
  // portal authorities classify as portal; everything else -- an attacker origin,
  // a longer/short IP, a nondefault port, or an absent Host -- does not.
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(
      I(HttpParseStatus::Complete),
      I(parse(&p, "GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n")));
  TEST_ASSERT_TRUE(httpRequestValue(p).has_host);
  TEST_ASSERT_TRUE(httpRequestValue(p).host_is_portal);
  parse(&p, "GET / HTTP/1.1\r\nHost: 192.168.4.1:80\r\n\r\n");
  TEST_ASSERT_TRUE(httpRequestValue(p).host_is_portal);
  // Trailing whitespace is trimmed before classification.
  parse(&p, "GET / HTTP/1.1\r\nHost: 192.168.4.1  \r\n\r\n");
  TEST_ASSERT_TRUE(httpRequestValue(p).host_is_portal);
  // Any other authority (attacker origin, mDNS name, longer IP, other port).
  parse(&p, "GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n");
  TEST_ASSERT_TRUE(httpRequestValue(p).has_host);
  TEST_ASSERT_FALSE(httpRequestValue(p).host_is_portal);
  parse(&p, "GET / HTTP/1.1\r\nHost: plane-radar.local\r\n\r\n");
  TEST_ASSERT_FALSE(httpRequestValue(p).host_is_portal);
  parse(&p, "GET / HTTP/1.1\r\nHost: 192.168.4.10\r\n\r\n");
  TEST_ASSERT_FALSE(httpRequestValue(p).host_is_portal);
  parse(&p, "GET / HTTP/1.1\r\nHost: 192.168.4.1:8080\r\n\r\n");
  TEST_ASSERT_FALSE(httpRequestValue(p).host_is_portal);
  // An absent Host (HTTP/1.0) is never canonical.
  parse(&p, "GET / HTTP/1.0\r\n\r\n");
  TEST_ASSERT_FALSE(httpRequestValue(p).has_host);
  TEST_ASSERT_FALSE(httpRequestValue(p).host_is_portal);
}

// --- limit boundaries (tight test policies) ---------------------------------

void test_payload_too_large_before_body() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.body_max = 4;
  HttpRequestParser p;
  parse(&p,
        "POST /save HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 5\r\n\r\n",
        lim);
  TEST_ASSERT_EQUAL_INT(E(HttpError::PayloadTooLarge), E(httpRequestError(p)));
}

void test_body_at_limit_ok() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.body_max = 4;
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(
      I(HttpParseStatus::Complete),
      I(parse(&p,
              "POST /save HTTP/1.1\r\n"
              "Host: x\r\n"
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: 4\r\n\r\nabcd",
              lim)));
}

void test_request_line_too_long() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.request_line_max = 20;  // "GET / HTTP/1.1" is 14; push past 20
  HttpRequestParser p;
  parse(&p, "GET /aaaaaaaaaaaaaa HTTP/1.1\r\n\r\n", lim);
  TEST_ASSERT_EQUAL_INT(E(HttpError::UriTooLong), E(httpRequestError(p)));
}

void test_target_too_long() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.target_max = 8;  // request_line stays generous
  HttpRequestParser p;
  parse(&p, "GET /abcdefghij HTTP/1.1\r\n\r\n", lim);  // target is 11 bytes
  TEST_ASSERT_EQUAL_INT(E(HttpError::UriTooLong), E(httpRequestError(p)));
  // Exactly at the limit is accepted.
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Complete),
                        I(parse(&p, "GET /abcdefg HTTP/1.1\r\nHost: x\r\n\r\n", lim)));
}

void test_header_line_too_long() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.header_line_max = 12;
  HttpRequestParser p;
  parse(&p, "GET / HTTP/1.1\r\nX-Long-Header: yyyyyy\r\n\r\n", lim);
  TEST_ASSERT_EQUAL_INT(E(HttpError::HeaderLineTooLong), E(httpRequestError(p)));
}

void test_too_many_header_fields() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.header_count_max = 2;
  HttpRequestParser p;
  parse(&p, "GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", lim);
  TEST_ASSERT_EQUAL_INT(E(HttpError::TooManyHeaderFields),
                        E(httpRequestError(p)));
  // Exactly at the field limit is accepted (HTTP/1.0 needs no Host, keeping the
  // field count at the 2-header limit under test).
  TEST_ASSERT_EQUAL_INT(
      I(HttpParseStatus::Complete),
      I(parse(&p, "GET / HTTP/1.0\r\nA: 1\r\nB: 2\r\n\r\n", lim)));
}

void test_header_section_too_large() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.header_total_max = 20;  // small cumulative header budget
  HttpRequestParser p;
  parse(&p, "GET / HTTP/1.1\r\nA: 111111\r\nB: 222222\r\n\r\n", lim);
  TEST_ASSERT_EQUAL_INT(E(HttpError::HeaderSectionTooLarge),
                        E(httpRequestError(p)));
}

// --- misconfigured limits fail closed (each backing-buffer cap) --------------

// Feed a normal request under `lim`; assert it fails closed with InvalidLimits
// and parses nothing (still at the request line, no method/path captured).
void expectInvalidLimits(const HttpLimits& lim) {
  HttpRequestParser p;
  TEST_ASSERT_FALSE(core::httpLimitsValid(lim));
  HttpParseStatus st = parse(&p, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", lim);
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Error), I(st));
  TEST_ASSERT_EQUAL_INT(E(HttpError::InvalidLimits), E(httpRequestError(p)));
  const HttpRequest& r = httpRequestValue(p);
  TEST_ASSERT_EQUAL_INT(M(HttpMethod::None), M(r.method));  // nothing written
  TEST_ASSERT_EQUAL_UINT16(0, r.path_len);
}

void test_invalid_limits_request_line_cap() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.request_line_max = core::kHttpRequestLineCap + 1;
  expectInvalidLimits(lim);
}

void test_invalid_limits_target_cap() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.target_max = core::kHttpTargetCap + 1;
  expectInvalidLimits(lim);
}

void test_invalid_limits_header_line_cap() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.header_line_max = core::kHttpHeaderLineCap + 1;
  expectInvalidLimits(lim);
}

void test_invalid_limits_header_count_cap() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.header_count_max = core::kHttpHeaderCountCap + 1;
  expectInvalidLimits(lim);
}

void test_invalid_limits_header_total_cap() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.header_total_max = core::kHttpHeaderTotalCap + 1;
  expectInvalidLimits(lim);
}

void test_invalid_limits_body_cap() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.body_max = core::kHttpBodyCap + 1;
  expectInvalidLimits(lim);
}

void test_invalid_limits_mixed() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.request_line_max = core::kHttpRequestLineCap + 5;
  lim.target_max = core::kHttpTargetCap + 9;
  lim.body_max = static_cast<uint16_t>(core::kHttpBodyCap * 2);
  expectInvalidLimits(lim);
}

void test_invalid_limits_buffer_feed_consumes_nothing() {
  HttpLimits lim = kDefaultHttpLimits;
  lim.body_max = core::kHttpBodyCap + 1;
  HttpRequestParser p;
  httpRequestInit(&p);
  const char* req = "GET / HTTP/1.1\r\n\r\n";
  uint16_t consumed = 99;
  HttpParseStatus st = httpRequestFeed(
      &p, lim, reinterpret_cast<const uint8_t*>(req),
      static_cast<uint16_t>(std::strlen(req)), &consumed);
  TEST_ASSERT_EQUAL_INT(I(HttpParseStatus::Error), I(st));
  TEST_ASSERT_EQUAL_INT(E(HttpError::InvalidLimits), E(httpRequestError(p)));
  TEST_ASSERT_EQUAL_UINT16(0, consumed);
}

void test_default_and_at_cap_limits_are_valid() {
  TEST_ASSERT_TRUE(core::httpLimitsValid(kDefaultHttpLimits));
  HttpLimits at_caps = {
      core::kHttpRequestLineCap, core::kHttpTargetCap,
      core::kHttpHeaderLineCap,  core::kHttpHeaderCountCap,
      core::kHttpHeaderTotalCap, core::kHttpBodyCap};
  TEST_ASSERT_TRUE(core::httpLimitsValid(at_caps));
  // A policy exactly at every cap still parses a normal request.
  HttpRequestParser p;
  TEST_ASSERT_EQUAL_INT(
      I(HttpParseStatus::Complete),
      I(parse(&p, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", at_caps)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_simple_get_root);
  RUN_TEST(test_get_http_1_0_no_host_ok);
  RUN_TEST(test_get_splits_path_and_query_without_normalizing);
  RUN_TEST(test_empty_query_after_question_mark);
  RUN_TEST(test_post_form_body);
  RUN_TEST(test_post_content_type_with_charset_ok);
  RUN_TEST(test_header_names_case_insensitive);
  RUN_TEST(test_get_with_zero_content_length_ok);
  RUN_TEST(test_fragmentation_one_byte_vs_chunks_identical);
  RUN_TEST(test_truncated_request_stays_incomplete);
  RUN_TEST(test_truncated_body_stays_incomplete);
  RUN_TEST(test_extra_bytes_after_complete_are_ignored);
  RUN_TEST(test_body_extra_bytes_ignored);
  RUN_TEST(test_bare_lf_rejected);
  RUN_TEST(test_bare_cr_rejected);
  RUN_TEST(test_control_byte_rejected);
  RUN_TEST(test_non_ascii_byte_rejected);
  RUN_TEST(test_obs_fold_rejected);
  RUN_TEST(test_empty_request_line_rejected);
  RUN_TEST(test_space_before_colon_rejected);
  RUN_TEST(test_header_without_colon_rejected);
  RUN_TEST(test_unknown_method_405);
  RUN_TEST(test_lowercase_method_405);
  RUN_TEST(test_head_and_delete_405);
  RUN_TEST(test_malformed_request_line_spacing);
  RUN_TEST(test_bad_versions_400);
  RUN_TEST(test_non_origin_form_targets_400);
  RUN_TEST(test_traversal_and_alias_targets_rejected);
  RUN_TEST(test_backslash_target_rejected);
  RUN_TEST(test_duplicate_content_length_rejected);
  RUN_TEST(test_bad_content_length_syntax_rejected);
  RUN_TEST(test_content_length_zero_is_valid);
  RUN_TEST(test_any_transfer_encoding_rejected_501);
  RUN_TEST(test_post_without_content_length_411);
  RUN_TEST(test_post_wrong_content_type_415);
  RUN_TEST(test_post_missing_content_type_415);
  RUN_TEST(test_get_with_positive_content_length_rejected);
  RUN_TEST(test_duplicate_host_and_content_type_rejected);
  RUN_TEST(test_http_1_1_missing_host_rejected);
  RUN_TEST(test_http_1_0_may_omit_host);
  RUN_TEST(test_empty_host_rejected);
  RUN_TEST(test_external_host_value_accepted_and_unreflected);
  RUN_TEST(test_host_classification_matches_canonical_portal_only);
  RUN_TEST(test_payload_too_large_before_body);
  RUN_TEST(test_body_at_limit_ok);
  RUN_TEST(test_request_line_too_long);
  RUN_TEST(test_target_too_long);
  RUN_TEST(test_header_line_too_long);
  RUN_TEST(test_too_many_header_fields);
  RUN_TEST(test_header_section_too_large);
  RUN_TEST(test_invalid_limits_request_line_cap);
  RUN_TEST(test_invalid_limits_target_cap);
  RUN_TEST(test_invalid_limits_header_line_cap);
  RUN_TEST(test_invalid_limits_header_count_cap);
  RUN_TEST(test_invalid_limits_header_total_cap);
  RUN_TEST(test_invalid_limits_body_cap);
  RUN_TEST(test_invalid_limits_mixed);
  RUN_TEST(test_invalid_limits_buffer_feed_consumes_nothing);
  RUN_TEST(test_default_and_at_cap_limits_are_valid);
  return UNITY_END();
}
