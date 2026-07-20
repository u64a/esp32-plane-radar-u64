#pragma once

#include "core/coordinates.h"

namespace services::location {

/** Load saved lat/lon from NVS, or use config defaults. Call once before WiFi setup. */
void init();

/** Factory defaults when nothing is stored (also used for portal field prefill). */
double lat();
double lon();

/**
 * Parse portal strings, validate, and -- only on an effective change -- persist
 * to NVS and update the runtime values. Returns Changed when the coordinates
 * parsed, are valid, and differ from the current location; Unchanged and Invalid
 * neither persist nor should bump the runtime settings revision.
 */
core::CoordinateSaveResult saveFromStrings(const char* lat_str,
                                           const char* lon_str);

/** Clear stored coordinates (e.g. with WiFi credential reset). */
void clear();

}  // namespace services::location
