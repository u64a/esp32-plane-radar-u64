#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "services/adsb_fetch.h"

#include "../support/adsb_stream_fakes.h"

using namespace services::adsb;
using test_support::AdvancingIdle;
using test_support::CountingClock;
using test_support::ScriptedByteSource;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

struct Buffers {
  AircraftSnapshot snapshots[2];
  float distances[kMaxAircraft];
  uint16_t ordinals[kMaxAircraft];
  char object_buffer[kObjectBufferBytes];
  unsigned char arena[kJsonArenaBytes];
  uint8_t scratch[512];
  char line[1024];
};

Buffers g_b;
uint8_t g_active;
uint32_t g_revision;

FetchWorkspace ws() {
  FetchWorkspace w{};
  w.http = HttpWorkspace{g_b.scratch, sizeof(g_b.scratch), g_b.line, sizeof(g_b.line)};
  w.object_buffer = g_b.object_buffer;
  w.object_capacity = sizeof(g_b.object_buffer);
  w.arena = g_b.arena;
  w.arena_size = sizeof(g_b.arena);
  w.distances = g_b.distances;
  w.ordinals = g_b.ordinals;
  return w;
}

FetchResult fetch(const std::string& raw, size_t frag,
                  HttpLimits limits = kDefaultHttpLimits,
                  ParseOptions po = ParseOptions{kDefaultParseLimits, false},
                  size_t arena_size = kJsonArenaBytes) {
  ScriptedByteSource src(raw, frag, ScriptedByteSource::Terminal::End);
  CountingClock clock;
  AdvancingIdle idle(&clock, 1);
  const uint8_t inactive = g_active ^ 1;
  HttpDeadlines dl{60000, 30000};
  FetchWorkspace w = ws();
  w.arena_size = arena_size;
  FetchResult fr = runFetch(src, clock, idle, 0.0, 0.0, po, limits, dl, w,
                            g_b.snapshots[inactive], g_revision + 1);
  if (fr.outcome == FetchOutcome::Ok) {
    g_revision += 1;
    g_active = inactive;
  }
  return fr;
}

std::string resp(const std::string& body) {
  char hdr[128];
  std::snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                body.size());
  return std::string(hdr) + body;
}

std::string manyAircraftBody(int valid, const std::string& tail) {
  std::string j = "{\"ac\":[";
  for (int i = 0; i < valid; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s{\"lat\":0,\"lon\":%f}", i ? "," : "",
                  0.001 * (i + 1));
    j += buf;
  }
  j += tail;  // e.g. a trailing malformed/oversized element then "]}"
  return j;
}

}  // namespace

void setUp() {
  std::memset(&g_b, 0, sizeof(g_b));
  g_active = 0;
  g_revision = 0;
}
void tearDown() {}

void test_success_with_aircraft_publishes_and_bumps_revision() {
  FetchResult r = fetch(resp(R"({"ac":[{"lat":1,"lon":2}]})"), 4);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Ok), static_cast<int>(r.outcome));
  TEST_ASSERT_EQUAL_UINT16(1, r.aircraft_count);
  TEST_ASSERT_EQUAL_UINT16(1, g_b.snapshots[g_active].count);
  TEST_ASSERT_EQUAL_UINT32(g_revision, g_b.snapshots[g_active].source_revision);
}

void test_empty_success_publishes_once() {
  FetchResult r = fetch(resp(R"({"ac":[]})"), 7);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Ok), static_cast<int>(r.outcome));
  TEST_ASSERT_EQUAL_UINT16(0, g_b.snapshots[g_active].count);
  TEST_ASSERT_EQUAL_UINT32(1, g_revision);
}

void test_failures_preserve_prior_snapshot_byte_for_byte() {
  fetch(resp(R"({"ac":[{"lat":10,"lon":20,"flight":"KEEP"}]})"), 5);
  const AircraftSnapshot saved = g_b.snapshots[g_active];
  const uint8_t saved_active = g_active;
  const uint32_t saved_rev = g_revision;

  struct Case {
    std::string raw;
    FetchOutcome expected;
  };
  const Case cases[] = {
      {resp(R"({"ac":[{"lat":1,"lon":)"), FetchOutcome::ParseError},   // truncated
      {resp(R"({"ac":[{"lat":1 "lon":2}]})"), FetchOutcome::ParseError},  // malformed
      {resp(R"({"bad":1})"), FetchOutcome::ParseError},                // missing ac
  };
  for (const Case& c : cases) {
    FetchResult r = fetch(c.raw, 5);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(c.expected), static_cast<int>(r.outcome));
    TEST_ASSERT_EQUAL_UINT8(saved_active, g_active);
    TEST_ASSERT_EQUAL_UINT32(saved_rev, g_revision);
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(&saved, &g_b.snapshots[g_active], sizeof(saved)));
  }
  TEST_ASSERT_EQUAL_STRING("KEEP", g_b.snapshots[g_active].aircraft[0].callsign);
}

