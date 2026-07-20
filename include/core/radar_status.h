#pragma once

#include <cstdint>

#include "core/radar_data_state.h"

// Pure, Arduino-free selection of the compact radar status badge and its text
// from the already-advanced freshness view plus the published snapshot revision
// and Wi-Fi/activity inputs. The renderer maps the returned badge to a theme
// color and draws the text; keeping the decision here lets it be native-tested
// without any graphics dependency.

namespace core {

// Which compact status chrome (if any) the renderer should draw. Ordering has no
// semantic meaning; values are stable for tests.
enum class RadarStatusBadge : uint8_t {
  None = 0,  // Live + Wi-Fi connected: no extra chrome
  Loading,   // LOADING + deterministic activity dots
  NoWifi,    // Wi-Fi down while data is not yet Stale/Offline
  Stale,     // STALE <age>s (warning)
  Offline,   // OFFLINE (error)
};

// Aircraft-visibility decision plus the chosen badge and its NUL-terminated
// compact text. text is empty when badge == None.
struct RadarStatusPlan {
  bool draw_aircraft;
  RadarStatusBadge badge;
  char text[16];
};

// STALE age is clamped to this many seconds so the compact text stays bounded.
inline constexpr uint32_t kRadarStatusMaxAgeSeconds = 999;

// LOADING shows 1..kRadarStatusLoadingDots dots, cycling with activity_phase.
inline constexpr uint8_t kRadarStatusLoadingDots = 3;

// Decide aircraft visibility and the status badge/text.
//   draw_aircraft is true only when the freshness view still shows aircraft AND
//   the published snapshot revision matches the view's revision (a mismatch
//   hides them defensively). Badge precedence: Stale/Offline freshness always
//   wins; a Wi-Fi drop is surfaced as NoWifi only while Live or Loading.
RadarStatusPlan radarStatusPlan(RadarDataMode mode, uint32_t age_seconds,
                                bool show_aircraft, uint32_t data_revision,
                                uint32_t snapshot_revision, bool wifi_connected,
                                uint8_t activity_phase);

}  // namespace core
