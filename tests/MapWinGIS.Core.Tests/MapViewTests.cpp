// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapViewTests.cpp
 * @brief Tests the map view: shared viewport, scene invalidation, resizing,
 *        and programmatic navigation.
 *
 * Verifies that layers with different extents are shown through one
 * viewport, that map changes rebuild the render scene and report a redraw,
 * that resizing keeps the view stable, and that zoom-to-map, zoom-to-layer,
 * zoom in and out, anchored zoom, and pan work without a window or ActiveX.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/map/MapView.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    constexpr double kTolerance = 1e-9;

    /// True when the extent lies within the viewport's visible extent.
    bool IsVisible(const Viewport& viewport, const Extent& extent)
    {
        const Extent visible = viewport.VisibleExtent();
        return visible.xMin <= extent.xMin && visible.yMin <= extent.yMin &&
               visible.xMax >= extent.xMax && visible.yMax >= extent.yMax;
    }

    class MapViewTest : public testing::Test
    {
    protected:
        std::shared_ptr<VectorDataset> OpenRectangles(const std::vector<Extent>& rectangles)
        {
            const std::string fileName = "layer-" + std::to_string(_datasetCount++) + ".gpkg";
            return VectorDataset::Open(
                MakeRectanglesDataset(_directory.Path(), fileName, rectangles));
        }

        TempDirectory _directory;
        MapView _view;
        int _datasetCount = 0;
    };
}

TEST_F(MapViewTest, NewViewHasNoLayersSizeOrView)
{
    EXPECT_EQ(_view.GetMap().LayerCount(), 0);
    EXPECT_FALSE(_view.GetViewport().HasView());
    EXPECT_EQ(_view.GetViewport().Width(), 0);
    EXPECT_TRUE(_view.Scene().layers.empty());
    EXPECT_FALSE(_view.FitToMap());
}

TEST_F(MapViewTest, FirstLayerFitsTheViewWhenTheViewportHasASize)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));

    const Viewport& viewport = _view.GetViewport();
    ASSERT_TRUE(viewport.IsValid());
    EXPECT_NEAR(viewport.Center()->x, 5.0, kTolerance);
    EXPECT_NEAR(viewport.Center()->y, 2.5, kTolerance);
    EXPECT_TRUE(IsVisible(viewport, {0.0, 0.0, 10.0, 5.0}));
}

TEST_F(MapViewTest, LaterLayersKeepTheViewUntilFitToMap)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    const MapPoint center = *_view.GetViewport().Center();
    const double resolution = *_view.GetViewport().Resolution();

    _view.AddLayer(OpenRectangles({{90.0, 40.0, 100.0, 50.0}}));

    EXPECT_DOUBLE_EQ(_view.GetViewport().Center()->x, center.x);
    EXPECT_DOUBLE_EQ(_view.GetViewport().Center()->y, center.y);
    EXPECT_DOUBLE_EQ(*_view.GetViewport().Resolution(), resolution);

    ASSERT_TRUE(_view.FitToMap());
    EXPECT_TRUE(IsVisible(_view.GetViewport(), {0.0, 0.0, 100.0, 50.0}));
}

TEST_F(MapViewTest, FitToMapShowsLayersWithDifferentExtentsInOneViewport)
{
    ASSERT_TRUE(_view.Resize(400, 300));
    const Extent small{-1.0, -1.0, 1.0, 1.0};
    const Extent large{500.0, 200.0, 1500.0, 900.0};
    _view.AddLayer(OpenRectangles({small}));
    _view.AddLayer(OpenRectangles({large}));

    ASSERT_TRUE(_view.FitToMap(MapView::kDefaultPadding));

    const Viewport& viewport = _view.GetViewport();
    EXPECT_TRUE(IsVisible(viewport, small));
    EXPECT_TRUE(IsVisible(viewport, large));
    // Both layers map through the same transformation: the screen distance
    // between them equals their map distance divided by the resolution.
    const auto first = viewport.MapToScreen({small.xMin, small.yMin});
    const auto second = viewport.MapToScreen({large.xMax, large.yMax});
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NEAR(second->x - first->x, (large.xMax - small.xMin) / *viewport.Resolution(), 1e-6);
    EXPECT_NEAR(first->y - second->y, (large.yMax - small.yMin) / *viewport.Resolution(), 1e-6);
}

TEST_F(MapViewTest, FitToMapIgnoresHiddenLayers)
{
    ASSERT_TRUE(_view.Resize(200, 200));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 10.0}}));
    const LayerId distant = _view.AddLayer(OpenRectangles({{1000.0, 1000.0, 1010.0, 1010.0}}));
    ASSERT_TRUE(_view.SetLayerVisible(distant, false));

    ASSERT_TRUE(_view.FitToMap());

    EXPECT_NEAR(_view.GetViewport().Center()->x, 5.0, kTolerance);
    EXPECT_FALSE(IsVisible(_view.GetViewport(), {1000.0, 1000.0, 1010.0, 1010.0}));
}

