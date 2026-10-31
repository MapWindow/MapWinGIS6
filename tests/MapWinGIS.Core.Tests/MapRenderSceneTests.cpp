// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapRenderSceneTests.cpp
 * @brief Tests render-scene creation for all layers of a map.
 *
 * Verifies drawing order, visibility, skipped layers, per-layer failure
 * isolation, and that the scene is an owned snapshot of the map.
 */
#include <gtest/gtest.h>

#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "TestData.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/map/Map.h"
#include "mapwingis/map/MapView.h"
#include "mapwingis/rendering/MapRenderScene.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    class ThrowingLayer final : public OGRLayer
    {
    public:
        explicit ThrowingLayer(const bool allocationFailure)
            : _allocationFailure(allocationFailure),
              _definition(OGRFeatureDefn::CreateFeatureDefn("throwing"))
        {
            _definition->Reference();
            _definition->SetGeomType(wkbPolygon);
        }

        ~ThrowingLayer() override { _definition->Release(); }
        void ResetReading() override {}
        const OGRFeatureDefn* GetLayerDefn() const override { return _definition; }
        int TestCapability(const char*) const override { return FALSE; }
        GIntBig GetFeatureCount(int) override { return 1; }

        OGRFeature* GetNextFeature() override
        {
            if (_allocationFailure)
            {
                throw std::bad_alloc();
            }
            throw std::runtime_error("Simulated geometry read exception");
        }

    protected:
        OGRErr IGetExtent(int, OGREnvelope* extent, bool) override
        {
            extent->MinX = 0.0;
            extent->MinY = 0.0;
            extent->MaxX = 1.0;
            extent->MaxY = 1.0;
            return OGRERR_NONE;
        }

    private:
        bool _allocationFailure;
        OGRFeatureDefn* _definition;
    };

    class ThrowingDataset final : public GDALDataset
    {
    public:
        explicit ThrowingDataset(const bool allocationFailure)
            : _layer(allocationFailure)
        {
        }
        int GetLayerCount() const override { return 1; }
        const OGRLayer* GetLayer(const int index) const override
        {
            return index == 0 ? &_layer : nullptr;
        }

    private:
        ThrowingLayer _layer;
    };

    /// Writes a GeoPackage whose only layer holds one line string.
    std::filesystem::path MakeLineDataset(const std::filesystem::path& directory)
    {
        GdalEnvironment::Initialize();
        const std::filesystem::path path = directory / "line.gpkg";
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
        if (driver == nullptr)
        {
            throw std::runtime_error("GPKG driver is not available");
        }

        const std::u8string utf8 = path.u8string();
        GDALDatasetUniquePtr output(driver->Create(
            reinterpret_cast<const char*>(utf8.c_str()), 0, 0, 0, GDT_Unknown, nullptr));
        if (output == nullptr)
        {
            throw std::runtime_error("Cannot create the line dataset");
        }

        OGRLayer* layer = output->CreateLayer("lines", nullptr, wkbLineString, nullptr);
        if (layer == nullptr)
        {
            throw std::runtime_error("Cannot create the line layer");
        }

        OGRLineString line;
        line.addPoint(0.0, 0.0);
        line.addPoint(5.0, 5.0);
        OGRFeatureUniquePtr feature(OGRFeature::CreateFeature(layer->GetLayerDefn()));
        if (feature->SetGeometry(&line) != OGRERR_NONE ||
            layer->CreateFeature(feature.get()) != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot write the line feature");
        }
        return path;
    }

    std::filesystem::path MakeGeographicPointDataset(
        const std::filesystem::path& directory)
    {
        GdalEnvironment::Initialize();
        const std::filesystem::path path = directory / "geographic-point.gpkg";
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
        if (driver == nullptr)
        {
            throw std::runtime_error("GPKG driver is not available");
        }

        const std::u8string utf8 = path.u8string();
        GDALDatasetUniquePtr output(driver->Create(
            reinterpret_cast<const char*>(utf8.c_str()), 0, 0, 0, GDT_Unknown, nullptr));
        if (output == nullptr)
        {
            throw std::runtime_error("Cannot create the geographic point dataset");
        }

        OGRSpatialReference srs;
        srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        if (srs.importFromEPSG(4326) != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot create the point projection");
        }
        OGRLayer* layer = output->CreateLayer("points", &srs, wkbPoint, nullptr);
        if (layer == nullptr)
        {
            throw std::runtime_error("Cannot create the point layer");
        }

        OGRPoint point(5.3872, 52.1552);
        OGRFeatureUniquePtr feature(OGRFeature::CreateFeature(layer->GetLayerDefn()));
        if (feature->SetGeometry(&point) != OGRERR_NONE ||
            layer->CreateFeature(feature.get()) != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot write the point feature");
        }
        return path;
    }

    class MapRenderSceneTest : public testing::Test
    {
    protected:
        std::shared_ptr<VectorDataset> OpenRectangles(const std::vector<Extent>& rectangles)
        {
            const std::string fileName = "layer-" + std::to_string(_datasetCount++) + ".gpkg";
            return VectorDataset::Open(
                MakeRectanglesDataset(_directory.Path(), fileName, rectangles));
        }

        TempDirectory _directory;
        Map _map;
        int _datasetCount = 0;
    };
}

