#include "core/radar_status.h"

#include <cstdio>

namespace core {

namespace {

RadarStatusBadge selectBadge(RadarDataMode mode, bool wifi_connected) {
  switch (mode) {
    case RadarDataMode::Live:
      // Fresh data: only a Wi-Fi drop adds chrome, and targets are retained.
      return wifi_connected ? RadarStatusBadge::None : RadarStatusBadge::NoWifi;
    case RadarDataMode::Loading:
      // No current-revision success yet. A disconnected radio shows NO WIFI
      // rather than implying an active request.
      return wifi_connected ? RadarStatusBadge::Loading : RadarStatusBadge::NoWifi;
    case RadarDataMode::Stale:
      // Freshness state takes precedence over a Wi-Fi drop.
      return RadarStatusBadge::Stale;
    case RadarDataMode::Offline:
      return RadarStatusBadge::Offline;
  }
  return RadarStatusBadge::None;
}

void formatText(RadarStatusPlan* plan, uint32_t age_seconds,
                uint8_t activity_phase) {
  switch (plan->badge) {
    case RadarStatusBadge::None:
      plan->text[0] = '\0';
      break;
    case RadarStatusBadge::Loading: {
      const uint8_t dots =
          static_cast<uint8_t>(activity_phase % kRadarStatusLoadingDots) + 1U;
      char dot_buf[kRadarStatusLoadingDots + 1];
      uint8_t i = 0;
      for (; i < dots; ++i) {
        dot_buf[i] = '.';
      }
      dot_buf[i] = '\0';
      std::snprintf(plan->text, sizeof(plan->text), "LOADING%s", dot_buf);
      break;
    }
    case RadarStatusBadge::NoWifi:
      std::snprintf(plan->text, sizeof(plan->text), "NO WIFI");
      break;
    case RadarStatusBadge::Stale: {
      const uint32_t shown = age_seconds > kRadarStatusMaxAgeSeconds
                                 ? kRadarStatusMaxAgeSeconds
                                 : age_seconds;
      std::snprintf(plan->text, sizeof(plan->text), "STALE %lus",
                    static_cast<unsigned long>(shown));
      break;
    }
    case RadarStatusBadge::Offline:
      std::snprintf(plan->text, sizeof(plan->text), "OFFLINE");
      break;
  }
}

}  // namespace

RadarStatusPlan radarStatusPlan(RadarDataMode mode, uint32_t age_seconds,
                                bool show_aircraft, uint32_t data_revision,
                                uint32_t snapshot_revision, bool wifi_connected,
                                uint8_t activity_phase) {
  RadarStatusPlan plan{};
  // A revision mismatch means the published snapshot belongs to a superseded
  // settings revision; hide its targets defensively even if the view still
  // reports show_aircraft.
  plan.draw_aircraft = show_aircraft && (data_revision == snapshot_revision);
  plan.badge = selectBadge(mode, wifi_connected);
  formatText(&plan, age_seconds, activity_phase);
  return plan;
}

}  // namespace core
