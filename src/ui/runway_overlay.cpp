#include "ui/runway_overlay.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>
#include <cstdlib>

#include "core/geo_projection.h"
#include "core/screen_geometry.h"
#include "data/large_airports.h"
#include "hardware/display_font.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace plane_radar_fonts = lgfx::v1::fonts;

namespace ui::runway {
namespace {

constexpr size_t kMaxAirportLabels = 32;

enum class AirportRangeState : uint8_t {
  kUnknown,
  kOut,
  kIn,
};
static_assert(sizeof(AirportRangeState) <= sizeof(bool));

AirportRangeState s_range_state[data::large_airports::kAirportCount];
bool s_label_pending[data::large_airports::kAirportCount];

bool s_runway_label_ready = false;
bool s_runway_label_use_vlw = false;
float s_runway_label_vlw_size = 0.38f;
const lgfx::GFXfont* s_runway_label_gfx =
    &plane_radar_fonts::FreeSansBold12pt7b;

int measureVlwHeight(lgfx::LGFXBase& gfx, float size) {
  gfx.setTextSize(size);
  return gfx.fontHeight();
}

float findVlwSizeForHeight(lgfx::LGFXBase& gfx, int target_px) {
  float lo = 0.2f;
  float hi = 1.2f;
  for (int i = 0; i < 14; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(gfx, mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void initRunwayLabelStyle(lgfx::LGFXBase& gfx) {
  if (s_runway_label_ready) {
    return;
  }

  const int target = radar::kRunwayLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_runway_label_use_vlw = true;
    s_runway_label_vlw_size = findVlwSizeForHeight(gfx, target);
  } else {
    s_runway_label_gfx = &plane_radar_fonts::FreeSansBold12pt7b;
    s_runway_label_use_vlw = false;
  }
  s_runway_label_ready = true;
}

void applyRunwayLabelStyle(lgfx::LGFXBase& gfx) {
  if (s_runway_label_use_vlw) {
    displayFontSetSmoothSize(gfx, s_runway_label_vlw_size);
  } else {
    displayFontSetBitmap(gfx, s_runway_label_gfx);
  }
}

double e7ToDeg(int32_t e7) { return static_cast<double>(e7) * 1e-7; }

bool latLonToScreen(
    const core::LocalProjection& projection, double lat, double lon,
    float pixels_per_km, core::geometry::ScreenPoint* screen) {
  if (screen == nullptr) {
    return false;
  }
  const core::LocalOffsetKm offset = projection.project(lat, lon);
  if (!core::localOffsetValid(offset)) {
    return false;
  }
  *screen = core::geometry::offsetToScreen(
      offset, {radar::kCenterX, radar::kCenterY}, pixels_per_km);
  return true;
}

uint16_t packScreenPoint(core::geometry::ScreenPoint point) {
  return static_cast<uint16_t>(static_cast<uint8_t>(point.x)) |
         static_cast<uint16_t>(static_cast<uint8_t>(point.y)) << 8;
}

core::geometry::ScreenPoint unpackScreenPoint(uint16_t packed) {
  return {
      static_cast<uint8_t>(packed),
      static_cast<uint8_t>(packed >> 8),
  };
}

void drawBoldRunwayLabel(lgfx::LGFXBase& gfx, const char* ident, int mx, int my) {
  const int tw = gfx.textWidth(ident);
  const int th = gfx.fontHeight();
  constexpr int kPadX = 2;
  constexpr int kPadY = 1;

  gfx.setTextDatum(textdatum_t::bottom_center);
  const int left = mx - tw / 2 - kPadX;
  const int top = my - th - kPadY;
  gfx.fillRect(left, top, tw + kPadX * 2, th + kPadY, radar::kColorBackground);
  gfx.setTextColor(radar::kColorRunwayLabel, radar::kColorBackground);
  gfx.drawString(ident, mx - 1, my);
  gfx.drawString(ident, mx + 1, my);
  gfx.drawString(ident, mx, my);
}

bool drawRunwayLine(lgfx::LGFXBase& gfx,
                    const core::LocalProjection& projection,
                    float pixels_per_km,
                    const data::large_airports::Runway& rw) {
  core::geometry::ScreenSegment runway{};
  if (!latLonToScreen(projection, e7ToDeg(rw.le_lat_e7),
                      e7ToDeg(rw.le_lon_e7), pixels_per_km, &runway.start) ||
      !latLonToScreen(projection, e7ToDeg(rw.he_lat_e7),
                      e7ToDeg(rw.he_lon_e7), pixels_per_km, &runway.end)) {
    return false;
  }
  core::geometry::ScreenSegment clipped{};
  if (!core::geometry::clipSegmentToDisc(
          runway, {radar::kCenterX, radar::kCenterY},
          radar::kGridOuterRadius, &clipped)) {
    return false;
  }

  gfx.drawWideLine(clipped.start.x, clipped.start.y, clipped.end.x,
                   clipped.end.y, radar::kRunwayLineHalfWidth,
                   radar::kColorRunway);
  return true;
}

void offsetLabelFromCenter(int ax, int ay, int* lx, int* ly) {
  const int dx = ax - radar::kCenterX;
  const int dy = ay - radar::kCenterY;
  const float len = sqrtf(static_cast<float>(dx * dx + dy * dy));
  const int gap = radar::kRunwayLabelGapPx;
  if (len < 1.0f) {
    *lx = ax;
    *ly = ay - gap;
    return;
  }
  *lx = ax + static_cast<int>(lroundf(dx / len * static_cast<float>(gap)));
  *ly = ay + static_cast<int>(lroundf(dy / len * static_cast<float>(gap)));
}

void drawAirportLabel(lgfx::LGFXBase& gfx,
                      const data::large_airports::Airport& ap,
                      core::geometry::ScreenPoint anchor) {
  int lx = 0;
  int ly = 0;
  offsetLabelFromCenter(anchor.x, anchor.y, &lx, &ly);
  drawBoldRunwayLabel(gfx, ap.ident, lx, ly);
}

}  // namespace

void drawLargeAirportRunways(lgfx::LGFXBase& gfx) {
  if (!radar::showRunways()) {
    return;
  }
  displayFontEnsureLoaded(gfx);
  const float radius_km = radar::fetchRadiusKm();
  const core::LocalProjection projection(services::location::lat(),
                                         services::location::lon());
  const float pixels_per_km =
      static_cast<float>(radar::kGridOuterRadius) /
      radar::rangeCurrent().outer_km;

  uint16_t label_anchors[kMaxAirportLabels];
  size_t label_count = 0;
  // Generated runways are grouped by airport, so one anchor serves its group.
  core::geometry::ScreenPoint current_airport_anchor{};

  for (size_t i = 0; i < data::large_airports::kAirportCount; ++i) {
    s_range_state[i] = AirportRangeState::kUnknown;
    s_label_pending[i] = false;
  }

  for (size_t i = 0; i < data::large_airports::kRunwayCount; ++i) {
    const auto& rw = data::large_airports::kRunways[i];
    const uint16_t ap_idx = rw.airport_idx;
    if (s_range_state[ap_idx] == AirportRangeState::kUnknown) {
      const auto& ap = data::large_airports::kAirports[ap_idx];
      const core::LocalOffsetKm offset =
          projection.project(e7ToDeg(ap.lat_e7), e7ToDeg(ap.lon_e7));
      s_range_state[ap_idx] = core::isWithinDistanceKm(offset, radius_km)
                                  ? AirportRangeState::kIn
                                  : AirportRangeState::kOut;
      if (s_range_state[ap_idx] == AirportRangeState::kIn) {
        current_airport_anchor = core::geometry::clampPointToDisc(
            core::geometry::offsetToScreen(
                offset, {radar::kCenterX, radar::kCenterY}, pixels_per_km),
            {radar::kCenterX, radar::kCenterY}, radar::kGridOuterRadius);
      }
    }
    if (s_range_state[ap_idx] != AirportRangeState::kIn) {
      continue;
    }
    if (!drawRunwayLine(gfx, projection, pixels_per_km, rw)) {
      continue;
    }
    if (!s_label_pending[ap_idx] && label_count < kMaxAirportLabels) {
      s_label_pending[ap_idx] = true;
      label_anchors[label_count++] = packScreenPoint(current_airport_anchor);
    }
  }

  if (label_count == 0) {
    return;
  }

  initRunwayLabelStyle(gfx);
  applyRunwayLabelStyle(gfx);
  size_t label_index = 0;
  for (size_t airport_index = 0;
       airport_index < data::large_airports::kAirportCount;
       ++airport_index) {
    if (!s_label_pending[airport_index]) {
      continue;
    }
    drawAirportLabel(gfx, data::large_airports::kAirports[airport_index],
                     unpackScreenPoint(label_anchors[label_index++]));
  }
}

}  // namespace ui::runway
