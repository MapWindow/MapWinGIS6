// SPDX-License-Identifier: MPL-2.0

/**
 * @file ViewportTests.cpp
 * @brief Tests viewport coordinate transformations, extent fitting, and
 *        pan and zoom calculations.
 *
 * Verifies map-to-screen and screen-to-map conversions, aspect-preserving
 * extent fitting with padding, pan, center and anchored zoom, limits, and the
 * handling of zero-sized viewports, degenerate extents, and non-finite inputs.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "mapwingis/core/Error.h"
#include "mapwingis/map/Viewport.h"

using namespace mwg;

namespace
{
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    constexpr double kInfinity = std::numeric_limits<double>::infinity();
    constexpr double kTolerance = 1e-9;

    void ExpectScreen(const std::optional<ScreenPoint>& actual, const double x, const double y)
    {
        ASSERT_TRUE(actual.has_value());
        EXPECT_NEAR(actual->x, x, kTolerance);
        EXPECT_NEAR(actual->y, y, kTolerance);
    }

    void ExpectMap(const std::optional<MapPoint>& actual, const double x, const double y,
                   const double tolerance = kTolerance)
    {
        ASSERT_TRUE(actual.has_value());
        EXPECT_NEAR(actual->x, x, tolerance);
        EXPECT_NEAR(actual->y, y, tolerance);
    }

    void ExpectExtent(const Extent& actual, const Extent& expected)
    {
        EXPECT_NEAR(actual.xMin, expected.xMin, kTolerance);
        EXPECT_NEAR(actual.yMin, expected.yMin, kTolerance);
        EXPECT_NEAR(actual.xMax, expected.xMax, kTolerance);
        EXPECT_NEAR(actual.yMax, expected.yMax, kTolerance);
    }

    /// True when the extent lies within the visible extent.
    bool IsVisible(const Viewport& viewport, const Extent& extent)
    {
        const Extent visible = viewport.VisibleExtent();
        return visible.xMin <= extent.xMin && visible.yMin <= extent.yMin &&
               visible.xMax >= extent.xMax && visible.yMax >= extent.yMax;
    }

    /// Viewport of 101 x 51 pixels fitted to (0, 0)-(100, 50): one map unit
    /// per pixel, with the extent edges on the outermost pixel centers.
    Viewport MakeFittedViewport()
    {
        Viewport viewport(101, 51);
        EXPECT_TRUE(viewport.FitExtent({0, 0, 100, 50}));
        return viewport;
    }
}

TEST(Viewport, DefaultViewportHasNoSizeAndNoView)
{
    const Viewport viewport;

    EXPECT_EQ(viewport.Width(), 0);
    EXPECT_EQ(viewport.Height(), 0);
    EXPECT_FALSE(viewport.HasView());
    EXPECT_FALSE(viewport.IsValid());
    EXPECT_FALSE(viewport.Center().has_value());
    EXPECT_FALSE(viewport.Resolution().has_value());
    EXPECT_TRUE(viewport.VisibleExtent().IsEmpty());
    EXPECT_FALSE(viewport.MapToScreen({0, 0}).has_value());
    EXPECT_FALSE(viewport.ScreenToMap({0, 0}).has_value());
}

TEST(Viewport, SizedViewportWithoutViewIsNotValid)
{
    const Viewport viewport(640, 480);

    EXPECT_EQ(viewport.Width(), 640);
    EXPECT_EQ(viewport.Height(), 480);
    EXPECT_FALSE(viewport.IsValid());
    EXPECT_FALSE(viewport.MapToScreen({0, 0}).has_value());
}

TEST(Viewport, RejectsNegativeAndOversizedDimensions)
{
    EXPECT_THROW(Viewport(-1, 10), InvalidArgument);
    EXPECT_THROW(Viewport(10, -1), InvalidArgument);
    EXPECT_THROW(Viewport(Viewport::kMaxDimension + 1, 10), InvalidArgument);

    Viewport viewport(10, 10);
    EXPECT_THROW(viewport.SetSize(10, Viewport::kMaxDimension + 1), InvalidArgument);
    EXPECT_EQ(viewport.Width(), 10);
    EXPECT_EQ(viewport.Height(), 10);
    EXPECT_NO_THROW(viewport.SetSize(Viewport::kMaxDimension, Viewport::kMaxDimension));
}

TEST(Viewport, MapsExtentCornersToOutermostPixelCenters)
{
    const Viewport viewport = MakeFittedViewport();

    ExpectScreen(viewport.MapToScreen({0, 50}), 0.5, 0.5);     // top-left
    ExpectScreen(viewport.MapToScreen({100, 50}), 100.5, 0.5); // top-right
    ExpectScreen(viewport.MapToScreen({0, 0}), 0.5, 50.5);     // bottom-left
    ExpectScreen(viewport.MapToScreen({100, 0}), 100.5, 50.5); // bottom-right
}

TEST(Viewport, MapsMapCenterToViewportCenter)
{
    const Viewport viewport = MakeFittedViewport();

    ExpectMap(viewport.Center(), 50, 25);
    ExpectScreen(viewport.MapToScreen({50, 25}), 50.5, 25.5);
    ExpectMap(viewport.ScreenToMap({50.5, 25.5}), 50, 25);
}

TEST(Viewport, ScreenYIncreasesDownwardsWhileMapYIncreasesUpwards)
{
    const Viewport viewport = MakeFittedViewport();

    const std::optional<ScreenPoint> lower = viewport.MapToScreen({50, 10});
    const std::optional<ScreenPoint> higher = viewport.MapToScreen({50, 40});
    ASSERT_TRUE(lower.has_value());
    ASSERT_TRUE(higher.has_value());
    EXPECT_GT(lower->y, higher->y);

    const std::optional<ScreenPoint> left = viewport.MapToScreen({10, 25});
    const std::optional<ScreenPoint> right = viewport.MapToScreen({90, 25});
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(right.has_value());
    EXPECT_LT(left->x, right->x);
}

TEST(Viewport, MapsViewportCornersToVisibleExtentCorners)
{
    const Viewport viewport = MakeFittedViewport();

    ExpectExtent(viewport.VisibleExtent(), {-0.5, -0.5, 100.5, 50.5});
    ExpectMap(viewport.ScreenToMap({0, 0}), -0.5, 50.5);
    ExpectMap(viewport.ScreenToMap({101, 51}), 100.5, -0.5);
}

TEST(Viewport, KeepsFractionalScreenCoordinates)
{
    const Viewport viewport = MakeFittedViewport();

    ExpectScreen(viewport.MapToScreen({10.25, 20.75}), 10.75, 29.75);
}

TEST(Viewport, RoundTripsMapToScreenToMap)
{
    const Extent extents[] = {
        {0, 0, 100, 50},
        {-180, -90, 180, 90},
        {500'000, 6'000'000, 501'000, 6'000'010},
        {-20'037'508.34, -20'037'508.34, 20'037'508.34, 20'037'508.34},
        {0.001, 0.002, 0.003, 0.004},
    };
    const int sizes[][2] = {{1, 1}, {101, 51}, {800, 600}, {37, 1200}};

    for (const Extent& extent : extents)
    {
        for (const auto& size : sizes)
        {
            Viewport viewport(size[0], size[1]);
            ASSERT_TRUE(viewport.FitExtent(extent, 5.0));
            const double tolerance = 1e-9 * std::max(1.0, std::abs(extent.xMax) + std::abs(extent.yMax));

            const MapPoint points[] = {
                {extent.xMin, extent.yMin},
                {extent.xMax, extent.yMax},
                {extent.CenterX(), extent.CenterY()},
                {extent.xMin + extent.Width() / 3.0, extent.yMax - extent.Height() / 7.0},
                {extent.xMax + extent.Width(), extent.yMin - extent.Height()},
            };
            for (const MapPoint& point : points)
            {
                const std::optional<ScreenPoint> screen = viewport.MapToScreen(point);
                ASSERT_TRUE(screen.has_value());
                ExpectMap(viewport.ScreenToMap(*screen), point.x, point.y, tolerance);
            }
        }
    }
}

TEST(Viewport, RoundTripsScreenToMapToScreen)
{
    const Viewport viewport = MakeFittedViewport();

    for (const ScreenPoint point : {ScreenPoint{0, 0}, ScreenPoint{17.3, 42.9}, ScreenPoint{-20, 300}})
    {
        const std::optional<MapPoint> map = viewport.ScreenToMap(point);
        ASSERT_TRUE(map.has_value());
        ExpectScreen(viewport.MapToScreen(*map), point.x, point.y);
    }
}

TEST(Viewport, WideViewportExpandsVisibleExtentHorizontally)
{
    Viewport viewport(201, 51);
    ASSERT_TRUE(viewport.FitExtent({0, 0, 100, 50}));

    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 1.0);
    ExpectExtent(viewport.VisibleExtent(), {-50.5, -0.5, 150.5, 50.5});
    ExpectScreen(viewport.MapToScreen({0, 0}), 50.5, 50.5);
    ExpectScreen(viewport.MapToScreen({100, 50}), 150.5, 0.5);
}

TEST(Viewport, TallViewportExpandsVisibleExtentVertically)
{
    Viewport viewport(101, 201);
    ASSERT_TRUE(viewport.FitExtent({0, 0, 100, 50}));

    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 1.0);
    ExpectExtent(viewport.VisibleExtent(), {-0.5, -75.5, 100.5, 125.5});
    ExpectScreen(viewport.MapToScreen({0, 0}), 0.5, 125.5);
    ExpectScreen(viewport.MapToScreen({100, 50}), 100.5, 75.5);
}

TEST(Viewport, WideExtentIsLimitedByViewportWidth)
{
    Viewport viewport(101, 101);
    ASSERT_TRUE(viewport.FitExtent({0, 0, 400, 10}));

    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 4.0);
    ExpectScreen(viewport.MapToScreen({0, 0}), 0.5, 51.75);
    ExpectScreen(viewport.MapToScreen({400, 10}), 100.5, 49.25);
}

TEST(Viewport, TallExtentIsLimitedByViewportHeight)
{
    Viewport viewport(101, 101);
    ASSERT_TRUE(viewport.FitExtent({0, 0, 10, 400}));

    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 4.0);
    ExpectScreen(viewport.MapToScreen({0, 0}), 49.25, 100.5);
    ExpectScreen(viewport.MapToScreen({10, 400}), 51.75, 0.5);
}

TEST(Viewport, PreservesAspectRatioForAllFits)
{
    const Extent extents[] = {{0, 0, 100, 50}, {0, 0, 10, 400}, {0, 0, 400, 10}};
    const int sizes[][2] = {{300, 100}, {100, 300}, {256, 256}};

    for (const Extent& extent : extents)
    {
        for (const auto& size : sizes)
        {
            Viewport viewport(size[0], size[1]);
            ASSERT_TRUE(viewport.FitExtent(extent, 8.0));

            // A map square stays a screen square.
            const std::optional<ScreenPoint> origin = viewport.MapToScreen({0, 0});
            const std::optional<ScreenPoint> corner = viewport.MapToScreen({5, 5});
            ASSERT_TRUE(origin.has_value());
            ASSERT_TRUE(corner.has_value());
            EXPECT_NEAR(corner->x - origin->x, origin->y - corner->y, kTolerance);

            const Extent visible = viewport.VisibleExtent();
            EXPECT_NEAR(visible.Width() / visible.Height(),
                        static_cast<double>(size[0]) / size[1], kTolerance);
            EXPECT_TRUE(IsVisible(viewport, extent));
        }
    }
}

TEST(Viewport, KeepsPaddingFreeOnEverySide)
{
    Viewport viewport(121, 71);
    ASSERT_TRUE(viewport.FitExtent({0, 0, 100, 50}, 10.0));

    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 1.0);
    ExpectScreen(viewport.MapToScreen({0, 50}), 10.5, 10.5);
    ExpectScreen(viewport.MapToScreen({100, 0}), 110.5, 60.5);
}

TEST(Viewport, CentersExtentOnTheAxisWithSpareRoom)
{
    Viewport viewport(221, 71);
    ASSERT_TRUE(viewport.FitExtent({0, 0, 100, 50}, 10.0));

    // Height constrains: 50 units over 50 available pixels. The width has
    // more than the padding to spare.
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 1.0);
    ExpectScreen(viewport.MapToScreen({0, 50}), 60.5, 10.5);
    ExpectScreen(viewport.MapToScreen({100, 0}), 160.5, 60.5);
}

TEST(Viewport, PaddingLargerThanViewportLeavesHalfAPixel)
{
    Viewport viewport(11, 11);
    ASSERT_TRUE(viewport.FitExtent({0, 0, 10, 10}, 100.0));

    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 20.0);
    ExpectMap(viewport.Center(), 5, 5);
}

TEST(Viewport, RejectsNegativeAndNonFinitePadding)
{
    Viewport viewport(100, 100);

    EXPECT_THROW((void)viewport.FitExtent({0, 0, 1, 1}, -1.0), InvalidArgument);
    EXPECT_THROW((void)viewport.FitExtent({0, 0, 1, 1}, kNaN), InvalidArgument);
    EXPECT_THROW((void)viewport.FitExtent({0, 0, 1, 1}, kInfinity), InvalidArgument);
    EXPECT_FALSE(viewport.HasView());
}

TEST(Viewport, FittedBoundaryGeometryRoundsToPixelsInsideViewport)
{
    const int sizes[][2] = {{1, 1}, {2, 2}, {7, 3}, {640, 480}, {1000, 17}};
    const Extent extent{-123.456, 7.89, 987.654, 321.0};

    for (const auto& size : sizes)
    {
        Viewport viewport(size[0], size[1]);
        ASSERT_TRUE(viewport.FitExtent(extent));

        for (const MapPoint corner : {MapPoint{extent.xMin, extent.yMin}, MapPoint{extent.xMin, extent.yMax},
                                      MapPoint{extent.xMax, extent.yMin}, MapPoint{extent.xMax, extent.yMax}})
        {
            const std::optional<ScreenPoint> screen = viewport.MapToScreen(corner);
            ASSERT_TRUE(screen.has_value());
            const std::optional<PixelPoint> pixel = ToPixel(*screen);
            ASSERT_TRUE(pixel.has_value());
            EXPECT_GE(pixel->x, 0);
            EXPECT_GE(pixel->y, 0);
            EXPECT_LT(pixel->x, size[0]);
            EXPECT_LT(pixel->y, size[1]);
        }
    }
}

TEST(Viewport, ZeroSizedViewportCannotFitOrTransform)
{
    Viewport viewport(0, 100);

    EXPECT_FALSE(viewport.FitExtent({0, 0, 10, 10}));
    EXPECT_FALSE(viewport.HasView());

    viewport.SetSize(100, 0);
    EXPECT_FALSE(viewport.FitExtent({0, 0, 10, 10}));
    EXPECT_FALSE(viewport.HasView());
}

TEST(Viewport, ZeroSizedViewportKeepsItsView)
{
    Viewport viewport = MakeFittedViewport();

    viewport.SetSize(0, 0);
    EXPECT_TRUE(viewport.HasView());
    EXPECT_FALSE(viewport.IsValid());
    EXPECT_TRUE(viewport.VisibleExtent().IsEmpty());
    EXPECT_FALSE(viewport.MapToScreen({50, 25}).has_value());

    viewport.SetSize(101, 51);
    EXPECT_TRUE(viewport.IsValid());
    ExpectScreen(viewport.MapToScreen({0, 50}), 0.5, 0.5);
}

TEST(Viewport, ResizeKeepsCenterAndResolution)
{
    Viewport viewport = MakeFittedViewport();

    viewport.SetSize(301, 11);

    ExpectMap(viewport.Center(), 50, 25);
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 1.0);
    ExpectScreen(viewport.MapToScreen({50, 25}), 150.5, 5.5);
}

TEST(Viewport, FitsHorizontalAndVerticalLines)
{
    Viewport viewport(101, 101);

    ASSERT_TRUE(viewport.FitExtent({0, 5, 100, 5}));
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 1.0);
    ExpectMap(viewport.Center(), 50, 5);

    ASSERT_TRUE(viewport.FitExtent({5, 0, 5, 200}));
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 2.0);
    ExpectMap(viewport.Center(), 5, 100);
}

TEST(Viewport, FitsSinglePointWithDefaultOrCurrentResolution)
{
    Viewport viewport(100, 100);

    ASSERT_TRUE(viewport.FitExtent({3, 4, 3, 4}));
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), Viewport::kDefaultPointResolution);
    ExpectMap(viewport.Center(), 3, 4);

    viewport.SetView({0, 0}, 0.25);
    ASSERT_TRUE(viewport.FitExtent({-7, 8, -7, 8}));
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 0.25);
    ExpectMap(viewport.Center(), -7, 8);
    ExpectScreen(viewport.MapToScreen({-7, 8}), 50, 50);
}

TEST(Viewport, RejectsInvalidExtentsWithoutChangingTheView)
{
    Viewport viewport = MakeFittedViewport();

    EXPECT_FALSE(viewport.FitExtent(Extent{}));
    EXPECT_FALSE(viewport.FitExtent({10, 0, 0, 10}));
    EXPECT_FALSE(viewport.FitExtent({kNaN, 0, 10, 10}));
    EXPECT_FALSE(viewport.FitExtent({0, 0, kInfinity, 10}));
    EXPECT_FALSE(viewport.FitExtent({-kInfinity, 0, 10, 10}));

    ExpectMap(viewport.Center(), 50, 25);
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 1.0);
}

TEST(Viewport, TransformationsRejectNonFiniteInputs)
{
    const Viewport viewport = MakeFittedViewport();

    EXPECT_FALSE(viewport.MapToScreen({kNaN, 0}).has_value());
    EXPECT_FALSE(viewport.MapToScreen({0, kInfinity}).has_value());
    EXPECT_FALSE(viewport.MapToScreen({-kInfinity, 0}).has_value());
    EXPECT_FALSE(viewport.ScreenToMap({kNaN, 0}).has_value());
    EXPECT_FALSE(viewport.ScreenToMap({0, -kInfinity}).has_value());
}

TEST(Viewport, TransformationsRejectResultsThatOverflow)
{
    Viewport viewport(100, 100);
    viewport.SetView({0, 0}, Viewport::MinResolution({0, 0}));
    EXPECT_FALSE(viewport.MapToScreen({std::numeric_limits<double>::max(), 0}).has_value());

    viewport.SetView({0, 0}, Viewport::kMaxResolution);
    EXPECT_FALSE(viewport.ScreenToMap({std::numeric_limits<double>::max(), 0}).has_value());
}

TEST(Viewport, SetViewRejectsNonFiniteAndNonPositiveValues)
{
    Viewport viewport(100, 100);

    EXPECT_THROW(viewport.SetView({kNaN, 0}, 1.0), InvalidArgument);
    EXPECT_THROW(viewport.SetView({0, kInfinity}, 1.0), InvalidArgument);
    EXPECT_THROW(viewport.SetView({0, 0}, 0.0), InvalidArgument);
    EXPECT_THROW(viewport.SetView({0, 0}, -1.0), InvalidArgument);
    EXPECT_THROW(viewport.SetView({0, 0}, kNaN), InvalidArgument);
    EXPECT_THROW(viewport.SetView({0, 0}, kInfinity), InvalidArgument);
    EXPECT_FALSE(viewport.HasView());
}

TEST(Viewport, SetViewClampsToLimits)
{
    Viewport viewport(100, 100);

    viewport.SetView({1.0e20, -1.0e20}, 1.0e-30);
    ExpectMap(viewport.Center(), Viewport::kMaxCoordinate, -Viewport::kMaxCoordinate);
    EXPECT_DOUBLE_EQ(*viewport.Resolution(),
                     Viewport::kMinRelativeResolution * Viewport::kMaxCoordinate);

    viewport.SetView({0, 0}, 1.0e-30);
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), Viewport::kMinRelativeResolution);

    viewport.SetView({0, 0}, 1.0e30);
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), Viewport::kMaxResolution);
}

TEST(Viewport, FitClampsHugeExtentsToLimits)
{
    Viewport viewport(100, 100);
    const double huge = std::numeric_limits<double>::max();

    ASSERT_TRUE(viewport.FitExtent({-huge, -huge, huge, huge}));
    ExpectMap(viewport.Center(), 0, 0);
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), Viewport::kMaxResolution);

    ASSERT_TRUE(viewport.FitExtent({1.0e15, 1.0e15, 1.0e15 + 1.0, 1.0e15 + 1.0}));
    ExpectMap(viewport.Center(), Viewport::kMaxCoordinate, Viewport::kMaxCoordinate);
    EXPECT_GE(*viewport.Resolution(), Viewport::MinResolution(*viewport.Center()));
}

TEST(Viewport, FitClampsTinyExtentsToMinimumResolution)
{
    Viewport viewport(100, 100);

    ASSERT_TRUE(viewport.FitExtent({1000, 1000, 1000 + 1.0e-12, 1000 + 1.0e-12}));
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), Viewport::MinResolution(*viewport.Center()));
    EXPECT_NEAR(*viewport.Resolution(), 1.0e-7, 1.0e-12);
    EXPECT_TRUE(IsVisible(viewport, {1000, 1000, 1000 + 1.0e-12, 1000 + 1.0e-12}));
}

TEST(Viewport, ClearViewKeepsSize)
{
    Viewport viewport = MakeFittedViewport();

    viewport.ClearView();

    EXPECT_FALSE(viewport.HasView());
    EXPECT_FALSE(viewport.IsValid());
    EXPECT_EQ(viewport.Width(), 101);
    EXPECT_EQ(viewport.Height(), 51);
}

TEST(Viewport, CopiesAreIndependentSnapshots)
{
    Viewport viewport = MakeFittedViewport();
    const Viewport snapshot = viewport;

    viewport.SetView({0, 0}, 2.0);

    ExpectMap(snapshot.Center(), 50, 25);
    EXPECT_DOUBLE_EQ(*snapshot.Resolution(), 1.0);
}

TEST(ToPixel, RoundsDownToTheContainingPixel)
{
    const std::optional<PixelPoint> inside = ToPixel({10.99, 0.0});
    ASSERT_TRUE(inside.has_value());
    EXPECT_EQ(inside->x, 10);
    EXPECT_EQ(inside->y, 0);

    const std::optional<PixelPoint> negative = ToPixel({-0.5, -1.0});
    ASSERT_TRUE(negative.has_value());
    EXPECT_EQ(negative->x, -1);
    EXPECT_EQ(negative->y, -1);
}

TEST(Viewport, PanShiftsTheVisibleExtentByTheMapDelta)
{
    Viewport viewport = MakeFittedViewport();
    const Extent before = viewport.VisibleExtent();

    ASSERT_TRUE(viewport.Pan(10.0, -5.0));

    ExpectMap(viewport.Center(), 60, 20);
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 1.0);
    ExpectExtent(viewport.VisibleExtent(),
                 {before.xMin + 10.0, before.yMin - 5.0, before.xMax + 10.0, before.yMax - 5.0});
    // Content moves the opposite way on screen: a pan east moves geometry
    // left, a pan south moves geometry up.
    ExpectScreen(viewport.MapToScreen({50, 25}), 40.5, 20.5);
}

TEST(Viewport, PanWithoutViewOrWithZeroDeltaChangesNothing)
{
    Viewport noView(100, 100);
    EXPECT_FALSE(noView.Pan(1.0, 1.0));
    EXPECT_FALSE(noView.HasView());

    Viewport viewport = MakeFittedViewport();
    EXPECT_FALSE(viewport.Pan(0.0, 0.0));
    ExpectMap(viewport.Center(), 50, 25);
}

TEST(Viewport, PanWorksOnAZeroSizedViewport)
{
    Viewport viewport = MakeFittedViewport();
    viewport.SetSize(0, 0);

    ASSERT_TRUE(viewport.Pan(-50.0, 0.0));

    ExpectMap(viewport.Center(), 0, 25);
}

TEST(Viewport, PanRejectsNonFiniteDeltasAndClampsTheCenter)
{
    Viewport viewport = MakeFittedViewport();
    EXPECT_THROW((void)viewport.Pan(kNaN, 0.0), InvalidArgument);
    EXPECT_THROW((void)viewport.Pan(0.0, kInfinity), InvalidArgument);
    ExpectMap(viewport.Center(), 50, 25);

    ASSERT_TRUE(viewport.Pan(std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()));
    EXPECT_DOUBLE_EQ(viewport.Center()->x, Viewport::kMaxCoordinate);
    EXPECT_DOUBLE_EQ(viewport.Center()->y, -Viewport::kMaxCoordinate);
    EXPECT_FALSE(viewport.Pan(1.0e6, -1.0e6));
}

TEST(Viewport, ZoomDividesTheResolutionAndKeepsTheCenter)
{
    Viewport viewport = MakeFittedViewport();

    ASSERT_TRUE(viewport.Zoom(2.0));
    ExpectMap(viewport.Center(), 50, 25);
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 0.5);
    EXPECT_NEAR(viewport.VisibleExtent().Width(), 50.5, kTolerance);

    ASSERT_TRUE(viewport.Zoom(0.25));
    ExpectMap(viewport.Center(), 50, 25);
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 2.0);
}

TEST(Viewport, ZoomInThenOutRestoresTheView)
{
    Viewport viewport = MakeFittedViewport();
    const Extent before = viewport.VisibleExtent();

    ASSERT_TRUE(viewport.Zoom(4.0));
    ASSERT_TRUE(viewport.Zoom(0.25));

    ExpectExtent(viewport.VisibleExtent(), before);
}

TEST(Viewport, ZoomRejectsInvalidFactors)
{
    Viewport viewport = MakeFittedViewport();
    for (const double factor : {0.0, -2.0, kNaN, kInfinity})
    {
        EXPECT_THROW((void)viewport.Zoom(factor), InvalidArgument) << factor;
        EXPECT_THROW((void)viewport.ZoomOut(factor), InvalidArgument) << factor;
        EXPECT_THROW((void)viewport.ZoomAt({0, 0}, factor), InvalidArgument) << factor;
    }
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 1.0);
}

TEST(Viewport, ZoomStopsAtTheResolutionLimits)
{
    Viewport viewport = MakeFittedViewport();
    const MapPoint center = *viewport.Center();

    ASSERT_TRUE(viewport.Zoom(1.0e300));
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), Viewport::MinResolution(center));
    EXPECT_FALSE(viewport.Zoom(2.0));

    ASSERT_TRUE(viewport.Zoom(1.0e-300));
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), Viewport::kMaxResolution);
    EXPECT_FALSE(viewport.Zoom(0.5));
    ExpectMap(viewport.Center(), center.x, center.y);
}

TEST(Viewport, ZoomWithoutViewChangesNothing)
{
    Viewport viewport(100, 100);
    EXPECT_FALSE(viewport.Zoom(2.0));
    EXPECT_FALSE(viewport.ZoomOut(2.0));
    EXPECT_FALSE(viewport.ZoomAt({10, 10}, 2.0));
    EXPECT_FALSE(viewport.HasView());
}

TEST(Viewport, ZoomOutMultipliesTheResolutionAndClampsOverflow)
{
    Viewport viewport(100, 100);
    viewport.SetView({5.0, 10.0}, 0.3);

    ASSERT_TRUE(viewport.ZoomOut(3.0));
    EXPECT_EQ(*viewport.Resolution(), 0.3 * 3.0);
    ExpectMap(viewport.Center(), 5.0, 10.0);

    ASSERT_TRUE(viewport.ZoomOut(0.5));
    EXPECT_EQ(*viewport.Resolution(), 0.3 * 3.0 * 0.5);
    ASSERT_TRUE(viewport.ZoomOut(std::numeric_limits<double>::max()));
    EXPECT_EQ(*viewport.Resolution(), Viewport::kMaxResolution);
    EXPECT_FALSE(viewport.ZoomOut(2.0));

    ASSERT_TRUE(viewport.ZoomOut(1.0e-300));
    EXPECT_EQ(*viewport.Resolution(), Viewport::MinResolution(*viewport.Center()));
    EXPECT_FALSE(viewport.ZoomOut(0.5));
}

TEST(Viewport, ZoomAtKeepsTheMapCoordinateUnderTheAnchor)
{
    const ScreenPoint anchors[] = {{0.0, 0.0}, {100.25, 3.75}, {50.5, 25.5}, {-30.0, 200.0}};
    for (const ScreenPoint& anchor : anchors)
    {
        for (const double factor : {2.0, 0.5, 3.3})
        {
            Viewport viewport = MakeFittedViewport();
            const std::optional<MapPoint> before = viewport.ScreenToMap(anchor);
            ASSERT_TRUE(before.has_value());

            ASSERT_TRUE(viewport.ZoomAt(anchor, factor));

            EXPECT_NEAR(*viewport.Resolution(), 1.0 / factor, kTolerance);
            ExpectMap(viewport.ScreenToMap(anchor), before->x, before->y);
            ExpectScreen(viewport.MapToScreen(*before), anchor.x, anchor.y);
        }
    }
}

TEST(Viewport, ZoomAtTheCenterMatchesCenterZoom)
{
    Viewport anchored = MakeFittedViewport();
    Viewport centered = MakeFittedViewport();

    ASSERT_TRUE(anchored.ZoomAt({50.5, 25.5}, 2.0));
    ASSERT_TRUE(centered.Zoom(2.0));

    ExpectExtent(anchored.VisibleExtent(), centered.VisibleExtent());
}

TEST(Viewport, ZoomAtNeedsAValidViewportAndAFiniteAnchor)
{
    Viewport viewport = MakeFittedViewport();
    EXPECT_THROW((void)viewport.ZoomAt({kNaN, 0}, 2.0), InvalidArgument);
    EXPECT_THROW((void)viewport.ZoomAt({0, -kInfinity}, 2.0), InvalidArgument);

    viewport.SetSize(0, 10);
    EXPECT_FALSE(viewport.ZoomAt({0, 0}, 2.0));
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), 1.0);
}

TEST(Viewport, ZoomAtStopsAtTheResolutionLimit)
{
    Viewport viewport = MakeFittedViewport();
    ASSERT_TRUE(viewport.ZoomAt({0, 0}, 1.0e300));
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), Viewport::MinResolution(*viewport.Center()));
    EXPECT_FALSE(viewport.ZoomAt({0, 0}, 2.0));
}

TEST(Viewport, ZoomAtCanMoveTheAnchorWhenTheNewCenterRaisesTheMinimumResolution)
{
    Viewport viewport(Viewport::kMaxDimension, 100);
    viewport.SetView({Viewport::kMaxCoordinate - 1.0e8, 0.0}, 200.0);
    const ScreenPoint anchor{static_cast<double>(Viewport::kMaxDimension), 50.0};
    const MapPoint before = *viewport.ScreenToMap(anchor);
    const double oldMinimum = Viewport::MinResolution(*viewport.Center());

    ASSERT_TRUE(viewport.ZoomAt(anchor, 3.0));

    EXPECT_LT(viewport.Center()->x, Viewport::kMaxCoordinate);
    EXPECT_GT(*viewport.Resolution(), oldMinimum);
    EXPECT_EQ(*viewport.Resolution(), Viewport::MinResolution(*viewport.Center()));
    const MapPoint after = *viewport.ScreenToMap(anchor);
    EXPECT_GT(after.x - before.x, 1.0);
    EXPECT_EQ(after.y, before.y);
}

TEST(ToPixel, RejectsNonFiniteAndOutOfRangeCoordinates)
{
    EXPECT_FALSE(ToPixel({kNaN, 0}).has_value());
    EXPECT_FALSE(ToPixel({0, kInfinity}).has_value());
    EXPECT_FALSE(ToPixel({1.0e20, 0}).has_value());
    EXPECT_FALSE(ToPixel({0, -1.0e20}).has_value());
}
