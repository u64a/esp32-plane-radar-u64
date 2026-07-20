#include <unity.h>

#include <cmath>
#include <cstring>
#include <string>

#include "services/adsb_object_decoder.h"

using namespace services::adsb;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

void setUp() {}
void tearDown() {}

namespace {

unsigned char g_arena[8192];
char g_buffer[4096];

DecodeResult decode(const std::string& json, bool show_ground = false,
                    size_t arena_bytes = 4096, uint8_t depth = 16) {
  std::memset(g_buffer, 0, sizeof(g_buffer));
  std::memcpy(g_buffer, json.data(), json.size());
  ObjectDecoderConfig config{show_ground, depth};
  return decodeAircraftObject(g_buffer, json.size(), g_arena, arena_bytes,
                              config);
}

}  // namespace

void test_accepts_integer_and_float_coordinates() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Accepted),
                        static_cast<int>(decode(R"({"lat":10,"lon":20})").status));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(DecodeStatus::Accepted),
      static_cast<int>(decode(R"({"lat":-33.9,"lon":151.2})").status));
}

void test_rejects_missing_wrongtype_and_out_of_range_coordinates() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Skipped),
                        static_cast<int>(decode(R"({"lat":10})").status));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Skipped),
                        static_cast<int>(decode(R"({"lat":"10","lon":20})").status));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Skipped),
                        static_cast<int>(decode(R"({"lat":95,"lon":20})").status));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Skipped),
                        static_cast<int>(decode(R"({"lat":10,"lon":200})").status));
}

void test_ground_hidden_by_default_and_shown_on_request() {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(DecodeStatus::Skipped),
      static_cast<int>(decode(R"({"lat":1,"lon":2,"alt_baro":"ground"})").status));
  DecodeResult shown = decode(R"({"lat":1,"lon":2,"alt_baro":"ground"})", true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Accepted),
                        static_cast<int>(shown.status));
  TEST_ASSERT_EQUAL_STRING("GND", shown.aircraft.alt);
}

void test_heading_and_speed_precedence() {
  // Nose prefers true_heading; track prefers track.
  DecodeResult r = decode(
      R"({"lat":1,"lon":2,"true_heading":10,"mag_heading":20,"track":30,"gs":100})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Accepted),
                        static_cast<int>(r.status));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 10.0f, r.aircraft.nose_deg);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 30.0f, r.aircraft.track_deg);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 100.0f, r.aircraft.gs_knots);
}

void test_wrong_typed_optionals_fall_through() {
  DecodeResult r = decode(
      R"({"lat":1,"lon":2,"true_heading":"x","mag_heading":null,"track":45,"gs":"y","tas":220})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Accepted),
                        static_cast<int>(r.status));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 45.0f, r.aircraft.nose_deg);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 220.0f, r.aircraft.gs_knots);
}

void test_float_unrepresentable_optionals_fall_through_then_zero() {
  // A finite double that overflows float (1e300) must not become +infinity in
  // nose/track/speed. The preferred key falls through to the next; when every
  // candidate is unrepresentable the field is a finite zero.
  DecodeResult r = decode(
      R"({"lat":1,"lon":2,"true_heading":1e300,"mag_heading":1e300,"track":33,"gs":1e300,"tas":175})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Accepted),
                        static_cast<int>(r.status));
  // nose falls true_heading->mag_heading->track (=33); track prefers track.
  TEST_ASSERT_TRUE(std::isfinite(r.aircraft.nose_deg));
  TEST_ASSERT_TRUE(std::isfinite(r.aircraft.track_deg));
  TEST_ASSERT_TRUE(std::isfinite(r.aircraft.gs_knots));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 33.0f, r.aircraft.nose_deg);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 33.0f, r.aircraft.track_deg);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 175.0f, r.aircraft.gs_knots);

  // Every candidate unrepresentable -> finite zero, aircraft still valid.
  DecodeResult z = decode(
      R"({"lat":1,"lon":2,"true_heading":-1e39,"mag_heading":1e39,"track":1e40,"dir":1e300,"gs":1e300,"tas":1e300,"ias":1e300})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Accepted),
                        static_cast<int>(z.status));
  TEST_ASSERT_TRUE(std::isfinite(z.aircraft.nose_deg));
  TEST_ASSERT_TRUE(std::isfinite(z.aircraft.track_deg));
  TEST_ASSERT_TRUE(std::isfinite(z.aircraft.gs_knots));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, z.aircraft.nose_deg);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, z.aircraft.track_deg);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, z.aircraft.gs_knots);
}

