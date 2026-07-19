#include <unity.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/geo_projection.h"
#include "core/screen_geometry.h"

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegreesToRadians = kPi / 180.0;
constexpr double kRadiansToDegrees = 180.0 / kPi;
constexpr core::geometry::ScreenPoint kCenter{120, 120};
constexpr float kPixelsPerKm = 107.0f / (10.0f * 4.0f / 3.0f);

struct GeoPoint {
  double lat_deg;
  double lon_deg;
};

GeoPoint destinationPoint(GeoPoint origin, float distance_km,
                          float bearing_deg) {
  const double angular_distance =
      static_cast<double>(distance_km) / core::kMeanEarthRadiusKm;
  const double bearing_rad = bearing_deg * kDegreesToRadians;
  const double lat1 = origin.lat_deg * kDegreesToRadians;
  const double lon1 = origin.lon_deg * kDegreesToRadians;
  const double lat2 =
      std::asin(std::sin(lat1) * std::cos(angular_distance) +
                std::cos(lat1) * std::sin(angular_distance) *
                    std::cos(bearing_rad));
  const double lon2 =
      lon1 + std::atan2(std::sin(bearing_rad) *
                            std::sin(angular_distance) * std::cos(lat1),
                        std::cos(angular_distance) -
                            std::sin(lat1) * std::sin(lat2));
  double lon2_deg = lon2 * kRadiansToDegrees;
  while (lon2_deg > 180.0) {
    lon2_deg -= 360.0;
  }
  while (lon2_deg < -180.0) {
    lon2_deg += 360.0;
  }
  return {lat2 * kRadiansToDegrees, lon2_deg};
}

float projectedBearingDeg(const core::LocalOffsetKm& offset) {
  float bearing = static_cast<float>(
      std::atan2(offset.east_km, offset.north_km) * kRadiansToDegrees);
  if (bearing < 0.0f) {
    bearing += 360.0f;
  }
  return bearing;
}

float bearingDifferenceDeg(float first, float second) {
  float difference = std::fabs(first - second);
  if (difference > 180.0f) {
    difference = 360.0f - difference;
  }
  return difference;
}

core::LocalOffsetKm sphericalReference(GeoPoint center, GeoPoint point) {
  const double center_lat_rad = center.lat_deg * kDegreesToRadians;
  const double point_lat_rad = point.lat_deg * kDegreesToRadians;
  const double latitude_delta_rad = point_lat_rad - center_lat_rad;
  double longitude_delta_deg = point.lon_deg - center.lon_deg;
  if (longitude_delta_deg > 180.0) {
    longitude_delta_deg -= 360.0;
  } else if (longitude_delta_deg < -180.0) {
    longitude_delta_deg += 360.0;
  }
  const double longitude_delta_rad =
      longitude_delta_deg * kDegreesToRadians;
  const double sin_half_lat_delta = std::sin(0.5 * latitude_delta_rad);
  const double sin_half_lon_delta = std::sin(0.5 * longitude_delta_rad);
  const double haversine =
      sin_half_lat_delta * sin_half_lat_delta +
      std::cos(center_lat_rad) * std::cos(point_lat_rad) *
          sin_half_lon_delta * sin_half_lon_delta;
  const double central_angle =
      2.0 * std::atan2(std::sqrt(haversine),
                       std::sqrt(std::max(0.0, 1.0 - haversine)));
  const double bearing_rad = std::atan2(
      std::sin(longitude_delta_rad) * std::cos(point_lat_rad),
      std::cos(center_lat_rad) * std::sin(point_lat_rad) -
          std::sin(center_lat_rad) * std::cos(point_lat_rad) *
              std::cos(longitude_delta_rad));
  const double distance_km = central_angle * core::kMeanEarthRadiusKm;
  return {static_cast<float>(distance_km * std::sin(bearing_rad)),
          static_cast<float>(distance_km * std::cos(bearing_rad)),
          static_cast<float>(distance_km)};
}