void test_oversized_body_does_not_publish() {
  fetch(resp(R"({"ac":[{"lat":10,"lon":20,"flight":"KEEP"}]})"), 5);
  const uint8_t saved_active = g_active;
  HttpLimits l = kDefaultHttpLimits;
  l.max_body_bytes = 4;
  FetchResult r = fetch(resp(R"({"ac":[]})"), 5, l);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::ResponseTooLarge), static_cast<int>(r.outcome));
  TEST_ASSERT_EQUAL_UINT8(saved_active, g_active);
  TEST_ASSERT_EQUAL_STRING("KEEP", g_b.snapshots[g_active].aircraft[0].callsign);
}

void test_no_memory_maps_and_preserves() {
  fetch(resp(R"({"ac":[{"lat":10,"lon":20,"flight":"KEEP"}]})"), 5);
  const uint8_t saved_active = g_active;
  FetchResult r = fetch(resp(R"({"ac":[{"lat":1,"lon":2}]})"), 5, kDefaultHttpLimits,
                        ParseOptions{kDefaultParseLimits, false}, 64);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::NoMemory), static_cast<int>(r.outcome));
  TEST_ASSERT_EQUAL_UINT8(saved_active, g_active);
  TEST_ASSERT_EQUAL_STRING("KEEP", g_b.snapshots[g_active].aircraft[0].callsign);
}

void test_malformed_after_64_retained_fails_without_publishing() {
  fetch(resp(R"({"ac":[{"lat":10,"lon":20,"flight":"KEEP"}]})"), 5);
  const uint8_t saved_active = g_active;
  const uint32_t saved_rev = g_revision;

  // 100 valid aircraft (parser keeps nearest 64) then a malformed element.
  std::string body = manyAircraftBody(100, R"(,{"lat":1 "lon":2}]})");
  FetchResult r = fetch(resp(body), 7);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::ParseError), static_cast<int>(r.outcome));
  TEST_ASSERT_EQUAL_UINT8(saved_active, g_active);
  TEST_ASSERT_EQUAL_UINT32(saved_rev, g_revision);
  TEST_ASSERT_EQUAL_STRING("KEEP", g_b.snapshots[g_active].aircraft[0].callsign);
}

void test_oversized_object_after_64_retained_fails_without_publishing() {
  fetch(resp(R"({"ac":[{"lat":10,"lon":20,"flight":"KEEP"}]})"), 5);
  const uint8_t saved_active = g_active;
  ParseOptions po{kDefaultParseLimits, false};
  po.limits.max_object_bytes = 40;
  std::string body =
      manyAircraftBody(100, R"(,{"lat":1,"lon":2,"flight":"WAYTOOLONGTOFITTHELIMIT"}]})");
  FetchResult r = fetch(resp(body), 9, kDefaultHttpLimits, po);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::ResponseTooLarge), static_cast<int>(r.outcome));
  TEST_ASSERT_EQUAL_UINT8(saved_active, g_active);
  TEST_ASSERT_EQUAL_STRING("KEEP", g_b.snapshots[g_active].aircraft[0].callsign);
}

void test_status_mapping_with_retry_after() {
  auto statusResp = [](int code, const std::string& extra = "") {
    char b[160];
    std::snprintf(b, sizeof(b), "HTTP/1.1 %d X\r\n%sContent-Length: 0\r\n\r\n", code, extra.c_str());
    return std::string(b);
  };
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::HttpOther), static_cast<int>(fetch(statusResp(400), 5).outcome));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::HttpOther), static_cast<int>(fetch(statusResp(204), 5).outcome));
  FetchResult r429 = fetch(statusResp(429, "Retry-After: 12\r\n"), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Http429), static_cast<int>(r429.outcome));
  TEST_ASSERT_EQUAL_UINT32(12000, r429.retry_after_ms);
  FetchResult r503 = fetch(statusResp(503, "Retry-After: 5\r\n"), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Http5xx), static_cast<int>(r503.outcome));
  TEST_ASSERT_EQUAL_UINT32(5000, r503.retry_after_ms);
  FetchResult r500 = fetch(statusResp(500), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Http5xx), static_cast<int>(r500.outcome));
  TEST_ASSERT_EQUAL_UINT32(0, r500.retry_after_ms);
}

void test_one_byte_reads_and_split_offsets() {
  std::string raw = resp(R"({"ac":[{"lat":1,"lon":2},{"lat":3,"lon":4}]})");
  for (size_t f = 1; f <= raw.size(); ++f) {
    setUp();  // reset buffers per split so publishing is observable
    FetchResult r = fetch(raw, f);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Ok), static_cast<int>(r.outcome));
    TEST_ASSERT_EQUAL_UINT16(2, g_b.snapshots[g_active].count);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_success_with_aircraft_publishes_and_bumps_revision);
  RUN_TEST(test_empty_success_publishes_once);
  RUN_TEST(test_failures_preserve_prior_snapshot_byte_for_byte);
  RUN_TEST(test_oversized_body_does_not_publish);
  RUN_TEST(test_no_memory_maps_and_preserves);
  RUN_TEST(test_malformed_after_64_retained_fails_without_publishing);
  RUN_TEST(test_oversized_object_after_64_retained_fails_without_publishing);
  RUN_TEST(test_status_mapping_with_retry_after);
  RUN_TEST(test_one_byte_reads_and_split_offsets);
  return UNITY_END();
}