TEST_F(MapViewTest, FitToMapRejectsInvalidPadding)
{
    EXPECT_THROW((void)_view.FitToMap(-1.0), InvalidArgument);
}

TEST_F(MapViewTest, LayerAddedBeforeTheViewportHasASizeIsFittedOnFirstResize)
{
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    EXPECT_FALSE(_view.GetViewport().HasView());

    ASSERT_TRUE(_view.Resize(200, 100));

    ASSERT_TRUE(_view.GetViewport().IsValid());
    EXPECT_TRUE(IsVisible(_view.GetViewport(), {0.0, 0.0, 10.0, 5.0}));
}

TEST_F(MapViewTest, FitRequestedWhileMinimizedHappensOnRestore)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    _view.AddLayer(OpenRectangles({{100.0, 0.0, 110.0, 5.0}}));
    ASSERT_TRUE(_view.Resize(0, 0));

    EXPECT_FALSE(_view.FitToMap());
    ASSERT_TRUE(_view.Resize(200, 100));

    EXPECT_TRUE(IsVisible(_view.GetViewport(), {0.0, 0.0, 110.0, 5.0}));
}

TEST_F(MapViewTest, ResizeKeepsTheCenterAndScale)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    const MapPoint center = *_view.GetViewport().Center();
    const double resolution = *_view.GetViewport().Resolution();

    ASSERT_TRUE(_view.Resize(640, 80));

    EXPECT_EQ(_view.GetViewport().Width(), 640);
    EXPECT_EQ(_view.GetViewport().Height(), 80);
    EXPECT_DOUBLE_EQ(_view.GetViewport().Center()->x, center.x);
    EXPECT_DOUBLE_EQ(_view.GetViewport().Center()->y, center.y);
    EXPECT_DOUBLE_EQ(*_view.GetViewport().Resolution(), resolution);
    EXPECT_FALSE(_view.Resize(640, 80));
}

TEST_F(MapViewTest, ResizeHandlesTinyAndElongatedSizes)
{
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    const int sizes[][2] = {{1, 1}, {Viewport::kMaxDimension, 1}, {1, Viewport::kMaxDimension}, {3, 2}};
    for (const auto& size : sizes)
    {
        ASSERT_NO_THROW((void)_view.Resize(size[0], size[1]));
        ASSERT_TRUE(_view.FitToMap()) << size[0] << " x " << size[1];
        const Viewport& viewport = _view.GetViewport();
        EXPECT_TRUE(viewport.IsValid());
        // The resolution is the same on both axes, so the map is not distorted.
        const Extent visible = viewport.VisibleExtent();
        EXPECT_NEAR(visible.Width() / visible.Height(),
                    static_cast<double>(size[0]) / static_cast<double>(size[1]),
                    1e-9 * static_cast<double>(Viewport::kMaxDimension));
        EXPECT_TRUE(IsVisible(viewport, {0.0, 0.0, 10.0, 5.0}));
    }
    EXPECT_THROW((void)_view.Resize(Viewport::kMaxDimension + 1, 10), InvalidArgument);
}

TEST_F(MapViewTest, DegenerateExtentsCompleteTheInitialFit)
{
    const Extent extents[] = {
        {4.0, 2.0, 4.0, 8.0}, {2.0, 4.0, 8.0, 4.0}, {4.0, 4.0, 4.0, 4.0}};
    for (const Extent& extent : extents)
    {
        MapView view;
        view.AddLayer(OpenRectangles({extent}));
        EXPECT_FALSE(view.GetViewport().HasView());
        ASSERT_TRUE(view.Resize(200, 100));
        ASSERT_TRUE(view.GetViewport().IsValid());
        EXPECT_TRUE(IsVisible(view.GetViewport(), extent));
        const double resolution = *view.GetViewport().Resolution();
        EXPECT_FALSE(view.Resize(200, 100)) << "the initial fit must not remain pending";
        EXPECT_DOUBLE_EQ(*view.GetViewport().Resolution(), resolution);
        if (extent.Width() == 0.0 && extent.Height() == 0.0)
        {
            EXPECT_DOUBLE_EQ(resolution, Viewport::kDefaultPointResolution);
            ASSERT_TRUE(view.Resize(400, 200));
            ASSERT_TRUE(view.FitToMap());
            EXPECT_DOUBLE_EQ(*view.GetViewport().Resolution(), resolution);
        }
    }
}

