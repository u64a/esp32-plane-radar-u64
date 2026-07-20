#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "services/adsb_stream_parser.h"

using namespace services::adsb;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

void setUp() {}
void tearDown() {}

namespace {

struct Harness {
  Aircraft aircraft[kMaxAircraft];
  float distances[kMaxAircraft];
  uint16_t ordinals[kMaxAircraft];
  char object_buffer[kObjectBufferBytes];
  unsigned char arena[kJsonArenaBytes];
};

Harness g_h;

ParserWorkspace workspace() {
  ParserWorkspace w{};
  w.aircraft = g_h.aircraft;
  w.distances = g_h.distances;
  w.ordinals = g_h.ordinals;
  w.object_buffer = g_h.object_buffer;
  w.object_capacity = sizeof(g_h.object_buffer);
  w.arena = g_h.arena;
  w.arena_size = sizeof(g_h.arena);
  return w;
}

struct Outcome {
  bool ok;
  ParseError error;
  size_t count;
};

Outcome run(const std::string& json, ParseOptions opts, double lat = 0.0,
            double lon = 0.0, bool bulk = false) {
  StreamParser parser(lat, lon, opts, workspace());
  bool alive = true;
  if (bulk) {
    alive = parser.consume(reinterpret_cast<const uint8_t*>(json.data()),
                           json.size());
  } else {
    for (char c : json) {
      if (!parser.consume(static_cast<uint8_t>(c))) {
        alive = false;
        break;
      }
    }
  }
  Outcome o{};
  o.ok = alive && parser.finish();
  o.error = parser.error();
  o.count = parser.count();
  return o;
}

ParseOptions defaults() { return ParseOptions{kDefaultParseLimits, false}; }

void expectOk(const std::string& json, size_t count) {
  Outcome o = run(json, defaults());
  TEST_ASSERT_TRUE_MESSAGE(o.ok, json.c_str());
  TEST_ASSERT_EQUAL_UINT32(count, o.count);
  Outcome bulk = run(json, defaults(), 0, 0, true);
  TEST_ASSERT_TRUE(bulk.ok);
  TEST_ASSERT_EQUAL_UINT32(count, bulk.count);
}

void expectError(const std::string& json, ParseError err) {
  Outcome o = run(json, defaults());
  TEST_ASSERT_FALSE_MESSAGE(o.ok, json.c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(err), static_cast<int>(o.error));
}

// Assemble a byte string from explicit values so raw (possibly invalid) UTF-8
// can be embedded without \x escapes greedily swallowing following hex digits.
std::string rawBytes(std::initializer_list<int> bytes) {
  std::string s;
  for (int b : bytes) s.push_back(static_cast<char>(b));
  return s;
}

// Exact replica of the parser's great-circle metric so the oracle ranks
// identically (double intermediates cast to float).
float haversineKm(double lat1, double lon1, double lat2, double lon2) {
  constexpr double kDegToRad = 0.017453292519943295;
  constexpr double kEarthRadiusKm = 6371.0088;
  const double dphi = (lat2 - lat1) * kDegToRad;
  const double dlambda = (lon2 - lon1) * kDegToRad;
  const double s1 = std::sin(dphi * 0.5);
  const double s2 = std::sin(dlambda * 0.5);
  double a = s1 * s1 + std::cos(lat1 * kDegToRad) * std::cos(lat2 * kDegToRad) *
                           s2 * s2;
  if (a < 0.0) a = 0.0;
  if (a > 1.0) a = 1.0;
  return static_cast<float>(kEarthRadiusKm * 2.0 *
                            std::atan2(std::sqrt(a), std::sqrt(1.0 - a)));
}

}  // namespace

void test_canonical_and_empty() {
  expectOk(R"({"ac":[{"lat":10,"lon":20}]})", 1);
  expectOk(R"({"ac":[]})", 0);
  expectOk(R"({"now":1,"ac":[],"msg":"ok"})", 0);
  expectOk(R"(  {  "ac"  :  [ ]  } )", 0);
}

