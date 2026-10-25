// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderSceneTests.cpp
 * @brief Tests backend-neutral scene creation from polygon vector layers.
 *
 * Verifies that generated polygon dataset geometry becomes owned scene data
 * with the source extent and coordinates preserved.
 */
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <tuple>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/rendering/VectorRenderScene.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    std::string ToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    std::filesystem::path MakeDimensionalPolygonDataset(
        const std::filesystem::path& directory,
        const char* fileName,
        const OGRwkbGeometryType geometryType,
        const char* geometryWkt)
    {
        GdalEnvironment::Initialize();
        const std::filesystem::path path = directory / fileName;
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
        if (driver == nullptr)
        {
            throw std::runtime_error("GPKG driver is not available");
        }

        GDALDatasetUniquePtr output(
            driver->Create(ToUtf8(path).c_str(), 0, 0, 0, GDT_Unknown, nullptr));
        if (output == nullptr)
        {
            throw std::runtime_error("Cannot create dimensional polygon dataset");
        }

        OGRLayer* layer = output->CreateLayer("polygons", nullptr, geometryType, nullptr);
        if (layer == nullptr)
        {
            throw std::runtime_error("Cannot create dimensional polygon layer");
        }

        const char* text = geometryWkt;
        OGRGeometry* geometry = nullptr;
        if (OGRGeometryFactory::createFromWkt(&text, nullptr, &geometry) != OGRERR_NONE ||
            geometry == nullptr)
        {
            throw std::runtime_error("Cannot parse dimensional polygon geometry");
        }

        OGRFeatureUniquePtr feature(OGRFeature::CreateFeature(layer->GetLayerDefn()));
        const OGRErr geometryResult = feature->SetGeometry(geometry);
        OGRGeometryFactory::destroyGeometry(geometry);
        if (geometryResult != OGRERR_NONE || layer->CreateFeature(feature.get()) != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot write dimensional polygon feature");
        }

        output.reset();
        return path;
    }

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

TEST(VectorRenderScene, AcceptsPolygonZAndPolygonM)
{
    GdalEnvironment::Initialize();
    TempDirectory directory;
    const std::tuple<OGRwkbGeometryType, ShapeType, const char*, const char*> cases[] = {
        {wkbPolygon25D, ShapeType::PolygonZ, "polygon-z.gpkg",
         "POLYGON Z ((0 0 1, 1 0 1, 1 1 1, 0 1 1, 0 0 1))"},
        {wkbPolygonM, ShapeType::PolygonM, "polygon-m.gpkg",
         "POLYGON M ((0 0 2, 1 0 3, 1 1 4, 0 1 5, 0 0 2))"}};

    for (const auto& [geometryType, shapeType, fileName, geometryWkt] : cases)
    {
        const auto path = MakeDimensionalPolygonDataset(
            directory.Path(), fileName, geometryType, geometryWkt);
        const auto dataset = VectorDataset::Open(path);
        const VectorLayer& layer = dataset->Layer(0);
        ASSERT_EQ(layer.Type(), shapeType);

        const rendering::VectorRenderScene scene = rendering::CreateVectorRenderScene(layer);
        ASSERT_EQ(scene.polygons.size(), 1);
        ASSERT_EQ(scene.polygons.front().size(), 1);
        EXPECT_EQ(scene.polygons.front().front().size(), 5);
    }
}