TEST_F(MapViewTest, MapChangesRebuildTheSceneAndReportARedraw)
{
    const LayerId first = _view.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));
    ASSERT_EQ(_view.Scene().layers.size(), 1U);

    const LayerId second = _view.AddLayer(OpenRectangles({{2.0, 2.0, 3.0, 3.0}}));
    ASSERT_EQ(_view.Scene().layers.size(), 2U);
    EXPECT_EQ(_view.Scene().layers[1].layerId, second);

    EXPECT_TRUE(_view.SetLayerVisible(first, false));
    EXPECT_FALSE(_view.SetLayerVisible(first, false));
    ASSERT_EQ(_view.Scene().layers.size(), 1U);
    EXPECT_EQ(_view.Scene().layers[0].layerId, second);

    EXPECT_TRUE(_view.SetLayerVisible(first, true));
    ASSERT_EQ(_view.Scene().layers.size(), 2U);
    EXPECT_EQ(_view.Scene().layers[0].layerId, first);

    EXPECT_TRUE(_view.RemoveLayer(first));
    EXPECT_FALSE(_view.RemoveLayer(first));
    ASSERT_EQ(_view.Scene().layers.size(), 1U);

    EXPECT_TRUE(_view.ClearLayers());
    EXPECT_FALSE(_view.ClearLayers());
    EXPECT_TRUE(_view.Scene().layers.empty());
    EXPECT_FALSE(_view.SetLayerVisible(second, true));
}

TEST_F(MapViewTest, MovingLayerChangesSceneOrderAndRequestsRedraw)
{
    const LayerId bottom = _view.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));
    const LayerId top = _view.AddLayer(OpenRectangles({{2.0, 2.0, 3.0, 3.0}}));
    ASSERT_EQ(_view.Scene().layers.size(), 2U);

    ASSERT_TRUE(_view.MoveLayer(bottom, 1));
    const auto& scene = _view.Scene();

    ASSERT_EQ(scene.layers.size(), 2U);
    EXPECT_EQ(scene.layers[0].layerId, top);
    EXPECT_EQ(scene.layers[1].layerId, bottom);
    EXPECT_FALSE(_view.MoveLayer(bottom, 1));
    EXPECT_FALSE(_view.MoveLayer(LayerId{99}, 0));
    EXPECT_THROW((void)_view.MoveLayer(bottom, 2), InvalidArgument);
}

TEST_F(MapViewTest, SceneIsReusedUntilTheMapChanges)
{
    const LayerId first = _view.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));
    const rendering::MapRenderScene& scene = _view.Scene();
    const rendering::Polygon* polygons = scene.layers.front().scene.polygons.data();

    ASSERT_TRUE(_view.Resize(300, 200));
    EXPECT_EQ(_view.Scene().layers.front().scene.polygons.data(), polygons);

    _view.AddLayer(OpenRectangles({{2.0, 2.0, 3.0, 3.0}}));
    EXPECT_EQ(_view.Scene().layers.front().scene.polygons.data(), polygons);

    ASSERT_TRUE(_view.SetLayerVisible(first, false));
    ASSERT_TRUE(_view.SetLayerVisible(first, true));
    EXPECT_EQ(_view.Scene().layers.front().scene.polygons.data(), polygons);

    EXPECT_TRUE(_view.InvalidateScene());
    EXPECT_EQ(&_view.Scene(), &scene);
    EXPECT_EQ(scene.layers.size(), 2U);
}

TEST_F(MapViewTest, RasterSceneTracksTheViewAndUsesViewportPixelDimensions)
{
    const std::shared_ptr<RasterDataset> raster =
        RasterDataset::Open(MakeRaster(_directory.Path()));
    ASSERT_TRUE(_view.Resize(60, 40));
    _view.AddRasterLayer(raster);

    const RasterImage first = *_view.Scene().layers.front().raster;
    ASSERT_TRUE(_view.ZoomIn());
    const RasterImage zoomed = *_view.Scene().layers.front().raster;

    EXPECT_EQ(first.width, 60);
    EXPECT_EQ(first.height, 40);
    EXPECT_EQ(zoomed.width, 60);
    EXPECT_EQ(zoomed.height, 40);
    EXPECT_LT(zoomed.extent.Width(), first.extent.Width());
    EXPECT_LT(zoomed.extent.Height(), first.extent.Height());
}

TEST_F(MapViewTest, ClearingLayersKeepsTheView)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    const MapPoint center = *_view.GetViewport().Center();

    ASSERT_TRUE(_view.ClearLayers());

    ASSERT_TRUE(_view.GetViewport().HasView());
    EXPECT_DOUBLE_EQ(_view.GetViewport().Center()->x, center.x);
    EXPECT_FALSE(_view.FitToMap());
}

TEST_F(MapViewTest, ZoomToMapWithOneLayerCentersAndFitsIt)
{
    ASSERT_TRUE(_view.Resize(201, 101));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    ASSERT_TRUE(_view.ZoomIn(8.0));
    ASSERT_TRUE(_view.Pan(100.0, 100.0));

    ASSERT_TRUE(_view.FitToMap(0.0));

    const Viewport& viewport = _view.GetViewport();
    EXPECT_NEAR(viewport.Center()->x, 5.0, kTolerance);
    EXPECT_NEAR(viewport.Center()->y, 2.5, kTolerance);
    // 10 x 5 map units over 200 x 100 pixel-center spans: both axes fit exactly.
    EXPECT_NEAR(*viewport.Resolution(), 0.05, kTolerance);
}