void test_ac_position_first_middle_last_and_escaped() {
  expectOk(R"({"ac":[{"lat":1,"lon":2}],"x":1})", 1);
  expectOk(R"({"x":1,"ac":[{"lat":1,"lon":2}],"y":2})", 1);
  expectOk(R"({"x":{"n":[1,2,3]},"y":true,"ac":[{"lat":1,"lon":2}]})", 1);
  expectOk(R"({"\u0061c":[{"lat":1,"lon":2}]})", 1);
}

void test_ac_missing_null_nonarray_duplicate() {
  expectError(R"({"x":1})", ParseError::Grammar);
  expectError(R"({"ac":null})", ParseError::Grammar);
  expectError(R"({"ac":{}})", ParseError::Grammar);
  expectError(R"({"ac":5})", ParseError::Grammar);
  expectError(R"({"ac":[],"ac":[]})", ParseError::Grammar);
  expectError(R"({"ac":[],"\u0061c":[]})", ParseError::Grammar);
  expectError(R"({"meta":{"ac":[1]}})", ParseError::Grammar);  // nested is not semantic
}

void test_strings_escapes_and_structural_content() {
  expectOk(R"({"ac":[],"s":"{}[],:\"\\\/\b\f\n\r\t"})", 0);
  expectOk(R"({"ac":[],"u":"\u00e9\uD83D\uDE00"})", 0);
  expectError(R"({"ac":[],"s":"\uD83D"})", ParseError::Grammar);
  expectError(R"({"ac":[],"s":"\uDE00"})", ParseError::Grammar);
  expectError(R"({"ac":[],"s":"\uD83Dx"})", ParseError::Grammar);
  expectError(R"({"ac":[],"s":"\u00gg"})", ParseError::Grammar);
  expectError(R"({"ac":[],"s":"\x"})", ParseError::Grammar);
  expectError("{\"ac\":[],\"s\":\"\x01\"}", ParseError::Grammar);
}

void test_number_grammar_and_literals() {
  expectOk(R"({"ac":[],"n":[0,-0,1,-1,3.14,-2.5,1e3,1E-3,1.5e+2,120]})", 0);
  expectOk(R"({"ac":[],"a":true,"b":false,"c":null})", 0);
  expectError(R"({"ac":[],"n":01})", ParseError::Grammar);
  expectError(R"({"ac":[],"n":1.})", ParseError::Grammar);
  expectError(R"({"ac":[],"n":.5})", ParseError::Grammar);
  expectError(R"({"ac":[],"n":-})", ParseError::Grammar);
  expectError(R"({"ac":[],"n":1e})", ParseError::Grammar);
  expectError(R"({"ac":[],"n":+1})", ParseError::Grammar);
  expectError(R"({"ac":[],"a":tru})", ParseError::Grammar);
  expectError(R"({"ac":[],"a":True})", ParseError::Grammar);
}

void test_trailing_garbage_and_truncation() {
  expectError(R"({"ac":[]}x)", ParseError::Grammar);
  expectError(R"({"ac":[])", ParseError::Grammar);
  expectError(R"({"ac":[{"lat":1,"lon":2)", ParseError::Grammar);
}

void test_non_object_elements_skipped_but_counted() {
  expectOk(R"({"ac":[1,"x",true,null,[1,2],{"lat":1,"lon":2}]})", 1);
  ParseOptions o = defaults();
  o.limits.max_ac_entries = 3;
  Outcome r = run(R"({"ac":[1,2,3,4]})", o);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseError::TooLarge),
                        static_cast<int>(r.error));
}

void test_object_and_depth_limits_are_fatal() {
  ParseOptions obj = defaults();
  obj.limits.max_object_bytes = 20;
  Outcome ro = run(R"({"ac":[{"lat":10,"lon":20,"flight":"LONGONE"}]})", obj);
  TEST_ASSERT_FALSE(ro.ok);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseError::TooLarge),
                        static_cast<int>(ro.error));

  ParseOptions depth = defaults();
  depth.limits.max_depth = 4;
  Outcome rd = run(R"({"ac":[],"x":[[[[1]]]]})", depth);
  TEST_ASSERT_FALSE(rd.ok);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseError::TooLarge),
                        static_cast<int>(rd.error));
}

