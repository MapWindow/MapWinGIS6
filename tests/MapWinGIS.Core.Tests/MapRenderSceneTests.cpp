// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapRenderSceneTests.cpp
 * @brief Tests render-scene creation for all layers of a map.
 *
 * Verifies drawing order, visibility, skipped layers, per-layer failure
 * isolation, and that the scene is an owned snapshot of the map.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "TestData.h"
#include "mapwingis/core/Error.h"
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

    std::filesystem::path MakeWktDataset(
        const std::filesystem::path& directory,
        const std::string_view fileName,
        const int epsg,
        const char* const wkt)
    {
        GdalEnvironment::Initialize();
        const std::filesystem::path path = directory / fileName;
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
        if (driver == nullptr)
        {
            throw std::runtime_error("GPKG driver is not available");
        }
        GDALDatasetUniquePtr output(driver->Create(
            ToUtf8(path).c_str(), 0, 0, 0, GDT_Unknown, nullptr));
        if (output == nullptr)
        {
            throw std::runtime_error("Cannot create the WKT dataset");
        }
        OGRSpatialReference reference;
        if (reference.importFromEPSG(epsg) != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot create the WKT dataset projection");
        }
        OGRLayer* layer =
            output->CreateLayer("polygons", &reference, wkbPolygon, nullptr);
        if (layer == nullptr)
        {
            throw std::runtime_error("Cannot create the WKT dataset layer");
        }
        const char* cursor = wkt;
        OGRGeometry* geometry = nullptr;
        if (OGRGeometryFactory::createFromWkt(
                &cursor, nullptr, &geometry) != OGRERR_NONE ||
            geometry == nullptr)
        {
            throw std::runtime_error("Cannot parse the polygon WKT");
        }
        OGRFeatureUniquePtr feature(OGRFeature::CreateFeature(layer->GetLayerDefn()));
        const OGRErr result = feature->SetGeometry(geometry);
        OGRGeometryFactory::destroyGeometry(geometry);
        if (result != OGRERR_NONE ||
            layer->CreateFeature(feature.get()) != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot write the WKT polygon");
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
    EXPECT_EQ(scene.layers[0].scene->polygons.size(), 1U);
    EXPECT_EQ(scene.layers[1].scene->polygons.size(), 2U);
    EXPECT_EQ(scene.layers[1].scene->sourceFeatureCount, 2);
    for (const rendering::LayerRenderScene& layer : scene.layers)
    {
        EXPECT_TRUE(layer.scene->fillMeshes.empty());
    }
}

TEST_F(MapRenderSceneTest, KeepsEachLayersOwnExtentAndCoordinates)
{
    _map.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));
    _map.AddLayer(OpenRectangles({{100.0, 50.0, 140.0, 60.0}}));

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(_map);

    ASSERT_EQ(scene.layers.size(), 2U);
    EXPECT_DOUBLE_EQ(scene.layers[0].scene->extent.xMax, 1.0);
    EXPECT_DOUBLE_EQ(scene.layers[1].scene->extent.xMin, 100.0);
    EXPECT_DOUBLE_EQ(scene.layers[1].scene->extent.yMax, 60.0);
    for (const rendering::Point& point : scene.layers[1].scene->polygons.front().front())
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

TEST_F(MapRenderSceneTest, RejectsInvalidOptionsBeforeBuildingLayers)
{
    _map.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));

    EXPECT_THROW(
        (void)rendering::CreateMapRenderScene(
            _map,
            rendering::VectorRenderSceneOptions{.spatialFilter = Extent{}}),
        InvalidArgument);
}

