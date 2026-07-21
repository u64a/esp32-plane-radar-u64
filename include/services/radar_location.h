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
 * neither persist nor should bump the runtime settings revision. When persist_ok
 * is non-null it is set to whether the settings are durably saved: true for
 * Unchanged (already saved) and for a Changed value that persisted AND verified by
 * read-back; false for Invalid or a Changed value whose NVS write did not verify.
 */
core::CoordinateSaveResult saveFromStrings(const char* lat_str,
                                           const char* lon_str,
                                           bool* persist_ok = nullptr);

/** Clear stored coordinates (e.g. with WiFi credential reset). Returns true only
 *  when the NVS keys are verifiably absent afterward (read back), so a factory
 *  erase never claims success on a failed clear. */
bool clear();

}  // namespace services::location
