#include "core/status_badge_layout.h"

#include <algorithm>

namespace core {

namespace {

long sq(long value) { return value * value; }

}  // namespace

StatusBadgeRect placeStatusBadge(int text_w, int text_h,
                                 const StatusBadgeSlot& slot) {
  StatusBadgeRect rect{};
  rect.width = text_w + 2 * slot.pad_x;
  rect.height = text_h + 2 * slot.pad_y;

  // Right edge on the vertical centerline keeps the badge fully west and gives
  // the widest chord the slot can hold at its northmost row.
  const int right = slot.center_x;
  rect.left = right - rect.width;

  // Bottom edge sits a fixed clear gap above the center-dot disc, so the badge
  // never erases (or touches) the center marker.
  const int bottom =
      slot.center_y - slot.center_dot_radius - slot.center_clear_gap;
  rect.top = bottom - rect.height;

  // The upper-west corner (min x, min y) is farthest from the disc center; if
  // it fits, every other corner does too.
  const int reff = slot.disc_radius - slot.disc_inset;
  const long dx = slot.center_x - rect.left;
  const long dy = slot.center_y - rect.top;
  rect.fits = reff >= 0 && rect.left >= 0 && rect.top >= 0 &&
              (sq(dx) + sq(dy) <= sq(reff));
  return rect;
}

bool rectInsideDisc(int left, int top, int width, int height, int center_x,
                    int center_y, int radius) {
  if (radius < 0) {
    return false;
  }
  const int xs[2] = {left, left + width};
  const int ys[2] = {top, top + height};
  const long r2 = sq(radius);
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 2; ++j) {
      if (sq(center_x - xs[i]) + sq(center_y - ys[j]) > r2) {
        return false;
      }
    }
  }
  return true;
}

bool rectIntersectsDisc(int left, int top, int width, int height, int center_x,
                        int center_y, int radius) {
  if (radius < 0) {
    return false;
  }
  const int nearest_x = std::max(left, std::min(center_x, left + width));
  const int nearest_y = std::max(top, std::min(center_y, top + height));
  return sq(center_x - nearest_x) + sq(center_y - nearest_y) <= sq(radius);
}

}  // namespace core