core::LocalOffsetKm priorDoubleProjection(GeoPoint center, GeoPoint point) {
  const double latitude_delta_rad =
      (point.lat_deg - center.lat_deg) * kDegreesToRadians;
  double longitude_delta_deg = point.lon_deg - center.lon_deg;
  if (longitude_delta_deg > 180.0) {
    longitude_delta_deg -= 360.0;
  } else if (longitude_delta_deg < -180.0) {
    longitude_delta_deg += 360.0;
  }
  const double longitude_delta_rad =
      longitude_delta_deg * kDegreesToRadians;
  const double mean_latitude_rad =
      (center.lat_deg + point.lat_deg) * 0.5 * kDegreesToRadians;
  const double north_km = latitude_delta_rad * core::kMeanEarthRadiusKm;
  const double east_km = longitude_delta_rad * core::kMeanEarthRadiusKm *
                         std::cos(mean_latitude_rad);
  return {static_cast<float>(east_km), static_cast<float>(north_km),
          static_cast<float>(std::hypot(east_km, north_km))};
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_center_projects_to_zero() {
  const core::LocalProjection projection(52.3676, 4.9041);
  const core::LocalOffsetKm offset = projection.project(52.3676, 4.9041);

  TEST_ASSERT_EQUAL_FLOAT(0.0f, offset.east_km);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, offset.north_km);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, offset.distance_km);
}

void assertInvalidOffset(const core::LocalOffsetKm& offset) {
  TEST_ASSERT_FALSE(offset.valid);
  TEST_ASSERT_FALSE(core::localOffsetValid(offset));
  TEST_ASSERT_FALSE(core::isWithinDistanceKm(offset, 36.8f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, offset.east_km);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, offset.north_km);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, offset.distance_km);
}

void test_invalid_targets_return_explicit_non_drawable_offsets() {
  const core::LocalProjection projection(52.3676, 4.9041);
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr GeoPoint kInvalidTargets[] = {
      {90.000001, 4.9041},
      {-90.000001, 4.9041},
      {52.3676, 180.000001},
      {52.3676, -180.000001},
      {kNan, 4.9041},
      {52.3676, kNan},
      {kInf, 4.9041},
      {52.3676, -kInf},
      {412.3676, 4.9041},
  };

  for (const GeoPoint target : kInvalidTargets) {
    assertInvalidOffset(projection.project(target.lat_deg, target.lon_deg));
  }
}

void test_invalid_centers_reject_otherwise_valid_targets() {
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr GeoPoint kInvalidCenters[] = {
      {90.000001, 0.0},   {-90.000001, 0.0}, {0.0, 180.000001},
      {0.0, -180.000001}, {kNan, 0.0},       {0.0, kInf},
  };

  for (const GeoPoint center : kInvalidCenters) {
    assertInvalidOffset(
        core::LocalProjection(center.lat_deg, center.lon_deg)
            .project(52.3676, 4.9041));
  }
}

void test_cardinal_offsets_preserve_east_positive_north_positive_axes() {
  const core::LocalProjection projection(0.0, 0.0);
  const core::LocalOffsetKm north = projection.project(1.0, 0.0);
  const core::LocalOffsetKm east = projection.project(0.0, 1.0);
  const core::LocalOffsetKm south = projection.project(-1.0, 0.0);
  const core::LocalOffsetKm west = projection.project(0.0, -1.0);

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 111.19508f, north.north_km);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, north.east_km);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 111.19508f, east.east_km);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, east.north_km);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -111.19508f, south.north_km);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -111.19508f, west.east_km);
}

void test_longitude_scale_tracks_equator_amsterdam_and_sixty_degrees() {
  const core::LocalOffsetKm equator =
      core::LocalProjection(0.0, 0.0).project(0.0, 1.0);
  const core::LocalOffsetKm amsterdam =
      core::LocalProjection(52.3676, 4.9041).project(52.3676, 5.1541);
  const core::LocalOffsetKm north_sixty =
      core::LocalProjection(60.0, 10.0).project(60.0, 10.25);
  const core::LocalOffsetKm south_sixty =
      core::LocalProjection(-60.0, 10.0).project(-60.0, 10.25);

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 111.19508f, equator.east_km);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 16.97374f, amsterdam.east_km);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.89939f, north_sixty.east_km);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.89939f, south_sixty.east_km);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, equator.east_km * 0.5f,
                           north_sixty.east_km * 4.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, north_sixty.east_km,
                           south_sixty.east_km);
}

