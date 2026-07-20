#pragma once

// Pure, Arduino-/graphics-free placement of the compact radar status badge.
// The renderer measures the worst-case status text (width/height) under the
// active smooth or bitmap style and asks this helper for a padded rectangle in
// an upper-west in-disc slot. Keeping the geometry here lets it be proven on
// the host: the returned rectangle stays inside the radar disc and never
// touches the center marker, independent of any one font's hardcoded width.

namespace core {

// Fixed slot geometry (all pixels, screen coordinates).
struct StatusBadgeSlot {
  int center_x;          // radar disc center X
  int center_y;          // radar disc center Y
  int disc_radius;       // radar (grid outer) disc radius
  int disc_inset;        // keep the padded rect this many px inside the disc
  int center_dot_radius; // center marker radius
  int center_clear_gap;  // gap between rect bottom and the center-dot disc top
  int pad_x;             // horizontal text padding inside the rect
  int pad_y;             // vertical text padding inside the rect
};

// Resulting fill rectangle. `fits` is false when the padded rectangle cannot
// stay inside the inset disc for the given text metrics.
struct StatusBadgeRect {
  int left;
  int top;
  int width;
  int height;
  bool fits;
};

// Place a status badge whose measured text is `text_w` x `text_h` pixels into
// the upper-west slot: the padded rectangle's right edge sits on the vertical
// centerline (fully west) and its bottom edge is `center_clear_gap` px above
// the top of the center-dot disc. The upper-west corner is the one farthest
// from the disc center; `fits` is true only when that corner stays within the
// inset disc radius. On `fits == false` the caller should shrink the status
// font (never abbreviate the text).
StatusBadgeRect placeStatusBadge(int text_w, int text_h,
                                 const StatusBadgeSlot& slot);

// True when the axis-aligned rectangle [left, left+width] x [top, top+height]
// lies fully within the disc (all four corners inside or on the circle).
bool rectInsideDisc(int left, int top, int width, int height, int center_x,
                    int center_y, int radius);

// True when the rectangle intersects the filled disc of the given radius.
bool rectIntersectsDisc(int left, int top, int width, int height, int center_x,
                        int center_y, int radius);

}  // namespace core
