#include "core/screen_geometry.h"

#include <algorithm>
#include <cmath>

namespace core::geometry {
namespace {

ScreenPoint roundInsideDisc(float x, float y, ScreenPoint center,
                            int radius) {
  if (radius <= 0) {
    return center;
  }

  float dx = x - static_cast<float>(center.x);
  float dy = y - static_cast<float>(center.y);
  const float distance = hypotf(dx, dy);
  if (distance > static_cast<float>(radius)) {
    const float scale = static_cast<float>(radius) / distance;
    dx *= scale;
    dy *= scale;
  }

  ScreenPoint rounded{
      center.x + static_cast<int>(lroundf(dx)),
      center.y + static_cast<int>(lroundf(dy)),
  };
  if (isInsideDisc(rounded, center, radius)) {
    return rounded;
  }

  return {
      center.x + static_cast<int>(truncf(dx)),
      center.y + static_cast<int>(truncf(dy)),
  };
}

ScreenPoint boundaryPoint(float unit_x, float unit_y, float perpendicular,
                          float along_from_closest, ScreenPoint center,
                          int radius) {
  const float closest_x =
      static_cast<float>(center.x) + unit_y * perpendicular;
  const float closest_y =
      static_cast<float>(center.y) - unit_x * perpendicular;
  return roundInsideDisc(closest_x + unit_x * along_from_closest,
                         closest_y + unit_y * along_from_closest, center,
                         radius);
}

}  // namespace

ScreenPoint offsetToScreen(const LocalOffsetKm& offset, ScreenPoint center,
                           float pixels_per_km) {
  if (!localOffsetValid(offset) || !std::isfinite(pixels_per_km)) {
    return center;
  }
  return {
      center.x +
          static_cast<int>(lroundf(offset.east_km * pixels_per_km)),
      center.y -
          static_cast<int>(lroundf(offset.north_km * pixels_per_km)),
  };
}

int64_t squaredDistance(ScreenPoint point, ScreenPoint center) {
  const int64_t dx = static_cast<int64_t>(point.x) - center.x;
  const int64_t dy = static_cast<int64_t>(point.y) - center.y;
  return dx * dx + dy * dy;
}

bool isInsideDisc(ScreenPoint point, ScreenPoint center, int radius) {
  if (radius < 0) {
    return false;
  }
  const int64_t radius_sq = static_cast<int64_t>(radius) * radius;
  return squaredDistance(point, center) <= radius_sq;
}

ScreenPoint clampPointToDisc(ScreenPoint point, ScreenPoint center, int radius) {
  if (isInsideDisc(point, center, radius)) {
    return point;
  }
  return roundInsideDisc(point.x, point.y, center, radius);
}

ScreenPoint pointOnDiscRim(float direction_x, float direction_y,
                           ScreenPoint center, int radius) {
  if (!std::isfinite(direction_x) || !std::isfinite(direction_y)) {
    return center;
  }
  const float length = hypotf(direction_x, direction_y);
  if (radius <= 0 || length == 0.0f) {
    return center;
  }
  const float scale = static_cast<float>(radius) / length;
  return roundInsideDisc(center.x + direction_x * scale,
                         center.y + direction_y * scale, center, radius);
}

bool clipSegmentToDisc(ScreenSegment segment, ScreenPoint center, int radius,
                       ScreenSegment* clipped) {
  if (clipped == nullptr || radius < 0) {
    return false;
  }

  const bool start_inside = isInsideDisc(segment.start, center, radius);
  const bool end_inside = isInsideDisc(segment.end, center, radius);
  if (start_inside && end_inside) {
    *clipped = segment;
    return true;
  }

  const float dx = static_cast<float>(segment.end.x) - segment.start.x;
  const float dy = static_cast<float>(segment.end.y) - segment.start.y;
  const float length = hypotf(dx, dy);
  if (length == 0.0f) {
    return false;
  }

  const float unit_x = dx / length;
  const float unit_y = dy / length;
  const float fx = static_cast<float>(segment.start.x) - center.x;
  const float fy = static_cast<float>(segment.start.y) - center.y;
  const float perpendicular = fx * unit_y - fy * unit_x;
  const float radius_f = static_cast<float>(radius);
  if (fabsf(perpendicular) > radius_f) {
    return false;
  }

  const float along_to_closest = -(fx * unit_x + fy * unit_y);
  const float half_chord =
      sqrtf(std::max(0.0f, radius_f * radius_f -
                               perpendicular * perpendicular));
  const float enter_distance = along_to_closest - half_chord;
  const float exit_distance = along_to_closest + half_chord;
  if (enter_distance > length || exit_distance < 0.0f) {
    return false;
  }

  clipped->start =
      start_inside
          ? segment.start
          : boundaryPoint(unit_x, unit_y, perpendicular, -half_chord, center,
                          radius);
  clipped->end =
      end_inside
          ? segment.end
          : boundaryPoint(unit_x, unit_y, perpendicular, half_chord, center,
                          radius);
  return true;
}

}  // namespace core::geometry
