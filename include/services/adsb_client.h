#pragma once

#include <cstddef>

#include "services/adsb_snapshot_store.h"
#include "services/adsb_types.h"

namespace services::adsb {

// Aircraft, kMaxAircraft, AircraftSnapshot, FetchOutcome, and FetchResult are
// defined in adsb_types.h and remain the renderer/main compatibility surface.
// SnapshotView, FetchRequest, CandidateHandle, CandidateResult, and PublishResult
// are defined in adsb_snapshot_store.h.

size_t aircraftCount();
const Aircraft* aircraftList();

/**
 * Copy-free view of the currently published nearest-aircraft snapshot: pointer,
 * count, and the settings revision it was fetched for, captured together so they
 * are mutually consistent. The runtime builds each RadarDisplayModel frame from
 * this (a revision mismatch against the freshness view hides stale targets).
 */
SnapshotView publishedSnapshot();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

/**
 * Richer Phase 5 seam: performs the same bounded fetch as fetchUpdate() and
 * returns full outcome detail. The fetch is bound to services::settings::revision()
 * captured before the request; on FetchOutcome::Ok the nearest-64 snapshot is
 * published only if that revision still matches afterward. A successful fetch
 * whose revision advanced mid-flight is not published and returns
 * FetchOutcome::Obsolete; every other outcome preserves the prior snapshot
 * byte-for-byte.
 */
FetchResult fetchLatest(double center_lat, double center_lon,
                        float fetch_radius_km);

/**
 * Phase 6 revision-aware two-step seam. Step 1: fetch a candidate for
 * settings_revision into the store's inactive slot (a non-Ok result auto-discards
 * it). The caller re-reads the current settings revision after this returns --
 * a slow fetch may span a settings change -- and then calls publishCandidate().
 */
CandidateResult fetchCandidate(double center_lat, double center_lon,
                               float fetch_radius_km, uint32_t settings_revision);

/**
 * Step 2: publish a candidate iff its handle is current and its revision still
 * equals current_settings_revision; otherwise the published snapshot is preserved
 * byte-for-byte. Returns the explicit PublishResult.
 */
PublishResult publishCandidate(const CandidateHandle& handle,
                               uint32_t current_settings_revision);

}  // namespace services::adsb