void test_invalid_and_ground_do_not_consume_slots() {
  // 64 valid near targets, interleaved with invalid and ground entries.
  std::string j = "{\"ac\":[";
  for (int i = 0; i < 64; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s{\"lat\":0,\"lon\":%f}", i ? "," : "",
                  0.001 * (i + 1));
    j += buf;
    j += R"(,{"lat":999,"lon":0})";                       // invalid
    j += R"(,{"lat":0,"lon":0.0005,"alt_baro":"ground"})";  // hidden ground
  }
  j += "]}";
  Outcome o = run(j, defaults(), 0.0, 0.0);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_UINT32(64, o.count);
}

void test_nearest_64_of_160_oracle_matches() {
  const int kN = 160;
  std::vector<std::pair<double, double>> pts;
  std::string j = "{\"ac\":[";
  for (int i = 0; i < kN; ++i) {
    const int row = i / 13;
    const int col = i % 13;
    const double lat = row * 0.5 - 3.0;
    const double lon = col * 0.5 - 3.0;
    pts.emplace_back(lat, lon);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s{\"lat\":%f,\"lon\":%f,\"flight\":\"%d\"}",
                  i ? "," : "", lat, lon, i);
    j += buf;
  }
  j += "]}";

  Outcome o = run(j, defaults(), 0.0, 0.0);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_UINT32(64, o.count);

  // Oracle: sort all ordinals by (distance, ordinal), take the first 64.
  std::vector<std::pair<float, int>> ranked;
  for (int i = 0; i < kN; ++i) {
    ranked.emplace_back(haversineKm(0.0, 0.0, pts[i].first, pts[i].second), i);
  }
  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const auto& a, const auto& b) {
                     if (a.first != b.first) return a.first < b.first;
                     return a.second < b.second;
                   });

  for (size_t i = 0; i < o.count; ++i) {
    const int expected_ordinal = ranked[i].second;
    const int actual_ordinal = std::atoi(g_h.aircraft[i].callsign);
    TEST_ASSERT_EQUAL_INT(expected_ordinal, actual_ordinal);
    if (i > 0) {
      TEST_ASSERT_TRUE(g_h.distances[i - 1] <= g_h.distances[i]);
    }
  }
}

void test_exact_distance_ties_keep_earlier_source_order() {
  // 63 near targets fill all but one slot; two farther targets are exactly
  // equidistant, so only the earlier ordinal claims the final slot.
  std::string j = "{\"ac\":[";
  for (int i = 0; i < 63; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s{\"lat\":0,\"lon\":%f,\"flight\":\"n%d\"}",
                  i ? "," : "", 0.001 * (i + 1), i);
    j += buf;
  }
  // Two more at the SAME (large) distance; the earlier ordinal must win the tie.
  j += R"(,{"lat":0,"lon":5.0,"flight":"early"})";
  j += R"(,{"lat":0,"lon":-5.0,"flight":"late"})";
  j += "]}";

  Outcome o = run(j, defaults(), 0.0, 0.0);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_UINT32(64, o.count);
  bool early_present = false;
  bool late_present = false;
  for (size_t i = 0; i < o.count; ++i) {
    if (std::strcmp(g_h.aircraft[i].callsign, "early") == 0) early_present = true;
    if (std::strcmp(g_h.aircraft[i].callsign, "late") == 0) late_present = true;
  }
  TEST_ASSERT_TRUE(early_present);
  TEST_ASSERT_FALSE(late_present);
}

