#include <unity.h>

#include <limits>

#include "core/screen_geometry.h"

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

constexpr core::geometry::ScreenPoint kCenter{0, 0};

void assertInside(core::geometry::ScreenPoint point, int radius) {
  TEST_ASSERT_TRUE(core::geometry::isInsideDisc(point, kCenter, radius));
}

void assertSegmentInside(const core::geometry::ScreenSegment& segment,
                         int radius) {
  assertInside(segment.start, radius);
  assertInside(segment.end, radius);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_offset_to_screen_uses_radar_axes_and_half_away_rounding() {
  const core::geometry::ScreenPoint northeast =
      core::geometry::offsetToScreen({1.25f, 2.5f, 0.0f}, {120, 120}, 2.0f);
  const core::geometry::ScreenPoint southwest =
      core::geometry::offsetToScreen({-1.25f, -2.5f, 0.0f}, {120, 120}, 2.0f);

  TEST_ASSERT_EQUAL_INT(123, northeast.x);
  TEST_ASSERT_EQUAL_INT(115, northeast.y);
  TEST_ASSERT_EQUAL_INT(117, southwest.x);
  TEST_ASSERT_EQUAL_INT(125, southwest.y);
}

void test_invalid_offsets_and_directions_do_not_reach_screen_rounding() {
  const core::LocalOffsetKm invalid{0.0f, 0.0f, 0.0f, false};
  const core::geometry::ScreenPoint center{120, 120};
  const core::geometry::ScreenPoint projected =
      core::geometry::offsetToScreen(invalid, center, 8.0f);
  const core::geometry::ScreenPoint non_finite_scale =
      core::geometry::offsetToScreen(
          {1.0f, 1.0f, 1.0f}, center,
          std::numeric_limits<float>::infinity());
  const core::geometry::ScreenPoint invalid_rim =
      core::geometry::pointOnDiscRim(
          std::numeric_limits<float>::quiet_NaN(), 1.0f, center, 107);

  TEST_ASSERT_EQUAL_INT(center.x, projected.x);
  TEST_ASSERT_EQUAL_INT(center.y, projected.y);
  TEST_ASSERT_EQUAL_INT(center.x, non_finite_scale.x);
  TEST_ASSERT_EQUAL_INT(center.y, non_finite_scale.y);
  TEST_ASSERT_EQUAL_INT(center.x, invalid_rim.x);
  TEST_ASSERT_EQUAL_INT(center.y, invalid_rim.y);
}

void test_squared_distance_and_inside_checks_include_boundary() {
  TEST_ASSERT_EQUAL_INT64(25, core::geometry::squaredDistance({3, 4}, kCenter));
  TEST_ASSERT_TRUE(core::geometry::isInsideDisc({3, 4}, kCenter, 5));
  TEST_ASSERT_TRUE(core::geometry::isInsideDisc({0, 0}, kCenter, 0));
  TEST_ASSERT_FALSE(core::geometry::isInsideDisc({4, 4}, kCenter, 5));
  TEST_ASSERT_FALSE(core::geometry::isInsideDisc({0, 0}, kCenter, -1));
}

void test_radial_clamp_preserves_inside_and_clamps_cardinal_points() {
  const core::geometry::ScreenPoint inside =
      core::geometry::clampPointToDisc({3, 4}, kCenter, 10);
  const core::geometry::ScreenPoint horizontal =
      core::geometry::clampPointToDisc({20, 0}, kCenter, 10);
  const core::geometry::ScreenPoint vertical =
      core::geometry::clampPointToDisc({0, -20}, kCenter, 10);

  TEST_ASSERT_EQUAL_INT(3, inside.x);
  TEST_ASSERT_EQUAL_INT(4, inside.y);
  TEST_ASSERT_EQUAL_INT(10, horizontal.x);
  TEST_ASSERT_EQUAL_INT(0, horizontal.y);
  TEST_ASSERT_EQUAL_INT(0, vertical.x);
  TEST_ASSERT_EQUAL_INT(-10, vertical.y);
  assertInside(horizontal, 10);
  assertInside(vertical, 10);
}

void test_radial_rounding_falls_inward_when_nearest_pixel_is_outside() {
  const core::geometry::ScreenPoint clamped =
      core::geometry::clampPointToDisc({20, 20}, kCenter, 5);
  const core::geometry::ScreenPoint rim =
      core::geometry::pointOnDiscRim(1.0, 1.0, kCenter, 5);

  TEST_ASSERT_EQUAL_INT(3, clamped.x);
  TEST_ASSERT_EQUAL_INT(3, clamped.y);
  TEST_ASSERT_EQUAL_INT(3, rim.x);
  TEST_ASSERT_EQUAL_INT(3, rim.y);
  assertInside(clamped, 5);
  assertInside(rim, 5);
}

void test_rim_points_cover_cardinal_and_diagonal_directions() {
  constexpr float kDirections[][2] = {
      {1.0, 0.0}, {0.0, 1.0}, {-1.0, 0.0}, {0.0, -1.0},
      {1.0, 1.0}, {-1.0, 1.0}, {-1.0, -1.0}, {1.0, -1.0},
  };
  for (const auto& direction : kDirections) {
    assertInside(core::geometry::pointOnDiscRim(
                     direction[0], direction[1], kCenter, 17),
                 17);
  }

  const core::geometry::ScreenPoint zero =
      core::geometry::pointOnDiscRim(0.0, 0.0, {8, 9}, 17);
  TEST_ASSERT_EQUAL_INT(8, zero.x);
  TEST_ASSERT_EQUAL_INT(9, zero.y);
}

void test_clip_keeps_fully_inside_segment_unchanged() {
  core::geometry::ScreenSegment clipped{};
  TEST_ASSERT_TRUE(core::geometry::clipSegmentToDisc(
      {{-3, -4}, {4, 3}}, kCenter, 10, &clipped));

  TEST_ASSERT_EQUAL_INT(-3, clipped.start.x);
  TEST_ASSERT_EQUAL_INT(-4, clipped.start.y);
  TEST_ASSERT_EQUAL_INT(4, clipped.end.x);
  TEST_ASSERT_EQUAL_INT(3, clipped.end.y);
  assertSegmentInside(clipped, 10);
}

void test_clip_handles_one_endpoint_outside_in_both_orders() {
  core::geometry::ScreenSegment outward{};
  core::geometry::ScreenSegment inward{};
  TEST_ASSERT_TRUE(core::geometry::clipSegmentToDisc(
      {{0, 0}, {20, 0}}, kCenter, 10, &outward));
  TEST_ASSERT_TRUE(core::geometry::clipSegmentToDisc(
      {{20, 0}, {0, 0}}, kCenter, 10, &inward));

  TEST_ASSERT_EQUAL_INT(0, outward.start.x);
  TEST_ASSERT_EQUAL_INT(10, outward.end.x);
  TEST_ASSERT_EQUAL_INT(10, inward.start.x);
  TEST_ASSERT_EQUAL_INT(0, inward.end.x);
  assertSegmentInside(outward, 10);
  assertSegmentInside(inward, 10);
}

void test_clip_handles_horizontal_vertical_and_diagonal_crossings() {
  core::geometry::ScreenSegment horizontal{};
  core::geometry::ScreenSegment vertical{};
  core::geometry::ScreenSegment diagonal{};
  TEST_ASSERT_TRUE(core::geometry::clipSegmentToDisc(
      {{-20, 0}, {20, 0}}, kCenter, 10, &horizontal));
  TEST_ASSERT_TRUE(core::geometry::clipSegmentToDisc(
      {{0, -20}, {0, 20}}, kCenter, 10, &vertical));
  TEST_ASSERT_TRUE(core::geometry::clipSegmentToDisc(
      {{-20, -20}, {20, 20}}, kCenter, 10, &diagonal));

  TEST_ASSERT_EQUAL_INT(-10, horizontal.start.x);
  TEST_ASSERT_EQUAL_INT(10, horizontal.end.x);
  TEST_ASSERT_EQUAL_INT(-10, vertical.start.y);
  TEST_ASSERT_EQUAL_INT(10, vertical.end.y);
  TEST_ASSERT_EQUAL_INT(-7, diagonal.start.x);
  TEST_ASSERT_EQUAL_INT(-7, diagonal.start.y);
  TEST_ASSERT_EQUAL_INT(7, diagonal.end.x);
  TEST_ASSERT_EQUAL_INT(7, diagonal.end.y);
  assertSegmentInside(horizontal, 10);
  assertSegmentInside(vertical, 10);
  assertSegmentInside(diagonal, 10);
}

void test_clip_tangent_returns_single_inside_point() {
  core::geometry::ScreenSegment clipped{};
  TEST_ASSERT_TRUE(core::geometry::clipSegmentToDisc(
      {{-20, 10}, {20, 10}}, kCenter, 10, &clipped));

  TEST_ASSERT_EQUAL_INT(0, clipped.start.x);
  TEST_ASSERT_EQUAL_INT(10, clipped.start.y);
  TEST_ASSERT_EQUAL_INT(clipped.start.x, clipped.end.x);
  TEST_ASSERT_EQUAL_INT(clipped.start.y, clipped.end.y);
  assertSegmentInside(clipped, 10);
}

void test_clip_degenerate_inside_succeeds_and_outside_fails() {
  core::geometry::ScreenSegment clipped{};
  TEST_ASSERT_TRUE(core::geometry::clipSegmentToDisc(
      {{3, 4}, {3, 4}}, kCenter, 5, &clipped));
  TEST_ASSERT_EQUAL_INT(3, clipped.start.x);
  TEST_ASSERT_EQUAL_INT(4, clipped.start.y);
  TEST_ASSERT_FALSE(core::geometry::clipSegmentToDisc(
      {{6, 0}, {6, 0}}, kCenter, 5, &clipped));
}

void test_clip_rejects_fully_outside_segment_and_null_output() {
  core::geometry::ScreenSegment clipped{};
  TEST_ASSERT_FALSE(core::geometry::clipSegmentToDisc(
      {{-20, 11}, {20, 11}}, kCenter, 10, &clipped));
  TEST_ASSERT_FALSE(core::geometry::clipSegmentToDisc(
      {{-20, 0}, {20, 0}}, kCenter, 10, nullptr));
}

void test_clip_integer_rounding_never_leaves_disc() {
  core::geometry::ScreenSegment clipped{};
  TEST_ASSERT_TRUE(core::geometry::clipSegmentToDisc(
      {{0, 0}, {10, 10}}, kCenter, 5, &clipped));

  TEST_ASSERT_EQUAL_INT(3, clipped.end.x);
  TEST_ASSERT_EQUAL_INT(3, clipped.end.y);
  assertSegmentInside(clipped, 5);

  for (int y = -12; y <= 12; ++y) {
    core::geometry::ScreenSegment swept{};
    if (core::geometry::clipSegmentToDisc(
            {{-20, y}, {20, -y}}, kCenter, 10, &swept)) {
      assertSegmentInside(swept, 10);
    }
  }
}

void test_clip_handles_long_screen_relevant_segments() {
  core::geometry::ScreenSegment horizontal{};
  core::geometry::ScreenSegment oblique{};
  core::geometry::ScreenSegment tangent{};

  TEST_ASSERT_TRUE(core::geometry::clipSegmentToDisc(
      {{-1000000, 0}, {1000000, 0}}, kCenter, 107, &horizontal));
  TEST_ASSERT_TRUE(core::geometry::clipSegmentToDisc(
      {{-1000000, -500000}, {1000000, 500000}}, kCenter, 100, &oblique));
  TEST_ASSERT_TRUE(core::geometry::clipSegmentToDisc(
      {{-1000000, 107}, {1000000, 107}}, kCenter, 107, &tangent));

  TEST_ASSERT_EQUAL_INT(-107, horizontal.start.x);
  TEST_ASSERT_EQUAL_INT(107, horizontal.end.x);
  TEST_ASSERT_EQUAL_INT(-89, oblique.start.x);
  TEST_ASSERT_EQUAL_INT(-45, oblique.start.y);
  TEST_ASSERT_EQUAL_INT(89, oblique.end.x);
  TEST_ASSERT_EQUAL_INT(45, oblique.end.y);
  TEST_ASSERT_EQUAL_INT(0, tangent.start.x);
  TEST_ASSERT_EQUAL_INT(107, tangent.start.y);
  TEST_ASSERT_EQUAL_INT(tangent.start.x, tangent.end.x);
  TEST_ASSERT_EQUAL_INT(tangent.start.y, tangent.end.y);
  assertSegmentInside(horizontal, 107);
  assertSegmentInside(oblique, 100);
  assertSegmentInside(tangent, 107);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_offset_to_screen_uses_radar_axes_and_half_away_rounding);
  RUN_TEST(test_invalid_offsets_and_directions_do_not_reach_screen_rounding);
  RUN_TEST(test_squared_distance_and_inside_checks_include_boundary);
  RUN_TEST(test_radial_clamp_preserves_inside_and_clamps_cardinal_points);
  RUN_TEST(test_radial_rounding_falls_inward_when_nearest_pixel_is_outside);
  RUN_TEST(test_rim_points_cover_cardinal_and_diagonal_directions);
  RUN_TEST(test_clip_keeps_fully_inside_segment_unchanged);
  RUN_TEST(test_clip_handles_one_endpoint_outside_in_both_orders);
  RUN_TEST(test_clip_handles_horizontal_vertical_and_diagonal_crossings);
  RUN_TEST(test_clip_tangent_returns_single_inside_point);
  RUN_TEST(test_clip_degenerate_inside_succeeds_and_outside_fails);
  RUN_TEST(test_clip_rejects_fully_outside_segment_and_null_output);
  RUN_TEST(test_clip_integer_rounding_never_leaves_disc);
  RUN_TEST(test_clip_handles_long_screen_relevant_segments);
  return UNITY_END();
}
