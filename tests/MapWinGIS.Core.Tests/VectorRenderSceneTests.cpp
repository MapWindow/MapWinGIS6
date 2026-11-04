// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderSceneTests.cpp
 * @brief Tests backend-neutral scene creation from polygon vector layers.
 *
 * Verifies that generated polygon dataset geometry becomes owned scene data
 * with the source extent and coordinates preserved.
 */
#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>

#include <cpl_error.h>
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

    std::filesystem::path MakeTwoLayerDataset(
        const std::filesystem::path& directory,
        const char* fileName,
        const OGRwkbGeometryType firstType,
        const char* firstWkt,
        const OGRwkbGeometryType secondType,
        const char* secondWkt)
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
            throw std::runtime_error("Cannot create multi-layer dataset");
        }

        const std::tuple<const char*, OGRwkbGeometryType, const char*> layers[] = {
            {"first", firstType, firstWkt},
            {"second", secondType, secondWkt}};
        for (const auto& [name, type, geometryWkt] : layers)
        {
            OGRLayer* layer = output->CreateLayer(name, nullptr, type, nullptr);
            if (layer == nullptr)
            {
                throw std::runtime_error("Cannot create multi-layer feature layer");
            }

            const char* text = geometryWkt;
            OGRGeometry* geometry = nullptr;
            if (OGRGeometryFactory::createFromWkt(
                    &text,
                    nullptr,
                    &geometry) != OGRERR_NONE ||
                geometry == nullptr)
            {
                throw std::runtime_error("Cannot parse multi-layer geometry");
            }

            OGRFeatureUniquePtr feature(
                OGRFeature::CreateFeature(layer->GetLayerDefn()));
            const OGRErr geometryResult = feature->SetGeometry(geometry);
            OGRGeometryFactory::destroyGeometry(geometry);
            if (geometryResult != OGRERR_NONE ||
                layer->CreateFeature(feature.get()) != OGRERR_NONE)
            {
                throw std::runtime_error("Cannot write multi-layer feature");
            }
        }

        output.reset();
        return path;
    }

    void ExpectSameScene(
        const rendering::VectorRenderScene& actual,
        const rendering::VectorRenderScene& expected)
    {
        EXPECT_DOUBLE_EQ(actual.extent.xMin, expected.extent.xMin);
        EXPECT_DOUBLE_EQ(actual.extent.yMin, expected.extent.yMin);
        EXPECT_DOUBLE_EQ(actual.extent.xMax, expected.extent.xMax);
        EXPECT_DOUBLE_EQ(actual.extent.yMax, expected.extent.yMax);
        EXPECT_EQ(actual.sourceFeatureCount, expected.sourceFeatureCount);
        ASSERT_EQ(actual.polygons.size(), expected.polygons.size());
        for (std::size_t polygon = 0; polygon < actual.polygons.size(); ++polygon)
        {
            ASSERT_EQ(
                actual.polygons[polygon].size(),
                expected.polygons[polygon].size());
            for (std::size_t ring = 0; ring < actual.polygons[polygon].size(); ++ring)
            {
                ASSERT_EQ(
                    actual.polygons[polygon][ring].size(),
                    expected.polygons[polygon][ring].size());
                for (std::size_t point = 0;
                     point < actual.polygons[polygon][ring].size();
                     ++point)
                {
                    EXPECT_DOUBLE_EQ(
                        actual.polygons[polygon][ring][point].x,
                        expected.polygons[polygon][ring][point].x);
                    EXPECT_DOUBLE_EQ(
                        actual.polygons[polygon][ring][point].y,
                        expected.polygons[polygon][ring][point].y);
                }
            }
        }
    }

    class MidStreamFailureLayer final : public OGRLayer
    {
    public:
        MidStreamFailureLayer()
            : _definition(OGRFeatureDefn::CreateFeatureDefn("features"))
        {
            _definition->Reference();
            _definition->SetGeomType(wkbPolygon);
        }

        ~MidStreamFailureLayer() override
        {
            _definition->Release();
        }

        void ResetReading() override
        {
            _firstFeatureReturned = false;
        }

        OGRFeature* GetNextFeature() override
        {
            if (!_firstFeatureReturned)
            {
                _firstFeatureReturned = true;
                OGRFeatureUniquePtr feature(
                    OGRFeature::CreateFeature(_definition));
                auto* polygon = new OGRPolygon();
                auto* ring = new OGRLinearRing();
                ring->addPoint(0.0, 0.0);
                ring->addPoint(1.0, 0.0);
                ring->addPoint(1.0, 1.0);
                ring->addPoint(0.0, 1.0);
                ring->addPoint(0.0, 0.0);
                polygon->addRingDirectly(ring);
                if (feature->SetGeometryDirectly(polygon) != OGRERR_NONE)
                {
                    OGRGeometryFactory::destroyGeometry(polygon);
                    return nullptr;
                }
                return feature.release();
            }

            CPLError(
                CE_Failure,
                CPLE_AppDefined,
                "%s",
                "Simulated mid-stream feature read failure");
            return nullptr;
        }

        const OGRFeatureDefn* GetLayerDefn() const override
        {
            return _definition;
        }

        int TestCapability(const char*) const override
        {
            return FALSE;
        }

    protected:
        OGRErr IGetExtent(
            int,
            OGREnvelope* extent,
            bool) override
        {
            extent->MinX = 0.0;
            extent->MinY = 0.0;
            extent->MaxX = 1.0;
            extent->MaxY = 1.0;
            return OGRERR_NONE;
        }

    private:
        OGRFeatureDefn* _definition;
        bool _firstFeatureReturned = false;
    };

    class MidStreamFailureDataset final : public GDALDataset
    {
    public:
        int GetLayerCount() const override
        {
            return 1;
        }

        const OGRLayer* GetLayer(int index) const override
        {
            return index == 0 ? &_layer : nullptr;
        }

    private:
        MidStreamFailureLayer _layer;
    };

    class EmptyVectorDataset final : public GDALDataset
    {
    public:
        int GetLayerCount() const override
        {
            return 0;
        }

        const OGRLayer* GetLayer(int) const override
        {
            return nullptr;
        }
    };

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

    ASSERT_EQ(scene.polygons.size(), static_cast<std::size_t>(Provinces::kFeatureCount));
    EXPECT_EQ(scene.sourceFeatureCount, _layer->FeatureCount());
    ASSERT_EQ(scene.polygons.front().size(), 1u);
    ASSERT_EQ(scene.polygons.front().front().size(), 5u);
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
        ASSERT_EQ(scene.polygons.size(), 1u);
        ASSERT_EQ(scene.polygons.front().size(), 1u);
        EXPECT_EQ(scene.polygons.front().front().size(), 5u);
    }
}