void test_raw_utf8_validation_in_strings() {
  // Valid 2/3/4-byte sequences are accepted in an unknown top-level string.
  std::string valid_top = std::string(R"({"ac":[],"s":")") +
                          rawBytes({0xC3, 0xA9, 0xE2, 0x9C, 0x93, 0xF0, 0x9F, 0x98, 0x80}) +
                          R"("})";
  expectOk(valid_top, 0);

  // Valid multibyte inside a captured aircraft string still decodes.
  std::string valid_cap = std::string(R"({"ac":[{"lat":1,"lon":2,"flight":")") +
                          rawBytes({0xC3, 0xA9, 0x41}) + R"("}]})";
  expectOk(valid_cap, 1);

  struct Bad {
    const char* name;
    std::string bytes;
  };
  const Bad bad[] = {
      {"lone-continuation", rawBytes({0x80})},
      {"overlong-2", rawBytes({0xC0, 0xAF})},
      {"overlong-3", rawBytes({0xE0, 0x80, 0xAF})},
      {"utf8-surrogate", rawBytes({0xED, 0xA0, 0x80})},
      {"above-10FFFF-f4", rawBytes({0xF4, 0x90, 0x80, 0x80})},
      {"above-10FFFF-f5", rawBytes({0xF5, 0x80, 0x80, 0x80})},
      {"truncated-2", rawBytes({0xC3})},
      {"truncated-3", rawBytes({0xE2, 0x9C})},
      {"bad-continuation", rawBytes({0xE2, 0x28, 0x93})},
  };
  for (const Bad& b : bad) {
    std::string top = std::string(R"({"ac":[],"s":")") + b.bytes + R"("})";
    Outcome o = run(top, defaults());
    TEST_ASSERT_FALSE_MESSAGE(o.ok, b.name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ParseError::Grammar),
                                  static_cast<int>(o.error), b.name);
    // Same rejection inside a captured aircraft string.
    std::string cap = std::string(R"({"ac":[{"lat":1,"lon":2,"flight":")") +
                      b.bytes + R"("}]})";
    Outcome c = run(cap, defaults());
    TEST_ASSERT_FALSE_MESSAGE(c.ok, b.name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ParseError::Grammar),
                                  static_cast<int>(c.error), b.name);
  }
}

void test_zero_retention_validates_but_keeps_nothing() {
  ParseOptions o = defaults();
  o.limits.max_aircraft = 0;
  // Poison slot 0 so a stray write (the pre-fix out-of-bounds behaviour) is
  // detectable: zero retention must never read or write any aircraft slot.
  g_h.distances[0] = 1e30f;
  std::strcpy(g_h.aircraft[0].callsign, "SENT");
  Outcome r = run(R"({"ac":[{"lat":1,"lon":2},{"lat":3,"lon":4}]})", o);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(0, r.count);
  TEST_ASSERT_EQUAL_STRING("SENT", g_h.aircraft[0].callsign);
  TEST_ASSERT_EQUAL_FLOAT(1e30f, g_h.distances[0]);
  // The response is still fully validated even though nothing is retained.
  Outcome bad = run(R"({"ac":[{"lat":1 "lon":2}]})", o);
  TEST_ASSERT_FALSE(bad.ok);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseError::Grammar),
                        static_cast<int>(bad.error));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_canonical_and_empty);
  RUN_TEST(test_ac_position_first_middle_last_and_escaped);
  RUN_TEST(test_ac_missing_null_nonarray_duplicate);
  RUN_TEST(test_strings_escapes_and_structural_content);
  RUN_TEST(test_raw_utf8_validation_in_strings);
  RUN_TEST(test_number_grammar_and_literals);
  RUN_TEST(test_trailing_garbage_and_truncation);
  RUN_TEST(test_non_object_elements_skipped_but_counted);
  RUN_TEST(test_object_and_depth_limits_are_fatal);
  RUN_TEST(test_invalid_and_ground_do_not_consume_slots);
  RUN_TEST(test_zero_retention_validates_but_keeps_nothing);
  RUN_TEST(test_nearest_64_of_160_oracle_matches);
  RUN_TEST(test_exact_distance_ties_keep_earlier_source_order);
  return UNITY_END();
}
