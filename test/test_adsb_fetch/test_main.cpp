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
  TEST_ASSERT_EQUAL_UINT32(g_revision, g_b.snapshots[g_active].settings_revision);
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
  TEST_ASSERT_TRUE(r429.retry_after_present);
  TEST_ASSERT_EQUAL_UINT32(12000, r429.retry_after_ms);
  FetchResult r503 = fetch(statusResp(503, "Retry-After: 5\r\n"), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Http5xx), static_cast<int>(r503.outcome));
  TEST_ASSERT_TRUE(r503.retry_after_present);
  TEST_ASSERT_EQUAL_UINT32(5000, r503.retry_after_ms);
  FetchResult r500 = fetch(statusResp(500), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Http5xx), static_cast<int>(r500.outcome));
  TEST_ASSERT_FALSE(r500.retry_after_present);
  TEST_ASSERT_EQUAL_UINT32(0, r500.retry_after_ms);
  // Presence bit distinguishes a header value of 0 from an absent header.
  FetchResult r429_zero = fetch(statusResp(429, "Retry-After: 0\r\n"), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Http429), static_cast<int>(r429_zero.outcome));
  TEST_ASSERT_TRUE(r429_zero.retry_after_present);
  TEST_ASSERT_EQUAL_UINT32(0, r429_zero.retry_after_ms);
  FetchResult r429_absent = fetch(statusResp(429), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Http429), static_cast<int>(r429_absent.outcome));
  TEST_ASSERT_FALSE(r429_absent.retry_after_present);
  // A malformed/date Retry-After is treated as absent (presence bit false).
  FetchResult r429_date = fetch(statusResp(429, "Retry-After: Wed, 21 Oct 2099 07:28:00 GMT\r\n"), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Http429), static_cast<int>(r429_date.outcome));
  TEST_ASSERT_FALSE(r429_date.retry_after_present);
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

void test_premature_eof_maps_to_transport_failure() {
  // Seed a prior published snapshot to prove the failure preserves it.
  fetch(resp(R"({"ac":[{"lat":10,"lon":20,"flight":"KEEP"}]})"), 5);
  const uint8_t saved_active = g_active;
  const uint32_t saved_rev = g_revision;

  // Content-Length claims 9 body bytes but the stream ends after 4: a premature
  // EOF is a network read failure (HttpOutcome::TransportError). It must map to
  // FetchOutcome::TransportFailure (transient), NOT ParseError (permanent), so a
  // mid-fetch link drop is never conflated with genuinely malformed JSON.
  const std::string truncated =
      "HTTP/1.1 200 OK\r\nContent-Length: 9\r\nConnection: close\r\n\r\n{\"ac";
  FetchResult r = fetch(truncated, 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::TransportFailure),
                        static_cast<int>(r.outcome));
  TEST_ASSERT_EQUAL_UINT8(saved_active, g_active);  // nothing published
  TEST_ASSERT_EQUAL_UINT32(saved_rev, g_revision);
  TEST_ASSERT_EQUAL_STRING("KEEP", g_b.snapshots[g_active].aircraft[0].callsign);
}

void test_network_abort_sensitive_classification() {
  // Link-interruption-sensitive: the network read/abort family plus ParseError
  // (a truncated body from a dropped link is indistinguishable from bad JSON).
  TEST_ASSERT_TRUE(fetchOutcomeNetworkAbortSensitive(FetchOutcome::Timeout));
  TEST_ASSERT_TRUE(fetchOutcomeNetworkAbortSensitive(FetchOutcome::DnsFailure));
  TEST_ASSERT_TRUE(fetchOutcomeNetworkAbortSensitive(FetchOutcome::TlsFailure));
  TEST_ASSERT_TRUE(
      fetchOutcomeNetworkAbortSensitive(FetchOutcome::TransportFailure));
  TEST_ASSERT_TRUE(fetchOutcomeNetworkAbortSensitive(FetchOutcome::ParseError));
  // Server-attributable / content-limit / non-failure: the link stayed up long
  // enough to frame a full response (or there is no failure at all).
  TEST_ASSERT_FALSE(fetchOutcomeNetworkAbortSensitive(FetchOutcome::Ok));
  TEST_ASSERT_FALSE(fetchOutcomeNetworkAbortSensitive(FetchOutcome::Http429));
  TEST_ASSERT_FALSE(fetchOutcomeNetworkAbortSensitive(FetchOutcome::Http5xx));
  TEST_ASSERT_FALSE(fetchOutcomeNetworkAbortSensitive(FetchOutcome::HttpOther));
  TEST_ASSERT_FALSE(
      fetchOutcomeNetworkAbortSensitive(FetchOutcome::ResponseTooLarge));
  TEST_ASSERT_FALSE(fetchOutcomeNetworkAbortSensitive(FetchOutcome::NoMemory));
  TEST_ASSERT_FALSE(fetchOutcomeNetworkAbortSensitive(FetchOutcome::Obsolete));
  // Phase 7 trust decisions are deterministic, not link-abort artifacts.
  TEST_ASSERT_FALSE(
      fetchOutcomeNetworkAbortSensitive(FetchOutcome::TimeUnavailable));
  TEST_ASSERT_FALSE(fetchOutcomeNetworkAbortSensitive(FetchOutcome::CertInvalid));
}

