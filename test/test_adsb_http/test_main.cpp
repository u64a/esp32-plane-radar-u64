#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "services/adsb_http.h"

#include "../support/adsb_stream_fakes.h"

using namespace services::adsb;
using test_support::AdvancingIdle;
using test_support::CountingClock;
using test_support::ScriptedByteSource;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

void setUp() {}
void tearDown() {}

namespace {

class RecordingSink : public BodySink {
 public:
  void begin(int status) override { status_ = status; }
  bool write(const uint8_t* data, size_t length) override {
    body_.append(reinterpret_cast<const char*>(data), length);
    return true;
  }
  bool finish() override { return true; }
  int status_ = -1;
  std::string body_;
};

struct Result {
  HttpResponseResult r;
  std::string body;
};

uint8_t g_scratch[512];
char g_line[1024];

Result run(const std::string& raw, size_t frag,
           ScriptedByteSource::Terminal terminal = ScriptedByteSource::Terminal::End,
           HttpLimits limits = kDefaultHttpLimits,
           HttpDeadlines dl = {60000, 30000}, uint32_t clock_start = 0) {
  ScriptedByteSource src(raw, frag, terminal);
  CountingClock clock(clock_start);
  AdvancingIdle idle(&clock, 10);
  RecordingSink sink;
  HttpWorkspace ws{g_scratch, sizeof(g_scratch), g_line, sizeof(g_line)};
  HttpResponseResult r = decodeHttpResponse(src, clock, idle, limits, dl, ws, sink);
  return {r, sink.body_};
}

std::string crlf(std::vector<std::string> lines) {
  std::string s;
  for (auto& l : lines) {
    s += l;
    s += "\r\n";
  }
  return s;
}

HttpOutcome outcome(const std::string& raw, size_t frag,
                    HttpLimits limits = kDefaultHttpLimits) {
  return run(raw, frag, ScriptedByteSource::Terminal::End, limits).r.outcome;
}

}  // namespace

void test_content_length_every_split_offset() {
  const std::string body = R"({"ac":[]})";
  std::string raw =
      crlf({"HTTP/1.1 200 OK", "Content-Length: 9", "Connection: close", ""}) + body;
  for (size_t f = 1; f <= raw.size(); ++f) {
    Result r = run(raw, f);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok),
                          static_cast<int>(r.r.outcome));
    TEST_ASSERT_EQUAL_INT(200, r.r.status);
    TEST_ASSERT_EQUAL_STRING(body.c_str(), r.body.c_str());
    TEST_ASSERT_EQUAL_UINT32(body.size(), r.r.decoded_body_bytes);
  }
}

void test_chunked_and_close_delimited() {
  std::string chunked = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", ""}) +
                        "5\r\n{\"ac\"\r\n5\r\n:[1]}\r\n0\r\n\r\n";
  for (size_t f = 1; f <= chunked.size(); ++f) {
    Result r = run(chunked, f);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok),
                          static_cast<int>(r.r.outcome));
    TEST_ASSERT_EQUAL_STRING(R"({"ac":[1]})", r.body.c_str());
  }
  std::string close = crlf({"HTTP/1.1 200 OK", ""}) + R"({"ac":[]})";
  Result r = run(close, 3);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok),
                        static_cast<int>(r.r.outcome));
  TEST_ASSERT_EQUAL_STRING(R"({"ac":[]})", r.body.c_str());
}

void test_crlf_errors() {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(HttpOutcome::ParseError),
      static_cast<int>(outcome("HTTP/1.1 200 OK\nContent-Length: 0\r\n\r\n", 5)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError),
                        static_cast<int>(outcome("HTTP/1.1 200 OK\rX", 5)));
}

void test_status_line_exact_limit_and_over() {
  std::string raw = crlf({"HTTP/1.1 200 OK", "Content-Length: 0", ""});
  HttpLimits ok = kDefaultHttpLimits;
  ok.status_line_bytes = 15;  // "HTTP/1.1 200 OK" is 15 bytes
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(outcome(raw, 4, ok)));
  HttpLimits over = kDefaultHttpLimits;
  over.status_line_bytes = 14;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ResponseTooLarge),
                        static_cast<int>(outcome(raw, 4, over)));
}

