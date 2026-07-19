#include "core/geo_projection.h"

#include <cmath>

namespace core {

namespace {

// Phase 3b deliberately preserves the legacy projection. Phase 4 will add
// latitude scaling and antimeridian normalization here, for every consumer.
constexpr float kLegacyKmPerDegree = 111.0f;

}  // namespace

LocalProjection::LocalProjection(double center_lat_deg, double center_lon_deg)
    : center_lat_deg_(center_lat_deg),
      center_lon_deg_(center_lon_deg),
      east_km_per_degree_(kLegacyKmPerDegree),
      north_km_per_degree_(kLegacyKmPerDegree) {}

LocalOffsetKm LocalProjection::project(float lat_deg, float lon_deg) const {
  const float east_km =
      static_cast<float>(lon_deg - center_lon_deg_) * east_km_per_degree_;
  const float north_km =
      static_cast<float>(lat_deg - center_lat_deg_) * north_km_per_degree_;
  return {east_km, north_km,
          sqrtf(east_km * east_km + north_km * north_km)};
}

}  // namespace core
