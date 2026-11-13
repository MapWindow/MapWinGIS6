// SPDX-License-Identifier: MPL-2.0

/**
 * @file RenderingBaselineTests.cpp
 * @brief Measures an opt-in timing and memory baseline for the polygon render path.
 *
 * Generates a GeoPackage polygon grid and separately times Core dataset access,
 * render-scene preparation, GDI frames, and OpenGL first and steady frames.
 * A second scenario generates concave, holed multipart, and many-vertex star
 * polygons so the triangulation and mesh-storage costs of complex geometry
 * are measured too. Results are printed and recorded as GoogleTest
 * properties; the tests only fail when a stage fails, never on a timing
 * threshold.
 */
#include <Windows.h>
#include <psapi.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "OpenGlVectorLayerRenderer.h"
#include "GdiVectorLayerRenderer.h"
#include "TestData.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/map/Map.h"
#include "mapwingis/map/Viewport.h"
#include "mapwingis/rendering/MapRenderScene.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    constexpr int kDefaultFeatureCount = 50000;
    constexpr int kVerticesPerPolygon = 32;
    constexpr int kTargetWidth = 1280;
    constexpr int kTargetHeight = 720;
    constexpr int kSteadyFrames = 5;

    using Clock = std::chrono::steady_clock;

    [[nodiscard]] double ElapsedMilliseconds(const Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    [[nodiscard]] double Median(std::vector<double> values)
    {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    /// Returns the process private usage, or an empty value with the Win32
    /// error code reported through @p error when it cannot be queried.
    [[nodiscard]] std::optional<double> PrivateMiB(DWORD& error)
    {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(
                GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                sizeof(counters)))
        {
            error = GetLastError();
            return std::nullopt;
        }
        return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
    }

    [[nodiscard]] int FeatureCountFromEnvironment()
    {
        const char* value = std::getenv("MWG_RENDERING_BASELINE_FEATURES");
        if (value == nullptr)
        {
            return kDefaultFeatureCount;
        }
        const int count = std::atoi(value);
        return count > 0 ? count : kDefaultFeatureCount;
    }

    /// Writes a square grid of convex 32-vertex polygons so the whole scene
    /// is eligible for the OpenGL backend.
    std::filesystem::path MakePolygonGrid(const std::filesystem::path& directory, const int count)
    {
        GdalEnvironment::Initialize();
        const std::filesystem::path path = directory / "grid.gpkg";
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
        if (driver == nullptr)
        {
            throw std::runtime_error("GPKG driver is not available");
        }

        const std::u8string utf8 = path.u8string();
        GDALDatasetUniquePtr dataset(driver->Create(
            reinterpret_cast<const char*>(utf8.c_str()), 0, 0, 0, GDT_Unknown, nullptr));
        if (dataset == nullptr)
        {
            throw std::runtime_error("Cannot create the baseline dataset");
        }

        OGRLayer* layer = dataset->CreateLayer("grid", nullptr, wkbPolygon, nullptr);
        if (layer == nullptr || dataset->StartTransaction() != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot create the baseline layer");
        }

        const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
        for (int index = 0; index < count; ++index)
        {
            const double centerX = static_cast<double>(index % columns);
            const double centerY = static_cast<double>(index / columns);
            OGRLinearRing ring;
            for (int vertex = 0; vertex < kVerticesPerPolygon; ++vertex)
            {
                const double angle = 2.0 * std::numbers::pi * vertex / kVerticesPerPolygon;
                ring.addPoint(centerX + 0.4 * std::cos(angle), centerY + 0.4 * std::sin(angle));
            }
            ring.closeRings();
            OGRPolygon polygon;
            polygon.addRing(&ring);

            OGRFeature feature(layer->GetLayerDefn());
            if (feature.SetGeometry(&polygon) != OGRERR_NONE ||
                layer->CreateFeature(&feature) != OGRERR_NONE)
            {
                throw std::runtime_error("Cannot write a baseline feature");
            }
        }

        if (dataset->CommitTransaction() != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot commit the baseline dataset");
        }
        return path;
    }

    /// Adds a closed ring to a polygon from (x, y) coordinate pairs.
    void AddRing(OGRPolygon& polygon, std::initializer_list<std::pair<double, double>> points)
    {
        OGRLinearRing ring;
        for (const auto& [x, y] : points)
        {
            ring.addPoint(x, y);
        }
        ring.closeRings();
        polygon.addRing(&ring);
    }

    /// Writes three polygon layers that exercise triangulation with complex
    /// geometry: a grid of small concave L-shaped polygons, a grid of
    /// holed rectangles where every feature is a two-component multipolygon,
    /// and a sparser grid of concave 32-vertex star polygons.
    std::filesystem::path MakeComplexPolygonDataset(
        const std::filesystem::path& directory,
        const int count)
    {
        GdalEnvironment::Initialize();
        const std::filesystem::path path = directory / "complex.gpkg";
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
        if (driver == nullptr)
        {
            throw std::runtime_error("GPKG driver is not available");
        }

        const std::u8string utf8 = path.u8string();
        GDALDatasetUniquePtr dataset(driver->Create(
            reinterpret_cast<const char*>(utf8.c_str()), 0, 0, 0, GDT_Unknown, nullptr));
        if (dataset == nullptr)
        {
            throw std::runtime_error("Cannot create the complex baseline dataset");
        }

        const auto makeLayer = [&](const char* name, const OGRwkbGeometryType type) {
            OGRLayer* layer = dataset->CreateLayer(name, nullptr, type, nullptr);
            if (layer == nullptr || dataset->StartTransaction() != OGRERR_NONE)
            {
                throw std::runtime_error("Cannot create a complex baseline layer");
            }
            return layer;
        };

        // Many small concave polygons.
        OGRLayer* concave = makeLayer("concave", wkbPolygon);
        const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
        for (int index = 0; index < count; ++index)
        {
            const double cx = static_cast<double>(index % columns);
            const double cy = static_cast<double>(index / columns);
            OGRPolygon polygon;
            AddRing(polygon,
                    {{cx - 0.4, cy - 0.4},
                     {cx + 0.4, cy - 0.4},
                     {cx + 0.4, cy},
                     {cx, cy},
                     {cx, cy + 0.4},
                     {cx - 0.4, cy + 0.4}});
            OGRFeature feature(concave->GetLayerDefn());
            if (feature.SetGeometry(&polygon) != OGRERR_NONE ||
                concave->CreateFeature(&feature) != OGRERR_NONE)
            {
                throw std::runtime_error("Cannot write a concave baseline feature");
            }
        }
        if (dataset->CommitTransaction() != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot commit the concave baseline layer");
        }

        // Holed rectangles, every one a two-component multipart geometry.
        OGRLayer* holed = makeLayer("holed", wkbMultiPolygon);
        for (int index = 0; index < count; ++index)
        {
            const double cx = static_cast<double>(index % columns);
            const double cy = static_cast<double>(index / columns);
            OGRPolygon body;
            AddRing(body,
                    {{cx - 0.4, cy - 0.4},
                     {cx + 0.4, cy - 0.4},
                     {cx + 0.4, cy + 0.4},
                     {cx - 0.4, cy + 0.4},
                     {cx - 0.4, cy - 0.4}});
            AddRing(body,
                    {{cx - 0.15, cy - 0.15},
                     {cx + 0.15, cy - 0.15},
                     {cx + 0.15, cy + 0.15},
                     {cx - 0.15, cy + 0.15},
                     {cx - 0.15, cy - 0.15}});
            // Keep the satellite inside the gap between adjacent bodies
            // (bodies extend to +-0.4 on a 1.0 grid) so the multipolygon
            // components stay disjoint and the geometry remains valid.
            OGRPolygon satellite;
            AddRing(satellite,
                    {{cx + 0.41, cy + 0.41},
                     {cx + 0.59, cy + 0.41},
                     {cx + 0.59, cy + 0.59},
                     {cx + 0.41, cy + 0.59},
                     {cx + 0.41, cy + 0.41}});
            OGRMultiPolygon multipart;
            multipart.addGeometry(&body);
            multipart.addGeometry(&satellite);
            if (!multipart.IsValid())
            {
                throw std::runtime_error("Cannot build a valid holed baseline multipart");
            }
            OGRFeature feature(holed->GetLayerDefn());
            if (feature.SetGeometry(&multipart) != OGRERR_NONE ||
                holed->CreateFeature(&feature) != OGRERR_NONE)
            {
                throw std::runtime_error("Cannot write a holed baseline feature");
            }
        }
        if (dataset->CommitTransaction() != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot commit the holed baseline layer");
        }

        // Larger concave polygons with many vertices.
        const int starCount = std::max(1, count / 4);
        const int starColumns =
            static_cast<int>(std::ceil(std::sqrt(static_cast<double>(starCount))));
        OGRLayer* stars = makeLayer("stars", wkbPolygon);
        for (int index = 0; index < starCount; ++index)
        {
            const double cx = static_cast<double>(index % starColumns) * 4.0;
            const double cy = static_cast<double>(index / starColumns) * 4.0;
            OGRLinearRing ring;
            for (int vertex = 0; vertex < kVerticesPerPolygon; ++vertex)
            {
                const double angle = 2.0 * std::numbers::pi * vertex / kVerticesPerPolygon;
                const double radius = vertex % 2 == 0 ? 1.6 : 0.7;
                ring.addPoint(cx + radius * std::cos(angle), cy + radius * std::sin(angle));
            }
            ring.closeRings();
            OGRPolygon polygon;
            polygon.addRing(&ring);
            OGRFeature feature(stars->GetLayerDefn());
            if (feature.SetGeometry(&polygon) != OGRERR_NONE ||
                stars->CreateFeature(&feature) != OGRERR_NONE)
            {
                throw std::runtime_error("Cannot write a star baseline feature");
            }
        }
        if (dataset->CommitTransaction() != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot commit the star baseline layer");
        }
        return path;
    }

    /// A 32-bit DIB section target that does not depend on the screen DC.
    class DibSurface final
    {
    public:
        DibSurface(const int width, const int height) noexcept
        {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            _memory = CreateCompatibleDC(nullptr);
            void* bits = nullptr;
            _bitmap = CreateDIBSection(_memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (_memory != nullptr && _bitmap != nullptr)
            {
                _previous = SelectObject(_memory, _bitmap);
            }
        }

        ~DibSurface()
        {
            if (_previous != nullptr && _previous != HGDI_ERROR)
            {
                SelectObject(_memory, _previous);
            }
            if (_bitmap != nullptr)
            {
                DeleteObject(_bitmap);
            }
            if (_memory != nullptr)
            {
                DeleteDC(_memory);
            }
        }

        DibSurface(const DibSurface&) = delete;
        DibSurface& operator=(const DibSurface&) = delete;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return _previous != nullptr && _previous != HGDI_ERROR;
        }

        [[nodiscard]] HDC Get() const noexcept { return _memory; }

    private:
        HDC _memory{};
        HBITMAP _bitmap{};
        HGDIOBJ _previous{};
    };

    class RenderingBaseline : public testing::Test
    {
    protected:
        void Report(const std::string& name, const double value, const char* unit)
        {
            std::cout << "  " << std::left << std::setw(36) << name << std::right
                      << std::setw(12) << std::fixed << std::setprecision(1) << value
                      << ' ' << unit << '\n';
            RecordProperty(name, std::to_string(value));
        }
    };
}