TEST_F(MapViewTest, ZoomToMapWithMultipleLayersShowsAllOfThem)
{
    ASSERT_TRUE(_view.Resize(300, 300));
    const Extent first{0.0, 0.0, 10.0, 10.0};
    const Extent second{-50.0, 20.0, -40.0, 30.0};
    const Extent third{200.0, -100.0, 210.0, -90.0};
    _view.AddLayer(OpenRectangles({first}));
    _view.AddLayer(OpenRectangles({second}));
    _view.AddLayer(OpenRectangles({third}));

    ASSERT_TRUE(_view.FitToMap());

    const Viewport& viewport = _view.GetViewport();
    for (const Extent& extent : {first, second, third})
    {
        EXPECT_TRUE(IsVisible(viewport, extent));
    }
    EXPECT_NEAR(viewport.Center()->x, 80.0, kTolerance);
    EXPECT_NEAR(viewport.Center()->y, -35.0, kTolerance);
}

TEST_F(MapViewTest, ZoomToMapOnAnEmptyMapKeepsTheView)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    EXPECT_FALSE(_view.FitToMap());
    EXPECT_FALSE(_view.GetViewport().HasView());

    const LayerId id = _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    ASSERT_TRUE(_view.SetLayerVisible(id, false));
    const MapPoint center = *_view.GetViewport().Center();

    // Only hidden layers: no visible extent, so the view stays.
    EXPECT_FALSE(_view.FitToMap());
    EXPECT_DOUBLE_EQ(_view.GetViewport().Center()->x, center.x);
}

TEST_F(MapViewTest, ZoomToLayerFitsLargeAndSmallLayers)
{
    ASSERT_TRUE(_view.Resize(400, 200));
    const Extent large{-1000.0, -500.0, 1000.0, 500.0};
    const Extent small{10.0, 10.0, 10.5, 10.25};
    const LayerId largeId = _view.AddLayer(OpenRectangles({large}));
    const LayerId smallId = _view.AddLayer(OpenRectangles({small}));

    ASSERT_TRUE(_view.ZoomToLayer(smallId, 0.0));
    const Viewport& viewport = _view.GetViewport();
    EXPECT_NEAR(viewport.Center()->x, 10.25, kTolerance);
    EXPECT_NEAR(viewport.Center()->y, 10.125, kTolerance);
    EXPECT_TRUE(IsVisible(viewport, small));
    EXPECT_FALSE(IsVisible(viewport, large));
    const double smallResolution = *viewport.Resolution();

    ASSERT_TRUE(_view.ZoomToLayer(largeId, 0.0));
    EXPECT_NEAR(viewport.Center()->x, 0.0, kTolerance);
    EXPECT_NEAR(viewport.Center()->y, 0.0, kTolerance);
    EXPECT_TRUE(IsVisible(viewport, large));
    EXPECT_NEAR(*viewport.Resolution() / smallResolution, 4000.0, 1e-6);
}

TEST_F(MapViewTest, ZoomToLayerAppliesPaddingAndFitsHiddenLayers)
{
    ASSERT_TRUE(_view.Resize(201, 201));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 10.0}}));
    const LayerId hidden = _view.AddLayer(OpenRectangles({{100.0, 100.0, 120.0, 120.0}}));
    ASSERT_TRUE(_view.SetLayerVisible(hidden, false));

    ASSERT_TRUE(_view.ZoomToLayer(hidden, 50.0));

    const Viewport& viewport = _view.GetViewport();
    EXPECT_NEAR(viewport.Center()->x, 110.0, kTolerance);
    // 20 map units over 200 - 2 * 50 pixels between the outer pixel centers.
    EXPECT_NEAR(*viewport.Resolution(), 0.2, kTolerance);
    EXPECT_THROW((void)_view.ZoomToLayer(hidden, -1.0), InvalidArgument);
}

TEST_F(MapViewTest, ZoomToMissingOrEmptyLayerKeepsTheView)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    const LayerId id = _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    const LayerId empty = _view.AddLayer(OpenRectangles({}));
    const MapPoint center = *_view.GetViewport().Center();
    const double resolution = *_view.GetViewport().Resolution();

    EXPECT_FALSE(_view.ZoomToLayer(empty));
    ASSERT_TRUE(_view.RemoveLayer(id));
    EXPECT_FALSE(_view.ZoomToLayer(id));
    EXPECT_FALSE(_view.ZoomToLayer(LayerId{}));

    EXPECT_DOUBLE_EQ(_view.GetViewport().Center()->x, center.x);
    EXPECT_DOUBLE_EQ(_view.GetViewport().Center()->y, center.y);
    EXPECT_DOUBLE_EQ(*_view.GetViewport().Resolution(), resolution);
}