TEST_F(MapRenderSceneTest, EmptyMapGivesAnEmptyScene)
{
    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(_map);

    EXPECT_TRUE(scene.layers.empty());
    EXPECT_TRUE(scene.skippedLayers.empty());
}

TEST_F(MapRenderSceneTest, ContainsEveryVisibleLayerInDrawingOrder)
{
    const LayerId bottom = _map.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));
    const LayerId middle = _map.AddLayer(OpenRectangles({{10.0, 10.0, 12.0, 12.0},
                                                         {20.0, 20.0, 21.0, 21.0}}));
    const LayerId top = _map.AddLayer(OpenRectangles({{-5.0, -5.0, -4.0, -4.0}}));

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(_map);

    ASSERT_EQ(scene.layers.size(), 3U);
    EXPECT_TRUE(scene.skippedLayers.empty());
    EXPECT_EQ(scene.layers[0].layerId, bottom);
    EXPECT_EQ(scene.layers[1].layerId, middle);
    EXPECT_EQ(scene.layers[2].layerId, top);
    EXPECT_EQ(scene.layers[0].scene.polygons.size(), 1U);
    EXPECT_EQ(scene.layers[1].scene.polygons.size(), 2U);
    EXPECT_EQ(scene.layers[1].scene.sourceFeatureCount, 2);
}

TEST_F(MapRenderSceneTest, KeepsEachLayersOwnExtentAndCoordinates)
{
    _map.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));
    _map.AddLayer(OpenRectangles({{100.0, 50.0, 140.0, 60.0}}));

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(_map);

    ASSERT_EQ(scene.layers.size(), 2U);
    EXPECT_DOUBLE_EQ(scene.layers[0].scene.extent.xMax, 1.0);
    EXPECT_DOUBLE_EQ(scene.layers[1].scene.extent.xMin, 100.0);
    EXPECT_DOUBLE_EQ(scene.layers[1].scene.extent.yMax, 60.0);
    for (const rendering::Point& point : scene.layers[1].scene.polygons.front().front())
    {
        EXPECT_GE(point.x, 100.0);
        EXPECT_LE(point.x, 140.0);
        EXPECT_GE(point.y, 50.0);
        EXPECT_LE(point.y, 60.0);
    }
}

TEST_F(MapRenderSceneTest, SkipsHiddenLayers)
{
    const LayerId hidden = _map.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));
    const LayerId visible = _map.AddLayer(OpenRectangles({{2.0, 2.0, 3.0, 3.0}}));
    ASSERT_TRUE(_map.SetLayerVisible(hidden, false));

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(_map);

    ASSERT_EQ(scene.layers.size(), 1U);
    EXPECT_EQ(scene.layers[0].layerId, visible);
    ASSERT_EQ(scene.skippedLayers.size(), 1U);
    EXPECT_EQ(scene.skippedLayers[0].layerId, hidden);
    EXPECT_EQ(scene.skippedLayers[0].reason, rendering::LayerSkipReason::Hidden);
    EXPECT_TRUE(scene.skippedLayers[0].message.empty());
}