TEST_F(RenderingBaseline, MeasuresCoreSceneAndBackendCosts)
{
    const char* enabled = std::getenv("MWG_RUN_RENDERING_BASELINE");
    if (enabled == nullptr || std::string(enabled) != "1")
    {
        GTEST_SKIP() << "Set MWG_RUN_RENDERING_BASELINE=1 to measure the rendering baseline.";
    }

    const int featureCount = FeatureCountFromEnvironment();
    TempDirectory directory;
    const std::filesystem::path path = MakePolygonGrid(directory.Path(), featureCount);

    DWORD memoryError = ERROR_SUCCESS;
    const std::optional<double> memoryBefore = PrivateMiB(memoryError);
    ASSERT_TRUE(memoryBefore.has_value()) << "GetProcessMemoryInfo failed: " << memoryError;
    Clock::time_point start = Clock::now();
    Map map;
    map.AddLayer(VectorDataset::Open(path));
    const double openMs = ElapsedMilliseconds(start);

    start = Clock::now();
    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(map);
    const double sceneMs = ElapsedMilliseconds(start);
    const std::optional<double> memoryAfterScene = PrivateMiB(memoryError);
    ASSERT_TRUE(memoryAfterScene.has_value()) << "GetProcessMemoryInfo failed: " << memoryError;
    ASSERT_EQ(scene.layers.size(), 1U);
    ASSERT_EQ(scene.layers.front().scene.polygons.size(), static_cast<std::size_t>(featureCount));

    DibSurface surface(kTargetWidth, kTargetHeight);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, kTargetWidth, kTargetHeight};
    Viewport viewport(kTargetWidth, kTargetHeight);
    ASSERT_TRUE(viewport.FitExtent(map.GetExtent(), 12.0));

    activex::GdiVectorLayerRenderer gdi;
    std::vector<double> gdiFrames;
    for (int frame = 0; frame < kSteadyFrames; ++frame)
    {
        start = Clock::now();
        ASSERT_EQ(gdi.Draw(surface.Get(), bounds, scene, viewport), S_OK);
        gdiFrames.push_back(ElapsedMilliseconds(start));
    }

    std::cout << "Rendering baseline: " << featureCount << " convex polygons x "
              << kVerticesPerPolygon << " vertices, " << kTargetWidth << 'x'
              << kTargetHeight << " target\n";
    Report("Core open dataset", openMs, "ms");
    Report("Core prepare render scene", sceneMs, "ms");
    Report("Private memory increase for scene", *memoryAfterScene - *memoryBefore, "MiB");
    Report("GDI frame (median)", Median(gdiFrames), "ms");

    activex::OpenGlVectorLayerRenderer openGl;
    start = Clock::now();
    const HRESULT firstFrame = openGl.Draw(surface.Get(), bounds, scene, viewport);
    const double firstOpenGlMs = ElapsedMilliseconds(start);
    if (firstFrame == S_FALSE)
    {
        std::wcout << L"  OpenGL unavailable: " << openGl.GetLastFailureReason() << L'\n';
        return;
    }
    ASSERT_EQ(firstFrame, S_OK);

    std::vector<double> openGlFrames;
    for (int frame = 0; frame < kSteadyFrames; ++frame)
    {
        start = Clock::now();
        ASSERT_EQ(openGl.Draw(surface.Get(), bounds, scene, viewport), S_OK);
        openGlFrames.push_back(ElapsedMilliseconds(start));
    }

    Report("OpenGL first frame (context+draw)", firstOpenGlMs, "ms");
    Report("OpenGL frame (median)", Median(openGlFrames), "ms");
    const std::optional<double> memoryAfterOpenGl = PrivateMiB(memoryError);
    ASSERT_TRUE(memoryAfterOpenGl.has_value()) << "GetProcessMemoryInfo failed: " << memoryError;
    Report("Private memory increase total", *memoryAfterOpenGl - *memoryBefore, "MiB");
}