TEST_F(MapRenderSceneTest, LayerThatCannotBeRenderedDoesNotStopTheOthers)
{
    const LayerId below = _map.AddLayer(OpenRectangles({{0.0, 0.0, 1.0, 1.0}}));
    const LayerId lines = _map.AddLayer(VectorDataset::Open(MakeLineDataset(_directory.Path())));
    const LayerId above = _map.AddLayer(OpenRectangles({{2.0, 2.0, 3.0, 3.0}}));

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(_map);

    ASSERT_EQ(scene.layers.size(), 2U);
    EXPECT_EQ(scene.layers[0].layerId, below);
    EXPECT_EQ(scene.layers[1].layerId, above);
    ASSERT_EQ(scene.skippedLayers.size(), 1U);
    EXPECT_EQ(scene.skippedLayers[0].layerId, lines);
    EXPECT_EQ(scene.skippedLayers[0].reason, rendering::LayerSkipReason::Failed);
    EXPECT_EQ(scene.skippedLayers[0].message, "Vector rendering supports polygon layers only");
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

TEST_F(MapRenderSceneTest, ReprojectsSceneCoordinatesAndPreparedMeshes)
{
    const std::shared_ptr<VectorDataset> dataset = VectorDataset::Open(
        MakeProvincesDataset(_directory.Path(), "projected.gpkg"));
    const LayerId id = _map.AddLayer(dataset);
    const Projection source = dataset->Layer(0).Projection();
    const Projection display = Projection::FromEpsg(4326);
    ASSERT_TRUE(_map.SetLayerDisplayProjection(id, display));

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(
        _map,
        rendering::VectorRenderSceneOptions{.prepareFillMeshes = true});

    ASSERT_EQ(scene.layers.size(), 1U);
    ASSERT_EQ(scene.skippedLayers.size(), 0U);
    const rendering::VectorRenderScene& projected = *scene.layers.front().scene;
    ASSERT_FALSE(projected.polygons.empty());
    ASSERT_EQ(projected.fillMeshes.size(), projected.polygons.size());
    const ProjectionTransform transform = source.TransformTo(display);
    double x = Provinces::kMinX;
    double y = Provinces::kMinY;
    ASSERT_TRUE(transform(x, y));
    EXPECT_NEAR(projected.polygons.front().front()[0].x, x, 1e-10);
    EXPECT_NEAR(projected.polygons.front().front()[0].y, y, 1e-10);
    ASSERT_TRUE(projected.fillMeshes.front().has_value());
    ASSERT_FALSE(projected.fillMeshes.front()->vertices.empty());
    for (const rendering::Point& point :
         projected.fillMeshes.front()->vertices)
    {
        EXPECT_TRUE(projected.extent.Contains(point.x, point.y));
        EXPECT_LT(std::abs(point.x), 180.0);
        EXPECT_LT(std::abs(point.y), 90.0);
    }
    EXPECT_TRUE(projected.extent.Contains(
        projected.polygons.front().front()[0].x,
        projected.polygons.front().front()[0].y));
}

TEST_F(MapRenderSceneTest, RebuildsFillMeshesAfterNonlinearReprojection)
{
    const std::shared_ptr<VectorDataset> dataset = VectorDataset::Open(
        MakeWktDataset(
            _directory.Path(),
            "concave-after-reprojection.gpkg",
            4326,
            "POLYGON ((0 0, 170 85, 100 80, 50 55, 0 0))"));
    const LayerId id = _map.AddLayer(dataset);
    ASSERT_TRUE(_map.SetLayerDisplayProjection(id, Projection::FromEpsg(3857)));

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(
        _map,
        rendering::VectorRenderSceneOptions{.prepareFillMeshes = true});

    ASSERT_EQ(scene.layers.size(), 1U);
    ASSERT_TRUE(scene.skippedLayers.empty());
    const rendering::VectorRenderScene& projected = *scene.layers.front().scene;
    ASSERT_EQ(projected.fillMeshes.size(), 1U);
    ASSERT_TRUE(projected.fillMeshes.front().has_value());
    const rendering::TriangleMesh& mesh = *projected.fillMeshes.front();
    ASSERT_FALSE(mesh.indices.empty());
    ASSERT_EQ(mesh.indices.size() % 3U, 0U);
    for (std::size_t index = 0; index < mesh.indices.size(); index += 3)
    {
        const rendering::Point& a = mesh.vertices[mesh.indices[index]];
        const rendering::Point& b = mesh.vertices[mesh.indices[index + 1]];
        const rendering::Point& c = mesh.vertices[mesh.indices[index + 2]];
        const double twiceArea =
            (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        EXPECT_GT(twiceArea, 0.0);
    }
}

TEST_F(MapRenderSceneTest, SplitsWrappedProjectedPolygonsAtTheLongitudeSeam)
{
    const std::shared_ptr<VectorDataset> dataset = VectorDataset::Open(
        MakeWktDataset(
            _directory.Path(),
            "antimeridian.gpkg",
            3857,
            "POLYGON ("
            "(18924313 -1000000, 21150790 -1000000, "
            "21150790 1000000, 18924313 1000000, 18924313 -1000000), "
            "(19370000 -250000, 19600000 -250000, "
            "19600000 250000, 19370000 250000, 19370000 -250000))"));
    const LayerId id = _map.AddLayer(dataset);
    ASSERT_TRUE(_map.SetLayerDisplayProjection(id, Projection::FromEpsg(4326)));

    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(
        _map,
        rendering::VectorRenderSceneOptions{.prepareFillMeshes = true});

    ASSERT_EQ(scene.layers.size(), 1U);
    ASSERT_TRUE(scene.skippedLayers.empty());
    const rendering::VectorRenderScene& projected = *scene.layers.front().scene;
    ASSERT_EQ(projected.polygons.size(), 2U);
    std::size_t ringCount = 0;
    for (std::size_t index = 0; index < projected.polygons.size(); ++index)
    {
        const rendering::PolygonView polygon = projected.polygons[index];
        ringCount += polygon.size();
        for (std::size_t ringIndex = 0; ringIndex < polygon.size(); ++ringIndex)
        {
            for (const rendering::Point& point : polygon[ringIndex])
            {
                EXPECT_GT(std::abs(point.x), 169.0);
                EXPECT_LT(std::abs(point.x), 180.000001);
            }
        }
    }
    EXPECT_EQ(ringCount, 3U);
    ASSERT_EQ(projected.fillMeshes.size(), 2U);
    for (const std::optional<rendering::TriangleMesh>& mesh : projected.fillMeshes)
    {
        ASSERT_TRUE(mesh.has_value());
        for (const rendering::Point& point : mesh->vertices)
        {
            EXPECT_GT(std::abs(point.x), 169.0);
            EXPECT_LT(std::abs(point.x), 180.000001);
        }
    }
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
    const auto* polygons = scene.layers[0].scene->polygons.Coordinates().data();
    EXPECT_EQ(view.Scene().layers[0].scene->polygons.Coordinates().data(), polygons);

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
    EXPECT_EQ(scene.layers[0].scene->polygons.front().front().size(), 5U);
}
