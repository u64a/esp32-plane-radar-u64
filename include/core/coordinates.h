#pragma once

namespace core {

bool coordinatesValid(double lat, double lon);

bool parseCoordinates(const char* lat_text, const char* lon_text, double* lat_out,
                      double* lon_out);

// Outcome of validating a proposed coordinate save against the current values.
// Only Changed represents an effective query change that should bump the
// settings revision; Unchanged and Invalid must not.
enum class CoordinateSaveResult : unsigned char {
  Invalid,    // unparseable or out-of-range input; outputs untouched
  Unchanged,  // parsed and valid, but identical to the current coordinates
  Changed,    // parsed, valid, and different from the current coordinates
};

// Classify a proposed save. On a valid parse, *lat_out/*lon_out receive the
// parsed values (for the caller to persist on Changed); on Invalid they are left
// untouched. The comparison is exact so re-submitting the same value is
// Unchanged.
CoordinateSaveResult classifyCoordinateSave(const char* lat_text,
                                            const char* lon_text,
                                            double current_lat,
                                            double current_lon, double* lat_out,
                                            double* lon_out);

}  // namespace core