TEST(VectorRenderScene, LinearizesCurvedPolygonGeometry)
{
    GdalEnvironment::Initialize();
    TempDirectory directory;
    const auto path = MakeDimensionalPolygonDataset(
        directory.Path(),
        "curved-polygon.gpkg",
        wkbCurvePolygon,
        "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))");
    const auto dataset = VectorDataset::Open(path);

    const rendering::VectorRenderScene scene =
        rendering::CreateVectorRenderScene(dataset->Layer(0));

    ASSERT_EQ(scene.polygons.size(), 1u);
    ASSERT_EQ(scene.polygons.front().size(), 1u);
    EXPECT_GT(scene.polygons.front().front().size(), 5u);
}

TEST(VectorRenderScene, RejectsDatasetWithoutLayers)
{
    GdalEnvironment::Initialize();
    const auto deregister = [](GDALDriver* driver)
    {
        GetGDALDriverManager()->DeregisterDriver(driver);
        delete driver;
    };
    const std::unique_ptr<GDALDriver, decltype(deregister)> driver{
        std::make_unique<GDALDriver>().release(),
        deregister};
    driver->SetDescription("MWGEmptyVector");
    driver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    driver->SetMetadataItem(GDAL_DCAP_OPEN, "YES");
    driver->pfnIdentify = [](GDALOpenInfo* info)
    {
        return std::string_view(info->pszFilename).starts_with(
                   "MWG_VECTOR_NO_LAYERS:")
                   ? TRUE
                   : FALSE;
    };
    driver->pfnOpen = [](GDALOpenInfo* info) -> GDALDataset*
    {
        return std::string_view(info->pszFilename).starts_with(
                   "MWG_VECTOR_NO_LAYERS:")
                   ? new EmptyVectorDataset()
                   : nullptr;
    };
    GetGDALDriverManager()->RegisterDriver(driver.get());

    const auto dataset = VectorDataset::Open("MWG_VECTOR_NO_LAYERS:empty");
    ASSERT_EQ(dataset->LayerCount(), 0);
    EXPECT_THROW(
        (void)rendering::CreateVectorRenderScene(*dataset),
        CoreError);
}