TEST_F(MapRenderSceneTest, SkipsLayersWithoutGeometry)
{
    const LayerId empty = _map.AddLayer(OpenRectangles({}));
    const LayerId filled = _map.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(_map);

    ASSERT_EQ(scene.layers.size(), 1U);
    EXPECT_EQ(scene.layers[0].layerId, filled);
    ASSERT_EQ(scene.skippedLayers.size(), 1U);
    EXPECT_EQ(scene.skippedLayers[0].layerId, empty);
    EXPECT_EQ(scene.skippedLayers[0].reason, rendering::LayerSkipReason::NoGeometry);
}

TEST_F(MapRenderSceneTest, IncludesLineLayersInDrawingOrder)
{
    const LayerId below = _map.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));
    const LayerId lines = _map.AddLayer(VectorDataset::Open(MakeLineDataset(_directory.Path())));
    const LayerId above = _map.AddLayer(OpenRectangles({{2.0, 2.0, 3.0, 3.0}}));

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(_map);

    ASSERT_EQ(scene.layers.size(), 3U);
    EXPECT_EQ(scene.layers[0].layerId, below);
    EXPECT_EQ(scene.layers[1].layerId, lines);
    EXPECT_EQ(scene.layers[2].layerId, above);
    ASSERT_EQ(scene.layers[1].scene.lines.size(), 1U);
    EXPECT_EQ(scene.layers[1].scene.lines.front().size(), 2U);
    EXPECT_TRUE(scene.skippedLayers.empty());
}

TEST_F(MapRenderSceneTest, ReprojectsPointGeometryAndExtentIntoTheFirstLayersCoordinateSystem)
{
    const std::shared_ptr<VectorDataset> projected =
        VectorDataset::Open(MakeProvincesDataset(_directory.Path(), "projected.gpkg"));
    const std::shared_ptr<VectorDataset> geographic =
        VectorDataset::Open(MakeGeographicPointDataset(_directory.Path()));
    _map.AddLayer(projected);
    const LayerId pointId = _map.AddLayer(geographic);

    OGRSpatialReference source;
    OGRSpatialReference target;
    source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ASSERT_EQ(source.importFromEPSG(4326), OGRERR_NONE);
    ASSERT_EQ(target.importFromEPSG(Provinces::kEpsg), OGRERR_NONE);
    std::unique_ptr<OGRCoordinateTransformation, decltype(&OCTDestroyCoordinateTransformation)>
        transformation(OGRCreateCoordinateTransformation(&source, &target),
                       OCTDestroyCoordinateTransformation);
    ASSERT_NE(transformation, nullptr);
    double expectedX = 5.3872;
    double expectedY = 52.1552;
    ASSERT_NE(transformation->Transform(1, &expectedX, &expectedY), FALSE);

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(_map);
    ASSERT_EQ(scene.layers.size(), 2U);
    EXPECT_EQ(scene.layers[1].layerId, pointId);
    ASSERT_EQ(scene.layers[1].scene.points.size(), 1U);
    EXPECT_NEAR(scene.layers[1].scene.points.front().x, expectedX, 1e-6);
    EXPECT_NEAR(scene.layers[1].scene.points.front().y, expectedY, 1e-6);
    const Extent extent = _map.FindLayer(pointId)->GetExtent();
    EXPECT_NEAR(extent.xMin, expectedX, 1e-6);
    EXPECT_NEAR(extent.xMax, expectedX, 1e-6);
    EXPECT_NEAR(extent.yMin, expectedY, 1e-6);
    EXPECT_NEAR(extent.yMax, expectedY, 1e-6);
}