void test_header_line_exact_limit_and_over() {
  std::string raw = crlf({"HTTP/1.1 200 OK", "Content-Length: 0", ""});
  HttpLimits ok = kDefaultHttpLimits;
  ok.header_line_bytes = 17;  // "Content-Length: 0" is 17 bytes
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(outcome(raw, 4, ok)));
  HttpLimits over = kDefaultHttpLimits;
  over.header_line_bytes = 16;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ResponseTooLarge),
                        static_cast<int>(outcome(raw, 4, over)));
}

void test_header_total_and_field_count_limits() {
  std::string raw = crlf({"HTTP/1.1 200 OK", "A: 1", "B: 2", "Content-Length: 0", ""});
  HttpLimits count_ok = kDefaultHttpLimits;
  count_ok.max_header_fields = 3;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok),
                        static_cast<int>(outcome(raw, 5, count_ok)));
  HttpLimits count_over = kDefaultHttpLimits;
  count_over.max_header_fields = 2;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ResponseTooLarge),
                        static_cast<int>(outcome(raw, 5, count_over)));
  HttpLimits total_over = kDefaultHttpLimits;
  total_over.total_header_bytes = 10;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ResponseTooLarge),
                        static_cast<int>(outcome(raw, 5, total_over)));
}

void test_content_length_body_exact_and_over() {
  const std::string body = R"({"ac":[]})";  // 9 bytes
  std::string raw = crlf({"HTTP/1.1 200 OK", "Content-Length: 9", ""}) + body;
  HttpLimits ok = kDefaultHttpLimits;
  ok.max_body_bytes = 9;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(outcome(raw, 5, ok)));
  HttpLimits over = kDefaultHttpLimits;
  over.max_body_bytes = 8;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ResponseTooLarge),
                        static_cast<int>(outcome(raw, 5, over)));
}

void test_content_length_decimal_duplicate_conflict_overflow() {
  std::string dup = crlf({"HTTP/1.1 200 OK", "Content-Length: 9", "Content-Length: 9", ""}) + R"({"ac":[]})";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(outcome(dup, 5)));
  std::string conflict = crlf({"HTTP/1.1 200 OK", "Content-Length: 9", "Content-Length: 8", ""}) + R"({"ac":[]})";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError), static_cast<int>(outcome(conflict, 5)));
  std::string hexish = crlf({"HTTP/1.1 200 OK", "Content-Length: 0x9", ""}) + R"({"ac":[]})";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError), static_cast<int>(outcome(hexish, 5)));
  // A syntactically numeric value that overflows the counter is oversized, not a
  // grammar error.
  std::string overflow = crlf({"HTTP/1.1 200 OK", "Content-Length: 99999999999999999999999999", ""});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ResponseTooLarge), static_cast<int>(outcome(overflow, 5)));
}

void test_content_length_list_members() {
  // Identical comma-separated values (with OWS) remain valid and collapse to one.
  std::string list = crlf({"HTTP/1.1 200 OK", "Content-Length: 9,9", ""}) + R"({"ac":[]})";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(outcome(list, 5)));
  std::string listows = crlf({"HTTP/1.1 200 OK", "Content-Length: 9, 9", ""}) + R"({"ac":[]})";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(outcome(listows, 5)));
  // Empty list members (leading, trailing, repeated, OWS-only) are rejected.
  const char* bad[] = {",5", "5,", "5,,5", "5,   ", " ,5", "5, ,5"};
  for (const char* v : bad) {
    std::string raw = crlf({"HTTP/1.1 200 OK", std::string("Content-Length: ") + v, ""}) + R"({"ac":[]})";
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(HttpOutcome::ParseError),
                                  static_cast<int>(outcome(raw, 5)), v);
  }
}