void test_antimeridian_uses_shortest_signed_longitude_path_both_directions() {
  const core::LocalOffsetKm eastward =
      core::LocalProjection(0.0, 179.9).project(0.0, -179.9);
  const core::LocalOffsetKm westward =
      core::LocalProjection(0.0, -179.9).project(0.0, 179.9);

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.239016f, eastward.east_km);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -22.239016f, westward.east_km);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, eastward.distance_km,
                           westward.distance_km);
}

void test_distance_and_bearing_match_great_circle_local_scenes() {
  constexpr GeoPoint kOrigins[] = {
      {52.3676, 4.9041},
      {60.0, 10.0},
      {-60.0, 10.0},
  };
  constexpr float kDistancesKm[] = {
      5.0f * 4.0f / 3.0f,
      10.0f * 4.0f / 3.0f,
      15.0f * 4.0f / 3.0f,
      25.0f * 4.0f / 3.0f,
  };
  constexpr float kBearingsDeg[] = {0.0f, 45.0f, 90.0f, 225.0f};

  for (const GeoPoint origin : kOrigins) {
    const core::LocalProjection projection(origin.lat_deg, origin.lon_deg);
    for (size_t i = 0; i < 4; ++i) {
      const GeoPoint target =
          destinationPoint(origin, kDistancesKm[i], kBearingsDeg[i]);
      const core::LocalOffsetKm offset =
          projection.project(target.lat_deg, target.lon_deg);

      TEST_ASSERT_FLOAT_WITHIN(kDistancesKm[i] * 0.01f, kDistancesKm[i],
                               offset.distance_km);
      TEST_ASSERT_TRUE(bearingDifferenceDeg(projectedBearingDeg(offset),
                                            kBearingsDeg[i]) < 0.2f);
      TEST_ASSERT_TRUE(
          core::isWithinDistanceKm(offset, kDistancesKm[i] * 1.01f));
      TEST_ASSERT_FALSE(
          core::isWithinDistanceKm(offset, kDistancesKm[i] * 0.99f));
    }
  }
}

void test_numeric_aircraft_and_runway_scene_fixtures() {
  struct SceneFixture {
    GeoPoint center;
    GeoPoint aircraft;
    GeoPoint runway_start;
    GeoPoint runway_end;
    core::geometry::ScreenPoint aircraft_screen;
    core::geometry::ScreenPoint runway_start_screen;
    core::geometry::ScreenPoint runway_end_screen;
  };

  // Numeric Phase 4 fixtures only; desktop image goldens are deferred to Phase 10.
  constexpr SceneFixture kScenes[] = {
      {{52.3676, 4.9041},
       {52.3976, 4.9541},
       {52.3500, 4.8700},
       {52.3850, 4.9380},
       {147, 93},
       {101, 136},
       {138, 104}},
      {{60.0, 10.0},
       {60.04, 10.08},
       {59.98, 9.94},
       {60.02, 10.06},
       {156, 84},
       {93, 138},
       {147, 102}},
      {{-60.0, 10.0},
       {-59.96, 10.08},
       {-60.02, 9.94},
       {-59.98, 10.06},
       {156, 84},
       {93, 138},
       {147, 102}},
  };

  for (const SceneFixture& scene : kScenes) {
    const core::LocalProjection projection(scene.center.lat_deg,
                                           scene.center.lon_deg);
    const auto screen = [&](GeoPoint point) {
      return core::geometry::offsetToScreen(
          projection.project(point.lat_deg, point.lon_deg), kCenter,
          kPixelsPerKm);
    };

    const core::geometry::ScreenPoint aircraft = screen(scene.aircraft);
    const core::geometry::ScreenPoint runway_start =
        screen(scene.runway_start);
    const core::geometry::ScreenPoint runway_end = screen(scene.runway_end);

    TEST_ASSERT_EQUAL_INT(scene.aircraft_screen.x, aircraft.x);
    TEST_ASSERT_EQUAL_INT(scene.aircraft_screen.y, aircraft.y);
    TEST_ASSERT_EQUAL_INT(scene.runway_start_screen.x, runway_start.x);
    TEST_ASSERT_EQUAL_INT(scene.runway_start_screen.y, runway_start.y);
    TEST_ASSERT_EQUAL_INT(scene.runway_end_screen.x, runway_end.x);
    TEST_ASSERT_EQUAL_INT(scene.runway_end_screen.y, runway_end.y);
  }
}