void test_effective_outcome_after_flap() {
  using core::PollOutcome;
  auto po = [](PollOutcome o) { return static_cast<int>(o); };

  // No flap: the base class passes through untouched, whatever it is.
  TEST_ASSERT_EQUAL_INT(
      po(PollOutcome::Permanent),
      po(effectiveOutcomeAfterFlap(FetchOutcome::ParseError,
                                   PollOutcome::Permanent, false, false)));
  TEST_ASSERT_EQUAL_INT(
      po(PollOutcome::Transient),
      po(effectiveOutcomeAfterFlap(FetchOutcome::Timeout,
                                   PollOutcome::Transient, false, false)));

  // Flap + fully published success: stays Success (caller still forces the
  // immediate refresh separately).
  TEST_ASSERT_EQUAL_INT(
      po(PollOutcome::Success),
      po(effectiveOutcomeAfterFlap(FetchOutcome::Ok, PollOutcome::Success, true,
                                   true)));

  // Flap + non-published link-sensitive failure -> Obsolete (pause, no streak).
  TEST_ASSERT_EQUAL_INT(
      po(PollOutcome::Obsolete),
      po(effectiveOutcomeAfterFlap(FetchOutcome::TransportFailure,
                                   PollOutcome::Transient, false, true)));
  TEST_ASSERT_EQUAL_INT(
      po(PollOutcome::Obsolete),
      po(effectiveOutcomeAfterFlap(FetchOutcome::Timeout,
                                   PollOutcome::Transient, false, true)));
  // Flap + ParseError (base Permanent) -> Obsolete: a truncation is plausible.
  TEST_ASSERT_EQUAL_INT(
      po(PollOutcome::Obsolete),
      po(effectiveOutcomeAfterFlap(FetchOutcome::ParseError,
                                   PollOutcome::Permanent, false, true)));

  // Flap + non-link failure (server responded fully): base passes through.
  TEST_ASSERT_EQUAL_INT(
      po(PollOutcome::Transient),
      po(effectiveOutcomeAfterFlap(FetchOutcome::Http5xx,
                                   PollOutcome::Transient, false, true)));
  TEST_ASSERT_EQUAL_INT(
      po(PollOutcome::RateLimited),
      po(effectiveOutcomeAfterFlap(FetchOutcome::Http429,
                                   PollOutcome::RateLimited, false, true)));
  TEST_ASSERT_EQUAL_INT(
      po(PollOutcome::Permanent),
      po(effectiveOutcomeAfterFlap(FetchOutcome::HttpOther,
                                   PollOutcome::Permanent, false, true)));
}

void test_poll_outcome_mapping_covers_every_fetch_outcome() {
  using core::PollOutcome;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PollOutcome::Success),
                        static_cast<int>(pollOutcomeFor(FetchOutcome::Ok)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PollOutcome::Transient),
                        static_cast<int>(pollOutcomeFor(FetchOutcome::Timeout)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Transient),
      static_cast<int>(pollOutcomeFor(FetchOutcome::DnsFailure)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Transient),
      static_cast<int>(pollOutcomeFor(FetchOutcome::TlsFailure)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Transient),
      static_cast<int>(pollOutcomeFor(FetchOutcome::TransportFailure)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PollOutcome::Transient),
                        static_cast<int>(pollOutcomeFor(FetchOutcome::Http5xx)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PollOutcome::RateLimited),
                        static_cast<int>(pollOutcomeFor(FetchOutcome::Http429)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PollOutcome::Permanent),
                        static_cast<int>(pollOutcomeFor(FetchOutcome::HttpOther)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Permanent),
      static_cast<int>(pollOutcomeFor(FetchOutcome::ResponseTooLarge)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Permanent),
      static_cast<int>(pollOutcomeFor(FetchOutcome::ParseError)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PollOutcome::Permanent),
                        static_cast<int>(pollOutcomeFor(FetchOutcome::NoMemory)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PollOutcome::Obsolete),
                        static_cast<int>(pollOutcomeFor(FetchOutcome::Obsolete)));
  // TimeUnavailable is a "not ready yet" refusal (transient); CertInvalid is a
  // hard trust failure that backs off hard (permanent).
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Transient),
      static_cast<int>(pollOutcomeFor(FetchOutcome::TimeUnavailable)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Permanent),
      static_cast<int>(pollOutcomeFor(FetchOutcome::CertInvalid)));
}