TEST_F(MapRenderSceneTest, IncludesRasterPreviewAndExtentInMapScene)
{
    const std::shared_ptr<RasterDataset> raster =
        RasterDataset::Open(MakeRaster(_directory.Path()));
    const LayerId rasterId = _map.AddRasterLayer(raster);

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(_map);

    ASSERT_EQ(scene.layers.size(), 1U);
    EXPECT_EQ(scene.layers.front().layerId, rasterId);
    ASSERT_TRUE(scene.layers.front().raster.has_value());
    EXPECT_EQ(scene.layers.front().raster->width, 12);
    EXPECT_EQ(scene.layers.front().raster->height, 8);
    EXPECT_EQ(scene.layers.front().raster->pixels.front(), 0xFF2A2A2AU);
    EXPECT_DOUBLE_EQ(scene.layers.front().raster->extent.xMax, 12.0);
    EXPECT_DOUBLE_EQ(scene.layers.front().raster->extent.yMax, 8.0);
    EXPECT_TRUE(scene.layers.front().scene.polygons.empty());
}

TEST_F(MapRenderSceneTest, DuplicateLayersAreSeparateSceneLayers)
{
    const std::shared_ptr<VectorDataset> dataset = OpenRectangles({{0.0, 0.0, 1.0, 1.0}});
    const LayerId first = _map.AddLayer(dataset);
    const LayerId second = _map.AddLayer(dataset);

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(_map);

    ASSERT_EQ(scene.layers.size(), 2U);
    EXPECT_EQ(scene.layers[0].layerId, first);
    EXPECT_EQ(scene.layers[1].layerId, second);
}

TEST_F(MapRenderSceneTest, IsolatesStandardExceptionsButPropagatesAllocationFailures)
{
    GdalEnvironment::Initialize();
    const auto deregister = [](GDALDriver* driver)
    {
        GetGDALDriverManager()->DeregisterDriver(driver);
        delete driver;
    };
    const std::unique_ptr<GDALDriver, decltype(deregister)> driver{
        std::make_unique<GDALDriver>().release(), deregister};
    driver->SetDescription("MWGThrowingGeometry");
    driver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    driver->SetMetadataItem(GDAL_DCAP_OPEN, "YES");
    driver->pfnIdentify = [](GDALOpenInfo* info)
    {
        return std::string_view(info->pszFilename).starts_with("MWG_THROWING:") ? TRUE : FALSE;
    };
    driver->pfnOpen = [](GDALOpenInfo* info) -> GDALDataset*
    {
        const std::string_view name(info->pszFilename);
        return name.starts_with("MWG_THROWING:")
            ? std::make_unique<ThrowingDataset>(name.ends_with("allocation")).release()
            : nullptr;
    };
    GetGDALDriverManager()->RegisterDriver(driver.get());

    MapView view;
    const LayerId below = view.AddLayer(OpenRectangles({{0, 0, 1, 1}}));
    const LayerId failed = view.AddLayer(VectorDataset::Open("MWG_THROWING:runtime"));
    const LayerId above = view.AddLayer(OpenRectangles({{2, 2, 3, 3}}));
    const auto& scene = view.Scene();
    ASSERT_EQ(scene.layers.size(), 2U);
    EXPECT_EQ(scene.layers[0].layerId, below);
    EXPECT_EQ(scene.layers[1].layerId, above);
    ASSERT_EQ(scene.skippedLayers.size(), 1U);
    EXPECT_EQ(scene.skippedLayers[0].layerId, failed);
    EXPECT_EQ(scene.skippedLayers[0].reason, rendering::LayerSkipReason::Failed);
    EXPECT_EQ(scene.skippedLayers[0].message, "Simulated geometry read exception");
    const auto* polygons = scene.layers[0].scene.polygons.data();
    EXPECT_EQ(view.Scene().layers[0].scene.polygons.data(), polygons);

    view.AddLayer(VectorDataset::Open("MWG_THROWING:allocation"));
    EXPECT_THROW((void)view.Scene(), std::bad_alloc);
}

TEST_F(MapRenderSceneTest, SceneIsASnapshotThatOutlivesTheMapLayers)
{
    const LayerId id = _map.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));
    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(_map);

    ASSERT_TRUE(_map.SetLayerVisible(id, false));
    _map.Clear();

    ASSERT_EQ(scene.layers.size(), 1U);
    EXPECT_EQ(scene.layers[0].layerId, id);
    EXPECT_EQ(scene.layers[0].scene.polygons.front().front().size(), 5U);
}