void test_float_projection_remains_subpixel_from_prior_double_scenes() {
  constexpr GeoPoint kCenters[] = {
      {0.0, 0.0},
      {52.3676, 4.9041},
      {60.0, 10.0},
      {-60.0, 10.0},
      {0.0, 179.9},
      {0.0, -179.9},
  };
  constexpr GeoPoint kTargets[][3] = {
      {{0.0, 1.0}, {0.25, -0.25}, {-0.25, 0.25}},
      {{52.3976, 4.9541}, {52.3500, 4.8700}, {52.3850, 4.9380}},
      {{60.04, 10.08}, {59.98, 9.94}, {60.02, 10.06}},
      {{-59.96, 10.08}, {-60.02, 9.94}, {-59.98, 10.06}},
      {{0.0, -179.9}, {0.1, -179.95}, {-0.1, 179.95}},
      {{0.0, 179.9}, {0.1, 179.95}, {-0.1, -179.95}},
  };
  constexpr float kMaxPixelsPerKm =
      107.0f / (5.0f * 4.0f / 3.0f);

  float max_pixel_error = 0.0f;
  for (size_t center_index = 0;
       center_index < sizeof(kCenters) / sizeof(kCenters[0]);
       ++center_index) {
    const core::LocalProjection projection(kCenters[center_index].lat_deg,
                                           kCenters[center_index].lon_deg);
    for (const GeoPoint target : kTargets[center_index]) {
      const core::LocalOffsetKm actual =
          projection.project(target.lat_deg, target.lon_deg);
      const core::LocalOffsetKm prior =
          priorDoubleProjection(kCenters[center_index], target);
      const float error_pixels =
          std::hypot(actual.east_km - prior.east_km,
                     actual.north_km - prior.north_km) *
          kMaxPixelsPerKm;
      max_pixel_error = std::max(max_pixel_error, error_pixels);
    }
  }

  TEST_ASSERT_TRUE(max_pixel_error < 1.0f);
}

void assertPolarCardinalOffsets(GeoPoint center) {
  constexpr float kDistanceKm = 5.0f;
  constexpr float kBearingsDeg[] = {0.0f, 90.0f, 180.0f, 270.0f};
  const core::LocalProjection projection(center.lat_deg, center.lon_deg);

  for (const float bearing_deg : kBearingsDeg) {
    const GeoPoint target =
        destinationPoint(center, kDistanceKm, bearing_deg);
    const core::LocalOffsetKm offset =
        projection.project(target.lat_deg, target.lon_deg);

    TEST_ASSERT_FLOAT_WITHIN(0.002f, kDistanceKm, offset.distance_km);
    TEST_ASSERT_TRUE(
        bearingDifferenceDeg(projectedBearingDeg(offset), bearing_deg) <
        0.05f);
  }
}

void test_north_pole_crossing_preserves_cardinal_directions() {
  assertPolarCardinalOffsets({89.98, 23.0});
}

void test_south_pole_crossing_preserves_cardinal_directions() {
  assertPolarCardinalOffsets({-89.98, -47.0});
}

void test_polar_antimeridian_uses_local_bearing() {
  constexpr GeoPoint kCenter{89.5, 179.9};
  constexpr float kDistanceKm = 5.0f;
  const core::LocalProjection projection(kCenter.lat_deg, kCenter.lon_deg);
  const GeoPoint east_target = destinationPoint(kCenter, kDistanceKm, 90.0f);
  const GeoPoint west_target =
      destinationPoint(kCenter, kDistanceKm, 270.0f);
  const core::LocalOffsetKm east =
      projection.project(east_target.lat_deg, east_target.lon_deg);
  const core::LocalOffsetKm west =
      projection.project(west_target.lat_deg, west_target.lon_deg);

  TEST_ASSERT_TRUE(east_target.lon_deg < -170.0);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, kDistanceKm, east.distance_km);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, kDistanceKm, west.distance_km);
  TEST_ASSERT_TRUE(bearingDifferenceDeg(projectedBearingDeg(east), 90.0f) <
                   0.05f);
  TEST_ASSERT_TRUE(bearingDifferenceDeg(projectedBearingDeg(west), 270.0f) <
                   0.05f);
}