TEST_F(MapViewTest, ZoomToLayerBeforeTheViewportHasASizeHappensOnFirstResize)
{
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 100.0, 100.0}}));
    const LayerId target = _view.AddLayer(OpenRectangles({{50.0, 50.0, 51.0, 51.0}}));

    EXPECT_FALSE(_view.FitToMap(20.0));
    EXPECT_FALSE(_view.ZoomToLayer(target, 0.0));
    ASSERT_TRUE(_view.Resize(101, 101));

    EXPECT_NEAR(_view.GetViewport().Center()->x, 50.5, kTolerance);
    EXPECT_NEAR(*_view.GetViewport().Resolution(), 0.01, kTolerance);
}

TEST_F(MapViewTest, FitToMapReplacesAPendingZoomToLayer)
{
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 100.0, 100.0}}));
    const LayerId target = _view.AddLayer(OpenRectangles({{50.0, 50.0, 51.0, 51.0}}));
    EXPECT_FALSE(_view.ZoomToLayer(target, 20.0));
    EXPECT_FALSE(_view.FitToMap(0.0));

    ASSERT_TRUE(_view.Resize(101, 101));

    EXPECT_NEAR(_view.GetViewport().Center()->x, 50.0, kTolerance);
    EXPECT_NEAR(_view.GetViewport().Center()->y, 50.0, kTolerance);
    EXPECT_NEAR(*_view.GetViewport().Resolution(), 1.0, kTolerance);
}

TEST_F(MapViewTest, ZoomToMissingLayerCancelsAPendingMapFit)
{
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 10.0}}));
    const LayerId removed = _view.AddLayer(OpenRectangles({{50.0, 50.0, 51.0, 51.0}}));
    ASSERT_TRUE(_view.RemoveLayer(removed));
    EXPECT_FALSE(_view.FitToMap());
    EXPECT_FALSE(_view.ZoomToLayer(removed));

    ASSERT_TRUE(_view.Resize(200, 100));

    EXPECT_FALSE(_view.GetViewport().HasView());
    EXPECT_FALSE(_view.Resize(200, 100));
}

TEST_F(MapViewTest, ZoomToEmptyLayerCancelsAPendingMapFit)
{
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 10.0}}));
    const LayerId empty = _view.AddLayer(OpenRectangles({}));
    EXPECT_FALSE(_view.FitToMap());
    EXPECT_FALSE(_view.ZoomToLayer(empty));

    ASSERT_TRUE(_view.Resize(200, 100));

    EXPECT_FALSE(_view.GetViewport().HasView());
}

TEST_F(MapViewTest, PendingZoomToRemovedLayerIsDropped)
{
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 10.0}}));
    const LayerId target = _view.AddLayer(OpenRectangles({{50.0, 50.0, 51.0, 51.0}}));
    EXPECT_FALSE(_view.ZoomToLayer(target));
    ASSERT_TRUE(_view.RemoveLayer(target));

    ASSERT_TRUE(_view.Resize(200, 100));

    // The removed layer cannot be fitted, and no fit stays pending.
    EXPECT_FALSE(_view.GetViewport().HasView());
    ASSERT_TRUE(_view.Resize(0, 0));
    EXPECT_TRUE(_view.Resize(200, 100));
    EXPECT_FALSE(_view.GetViewport().HasView());
}

TEST_F(MapViewTest, ZoomInAndOutUseTheFactorAndKeepTheCenter)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    const Viewport& viewport = _view.GetViewport();
    const MapPoint center = *viewport.Center();
    const double resolution = *viewport.Resolution();

    ASSERT_TRUE(_view.ZoomIn());
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), resolution / MapView::kDefaultZoomFactor);
    ASSERT_TRUE(_view.ZoomIn(4.0));
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), resolution / 8.0);
    ASSERT_TRUE(_view.ZoomOut(1.5));
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), resolution / 8.0 * 1.5);
    ASSERT_TRUE(_view.ZoomOut());
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), resolution / 8.0 * 1.5 * 2.0);

    EXPECT_DOUBLE_EQ(viewport.Center()->x, center.x);
    EXPECT_DOUBLE_EQ(viewport.Center()->y, center.y);
}

TEST_F(MapViewTest, ZoomInAndOutRejectFactorsThatDoNotZoom)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    const double resolution = *_view.GetViewport().Resolution();

    for (const double factor : {1.0, 0.5, 0.0, -2.0, std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity()})
    {
        EXPECT_THROW((void)_view.ZoomIn(factor), InvalidArgument) << factor;
        EXPECT_THROW((void)_view.ZoomOut(factor), InvalidArgument) << factor;
    }
    EXPECT_DOUBLE_EQ(*_view.GetViewport().Resolution(), resolution);
}

