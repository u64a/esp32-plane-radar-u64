#pragma once

namespace core {

struct LocalOffsetKm {
  float east_km;
  float north_km;
  float distance_km;
};

class LocalProjection {
 public:
  LocalProjection(double center_lat_deg, double center_lon_deg);

  LocalOffsetKm project(float lat_deg, float lon_deg) const;

 private:
  double center_lat_deg_;
  double center_lon_deg_;
  float east_km_per_degree_;
  float north_km_per_degree_;
};

}  // namespace core
