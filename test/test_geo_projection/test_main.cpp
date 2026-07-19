#include <unity.h>

#include "core/geo_projection.h"

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

void setUp() {}
void tearDown() {}

void test_center_projects_to_zero() {
  constexpr float kLat = 52.0f;
  constexpr float kLon = 4.0f;
  const core::LocalProjection projection(kLat, kLon);

  const core::LocalOffsetKm offset = projection.project(kLat, kLon);

  TEST_ASSERT_EQUAL_FLOAT(0.0f, offset.east_km);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, offset.north_km);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, offset.distance_km);
}

void test_cardinal_offsets_use_east_and_north_axes() {
  const core::LocalProjection projection(10.0, -20.0);

  const core::LocalOffsetKm north = projection.project(11.0f, -20.0f);
  const core::LocalOffsetKm east = projection.project(10.0f, -19.0f);
  const core::LocalOffsetKm south = projection.project(9.0f, -20.0f);
  const core::LocalOffsetKm west = projection.project(10.0f, -21.0f);

  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 111.0f, north.north_km);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, north.east_km);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 111.0f, east.east_km);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, east.north_km);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, -111.0f, south.north_km);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, -111.0f, west.east_km);
}

void test_distance_uses_projected_components() {
  const core::LocalProjection projection(0.0, 0.0);

  const core::LocalOffsetKm offset = projection.project(4.0f, 3.0f);

  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 333.0f, offset.east_km);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 444.0f, offset.north_km);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 555.0f, offset.distance_km);
}

void test_aircraft_and_runway_consumers_receive_identical_projection() {
  const core::LocalProjection aircraft_projection(-33.75, 151.25);
  const core::LocalProjection runway_projection(-33.75, 151.25);

  const core::LocalOffsetKm aircraft =
      aircraft_projection.project(-33.70f, 151.30f);
  const core::LocalOffsetKm runway =
      runway_projection.project(-33.70f, 151.30f);

  TEST_ASSERT_EQUAL_FLOAT(aircraft.east_km, runway.east_km);
  TEST_ASSERT_EQUAL_FLOAT(aircraft.north_km, runway.north_km);
  TEST_ASSERT_EQUAL_FLOAT(aircraft.distance_km, runway.distance_km);
}

void test_legacy_longitude_scale_remains_111_km_at_high_latitude() {
  const core::LocalProjection projection(60.0, 10.0);

  const core::LocalOffsetKm offset = projection.project(60.0f, 11.0f);

  // Phase 4 will replace this legacy expectation with latitude-corrected math.
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 111.0f, offset.east_km);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, offset.north_km);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 111.0f, offset.distance_km);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_center_projects_to_zero);
  RUN_TEST(test_cardinal_offsets_use_east_and_north_axes);
  RUN_TEST(test_distance_uses_projected_components);
  RUN_TEST(test_aircraft_and_runway_consumers_receive_identical_projection);
  RUN_TEST(test_legacy_longitude_scale_remains_111_km_at_high_latitude);
  return UNITY_END();
}