TEST_F(RenderingBaseline, MeasuresComplexPolygonCosts)
{
    const char* enabled = std::getenv("MWG_RUN_RENDERING_BASELINE");
    if (enabled == nullptr || std::string(enabled) != "1")
    {
        GTEST_SKIP() << "Set MWG_RUN_RENDERING_BASELINE=1 to measure the rendering baseline.";
    }

    const int featureCount = FeatureCountFromEnvironment();
    TempDirectory directory;
    const std::filesystem::path path =
        MakeComplexPolygonDataset(directory.Path(), featureCount);

    DWORD memoryError = ERROR_SUCCESS;
    const std::optional<double> memoryBefore = PrivateMiB(memoryError);
    ASSERT_TRUE(memoryBefore.has_value()) << "GetProcessMemoryInfo failed: " << memoryError;
    Clock::time_point start = Clock::now();
    Map map;
    const std::shared_ptr<VectorDataset> dataset = VectorDataset::Open(path);
    map.AddLayer(dataset, 0);
    map.AddLayer(dataset, 1);
    map.AddLayer(dataset, 2);
    const double openMs = ElapsedMilliseconds(start);

    start = Clock::now();
    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(map);
    const double sceneMs = ElapsedMilliseconds(start);
    const std::optional<double> memoryAfterScene = PrivateMiB(memoryError);
    ASSERT_TRUE(memoryAfterScene.has_value()) << "GetProcessMemoryInfo failed: " << memoryError;
    ASSERT_EQ(scene.layers.size(), 3U);
    ASSERT_TRUE(scene.skippedLayers.empty());

    std::size_t polygonCount = 0;
    std::size_t meshedCount = 0;
    std::size_t meshVertexCount = 0;
    std::size_t meshIndexCount = 0;
    for (const rendering::LayerRenderScene& layer : scene.layers)
    {
        ASSERT_EQ(layer.scene.fillMeshes.size(), layer.scene.polygons.size());
        polygonCount += layer.scene.polygons.size();
        for (const std::optional<rendering::TriangleMesh>& mesh : layer.scene.fillMeshes)
        {
            ASSERT_TRUE(mesh.has_value())
                << "Every complex baseline polygon must triangulate";
            ++meshedCount;
            meshVertexCount += mesh->vertices.size();
            meshIndexCount += mesh->indices.size();
        }
    }
    const double meshMiB =
        (meshVertexCount * sizeof(rendering::Point) +
         meshIndexCount * sizeof(std::uint32_t)) /
        (1024.0 * 1024.0);
    ASSERT_EQ(meshedCount, polygonCount);

    DibSurface surface(kTargetWidth, kTargetHeight);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, kTargetWidth, kTargetHeight};
    Viewport viewport(kTargetWidth, kTargetHeight);
    ASSERT_TRUE(viewport.FitExtent(map.GetExtent(), 12.0));

    activex::GdiVectorLayerRenderer gdi;
    std::vector<double> gdiFrames;
    for (int frame = 0; frame < kSteadyFrames; ++frame)
    {
        start = Clock::now();
        ASSERT_EQ(gdi.Draw(surface.Get(), bounds, scene, viewport), S_OK);
        gdiFrames.push_back(ElapsedMilliseconds(start));
    }

    std::cout << "Complex polygon baseline: " << featureCount << " concave + "
              << featureCount << " holed multipart + " << std::max(1, featureCount / 4)
              << " star polygons (" << meshedCount << " triangulated), " << kTargetWidth
              << 'x' << kTargetHeight << " target\n";
    Report("Polygons with fill mesh", static_cast<double>(meshedCount), "");
    Report("Core open dataset", openMs, "ms");
    Report("Core prepare render scene", sceneMs, "ms");
    Report("Mesh storage (vertices+indices)", meshMiB, "MiB");
    Report("Private memory increase for scene", *memoryAfterScene - *memoryBefore, "MiB");
    Report("GDI frame (median)", Median(gdiFrames), "ms");

    activex::OpenGlVectorLayerRenderer openGl;
    start = Clock::now();
    const HRESULT firstFrame = openGl.Draw(surface.Get(), bounds, scene, viewport);
    const double firstOpenGlMs = ElapsedMilliseconds(start);
    if (firstFrame == S_FALSE)
    {
        std::wcout << L"  OpenGL unavailable: " << openGl.GetLastFailureReason() << L'\n';
        return;
    }
    ASSERT_EQ(firstFrame, S_OK);

    std::vector<double> openGlFrames;
    for (int frame = 0; frame < kSteadyFrames; ++frame)
    {
        start = Clock::now();
        ASSERT_EQ(openGl.Draw(surface.Get(), bounds, scene, viewport), S_OK);
        openGlFrames.push_back(ElapsedMilliseconds(start));
    }

    Report("OpenGL first frame (context+draw)", firstOpenGlMs, "ms");
    Report("OpenGL frame (median)", Median(openGlFrames), "ms");
    const std::optional<double> memoryAfterOpenGl = PrivateMiB(memoryError);
    ASSERT_TRUE(memoryAfterOpenGl.has_value()) << "GetProcessMemoryInfo failed: " << memoryError;
    Report("Private memory increase total", *memoryAfterOpenGl - *memoryBefore, "MiB");
}
