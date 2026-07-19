#include "core/coordinates.h"

#include <cmath>
#include <cstdlib>

namespace core {

namespace {

bool parseCoordinate(const char* text, double* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }

  char* end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || end == nullptr || *end != '\0' || !std::isfinite(value)) {
    return false;
  }

  *out = value;
  return true;
}

}  // namespace

bool coordinatesValid(double lat, double lon) {
  return std::isfinite(lat) && std::isfinite(lon) && lat >= -90.0 &&
         lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

bool parseCoordinates(const char* lat_text, const char* lon_text, double* lat_out,
                      double* lon_out) {
  if (lat_out == nullptr || lon_out == nullptr) {
    return false;
  }

  double lat = 0.0;
  double lon = 0.0;
  if (!parseCoordinate(lat_text, &lat) || !parseCoordinate(lon_text, &lon) ||
      !coordinatesValid(lat, lon)) {
    return false;
  }

  *lat_out = lat;
  *lon_out = lon;
  return true;
}

}  // namespace core