void test_rejected_just_below_cutoff_matches_true_bearing_both_hemispheres() {
  constexpr float kDistanceKm = 29.283f;
  constexpr float k25KmPixelsPerKm =
      107.0f / (25.0f * 4.0f / 3.0f);
  constexpr GeoPoint kCenters[] = {
      {88.999999, 0.0},
      {-88.999999, 0.0},
  };
  constexpr float kBearingsDeg[] = {282.82f, 257.18f};
  constexpr core::geometry::ScreenPoint kExpectedScreen[] = {
      {28, 99},
      {28, 141},
  };

  for (size_t i = 0; i < 2; ++i) {
    const GeoPoint target =
        destinationPoint(kCenters[i], kDistanceKm, kBearingsDeg[i]);
    const core::LocalOffsetKm offset =
        core::LocalProjection(kCenters[i].lat_deg, kCenters[i].lon_deg)
            .project(target.lat_deg, target.lon_deg);
    const core::geometry::ScreenPoint screen =
        core::geometry::offsetToScreen(offset, kCenter, k25KmPixelsPerKm);

    TEST_ASSERT_FLOAT_WITHIN(0.003f, kDistanceKm, offset.distance_km);
    TEST_ASSERT_TRUE(
        bearingDifferenceDeg(projectedBearingDeg(offset), kBearingsDeg[i]) <
        0.05f);
    TEST_ASSERT_EQUAL_INT(kExpectedScreen[i].x, screen.x);
    TEST_ASSERT_EQUAL_INT(kExpectedScreen[i].y, screen.y);
  }
}

void test_adaptive_dispatch_brackets_convergence_boundary_both_hemispheres() {
  constexpr double kCenterLatitudes[] = {80.0, -80.0};
  const double sin_center = std::sin(80.0 * kDegreesToRadians);
  const double fast_longitude_delta_deg = 0.38 / sin_center;
  const double spherical_longitude_delta_deg = 0.40 / sin_center;

  for (const double center_latitude : kCenterLatitudes) {
    const GeoPoint center{center_latitude, 12.0};
    const core::LocalProjection projection(center.lat_deg, center.lon_deg);
    const GeoPoint fast_target{
        center.lat_deg, center.lon_deg + fast_longitude_delta_deg};
    const GeoPoint spherical_target{
        center.lat_deg, center.lon_deg + spherical_longitude_delta_deg};

    const core::LocalOffsetKm fast =
        projection.project(fast_target.lat_deg, fast_target.lon_deg);
    const core::LocalOffsetKm prior_fast =
        priorDoubleProjection(center, fast_target);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, prior_fast.east_km, fast.east_km);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, fast.north_km);

    const core::LocalOffsetKm spherical =
        projection.project(spherical_target.lat_deg, spherical_target.lon_deg);
    const core::LocalOffsetKm reference =
        sphericalReference(center, spherical_target);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, reference.distance_km,
                             spherical.distance_km);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, reference.east_km,
                             spherical.east_km);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, reference.north_km,
                             spherical.north_km);
    TEST_ASSERT_TRUE(std::fabs(spherical.north_km) > 0.02f);
  }
}

void test_just_below_old_cutoff_antimeridian_uses_true_local_bearing() {
  constexpr GeoPoint kCenter{88.999999, 179.9};
  constexpr float kDistanceKm = 29.283f;
  constexpr float kBearingsDeg[] = {90.0f, 270.0f};
  const core::LocalProjection projection(kCenter.lat_deg, kCenter.lon_deg);

  for (const float bearing_deg : kBearingsDeg) {
    const GeoPoint target =
        destinationPoint(kCenter, kDistanceKm, bearing_deg);
    const core::LocalOffsetKm offset =
        projection.project(target.lat_deg, target.lon_deg);

    TEST_ASSERT_TRUE(target.lon_deg >= -180.0 && target.lon_deg <= 180.0);
    TEST_ASSERT_FLOAT_WITHIN(0.003f, kDistanceKm, offset.distance_km);
    TEST_ASSERT_TRUE(
        bearingDifferenceDeg(projectedBearingDeg(offset), bearing_deg) <
        0.05f);
  }
}

