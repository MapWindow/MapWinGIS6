// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderSceneTests.cpp
 * @brief Tests backend-neutral scene creation from polygon vector layers.
 *
 * Verifies that generated polygon dataset geometry becomes owned scene data
 * with the source extent and coordinates preserved.
 */
#include <gtest/gtest.h>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/rendering/VectorRenderScene.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    class VectorRenderSceneTest : public testing::TestWithParam<const char*>
    {
    protected:
        void SetUp() override
        {
            _dataset = VectorDataset::Open(MakeProvincesDataset(_directory.Path(), GetParam()));
            ASSERT_GT(_dataset->LayerCount(), 0);
            _layer = &_dataset->Layer(0);
        }

        TempDirectory _directory;
        std::unique_ptr<VectorDataset> _dataset;
        VectorLayer* _layer = nullptr;
    };
}

INSTANTIATE_TEST_SUITE_P(Formats, VectorRenderSceneTest,
                         testing::Values("scene.shp", "scene.gpkg"),
                         [](const testing::TestParamInfo<const char*>& info)
                         {
                             return std::string(info.param).find("gpkg") != std::string::npos
                                        ? "GeoPackage"
                                        : "Shapefile";
                         });

TEST_P(VectorRenderSceneTest, CopiesPolygonCoordinatesAndExtent)
{
    const rendering::VectorRenderScene scene = rendering::CreateVectorRenderScene(*_layer);

    ASSERT_EQ(scene.polygons.size(), Provinces::kFeatureCount);
    ASSERT_EQ(scene.polygons.front().size(), 1);
    ASSERT_EQ(scene.polygons.front().front().size(), 5);
    EXPECT_DOUBLE_EQ(scene.extent.xMin, Provinces::kMinX);
    EXPECT_DOUBLE_EQ(scene.extent.yMin, Provinces::kMinY);
    EXPECT_DOUBLE_EQ(scene.extent.xMax, Provinces::kMaxX);
    EXPECT_DOUBLE_EQ(scene.extent.yMax, Provinces::kMaxY);
    EXPECT_DOUBLE_EQ(scene.polygons.front().front().front().x, Provinces::kMinX);
    EXPECT_DOUBLE_EQ(scene.polygons.front().front().front().y, Provinces::kMinY);
    EXPECT_DOUBLE_EQ(scene.polygons.front().front().back().x, Provinces::kMinX);
    EXPECT_DOUBLE_EQ(scene.polygons.front().front().back().y, Provinces::kMinY);
}