void test_transfer_encoding_edge_cases() {
  std::string tecl = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", "Content-Length: 9", ""}) + R"({"ac":[]})";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError), static_cast<int>(outcome(tecl, 5)));
  std::string gzip = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: gzip, chunked", ""}) + "0\r\n\r\n";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError), static_cast<int>(outcome(gzip, 5)));
  std::string repeat = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", "Transfer-Encoding: chunked", ""}) + "0\r\n\r\n";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError), static_cast<int>(outcome(repeat, 5)));
  std::string param = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked; q=1", ""}) + "0\r\n\r\n";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError), static_cast<int>(outcome(param, 5)));
  std::string http10 = crlf({"HTTP/1.0 200 OK", "Transfer-Encoding: chunked", ""}) + "0\r\n\r\n";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError), static_cast<int>(outcome(http10, 5)));
}

void test_chunk_size_line_overflow_and_bad_terminator() {
  std::string overflow = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", ""}) +
                         "FFFFFFFFF\r\n";  // > 32-bit: numeric overflow, oversized
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ResponseTooLarge), static_cast<int>(outcome(overflow, 5)));
  std::string badterm = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", ""}) +
                        "1\r\nX";  // data byte then no CRLF (premature)
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::TransportError), static_cast<int>(outcome(badterm, 5)));
  std::string notcrlf = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", ""}) +
                        "1\r\nXY\r\n0\r\n\r\n";  // 2 bytes for a size-1 chunk
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError), static_cast<int>(outcome(notcrlf, 5)));
}

void test_chunk_extension_grammar() {
  // Valid bounded extensions on both data and terminating chunks, verified at
  // every fragmentation boundary so the extension scanner is fragment-safe.
  std::string ok = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", ""}) +
                   "5;n=1\r\n{\"ac\"\r\n5;t=\"a;b\"\r\n:[1]}\r\n0;final;k=v\r\n\r\n";
  for (size_t f = 1; f <= ok.size(); ++f) {
    Result r = run(ok, f);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(r.r.outcome));
    TEST_ASSERT_EQUAL_STRING(R"({"ac":[1]})", r.body.c_str());
  }
  // OWS/BWS around ';' and '=' is tolerated.
  std::string ows = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", ""}) +
                    "3 ; a = b \r\nabc\r\n0\r\n\r\n";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(outcome(ows, 4)));

  // Malformed extensions are ParseError, not accepted and not oversized.
  const char* bad[] = {
      "5;=b",       // empty extension name
      "5;;a",       // empty first member
      "5;a=",       // '=' with no value
      "5;a=\"b",    // unterminated quoted-string
      "5;a b",      // junk where a ';' was required
      "5;a=@",      // non-token, non-quoted value
      "5;",         // trailing ';' with no name
      "5 x",        // junk after size (no ';')
  };
  for (const char* sz : bad) {
    std::string raw = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", ""}) +
                      std::string(sz) + "\r\nHELLO\r\n0\r\n\r\n";
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(HttpOutcome::ParseError),
                                  static_cast<int>(outcome(raw, 1)), sz);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(HttpOutcome::ParseError),
                                  static_cast<int>(outcome(raw, 7)), sz);
  }
}

void test_chunk_count_limit() {
  // Three data chunks + one terminating zero chunk = four counted chunks.
  std::string raw = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", ""}) +
                    "1\r\na\r\n1\r\nb\r\n1\r\nc\r\n0\r\n\r\n";
  HttpLimits ok = kDefaultHttpLimits;
  ok.max_chunks = 4;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(outcome(raw, 6, ok)));
  HttpLimits over = kDefaultHttpLimits;
  over.max_chunks = 3;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ResponseTooLarge), static_cast<int>(outcome(raw, 6, over)));
}

void test_trailer_field_and_byte_limits_and_framing_reject() {
  std::string raw = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", ""}) +
                    "0\r\nX-A: 1\r\nX-B: 2\r\n\r\n";
  HttpLimits ok = kDefaultHttpLimits;
  ok.max_trailer_fields = 2;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(outcome(raw, 6, ok)));
  HttpLimits over = kDefaultHttpLimits;
  over.max_trailer_fields = 1;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ResponseTooLarge), static_cast<int>(outcome(raw, 6, over)));
  std::string framing = crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", ""}) +
                        "0\r\nContent-Length: 5\r\n\r\n";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError), static_cast<int>(outcome(framing, 6)));
}