void test_exact_and_near_poles_remain_finite() {
  constexpr GeoPoint kCenters[] = {
      {90.0, 0.0},
      {-90.0, 120.0},
      {89.999999, -179.999999},
      {-89.999999, 179.999999},
  };
  constexpr GeoPoint kTargets[] = {
      {90.0, 137.0},
      {-90.0, -73.0},
      {89.9999, 179.9999},
      {-89.9999, -179.9999},
  };

  for (size_t i = 0; i < sizeof(kCenters) / sizeof(kCenters[0]); ++i) {
    const core::LocalOffsetKm offset =
        core::LocalProjection(kCenters[i].lat_deg, kCenters[i].lon_deg)
            .project(kTargets[i].lat_deg, kTargets[i].lon_deg);
    TEST_ASSERT_TRUE(std::isfinite(offset.east_km));
    TEST_ASSERT_TRUE(std::isfinite(offset.north_km));
    TEST_ASSERT_TRUE(std::isfinite(offset.distance_km));
    TEST_ASSERT_TRUE(offset.distance_km >= 0.0f);
    if (i < 2) {
      TEST_ASSERT_EQUAL_FLOAT(0.0f, offset.distance_km);
    }
  }
}

void test_exact_and_near_poles_remain_finite_on_fast_side() {
  constexpr GeoPoint kCenters[] = {
      {90.0, 45.0},
      {-90.0, -45.0},
      {89.999999, 45.0},
      {-89.999999, -45.0},
  };
  constexpr GeoPoint kTargets[] = {
      {90.0, 45.1},
      {-90.0, -45.1},
      {89.9999, 45.1},
      {-89.9999, -45.1},
  };

  for (size_t i = 0; i < sizeof(kCenters) / sizeof(kCenters[0]); ++i) {
    const core::LocalOffsetKm offset =
        core::LocalProjection(kCenters[i].lat_deg, kCenters[i].lon_deg)
            .project(kTargets[i].lat_deg, kTargets[i].lon_deg);
    TEST_ASSERT_TRUE(std::isfinite(offset.east_km));
    TEST_ASSERT_TRUE(std::isfinite(offset.north_km));
    TEST_ASSERT_TRUE(std::isfinite(offset.distance_km));
    TEST_ASSERT_TRUE(offset.distance_km >= 0.0f);
    if (i < 2) {
      TEST_ASSERT_TRUE(offset.distance_km < 0.0001f);
    }
  }
}

void test_rejected_north_pole_example_projects_north() {
  const core::LocalOffsetKm offset =
      core::LocalProjection(89.98, 0.0).project(89.975034, -180.0);

  TEST_ASSERT_FLOAT_WITHIN(0.002f, 5.0f, offset.distance_km);
  TEST_ASSERT_TRUE(offset.north_km > 4.99f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, offset.east_km);
  TEST_ASSERT_TRUE(
      bearingDifferenceDeg(projectedBearingDeg(offset), 0.0f) < 0.05f);
}

void test_supported_radius_sweep_meets_distance_and_bearing_bound() {
  constexpr GeoPoint kCenters[] = {
      {0.0, 0.0},       {52.3676, 4.9041}, {80.0, 179.9},
      {-80.0, -179.9},  {89.98, 23.0},     {-89.98, -47.0},
  };
  constexpr float kDistancesKm[] = {5.0f, 20.0f,
                                    core::kMaxSupportedProjectionRadiusKm};

  for (const GeoPoint center : kCenters) {
    const core::LocalProjection projection(center.lat_deg, center.lon_deg);
    for (const float distance_km : kDistancesKm) {
      for (int bearing_deg = 0; bearing_deg < 360; bearing_deg += 5) {
        const GeoPoint target = destinationPoint(
            center, distance_km, static_cast<float>(bearing_deg));
        const core::LocalOffsetKm offset =
            projection.project(target.lat_deg, target.lon_deg);

        TEST_ASSERT_TRUE(core::localOffsetValid(offset));
        TEST_ASSERT_TRUE(
            offset.distance_km <
            core::kMaxSupportedProjectionRadiusKm + 0.5f);
        TEST_ASSERT_FLOAT_WITHIN(std::max(0.003f, distance_km * 0.01f),
                                 distance_km, offset.distance_km);
        TEST_ASSERT_TRUE(
            bearingDifferenceDeg(projectedBearingDeg(offset),
                                 static_cast<float>(bearing_deg)) < 0.2f);
      }
    }
  }
}

