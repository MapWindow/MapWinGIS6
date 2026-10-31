// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapViewTests.cpp
 * @brief Tests the map view: shared viewport, scene invalidation, and resizing.
 *
 * Verifies that layers with different extents are shown through one
 * viewport, that map changes rebuild the render scene and report a redraw,
 * and that resizing keeps the view stable without a window or ActiveX.
 */
#include <gtest/gtest.h>

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
        ASSERT_TRUE(_view.FitToMap(0.0)) << size[0] << " x " << size[1];
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

TEST_F(MapViewTest, SceneIsReusedUntilTheMapChanges)
{
    _view.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));
    const rendering::MapRenderScene& scene = _view.Scene();
    const rendering::Polygon* polygons = scene.layers.front().scene.polygons.data();

    ASSERT_TRUE(_view.Resize(300, 200));
    EXPECT_EQ(_view.Scene().layers.front().scene.polygons.data(), polygons);

    EXPECT_TRUE(_view.InvalidateScene());
    EXPECT_EQ(&_view.Scene(), &scene);
    EXPECT_EQ(scene.layers.size(), 1U);
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

TEST_F(MapViewTest, InvalidLayerIndexLeavesTheMapUnchanged)
{
    EXPECT_THROW((void)_view.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}), 1), InvalidArgument);
    EXPECT_THROW((void)_view.AddLayer(nullptr), InvalidArgument);
    EXPECT_EQ(_view.GetMap().LayerCount(), 0);
    EXPECT_TRUE(_view.Scene().layers.empty());
}