void test_publish_outcome_mapping_covers_every_publish_result() {
  using core::PollOutcome;
  // Only a real publication is a success.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Success),
      static_cast<int>(pollOutcomeForPublish(PublishResult::Published)));
  // A successful fetch discarded for a stale revision is Obsolete (streak intact).
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Obsolete),
      static_cast<int>(pollOutcomeForPublish(PublishResult::ObsoleteRevision)));
  // Internal publish-path faults never published: back off hard (Permanent).
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Permanent),
      static_cast<int>(pollOutcomeForPublish(PublishResult::InvalidHandle)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(PollOutcome::Permanent),
      static_cast<int>(pollOutcomeForPublish(PublishResult::NoCandidate)));
}

void test_authenticated_notbefore_is_zero_in_native_runfetch_paths() {
  // runFetch cannot see the peer certificate (that seam is ESP-only), so it must
  // ALWAYS leave the authenticated floor candidate at 0 -- on a complete Ok AND
  // on every failure path. Only realFetch stamps it, and only on Ok.
  FetchResult ok = fetch(resp(R"({"ac":[{"lat":1,"lon":2}]})"), 4);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Ok),
                        static_cast<int>(ok.outcome));
  TEST_ASSERT_EQUAL_INT64(0, ok.authenticated_cert_not_before_unix);

  // A malformed body (ParseError) also preserves the zero default.
  FetchResult bad = fetch(resp(R"({"ac":[{"lat":1 "lon":2}]})"), 5);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::ParseError),
                        static_cast<int>(bad.outcome));
  TEST_ASSERT_EQUAL_INT64(0, bad.authenticated_cert_not_before_unix);
}

void test_authenticated_notbefore_stamp_seam() {
  // The pure stamping seam realFetch uses: only a complete Ok carries the
  // verified leaf notBefore; every other outcome forces 0 so nothing that did not
  // fully verify a response can ratchet the persisted floor.
  const int64_t leaf = 1784592000;  // some CA-signed notBefore epoch
  TEST_ASSERT_EQUAL_INT64(
      leaf, authenticatedNotBeforeForResult(FetchOutcome::Ok, leaf));

  const FetchOutcome non_ok[] = {
      FetchOutcome::Timeout,          FetchOutcome::DnsFailure,
      FetchOutcome::TlsFailure,       FetchOutcome::TransportFailure,
      FetchOutcome::Http429,          FetchOutcome::Http5xx,
      FetchOutcome::HttpOther,        FetchOutcome::ResponseTooLarge,
      FetchOutcome::ParseError,       FetchOutcome::NoMemory,
      FetchOutcome::Obsolete,         FetchOutcome::TimeUnavailable,
      FetchOutcome::CertInvalid,
  };
  for (const FetchOutcome o : non_ok) {
    TEST_ASSERT_EQUAL_INT64(0, authenticatedNotBeforeForResult(o, leaf));
  }
  // A zero leaf (e.g. an unverified cert) stays zero even on Ok.
  TEST_ASSERT_EQUAL_INT64(0, authenticatedNotBeforeForResult(FetchOutcome::Ok, 0));
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
  RUN_TEST(test_premature_eof_maps_to_transport_failure);
  RUN_TEST(test_network_abort_sensitive_classification);
  RUN_TEST(test_effective_outcome_after_flap);
  RUN_TEST(test_poll_outcome_mapping_covers_every_fetch_outcome);
  RUN_TEST(test_publish_outcome_mapping_covers_every_publish_result);
  RUN_TEST(test_authenticated_notbefore_is_zero_in_native_runfetch_paths);
  RUN_TEST(test_authenticated_notbefore_stamp_seam);
  return UNITY_END();
}
