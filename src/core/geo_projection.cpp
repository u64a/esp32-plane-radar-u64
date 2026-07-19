#include "core/geo_projection.h"

#include <cmath>

#include "core/coordinates.h"

namespace core {

namespace {

constexpr float kDegreesToRadians = 0.01745329251994329577f;
constexpr float kMeanEarthRadiusKmFloat =
    static_cast<float>(kMeanEarthRadiusKm);
// For the 36.8 km maximum screen-corner radius, the tangent-bearing error is
// bounded by half the longitude convergence plus the squared angular radius.
// Keeping convergence at or below 0.39 degrees bounds that error below 0.2
// degrees, including margin for the linearized mean-latitude sine.
constexpr float kMaxFastLongitudeConvergenceRad =
    0.39f * kDegreesToRadians;
constexpr float kMaxSupportedAngularRadiusRad =
    kMaxSupportedProjectionRadiusKm / kMeanEarthRadiusKmFloat;
constexpr float kLocalGateRoundingMarginRad = 1.0e-6f;
constexpr float kOutsideSupportedDistanceKm =
    kMaxSupportedProjectionRadiusKm + 1.0f;
constexpr float kNearZeroCentralAngleRad = 1.0e-7f;
constexpr float kNearZeroDirection = 1.0e-7f;

double normalizeLongitudeDeltaDegrees(double delta_deg) {
  if (delta_deg > 180.0) {
    delta_deg -= 360.0;
  } else if (delta_deg < -180.0) {
    delta_deg += 360.0;
  }
  return delta_deg;
}

}  // namespace

LocalProjection::LocalProjection(double center_lat_deg, double center_lon_deg)
    : center_lat_deg_(center_lat_deg),
      center_lon_deg_(center_lon_deg),
      sin_center_lat_(0.0f),
      cos_center_lat_(0.0f) {
  if (coordinatesValid(center_lat_deg, center_lon_deg)) {
    const float center_lat_rad =
        static_cast<float>(center_lat_deg) * kDegreesToRadians;
    sin_center_lat_ = sinf(center_lat_rad);
    cos_center_lat_ = cosf(center_lat_rad);
  }
}

LocalOffsetKm LocalProjection::project(double lat_deg, double lon_deg) const {
  // No valid latitude has both sine and cosine equal to zero. The constructor
  // leaves both zero for an invalid center, avoiding another validity call in
  // every airport projection without growing this hot-path object.
  const bool center_valid =
      sin_center_lat_ != 0.0f || cos_center_lat_ != 0.0f;
  if (!center_valid || !coordinatesValid(lat_deg, lon_deg)) {
    return {0.0f, 0.0f, 0.0f, false};
  }

  const double latitude_delta_deg = lat_deg - center_lat_deg_;
  const double longitude_delta_deg =
      normalizeLongitudeDeltaDegrees(lon_deg - center_lon_deg_);
  const float latitude_delta_rad =
      static_cast<float>(latitude_delta_deg) * kDegreesToRadians;
  const float longitude_delta_rad =
      static_cast<float>(longitude_delta_deg) * kDegreesToRadians;

  // Latitude is 1-Lipschitz with respect to central angle on a sphere:
  // |lat2-lat1| <= distance/R. Therefore anything within the supported
  // radius must pass the latitude gate, including pole crossings.
  bool possibly_within_supported_radius =
      fabsf(latitude_delta_rad) <=
      kMaxSupportedAngularRadiusRad + kLocalGateRoundingMarginRad;
  if (possibly_within_supported_radius) {
    // A path of angular length alpha cannot leave the latitude band
    // |center_lat| + alpha. In a band that does not reach a pole,
    // ds >= min(cos(lat)) * |dlon|. The approximations below deliberately
    // underestimate cos(|center_lat| + alpha), so rejecting on this lower
    // bound is safe. Near-pole caps keep all longitudes for spherical math.
    const float conservative_min_cos =
        fabsf(cos_center_lat_) *
            (1.0f - 0.5f * kMaxSupportedAngularRadiusRad *
                        kMaxSupportedAngularRadiusRad) -
        fabsf(sin_center_lat_) * kMaxSupportedAngularRadiusRad -
        kLocalGateRoundingMarginRad;
    if (conservative_min_cos > 0.0f &&
        fabsf(longitude_delta_rad) * conservative_min_cos >
            kMaxSupportedAngularRadiusRad +
                kLocalGateRoundingMarginRad) {
      possibly_within_supported_radius = false;
    }
  }

  const float cos_mean =
      cos_center_lat_ - 0.5f * latitude_delta_rad * sin_center_lat_;
  const float north_km = latitude_delta_rad * kMeanEarthRadiusKmFloat;
  const float east_km =
      longitude_delta_rad * kMeanEarthRadiusKmFloat * cos_mean;
  if (!possibly_within_supported_radius) {
    return {east_km, north_km, kOutsideSupportedDistanceKm};
  }

  const float sin_mean =
      sin_center_lat_ + 0.5f * latitude_delta_rad * cos_center_lat_;
  if (possibly_within_supported_radius &&
      fabsf(longitude_delta_rad * sin_mean) >
          kMaxFastLongitudeConvergenceRad) {
    const float point_lat_rad =
        static_cast<float>(lat_deg) * kDegreesToRadians;
    const float sin_half_lat_delta = sinf(0.5f * latitude_delta_rad);
    const float sin_half_lon_delta = sinf(0.5f * longitude_delta_rad);
    const float sin_point_lat = sinf(point_lat_rad);
    const float cos_point_lat = cosf(point_lat_rad);
    float haversine =
        sin_half_lat_delta * sin_half_lat_delta +
        cos_center_lat_ * cos_point_lat * sin_half_lon_delta *
            sin_half_lon_delta;
    if (haversine < 0.0f) {
      haversine = 0.0f;
    } else if (haversine > 1.0f) {
      haversine = 1.0f;
    }

    const float central_angle =
        2.0f * atan2f(sqrtf(haversine), sqrtf(1.0f - haversine));
    if (central_angle <= kNearZeroCentralAngleRad) {
      return {0.0f, 0.0f, 0.0f};
    }

    const float sin_lon_delta = sinf(longitude_delta_rad);
    const float cos_lon_delta = cosf(longitude_delta_rad);
    const float east_direction = sin_lon_delta * cos_point_lat;
    const float north_direction =
        cos_center_lat_ * sin_point_lat -
        sin_center_lat_ * cos_point_lat * cos_lon_delta;
    const float direction_length =
        hypotf(east_direction, north_direction);
    const float distance_km = central_angle * kMeanEarthRadiusKmFloat;
    if (direction_length <= kNearZeroDirection) {
      return {0.0f, distance_km, distance_km};
    }

    return {distance_km * east_direction / direction_length,
            distance_km * north_direction / direction_length, distance_km};
  }

  return {east_km, north_km, hypotf(east_km, north_km)};
}

bool localOffsetValid(const LocalOffsetKm& offset) {
  return offset.valid && std::isfinite(offset.east_km) &&
         std::isfinite(offset.north_km) &&
         std::isfinite(offset.distance_km) && offset.distance_km >= 0.0f;
}

bool isWithinDistanceKm(const LocalOffsetKm& offset, float radius_km) {
  return localOffsetValid(offset) && std::isfinite(radius_km) &&
         radius_km >= 0.0f && offset.distance_km <= radius_km;
}

}  // namespace core
