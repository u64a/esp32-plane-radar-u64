#pragma once

namespace core {

bool coordinatesValid(double lat, double lon);

bool parseCoordinates(const char* lat_text, const char* lon_text, double* lat_out,
                      double* lon_out);

}  // namespace core
