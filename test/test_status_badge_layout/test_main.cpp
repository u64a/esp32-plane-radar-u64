#include <unity.h>

#include "core/status_badge_layout.h"

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

// Slot geometry mirrors the renderer (ui/radar_theme.h + radar_display.cpp):
// 240x240 disc, grid outer radius 107, center-dot radius 2, 3px disc inset,
// 3px clearance above the center dot, 3x2 text padding.
constexpr core::StatusBadgeSlot kSlot{/*center_x=*/120,
                                      /*center_y=*/120,
                                      /*disc_radius=*/107,
                                      /*disc_inset=*/3,
                                      /*center_dot_radius=*/2,
                                      /*center_clear_gap=*/3,
                                      /*pad_x=*/3,
                                      /*pad_y=*/2};

// Worst-case status text metrics measured offline against LovyanGFX (exactly
// replicating text_width()/fontHeight()), for both font paths the renderer
// selects. See the fixed strings LOADING.../STALE 999s/OFFLINE/NO WIFI.
//   Smooth (ui_font.vlw @ scale size 0.6875): max width 51, height 11.
//   Bitmap (FreeSansBold9pt7b) @ size 1.00: max width 105, height 18.
//                              @ size 0.95: max width  95, height 17.
//                              @ size 0.90: max width  86, height 16.
constexpr int kSmoothW = 51;
constexpr int kSmoothH = 11;
constexpr int kBitmapFullW = 105;
constexpr int kBitmapFullH = 18;
constexpr int kBitmap095W = 95;
constexpr int kBitmap095H = 17;
constexpr int kBitmap090W = 86;
constexpr int kBitmap090H = 16;

void assertRectInsideDiscAndClearsCenter(const core::StatusBadgeRect& rect) {
  // Never leaves the radar disc.
  TEST_ASSERT_TRUE(core::rectInsideDisc(rect.left, rect.top, rect.width,
                                        rect.height, kSlot.center_x,
                                        kSlot.center_y, kSlot.disc_radius));
  // Never touches the center-dot disc (test a 1px-larger disc for slack).
  TEST_ASSERT_FALSE(core::rectIntersectsDisc(
      rect.left, rect.top, rect.width, rect.height, kSlot.center_x,
      kSlot.center_y, kSlot.center_dot_radius + 1));
  // Stays fully west of (or on) the vertical centerline and above the center.
  TEST_ASSERT_TRUE(rect.left + rect.width <= kSlot.center_x);
  TEST_ASSERT_TRUE(rect.top + rect.height < kSlot.center_y);
}

}  // namespace

void setUp() {}
void tearDown() {}

// Predicate self-checks so the proofs below rest on verified primitives.
void test_rect_inside_disc_boundary() {
  // Corner exactly on the circle counts as inside.
  TEST_ASSERT_TRUE(core::rectInsideDisc(-3, -4, 6, 8, 0, 0, 5));
  // One corner one pixel outside fails.
  TEST_ASSERT_FALSE(core::rectInsideDisc(-3, -4, 7, 8, 0, 0, 5));
  TEST_ASSERT_FALSE(core::rectInsideDisc(0, 0, 1, 1, 0, 0, -1));
}

void test_rect_intersects_disc_boundary() {
  // Rectangle enclosing the center clearly intersects.
  TEST_ASSERT_TRUE(core::rectIntersectsDisc(-2, -2, 4, 4, 0, 0, 1));
  // Rectangle whose nearest edge is on the circle intersects (touching).
  TEST_ASSERT_TRUE(core::rectIntersectsDisc(5, -2, 4, 4, 0, 0, 5));
  // Rectangle whose nearest point is 6px away misses a radius-5 disc.
  TEST_ASSERT_FALSE(core::rectIntersectsDisc(6, -2, 4, 4, 0, 0, 5));
}

// Smooth (VLW) path: the worst-case text fits the slot at the scale size and
// the padded rectangle both stays inside the disc and clears the center dot.
void test_smooth_path_fits_and_is_safe() {
  const core::StatusBadgeRect rect =
      core::placeStatusBadge(kSmoothW, kSmoothH, kSlot);
  TEST_ASSERT_TRUE(rect.fits);
  TEST_ASSERT_EQUAL_INT(63, rect.left);
  TEST_ASSERT_EQUAL_INT(100, rect.top);
  TEST_ASSERT_EQUAL_INT(57, rect.width);
  TEST_ASSERT_EQUAL_INT(15, rect.height);
  assertRectInsideDiscAndClearsCenter(rect);
}

// Bitmap path at full scale size is too wide: it must be rejected so the
// renderer shrinks the style (rather than abbreviating the locked text).
void test_bitmap_full_size_does_not_fit() {
  const core::StatusBadgeRect rect =
      core::placeStatusBadge(kBitmapFullW, kBitmapFullH, kSlot);
  TEST_ASSERT_FALSE(rect.fits);
  // 0.95 is still too wide; the renderer keeps shrinking.
  const core::StatusBadgeRect rect095 =
      core::placeStatusBadge(kBitmap095W, kBitmap095H, kSlot);
  TEST_ASSERT_FALSE(rect095.fits);
}

// Bitmap path at the first fitting shrink (0.90): the padded rectangle stays
// inside the disc and clears the center dot with the full text preserved.
void test_bitmap_shrunk_path_fits_and_is_safe() {
  const core::StatusBadgeRect rect =
      core::placeStatusBadge(kBitmap090W, kBitmap090H, kSlot);
  TEST_ASSERT_TRUE(rect.fits);
  TEST_ASSERT_EQUAL_INT(28, rect.left);
  TEST_ASSERT_EQUAL_INT(95, rect.top);
  TEST_ASSERT_EQUAL_INT(92, rect.width);
  TEST_ASSERT_EQUAL_INT(20, rect.height);
  assertRectInsideDiscAndClearsCenter(rect);
}

// Property sweep: for EVERY text size the helper reports as fitting, the padded
// rectangle is inside the disc and never touches the center marker.
void test_all_fitting_rectangles_are_safe() {
  for (int w = 0; w <= 200; ++w) {
    for (int h = 0; h <= 60; ++h) {
      const core::StatusBadgeRect rect = core::placeStatusBadge(w, h, kSlot);
      if (rect.fits) {
        assertRectInsideDiscAndClearsCenter(rect);
      }
    }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_rect_inside_disc_boundary);
  RUN_TEST(test_rect_intersects_disc_boundary);
  RUN_TEST(test_smooth_path_fits_and_is_safe);
  RUN_TEST(test_bitmap_full_size_does_not_fit);
  RUN_TEST(test_bitmap_shrunk_path_fits_and_is_safe);
  RUN_TEST(test_all_fitting_rectangles_are_safe);
  return UNITY_END();
}
