// SPDX-License-Identifier: MPL-2.0

/**
 * @file RenderingBaselineTests.cpp
 * @brief Measures an opt-in timing and memory baseline for the polygon render path.
 *
 * Generates a GeoPackage polygon grid and separately times Core dataset access,
 * render-scene preparation, GDI frames, and OpenGL first and steady frames.
 * Results are printed and recorded as GoogleTest properties; the test only
 * fails when a stage fails, never on a timing threshold.
 */
#include <Windows.h>
#include <psapi.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "OpenGlVectorLayerRenderer.h"
#include "GdiVectorLayerRenderer.h"
#include "TestData.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/rendering/VectorRenderScene.h"
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
    std::unique_ptr<VectorDataset> dataset = VectorDataset::Open(path);
    VectorLayer& layer = dataset->Layer(0);
    const double openMs = ElapsedMilliseconds(start);

    start = Clock::now();
    const rendering::VectorRenderScene scene = rendering::CreateVectorRenderScene(layer);
    const double sceneMs = ElapsedMilliseconds(start);
    const std::optional<double> memoryAfterScene = PrivateMiB(memoryError);
    ASSERT_TRUE(memoryAfterScene.has_value()) << "GetProcessMemoryInfo failed: " << memoryError;
    ASSERT_EQ(scene.polygons.size(), static_cast<std::size_t>(featureCount));

    DibSurface surface(kTargetWidth, kTargetHeight);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, kTargetWidth, kTargetHeight};

    activex::GdiVectorLayerRenderer gdi;
    std::vector<double> gdiFrames;
    for (int frame = 0; frame < kSteadyFrames; ++frame)
    {
        start = Clock::now();
        ASSERT_EQ(gdi.Draw(surface.Get(), bounds, scene), S_OK);
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
    const HRESULT firstFrame = openGl.Draw(surface.Get(), bounds, scene);
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
        ASSERT_EQ(openGl.Draw(surface.Get(), bounds, scene), S_OK);
        openGlFrames.push_back(ElapsedMilliseconds(start));
    }

    Report("OpenGL first frame (context+draw)", firstOpenGlMs, "ms");
    Report("OpenGL frame (median)", Median(openGlFrames), "ms");
    const std::optional<double> memoryAfterOpenGl = PrivateMiB(memoryError);
    ASSERT_TRUE(memoryAfterOpenGl.has_value()) << "GetProcessMemoryInfo failed: " << memoryError;
    Report("Private memory increase total", *memoryAfterOpenGl - *memoryBefore, "MiB");
}