TEST(VectorRenderScene, DatasetOverloadUsesFirstPolygonLayer)
{
    TempDirectory directory;
    const auto path = MakeTwoLayerDataset(
        directory.Path(),
        "polygon-first.gpkg",
        wkbPolygon,
        "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
        wkbPolygon,
        "POLYGON ((10 10, 12 10, 12 12, 10 12, 10 10))");
    const auto dataset = VectorDataset::Open(path);

    ASSERT_EQ(dataset->LayerCount(), 2);
    const rendering::VectorRenderScene scene =
        rendering::CreateVectorRenderScene(*dataset);
    const rendering::VectorRenderScene firstLayerScene =
        rendering::CreateVectorRenderScene(dataset->Layer(0));
    EXPECT_EQ(scene.sourceFeatureCount, dataset->Layer(0).FeatureCount());
    ExpectSameScene(scene, firstLayerScene);
}

TEST(VectorRenderScene, DatasetOverloadDoesNotSearchPastUnsupportedFirstLayer)
{
    TempDirectory directory;
    const auto path = MakeTwoLayerDataset(
        directory.Path(),
        "collection-first.gpkg",
        wkbGeometryCollection,
        "GEOMETRYCOLLECTION (POINT (0 0))",
        wkbPolygon,
        "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
    const auto dataset = VectorDataset::Open(path);

    ASSERT_EQ(dataset->LayerCount(), 2);
    EXPECT_THROW(
        (void)rendering::CreateVectorRenderScene(*dataset),
        CoreError);
}

TEST(VectorRenderScene, PropagatesMidStreamFeatureReadFailure)
{
    GdalEnvironment::Initialize();
    const auto deregister = [](GDALDriver* driver)
    {
        GetGDALDriverManager()->DeregisterDriver(driver);
        delete driver;
    };
    const std::unique_ptr<GDALDriver, decltype(deregister)> driver{
        std::make_unique<GDALDriver>().release(),
        deregister};
    driver->SetDescription("MWGMidStreamFailure");
    driver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    driver->SetMetadataItem(GDAL_DCAP_OPEN, "YES");
    driver->pfnIdentify = [](GDALOpenInfo* info)
    {
        return std::string_view(info->pszFilename).starts_with(
                   "MWG_VECTOR_READ_FAILURE:")
                   ? TRUE
                   : FALSE;
    };
    driver->pfnOpen = [](GDALOpenInfo* info) -> GDALDataset*
    {
        return std::string_view(info->pszFilename).starts_with(
                   "MWG_VECTOR_READ_FAILURE:")
                   ? new MidStreamFailureDataset()
                   : nullptr;
    };
    GetGDALDriverManager()->RegisterDriver(driver.get());

    const auto dataset = VectorDataset::Open("MWG_VECTOR_READ_FAILURE:features");
    try
    {
        (void)rendering::CreateVectorRenderScene(dataset->Layer(0));
        FAIL() << "Expected scene creation to report the feature read failure";
    }
    catch (const CoreError& error)
    {
        EXPECT_NE(
            std::string(error.what()).find(
                "Simulated mid-stream feature read failure"),
            std::string::npos);
    }
}