TEST_F(MapViewTest, ZoomOutMultipliesWithoutTakingAReciprocal)
{
    ASSERT_TRUE(_view.Resize(101, 101));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 30.0, 30.0}}));
    ASSERT_TRUE(_view.FitToMap(0.0));
    const double resolution = *_view.GetViewport().Resolution();

    ASSERT_TRUE(_view.ZoomOut(3.0));

    EXPECT_EQ(*_view.GetViewport().Resolution(), resolution * 3.0);
}

TEST_F(MapViewTest, ZoomInStopsAtTheLimitAndReportsNoRedraw)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));

    int steps = 0;
    while (_view.ZoomIn(1.0e6))
    {
        ASSERT_LT(++steps, 100);
    }
    EXPECT_DOUBLE_EQ(*_view.GetViewport().Resolution(),
                     Viewport::MinResolution(*_view.GetViewport().Center()));
    EXPECT_FALSE(_view.ZoomIn());
    EXPECT_TRUE(_view.ZoomOut());
}

TEST_F(MapViewTest, NavigationWithoutAViewReportsNoChange)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    EXPECT_FALSE(_view.ZoomIn());
    EXPECT_FALSE(_view.ZoomOut());
    EXPECT_FALSE(_view.ZoomAt({10.0, 10.0}, 2.0));
    EXPECT_FALSE(_view.Pan(1.0, 1.0));
    EXPECT_FALSE(_view.GetViewport().HasView());
}

TEST_F(MapViewTest, ZoomAtKeepsTheMapCoordinateUnderTheCursor)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    const Viewport& viewport = _view.GetViewport();
    const ScreenPoint cursor{37.5, 81.25};
    const MapPoint before = *viewport.ScreenToMap(cursor);
    const double resolution = *viewport.Resolution();

    ASSERT_TRUE(_view.ZoomAt(cursor, 2.0));
    EXPECT_NEAR(*viewport.Resolution(), resolution / 2.0, kTolerance);
    EXPECT_NEAR(viewport.ScreenToMap(cursor)->x, before.x, kTolerance);
    EXPECT_NEAR(viewport.ScreenToMap(cursor)->y, before.y, kTolerance);

    ASSERT_TRUE(_view.ZoomAt(cursor, 0.25));
    EXPECT_NEAR(*viewport.Resolution(), resolution * 2.0, kTolerance);
    EXPECT_NEAR(viewport.ScreenToMap(cursor)->x, before.x, kTolerance);
    EXPECT_NEAR(viewport.ScreenToMap(cursor)->y, before.y, kTolerance);
}

TEST_F(MapViewTest, PanMovesTheViewByTheMapDelta)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    const Viewport& viewport = _view.GetViewport();
    const Extent before = viewport.VisibleExtent();
    const ScreenPoint origin = *viewport.MapToScreen({0.0, 0.0});
    const double resolution = *viewport.Resolution();

    ASSERT_TRUE(_view.Pan(-3.0, 2.0));

    const Extent after = viewport.VisibleExtent();
    EXPECT_NEAR(after.xMin, before.xMin - 3.0, kTolerance);
    EXPECT_NEAR(after.yMax, before.yMax + 2.0, kTolerance);
    EXPECT_DOUBLE_EQ(*viewport.Resolution(), resolution);
    // Panning west moves geometry right; panning north moves it down.
    const ScreenPoint moved = *viewport.MapToScreen({0.0, 0.0});
    EXPECT_NEAR(moved.x - origin.x, 3.0 / resolution, 1e-6);
    EXPECT_NEAR(moved.y - origin.y, 2.0 / resolution, 1e-6);
    EXPECT_FALSE(_view.Pan(0.0, 0.0));
    EXPECT_THROW((void)_view.Pan(std::numeric_limits<double>::quiet_NaN(), 0.0), InvalidArgument);
}

TEST_F(MapViewTest, NavigationHistoryRestoresViewsAndDiscardsForwardHistoryOnNewNavigation)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    const MapPoint initialCenter = *_view.GetViewport().Center();
    const double initialResolution = *_view.GetViewport().Resolution();
    EXPECT_FALSE(_view.CanGoBack());
    EXPECT_FALSE(_view.CanGoForward());

    ASSERT_TRUE(_view.ZoomIn(2.0));
    const MapPoint zoomedCenter = *_view.GetViewport().Center();
    const double zoomedResolution = *_view.GetViewport().Resolution();
    ASSERT_TRUE(_view.Pan(3.0, -2.0));

    ASSERT_TRUE(_view.CanGoBack());
    EXPECT_FALSE(_view.CanGoForward());
    ASSERT_TRUE(_view.GoBack());
    EXPECT_EQ(_view.GetViewport().Center()->x, zoomedCenter.x);
    EXPECT_EQ(_view.GetViewport().Center()->y, zoomedCenter.y);
    EXPECT_EQ(*_view.GetViewport().Resolution(), zoomedResolution);
    ASSERT_TRUE(_view.GoBack());
    EXPECT_EQ(_view.GetViewport().Center()->x, initialCenter.x);
    EXPECT_EQ(_view.GetViewport().Center()->y, initialCenter.y);
    EXPECT_EQ(*_view.GetViewport().Resolution(), initialResolution);
    EXPECT_FALSE(_view.GoBack());

    ASSERT_TRUE(_view.CanGoForward());
    ASSERT_TRUE(_view.GoForward());
    EXPECT_EQ(_view.GetViewport().Center()->x, zoomedCenter.x);
    EXPECT_EQ(*_view.GetViewport().Resolution(), zoomedResolution);
    ASSERT_TRUE(_view.Pan(1.0, 0.0));
    EXPECT_FALSE(_view.CanGoForward());
}

