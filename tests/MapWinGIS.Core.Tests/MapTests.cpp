// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapTests.cpp
 * @brief Tests the map model, its layer collection, and extent aggregation.
 *
 * Verifies stable layer identities, ordering, visibility, dataset ownership,
 * and combined extents without creating a window or using ActiveX.
 */
#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/map/Map.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    void ExpectExtent(const Extent& actual, const Extent& expected)
    {
        EXPECT_DOUBLE_EQ(actual.xMin, expected.xMin);
        EXPECT_DOUBLE_EQ(actual.yMin, expected.yMin);
        EXPECT_DOUBLE_EQ(actual.xMax, expected.xMax);
        EXPECT_DOUBLE_EQ(actual.yMax, expected.yMax);
    }

    class MapTest : public testing::Test
    {
    protected:
        /// Opens a generated dataset whose only layer holds these rectangles.
        std::shared_ptr<VectorDataset> OpenRectangles(const std::vector<Extent>& rectangles,
                                                      const OpenMode mode = OpenMode::ReadOnly)
        {
            const std::string fileName = "layer-" + std::to_string(_datasetCount++) + ".gpkg";
            return VectorDataset::Open(
                MakeRectanglesDataset(_directory.Path(), fileName, rectangles), mode);
        }

        TempDirectory _directory;
        Map _map;
        int _datasetCount = 0;
    };
}

TEST_F(MapTest, NewMapIsEmpty)
{
    EXPECT_EQ(_map.LayerCount(), 0);
    EXPECT_TRUE(_map.GetExtent().IsEmpty());
    EXPECT_TRUE(_map.GetExtent(ExtentScope::AllLayers).IsEmpty());
    EXPECT_EQ(_map.FindLayer(LayerId{1}), nullptr);
    EXPECT_FALSE(_map.LayerPosition(LayerId{1}).has_value());
    EXPECT_THROW((void)_map.Layer(0), InvalidArgument);
}

TEST_F(MapTest, AddsLayersWithDistinctValidIdentities)
{
    const LayerId first = _map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));
    const LayerId second = _map.AddLayer(OpenRectangles({{2, 2, 3, 3}}));

    EXPECT_TRUE(first.IsValid());
    EXPECT_TRUE(second.IsValid());
    EXPECT_NE(first, second);
    EXPECT_FALSE(LayerId{}.IsValid());
    ASSERT_EQ(_map.LayerCount(), 2);
    EXPECT_EQ(_map.Layer(0).Id(), first);
    EXPECT_EQ(_map.Layer(1).Id(), second);
    EXPECT_EQ(_map.Layer(0).Source().Name(), "rectangles");
    EXPECT_EQ(_map.Layer(0).SourceLayerIndex(), 0);
    EXPECT_TRUE(_map.Layer(0).IsVisible());
}

TEST_F(MapTest, AppendsNewLayersOnTop)
{
    const LayerId bottom = _map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));
    const LayerId middle = _map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));
    const LayerId top = _map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));

    EXPECT_EQ(_map.LayerPosition(bottom), 0);
    EXPECT_EQ(_map.LayerPosition(middle), 1);
    EXPECT_EQ(_map.LayerPosition(top), 2);
}

TEST_F(MapTest, RejectsMissingDatasetAndOutOfRangeLayerIndex)
{
    const std::shared_ptr<VectorDataset> dataset = OpenRectangles({{0, 0, 1, 1}});

    EXPECT_THROW((void)_map.AddLayer(nullptr), InvalidArgument);
    EXPECT_THROW((void)_map.AddLayer(dataset, -1), InvalidArgument);
    EXPECT_THROW((void)_map.AddLayer(dataset, 1), InvalidArgument);
    EXPECT_EQ(_map.LayerCount(), 0);
}

TEST_F(MapTest, AddsRequestedNonZeroSourceLayer)
{
    const std::shared_ptr<VectorDataset> dataset = VectorDataset::Open(
        MakeTwoLayerDataset(_directory.Path(), "two-layers.gpkg"));
    ASSERT_EQ(dataset->LayerCount(), 2);

    const LayerId id = _map.AddLayer(dataset, 1);
    const MapLayer* layer = _map.FindLayer(id);
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(layer->SourceLayerIndex(), 1);
    EXPECT_EQ(layer->Source().Name(), "second");
}

TEST_F(MapTest, RejectsOutOfRangePositions)
{
    (void)_map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));

    EXPECT_THROW((void)_map.Layer(-1), InvalidArgument);
    EXPECT_THROW((void)_map.Layer(1), InvalidArgument);
}