void test_same_latitude_near_pole_uses_spherical_local_direction() {
  constexpr GeoPoint kCenters[] = {
      {89.9, 20.0},
      {-89.9, -40.0},
  };

  for (const GeoPoint center : kCenters) {
    const GeoPoint target{center.lat_deg, center.lon_deg + 100.0};
    const core::LocalOffsetKm actual =
        core::LocalProjection(center.lat_deg, center.lon_deg)
            .project(target.lat_deg, target.lon_deg);
    const core::LocalOffsetKm reference = sphericalReference(center, target);
    const core::LocalOffsetKm planar = priorDoubleProjection(center, target);

    TEST_ASSERT_TRUE(
        actual.distance_km < core::kMaxSupportedProjectionRadiusKm);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, reference.east_km, actual.east_km);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, reference.north_km, actual.north_km);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, reference.distance_km,
                             actual.distance_km);
    TEST_ASSERT_TRUE(std::fabs(actual.north_km - planar.north_km) > 10.0f);
  }
}

void test_irrelevant_same_latitude_global_point_stays_on_fast_path() {
  constexpr GeoPoint kCenter{52.3676, 4.9041};
  constexpr GeoPoint kBerlin{52.361738, 13.502341};
  const core::LocalOffsetKm offset =
      core::LocalProjection(kCenter.lat_deg, kCenter.lon_deg)
          .project(kBerlin.lat_deg, kBerlin.lon_deg);

  TEST_ASSERT_TRUE(core::localOffsetValid(offset));
  TEST_ASSERT_TRUE(
      offset.distance_km > core::kMaxSupportedProjectionRadiusKm);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -0.6518f, offset.north_km);
  TEST_ASSERT_TRUE(offset.east_km > 580.0f);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_center_projects_to_zero);
  RUN_TEST(test_invalid_targets_return_explicit_non_drawable_offsets);
  RUN_TEST(test_invalid_centers_reject_otherwise_valid_targets);
  RUN_TEST(test_cardinal_offsets_preserve_east_positive_north_positive_axes);
  RUN_TEST(test_longitude_scale_tracks_equator_amsterdam_and_sixty_degrees);
  RUN_TEST(
      test_antimeridian_uses_shortest_signed_longitude_path_both_directions);
  RUN_TEST(test_distance_and_bearing_match_great_circle_local_scenes);
  RUN_TEST(test_numeric_aircraft_and_runway_scene_fixtures);
  RUN_TEST(test_float_projection_remains_subpixel_from_prior_double_scenes);
  RUN_TEST(test_north_pole_crossing_preserves_cardinal_directions);
  RUN_TEST(test_south_pole_crossing_preserves_cardinal_directions);
  RUN_TEST(test_polar_antimeridian_uses_local_bearing);
  RUN_TEST(
      test_rejected_just_below_cutoff_matches_true_bearing_both_hemispheres);
  RUN_TEST(
      test_adaptive_dispatch_brackets_convergence_boundary_both_hemispheres);
  RUN_TEST(
      test_just_below_old_cutoff_antimeridian_uses_true_local_bearing);
  RUN_TEST(test_exact_and_near_poles_remain_finite);
  RUN_TEST(test_exact_and_near_poles_remain_finite_on_fast_side);
  RUN_TEST(test_rejected_north_pole_example_projects_north);
  RUN_TEST(test_supported_radius_sweep_meets_distance_and_bearing_bound);
  RUN_TEST(test_same_latitude_near_pole_uses_spherical_local_direction);
  RUN_TEST(test_irrelevant_same_latitude_global_point_stays_on_fast_path);
  return UNITY_END();
}