void test_premature_eof_and_stream_error() {
  std::string partial = crlf({"HTTP/1.1 200 OK", "Content-Length: 9", ""}) + "{\"ac";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::TransportError),
                        static_cast<int>(run(partial, 5).r.outcome));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(HttpOutcome::TransportError),
      static_cast<int>(run(partial, 5, ScriptedByteSource::Terminal::Error).r.outcome));
}

void test_stall_timeout_and_uint32_rollover() {
  std::string partial = crlf({"HTTP/1.1 200 OK", "Content-Length: 9", ""}) + "{\"ac";
  HttpDeadlines dl{0, 50};
  Result r = run(partial, 5, ScriptedByteSource::Terminal::Stall, kDefaultHttpLimits, dl);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Timeout), static_cast<int>(r.r.outcome));
  // Same stall with the clock starting just below rollover.
  Result rollover =
      run(partial, 5, ScriptedByteSource::Terminal::Stall, kDefaultHttpLimits, dl, UINT32_MAX - 20);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Timeout), static_cast<int>(rollover.r.outcome));
}

void test_status_mapping_and_retry_after() {
  auto statusResp = [](int code, const std::string& extra = "") {
    char b[192];
    std::snprintf(b, sizeof(b), "HTTP/1.1 %d X\r\n%sContent-Length: 0\r\n\r\n", code, extra.c_str());
    return std::string(b);
  };
  TEST_ASSERT_EQUAL_INT(204, run(statusResp(204), 5).r.status);
  TEST_ASSERT_EQUAL_INT(304, run(statusResp(304), 5).r.status);
  Result r429 = run(statusResp(429, "Retry-After: 120\r\n"), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(r429.r.outcome));
  TEST_ASSERT_TRUE(r429.r.retry_after_present);
  TEST_ASSERT_EQUAL_UINT32(120000, r429.r.retry_after_ms);
  Result rdate = run(statusResp(503, "Retry-After: Wed, 21 Oct 2099 07:28:00 GMT\r\n"), 5);
  TEST_ASSERT_FALSE(rdate.r.retry_after_present);
  Result rclamp = run(statusResp(503, "Retry-After: 99999999\r\n"), 5);
  TEST_ASSERT_TRUE(rclamp.r.retry_after_present);
  TEST_ASSERT_EQUAL_UINT32(604800000, rclamp.r.retry_after_ms);  // clamped to 7 days
}

void test_bad_status_lines() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError),
                        static_cast<int>(outcome("HTTP/2.0 200 OK\r\n\r\n", 5)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError),
                        static_cast<int>(outcome("HTTP/1.1 20 OK\r\n\r\n", 5)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError),
                        static_cast<int>(outcome("garbage\r\n\r\n", 5)));
}

void test_invalid_header_name_and_obs_fold() {
  std::string badname = crlf({"HTTP/1.1 200 OK", "Bad Header: x", "Content-Length: 0", ""});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError), static_cast<int>(outcome(badname, 5)));
  std::string fold = "HTTP/1.1 200 OK\r\nX: a\r\n b\r\nContent-Length: 0\r\n\r\n";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError), static_cast<int>(outcome(fold, 5)));
}

void test_trailer_grammar_validation() {
  auto trailers = [](const std::string& body) {
    return crlf({"HTTP/1.1 200 OK", "Transfer-Encoding: chunked", ""}) + "0\r\n" +
           body + "\r\n";
  };
  // A well-formed trailer field is accepted.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok),
                        static_cast<int>(outcome(trailers("X-Trace: abc\r\n"), 5)));
  // Missing colon, empty name, and a bad field-name token are all rejected.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError),
                        static_cast<int>(outcome(trailers("nocolon\r\n"), 5)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError),
                        static_cast<int>(outcome(trailers(": value\r\n"), 5)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError),
                        static_cast<int>(outcome(trailers("Bad Name: v\r\n"), 5)));
  // An obs-fold trailer line is rejected.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError),
                        static_cast<int>(outcome(trailers(" X-Trace: abc\r\n"), 5)));
  // Framing field names remain rejected case-insensitively.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::ParseError),
                        static_cast<int>(outcome(trailers("content-length: 5\r\n"), 5)));
}

