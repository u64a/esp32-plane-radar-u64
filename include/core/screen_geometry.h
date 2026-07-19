#pragma once

#include <cstdint>

#include "core/geo_projection.h"

namespace core::geometry {

struct ScreenPoint {
  int x;
  int y;
};

struct ScreenSegment {
  ScreenPoint start;
  ScreenPoint end;
};

/**
 * Map east-positive/north-positive offsets to screen coordinates.
 * Pixel components are rounded to nearest integer, halfway away from zero.
 */
ScreenPoint offsetToScreen(const LocalOffsetKm& offset, ScreenPoint center,
                           float pixels_per_km);

int64_t squaredDistance(ScreenPoint point, ScreenPoint center);
bool isInsideDisc(ScreenPoint point, ScreenPoint center, int radius);

/**
 * Clamp an integer point radially to a disc. Boundary coordinates first use
 * nearest rounding; an outward-rounded result is truncated toward the center.
 */
ScreenPoint clampPointToDisc(ScreenPoint point, ScreenPoint center, int radius);

/**
 * Return the integer rim point in the supplied direction. A zero direction
 * returns the center. The result is guaranteed to be inside or on the disc.
 */
ScreenPoint pointOnDiscRim(float direction_x, float direction_y,
                           ScreenPoint center, int radius);

/**
 * Analytically clip a segment to a disc. Tangencies produce a degenerate
 * segment. Clipped boundary coordinates use the same guaranteed-inside
 * rounding as radial clamps.
 */
bool clipSegmentToDisc(ScreenSegment segment, ScreenPoint center, int radius,
                       ScreenSegment* clipped);

}  // namespace core::geometry