TEST_F(MapViewTest, NavigationGestureAddsOnlyOneHistoryEntry)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    const MapPoint startCenter = *_view.GetViewport().Center();

    ASSERT_TRUE(_view.BeginNavigationGesture());
    EXPECT_FALSE(_view.BeginNavigationGesture());
    ASSERT_TRUE(_view.Pan(2.0, 1.0));
    ASSERT_TRUE(_view.Pan(3.0, 2.0));
    EXPECT_FALSE(_view.CanGoBack());
    ASSERT_TRUE(_view.EndNavigationGesture());
    EXPECT_TRUE(_view.CanGoBack());
    EXPECT_FALSE(_view.EndNavigationGesture());
    ASSERT_TRUE(_view.GoBack());
    EXPECT_EQ(_view.GetViewport().Center()->x, startCenter.x);
    EXPECT_EQ(_view.GetViewport().Center()->y, startCenter.y);
    EXPECT_FALSE(_view.CanGoBack());
}

TEST_F(MapViewTest, AnimatedFitAdvancesSmoothlyAndAddsOneHistoryEntry)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    ASSERT_TRUE(_view.Pan(100.0, 50.0));
    ASSERT_TRUE(_view.ZoomIn(2.0));
    const MapPoint startCenter = *_view.GetViewport().Center();
    const double startResolution = *_view.GetViewport().Resolution();

    ASSERT_TRUE(_view.BeginTransitionToMap(std::chrono::milliseconds(100)));
    EXPECT_TRUE(_view.IsTransitioning());
    EXPECT_TRUE(_view.CanGoBack());
    EXPECT_FALSE(_view.AdvanceTransition(std::chrono::milliseconds(0)));
    ASSERT_TRUE(_view.AdvanceTransition(std::chrono::milliseconds(50)));
    EXPECT_TRUE(_view.IsTransitioning());
    EXPECT_NE(_view.GetViewport().Center()->x, startCenter.x);
    EXPECT_NE(*_view.GetViewport().Resolution(), startResolution);
    ASSERT_TRUE(_view.AdvanceTransition(std::chrono::milliseconds(50)));
    EXPECT_FALSE(_view.IsTransitioning());
    EXPECT_TRUE(IsVisible(_view.GetViewport(), {0.0, 0.0, 10.0, 5.0}));

    ASSERT_TRUE(_view.CanGoBack());
    ASSERT_TRUE(_view.GoBack());
    EXPECT_EQ(_view.GetViewport().Center()->x, startCenter.x);
    EXPECT_EQ(_view.GetViewport().Center()->y, startCenter.y);
    EXPECT_EQ(*_view.GetViewport().Resolution(), startResolution);
}

TEST_F(MapViewTest, CancellingAnAnimatedFitKeepsItsPartialViewInHistory)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    ASSERT_TRUE(_view.Pan(100.0, 50.0));
    const MapPoint startCenter = *_view.GetViewport().Center();

    ASSERT_TRUE(_view.BeginTransitionToMap(std::chrono::milliseconds(100)));
    ASSERT_TRUE(_view.AdvanceTransition(std::chrono::milliseconds(40)));
    const MapPoint partialCenter = *_view.GetViewport().Center();
    ASSERT_TRUE(_view.CancelTransition());
    EXPECT_FALSE(_view.IsTransitioning());
    ASSERT_TRUE(_view.GoBack());
    EXPECT_EQ(_view.GetViewport().Center()->x, startCenter.x);
    EXPECT_EQ(_view.GetViewport().Center()->y, startCenter.y);
    EXPECT_NE(partialCenter.x, startCenter.x);
}

TEST_F(MapViewTest, AnimatedFitRejectsNonPositiveDurations)
{
    EXPECT_THROW(
        (void)_view.BeginTransitionToMap(std::chrono::milliseconds(0)),
        InvalidArgument);
    EXPECT_THROW(
        (void)_view.BeginTransitionToLayer(LayerId{}, std::chrono::milliseconds(-1)),
        InvalidArgument);
    EXPECT_THROW(
        (void)_view.AdvanceTransition(std::chrono::milliseconds(-1)),
        InvalidArgument);
}

