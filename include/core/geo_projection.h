#pragma once

namespace core {

/** IUGG mean Earth radius, suitable for the radar's short local distances. */
inline constexpr double kMeanEarthRadiusKm = 6371.0088;
/** Largest radar-relevant center-to-corner distance supported by projection. */
inline constexpr float kMaxSupportedProjectionRadiusKm = 36.8f;

struct LocalOffsetKm {
  constexpr LocalOffsetKm(float east = 0.0f, float north = 0.0f,
                          float distance = 0.0f, bool is_valid = true)
      : east_km(east),
        north_km(north),
        distance_km(distance),
        valid(is_valid) {}

  float east_km;
  float north_km;
  float distance_km;
  // Invalid coordinates produce a zero-valued offset with valid == false.
  bool valid = true;
};

class LocalProjection {
 public:
  LocalProjection(double center_lat_deg, double center_lon_deg);

  /**
   * Accurate within kMaxSupportedProjectionRadiusKm. Points cheaply proven
   * outside that local scene retain an approximate direction and an
   * out-of-supported-range distance without invoking spherical math.
   */
  LocalOffsetKm project(double lat_deg, double lon_deg) const;

 private:
  double center_lat_deg_;
  double center_lon_deg_;
  float sin_center_lat_;
  float cos_center_lat_;
};

bool localOffsetValid(const LocalOffsetKm& offset);
bool isWithinDistanceKm(const LocalOffsetKm& offset, float radius_km);

}  // namespace core
