#pragma once

#include <cstdint>

namespace core {

// Renderer-facing freshness modes for the current settings revision.
enum class RadarDataMode : uint8_t {
  Loading = 0,  // grid only; no current-revision success yet
  Live = 1,     // fresh aircraft shown
  Stale = 2,    // aircraft retained but flagged aging
  Offline = 3,  // aircraft hidden; grid retained
};

// Freshness thresholds for the current settings revision. Supplied explicitly to
// keep the core Arduino-free; config.h mirrors these and static_asserts them
// against kDefaultRadarFreshnessPolicy.
struct RadarFreshnessPolicy {
  uint32_t stale_ms;    // age at which Live retained aircraft become Stale
  uint32_t offline_ms;  // age at which aircraft are hidden (Offline)
};

inline constexpr RadarFreshnessPolicy kDefaultRadarFreshnessPolicy = {
    /*stale_ms=*/15000,
    /*offline_ms=*/60000,
};

// Data-freshness state for one settings revision. The mode is a monotonic latch:
// within a revision it only advances Loading->Live->Stale->Offline and never
// regresses -- in particular not across the ~49-day millis() wrap. Only a new
// published success or a new settings revision resets it.
struct RadarDataState {
  uint32_t settings_revision;
  uint32_t revision_started_ms;  // age reference until the first current success
  uint32_t last_success_ms;      // age reference once has_success is true
  bool has_success;              // any published success for this revision
  RadarDataMode mode;            // latched, monotonic within a revision
};

// Pure projection for the renderer. age_seconds counts from the last success, or
// from the revision start while no current-revision success exists.
struct RadarDataView {
  RadarDataMode mode;
  uint32_t age_seconds;
  bool show_aircraft;          // true only in Live and Stale
  uint32_t settings_revision;
};

// Initialize at boot: revision 0, Loading, age reference = now.
void radarDataStart(RadarDataState* state, uint32_t now_ms);

// Adopt a new settings revision (effective location/radar-range change): reset to
// Loading, age reference = now, drop any prior success.
void radarDataRevisionChanged(RadarDataState* state, uint32_t settings_revision,
                              uint32_t now_ms);

// Record a published success. Ignored unless it carries the current revision, so
// a stale in-flight response cannot revive an obsolete revision. Latches Live and
// resets the age reference.
void radarDataSuccess(RadarDataState* state, uint32_t settings_revision,
                      uint32_t now_ms);

// Advance the latched mode for elapsed time. Monotonic: never regresses within a
// revision. Returns the mode after advancing. Call once per loop before reading
// the view.
RadarDataMode radarDataAdvance(RadarDataState* state, uint32_t now_ms,
                               const RadarFreshnessPolicy& policy);

// Pure snapshot of the current (already-advanced) latched state.
RadarDataView radarDataView(const RadarDataState& state, uint32_t now_ms);

}  // namespace core