TEST_F(MapViewTest, NavigationCancelsAPendingFit)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    _view.AddLayer(OpenRectangles({{100.0, 0.0, 110.0, 5.0}}));
    ASSERT_TRUE(_view.Resize(0, 0));
    EXPECT_FALSE(_view.FitToMap());

    ASSERT_TRUE(_view.Pan(1.0, 0.0));
    const MapPoint center = *_view.GetViewport().Center();
    ASSERT_TRUE(_view.Resize(200, 100));

    EXPECT_DOUBLE_EQ(_view.GetViewport().Center()->x, center.x);
    EXPECT_FALSE(IsVisible(_view.GetViewport(), {0.0, 0.0, 110.0, 5.0}));
}

TEST_F(MapViewTest, ZoomInCancelsAPendingFit)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    _view.AddLayer(OpenRectangles({{100.0, 0.0, 110.0, 5.0}}));
    ASSERT_TRUE(_view.Resize(0, 0));
    EXPECT_FALSE(_view.FitToMap());

    ASSERT_TRUE(_view.ZoomIn());
    const Viewport before = _view.GetViewport();
    ASSERT_TRUE(_view.Resize(200, 100));

    EXPECT_EQ(_view.GetViewport().Center()->x, before.Center()->x);
    EXPECT_EQ(_view.GetViewport().Center()->y, before.Center()->y);
    EXPECT_EQ(_view.GetViewport().Resolution(), before.Resolution());
}

TEST_F(MapViewTest, ZoomOutCancelsAPendingFit)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    _view.AddLayer(OpenRectangles({{100.0, 0.0, 110.0, 5.0}}));
    ASSERT_TRUE(_view.Resize(0, 0));
    EXPECT_FALSE(_view.FitToMap());

    ASSERT_TRUE(_view.ZoomOut());
    const Viewport before = _view.GetViewport();
    ASSERT_TRUE(_view.Resize(200, 100));

    EXPECT_EQ(_view.GetViewport().Center()->x, before.Center()->x);
    EXPECT_EQ(_view.GetViewport().Center()->y, before.Center()->y);
    EXPECT_EQ(_view.GetViewport().Resolution(), before.Resolution());
}

TEST_F(MapViewTest, ZoomAtOnAZeroSizedViewportKeepsAPendingFit)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    _view.AddLayer(OpenRectangles({{100.0, 0.0, 110.0, 5.0}}));
    ASSERT_TRUE(_view.Resize(0, 0));
    EXPECT_FALSE(_view.FitToMap(0.0));
    const Viewport before = _view.GetViewport();

    EXPECT_FALSE(_view.ZoomAt({37.5, 81.25}, 2.0));
    EXPECT_EQ(_view.GetViewport().Center()->x, before.Center()->x);
    EXPECT_EQ(_view.GetViewport().Center()->y, before.Center()->y);
    EXPECT_EQ(_view.GetViewport().Resolution(), before.Resolution());
    ASSERT_TRUE(_view.Resize(200, 100));

    EXPECT_NEAR(_view.GetViewport().Center()->x, 55.0, kTolerance);
    EXPECT_TRUE(IsVisible(_view.GetViewport(), {0.0, 0.0, 110.0, 5.0}));
}

TEST_F(MapViewTest, CenterZoomAtAResolutionLimitKeepsAPendingFit)
{
    ASSERT_TRUE(_view.Resize(200, 100));
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 10.0, 5.0}}));
    _view.AddLayer(OpenRectangles({{100.0, 0.0, 110.0, 5.0}}));
    ASSERT_TRUE(_view.ZoomOut(1.0e300));
    ASSERT_TRUE(_view.Resize(0, 0));
    EXPECT_FALSE(_view.FitToMap(0.0));

    EXPECT_FALSE(_view.ZoomOut());
    EXPECT_EQ(*_view.GetViewport().Resolution(), Viewport::kMaxResolution);
    ASSERT_TRUE(_view.Resize(200, 100));

    EXPECT_NEAR(_view.GetViewport().Center()->x, 55.0, kTolerance);
    EXPECT_TRUE(IsVisible(_view.GetViewport(), {0.0, 0.0, 110.0, 5.0}));
    EXPECT_LT(*_view.GetViewport().Resolution(), Viewport::kMaxResolution);
}

TEST_F(MapViewTest, InvalidLayerIndexLeavesTheMapUnchanged)
{
    EXPECT_THROW((void)_view.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}), 1), InvalidArgument);
    EXPECT_THROW((void)_view.AddLayer(nullptr), InvalidArgument);
    EXPECT_EQ(_view.GetMap().LayerCount(), 0);
    EXPECT_TRUE(_view.Scene().layers.empty());
}