void test_callsign_trims_then_falls_back_to_hex() {
  DecodeResult r = decode(R"({"lat":1,"lon":2,"flight":"QFA12   ","hex":"7c"})");
  TEST_ASSERT_EQUAL_STRING("QFA12", r.aircraft.callsign);
  DecodeResult f = decode(R"({"lat":1,"lon":2,"flight":"   ","hex":"7c0abc"})");
  TEST_ASSERT_EQUAL_STRING("7c0abc", f.aircraft.callsign);
}

void test_altitude_rounding_and_overflow_safety() {
  TEST_ASSERT_EQUAL_STRING("12000 ft",
                           decode(R"({"lat":1,"lon":2,"alt_baro":12000})").aircraft.alt);
  // alt_baro not representable -> fall back to alt_geom.
  TEST_ASSERT_EQUAL_STRING(
      "3500 ft",
      decode(R"({"lat":1,"lon":2,"alt_baro":1e30,"alt_geom":3500})").aircraft.alt);
  // Both extreme -> empty tag, aircraft still valid.
  DecodeResult r = decode(R"({"lat":1,"lon":2,"alt_baro":1e30})");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Accepted),
                        static_cast<int>(r.status));
  TEST_ASSERT_EQUAL_UINT8('\0', r.aircraft.alt[0]);
}

void test_fixed_buffers_stay_nul_terminated_when_truncating() {
  DecodeResult r = decode(
      R"({"lat":1,"lon":2,"flight":"TOOLONGCALLSIGN","t":"LONGTYPE"})");
  TEST_ASSERT_EQUAL_UINT8('\0', r.aircraft.callsign[8]);
  TEST_ASSERT_EQUAL_UINT8('\0', r.aircraft.type[4]);
}

void test_tiny_arena_reports_no_memory() {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(DecodeStatus::NoMemory),
      static_cast<int>(decode(R"({"lat":1,"lon":2})", false, 64).status));
}

void test_representative_object_fits_default_arena() {
  DecodeResult r = decode(
      R"({"hex":"7c0abc","flight":"QFA1234 ","lat":-33.9,"lon":151.2,"true_heading":92.5,"mag_heading":90,"track":91,"gs":210,"tas":220,"ias":205,"alt_baro":12000,"alt_geom":12100,"t":"A320"})",
      false, 4096);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DecodeStatus::Accepted),
                        static_cast<int>(r.status));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_accepts_integer_and_float_coordinates);
  RUN_TEST(test_rejects_missing_wrongtype_and_out_of_range_coordinates);
  RUN_TEST(test_ground_hidden_by_default_and_shown_on_request);
  RUN_TEST(test_heading_and_speed_precedence);
  RUN_TEST(test_wrong_typed_optionals_fall_through);
  RUN_TEST(test_float_unrepresentable_optionals_fall_through_then_zero);
  RUN_TEST(test_callsign_trims_then_falls_back_to_hex);
  RUN_TEST(test_altitude_rounding_and_overflow_safety);
  RUN_TEST(test_fixed_buffers_stay_nul_terminated_when_truncating);
  RUN_TEST(test_tiny_arena_reports_no_memory);
  RUN_TEST(test_representative_object_fits_default_arena);
  return UNITY_END();
}