TEST_F(MapTest, RemovesLayerByIdentity)
{
    const LayerId first = _map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));
    const LayerId second = _map.AddLayer(OpenRectangles({{2, 2, 3, 3}}));

    EXPECT_TRUE(_map.RemoveLayer(first));
    ASSERT_EQ(_map.LayerCount(), 1);
    EXPECT_EQ(_map.Layer(0).Id(), second);
    EXPECT_EQ(_map.FindLayer(first), nullptr);
    EXPECT_FALSE(_map.RemoveLayer(first));
    EXPECT_FALSE(_map.RemoveLayer(LayerId{}));
    EXPECT_EQ(_map.LayerCount(), 1);
}

TEST_F(MapTest, KeepsIdentitiesAndReferencesStableWhenOtherLayersAreRemoved)
{
    const LayerId first = _map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));
    const LayerId second = _map.AddLayer(OpenRectangles({{2, 2, 3, 3}}));
    const LayerId third = _map.AddLayer(OpenRectangles({{4, 4, 5, 5}}));
    const MapLayer* thirdLayer = _map.FindLayer(third);
    ASSERT_NE(thirdLayer, nullptr);

    ASSERT_TRUE(_map.RemoveLayer(second));

    EXPECT_EQ(_map.FindLayer(third), thirdLayer);
    EXPECT_EQ(thirdLayer->Id(), third);
    ExpectExtent(thirdLayer->GetExtent(), {4, 4, 5, 5});
    EXPECT_EQ(_map.LayerPosition(first), 0);
    EXPECT_EQ(_map.LayerPosition(third), 1);
    EXPECT_FALSE(_map.LayerPosition(second).has_value());
}

TEST_F(MapTest, NeverReusesIdentities)
{
    const LayerId first = _map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));
    ASSERT_TRUE(_map.RemoveLayer(first));
    const LayerId second = _map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));
    _map.Clear();
    const LayerId third = _map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));

    EXPECT_NE(second, first);
    EXPECT_NE(third, first);
    EXPECT_NE(third, second);
    EXPECT_EQ(_map.FindLayer(first), nullptr);
    EXPECT_EQ(_map.FindLayer(second), nullptr);
}

TEST_F(MapTest, ClearRemovesAllLayers)
{
    const LayerId first = _map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));
    (void)_map.AddLayer(OpenRectangles({{2, 2, 3, 3}}));

    _map.Clear();

    EXPECT_EQ(_map.LayerCount(), 0);
    EXPECT_EQ(_map.FindLayer(first), nullptr);
    EXPECT_TRUE(_map.GetExtent().IsEmpty());
}

TEST_F(MapTest, AddsTheSameSourceLayerAsSeparateLayers)
{
    const std::shared_ptr<VectorDataset> dataset = OpenRectangles({{0, 0, 1, 1}});
    const LayerId first = _map.AddLayer(dataset);
    const LayerId second = _map.AddLayer(dataset);

    ASSERT_EQ(_map.LayerCount(), 2);
    EXPECT_NE(first, second);
    EXPECT_EQ(&_map.Layer(0).Source(), &_map.Layer(1).Source());

    ASSERT_TRUE(_map.SetLayerVisible(first, false));
    EXPECT_FALSE(_map.FindLayer(first)->IsVisible());
    EXPECT_TRUE(_map.FindLayer(second)->IsVisible());
}

TEST_F(MapTest, SharesDatasetOwnershipWithItsLayers)
{
    std::shared_ptr<VectorDataset> dataset = OpenRectangles({{0, 0, 1, 1}});
    const std::weak_ptr<VectorDataset> observer = dataset;
    const LayerId first = _map.AddLayer(dataset);
    const LayerId second = _map.AddLayer(dataset);
    dataset.reset();

    ASSERT_FALSE(observer.expired());
    ExpectExtent(_map.FindLayer(first)->GetExtent(), {0, 0, 1, 1});
    EXPECT_EQ(&_map.FindLayer(first)->Dataset(), observer.lock().get());

    ASSERT_TRUE(_map.RemoveLayer(first));
    EXPECT_FALSE(observer.expired());
    ASSERT_TRUE(_map.RemoveLayer(second));
    EXPECT_TRUE(observer.expired());
}

