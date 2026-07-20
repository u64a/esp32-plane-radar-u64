#pragma once

#include <cstddef>

#include "services/adsb_types.h"

namespace services::adsb {

// Aircraft, kMaxAircraft, AircraftSnapshot, FetchOutcome, and FetchResult are
// defined in adsb_types.h and remain the renderer/main compatibility surface.

size_t aircraftCount();
const Aircraft* aircraftList();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

/**
 * Richer Phase 5 seam: performs the same bounded fetch as fetchUpdate() and
 * returns full outcome detail. On FetchOutcome::Ok the nearest-64 snapshot is
 * published; every other outcome preserves the prior snapshot byte-for-byte.
 */
FetchResult fetchLatest(double center_lat, double center_lon,
                        float fetch_radius_km);

}  // namespace services::adsb