void test_status_code_range_and_1xx_rejected() {
  auto st = [](const char* code) {
    return std::string("HTTP/1.1 ") + code + " X\r\nContent-Length: 0\r\n\r\n";
  };
  // Out-of-range and 1xx informational lines are unsupported framing.
  const char* rejected[] = {"099", "100", "199", "600", "999"};
  for (const char* code : rejected) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(HttpOutcome::ParseError),
                                  static_cast<int>(outcome(st(code), 5)), code);
  }
  Result r200 = run(st("200"), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(r200.r.outcome));
  TEST_ASSERT_EQUAL_INT(200, r200.r.status);
  Result r599 = run(st("599"), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(r599.r.outcome));
  TEST_ASSERT_EQUAL_INT(599, r599.r.status);
}

void test_retry_after_duplicate_handling() {
  auto resp2 = [](const std::string& a, const std::string& b) {
    return "HTTP/1.1 503 X\r\n" + a + b + "Content-Length: 0\r\n\r\n";
  };
  // Identical valid deltas stay usable.
  Result same = run(resp2("Retry-After: 30\r\n", "Retry-After: 30\r\n"), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(same.r.outcome));
  TEST_ASSERT_TRUE(same.r.retry_after_present);
  TEST_ASSERT_EQUAL_UINT32(30000, same.r.retry_after_ms);
  // Conflicting deltas disable Retry-After without failing the response.
  Result conflict = run(resp2("Retry-After: 30\r\n", "Retry-After: 60\r\n"), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(conflict.r.outcome));
  TEST_ASSERT_FALSE(conflict.r.retry_after_present);
  // A malformed (HTTP-date) duplicate also disables it.
  Result malformed =
      run(resp2("Retry-After: 30\r\n", "Retry-After: Wed, 21 Oct 2099 07:28:00 GMT\r\n"), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(malformed.r.outcome));
  TEST_ASSERT_FALSE(malformed.r.retry_after_present);
  // Disabling is order-independent and sticky across three headers.
  Result triple = run(
      "HTTP/1.1 429 X\r\nRetry-After: 5\r\nRetry-After: 5\r\nRetry-After: 9\r\nContent-Length: 0\r\n\r\n",
      5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpOutcome::Ok), static_cast<int>(triple.r.outcome));
  TEST_ASSERT_FALSE(triple.r.retry_after_present);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_content_length_every_split_offset);
  RUN_TEST(test_chunked_and_close_delimited);
  RUN_TEST(test_crlf_errors);
  RUN_TEST(test_status_line_exact_limit_and_over);
  RUN_TEST(test_header_line_exact_limit_and_over);
  RUN_TEST(test_header_total_and_field_count_limits);
  RUN_TEST(test_content_length_body_exact_and_over);
  RUN_TEST(test_content_length_decimal_duplicate_conflict_overflow);
  RUN_TEST(test_content_length_list_members);
  RUN_TEST(test_transfer_encoding_edge_cases);
  RUN_TEST(test_chunk_size_line_overflow_and_bad_terminator);
  RUN_TEST(test_chunk_extension_grammar);
  RUN_TEST(test_chunk_count_limit);
  RUN_TEST(test_trailer_field_and_byte_limits_and_framing_reject);
  RUN_TEST(test_trailer_grammar_validation);
  RUN_TEST(test_premature_eof_and_stream_error);
  RUN_TEST(test_stall_timeout_and_uint32_rollover);
  RUN_TEST(test_status_mapping_and_retry_after);
  RUN_TEST(test_status_code_range_and_1xx_rejected);
  RUN_TEST(test_retry_after_duplicate_handling);
  RUN_TEST(test_bad_status_lines);
  RUN_TEST(test_invalid_header_name_and_obs_fold);
  return UNITY_END();
}