TEST_F(MapTest, CombinesOverlappingLayerExtents)
{
    (void)_map.AddLayer(OpenRectangles({{0, 0, 10, 10}}));
    (void)_map.AddLayer(OpenRectangles({{5, -5, 15, 5}}));

    ExpectExtent(_map.GetExtent(), {0, -5, 15, 10});
}

TEST_F(MapTest, CombinesSeparatedLayerExtents)
{
    (void)_map.AddLayer(OpenRectangles({{-20, -20, -10, -10}}));
    (void)_map.AddLayer(OpenRectangles({{100, 200, 110, 210}, {120, 220, 130, 230}}));

    ExpectExtent(_map.GetExtent(), {-20, -20, 130, 230});
}

TEST_F(MapTest, CombinedExtentDoesNotDependOnLayerOrder)
{
    Map reversed;
    const std::vector<Extent> extents = {{0, 0, 1, 1}, {5, 6, 7, 8}, {-3, 2, -1, 4}};
    for (const Extent& extent : extents)
    {
        (void)_map.AddLayer(OpenRectangles({extent}));
    }
    for (auto extent = extents.rbegin(); extent != extents.rend(); ++extent)
    {
        (void)reversed.AddLayer(OpenRectangles({*extent}));
    }

    ExpectExtent(_map.GetExtent(), reversed.GetExtent());
    ExpectExtent(_map.GetExtent(), {-3, 0, 7, 8});
}

TEST_F(MapTest, IgnoresLayersWithoutAValidExtent)
{
    const LayerId empty = _map.AddLayer(OpenRectangles({}));
    EXPECT_TRUE(_map.FindLayer(empty)->GetExtent().IsEmpty());
    EXPECT_TRUE(_map.GetExtent().IsEmpty());

    (void)_map.AddLayer(OpenRectangles({{1, 2, 3, 4}}));

    ExpectExtent(_map.GetExtent(), {1, 2, 3, 4});
}

TEST_F(MapTest, HiddenLayersAreExcludedFromTheVisibleExtent)
{
    (void)_map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));
    const LayerId hidden = _map.AddLayer(OpenRectangles({{10, 10, 20, 20}}));

    ASSERT_TRUE(_map.SetLayerVisible(hidden, false));

    EXPECT_FALSE(_map.FindLayer(hidden)->IsVisible());
    ExpectExtent(_map.GetExtent(), {0, 0, 1, 1});
    ExpectExtent(_map.GetExtent(ExtentScope::AllLayers), {0, 0, 20, 20});

    ASSERT_TRUE(_map.SetLayerVisible(hidden, true));
    ExpectExtent(_map.GetExtent(), {0, 0, 20, 20});
}

TEST_F(MapTest, MapWithOnlyHiddenLayersHasNoVisibleExtent)
{
    const LayerId layer = _map.AddLayer(OpenRectangles({{0, 0, 1, 1}}));
    ASSERT_TRUE(_map.SetLayerVisible(layer, false));

    EXPECT_TRUE(_map.GetExtent().IsEmpty());
    ExpectExtent(_map.GetExtent(ExtentScope::AllLayers), {0, 0, 1, 1});
}

TEST_F(MapTest, SetLayerVisibleRejectsUnknownIdentity)
{
    EXPECT_FALSE(_map.SetLayerVisible(LayerId{42}, false));
}

TEST_F(MapTest, ExtentReflectsSourceGeometryEdits)
{
    const LayerId layer = _map.AddLayer(OpenRectangles({{0, 0, 1, 1}}, OpenMode::Update));
    ExpectExtent(_map.GetExtent(), {0, 0, 1, 1});

    _map.FindLayer(layer)->Source().SetGeometryWkt(0, "POLYGON ((0 0, 4 0, 4 3, 0 3, 0 0))");

    ExpectExtent(_map.GetExtent(), {0, 0, 4, 3});
}

TEST(Extent, IsValidRequiresANonEmptyFiniteExtent)
{
    constexpr double kInfinity = std::numeric_limits<double>::infinity();
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

    EXPECT_TRUE((Extent{0, 0, 1, 1}.IsValid()));
    EXPECT_TRUE((Extent{1, 1, 1, 1}.IsValid()));
    EXPECT_FALSE(Extent{}.IsValid());
    EXPECT_FALSE((Extent{1, 0, 0, 1}.IsValid()));
    EXPECT_FALSE((Extent{0, 0, kInfinity, 1}.IsValid()));
    EXPECT_FALSE((Extent{-kInfinity, 0, 1, 1}.IsValid()));
    EXPECT_FALSE((Extent{0, kNaN, 1, 1}.IsValid()));
}
