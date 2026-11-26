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

#include <atlbase.h>
#include <objbase.h>
#include <wrl/client.h>

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
#include <string_view>
#include <utility>
#include <vector>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "OpenGlVectorLayerRenderer.h"
#include "GdiVectorLayerRenderer.h"
#include "MapWinGIS.ActiveX_i.h"
#include "TestData.h"
#include "mapwingis/diagnostics/PerformanceDiagnostics.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/map/Map.h"
#include "mapwingis/map/Viewport.h"
#include "mapwingis/rendering/MapRenderScene.h"
#include "mapwingis/rendering/PolygonTriangulation.h"
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
    constexpr COLORREF kFillColor = RGB(61, 122, 169);

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

    [[nodiscard]] std::filesystem::path BaselineDatasetPath(
        const std::filesystem::path& directory,
        const int generatedFeatureCount)
    {
        const char* const configuredPath =
            std::getenv("MWG_RENDERING_BASELINE_DATASET");
        if (configuredPath == nullptr || configuredPath[0] == '\0')
        {
            return MakePolygonGrid(directory, generatedFeatureCount);
        }

        const std::filesystem::path path(configuredPath);
        if (!std::filesystem::exists(path))
        {
            throw std::runtime_error(
                "MWG_RENDERING_BASELINE_DATASET does not exist: " + path.string());
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

    /// Initializes COM on the calling thread for the object's lifetime.
    class ComApartment final
    {
    public:
        ComApartment() noexcept
            : _result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
        {
        }

        ~ComApartment()
        {
            if (SUCCEEDED(_result))
            {
                CoUninitialize();
            }
        }

        ComApartment(const ComApartment&) = delete;
        ComApartment& operator=(const ComApartment&) = delete;

        [[nodiscard]] HRESULT Result() const noexcept { return _result; }

    private:
        HRESULT _result;
    };

    /// Creates the ActiveX control through its class factory, without
    /// registry registration, exactly like a COM host activates it. The
    /// rendering backend preference stays at its Automatic default.
    class HostedControl final
    {
    public:
        HostedControl()
        {
            wchar_t executablePath[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
            if (length == 0 || length >= MAX_PATH)
            {
                return;
            }

            _module = LoadLibraryW(
                (std::filesystem::path(executablePath).parent_path() / L"MapWinGIS.ocx").c_str());
            if (_module == nullptr)
            {
                return;
            }

            using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
            const auto getClassObject = reinterpret_cast<GetClassObject>(
                GetProcAddress(_module, "DllGetClassObject"));
            Microsoft::WRL::ComPtr<IClassFactory> factory;
            if (getClassObject == nullptr ||
                FAILED(getClassObject(
                    __uuidof(MapWinGISControl), IID_PPV_ARGS(&factory))) ||
                FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&_control))) ||
                FAILED(_control.As(&_window)) ||
                FAILED(_control.As(&_viewObject)))
            {
                return;
            }
            _created = true;
        }

        ~HostedControl()
        {
            _viewObject.Reset();
            _window.Reset();
            _control.Reset();
            if (_module != nullptr)
            {
                // COM checks DllCanUnloadNow before unloading. An in-flight
                // asynchronous load may still own a worker or the dispatch
                // window; in that case leave the module loaded for the
                // process lifetime rather than crashing a later test.
                using DllCanUnloadNow = HRESULT(STDAPICALLTYPE*)();
                const auto canUnload = reinterpret_cast<DllCanUnloadNow>(
                    GetProcAddress(_module, "DllCanUnloadNow"));
                if (canUnload == nullptr || canUnload() == S_OK)
                {
                    FreeLibrary(_module);
                }
            }
        }

        HostedControl(const HostedControl&) = delete;
        HostedControl& operator=(const HostedControl&) = delete;

        [[nodiscard]] bool IsCreated() const noexcept { return _created; }

        [[nodiscard]] const Microsoft::WRL::ComPtr<IMapWinGISControl5>& Control() const noexcept
        {
            return _control;
        }

        [[nodiscard]] HRESULT Resize(const int width, const int height)
        {
            LRESULT result = 0;
            return _window->OnWindowMessage(
                WM_SIZE, 0, MAKELPARAM(width, height), &result);
        }

        [[nodiscard]] HRESULT Draw(
            const HDC target, const int width, const int height)
        {
            const RECTL bounds{0, 0, width, height};
            return _viewObject->Draw(
                DVASPECT_CONTENT, -1, nullptr, nullptr, nullptr, target,
                &bounds, nullptr, nullptr, 0);
        }

    private:
        HMODULE _module{};
        Microsoft::WRL::ComPtr<IMapWinGISControl5> _control;
        Microsoft::WRL::ComPtr<IOleInPlaceObjectWindowless> _window;
        Microsoft::WRL::ComPtr<IViewObjectEx> _viewObject;
        bool _created = false;
    };

    /// Drains the thread's dispatch queue; used to deliver the asynchronous
    /// vector-load completion while polling EndLoadVectorDataset.
    void PumpDispatchMessages()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (message.message == WM_QUIT)
            {
                PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

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

        void ReportPerformanceDiagnostics()
        {
            using diagnostics::PerformanceCounter;
            using diagnostics::PerformanceDiagnostics;
            using diagnostics::PerformanceOperation;

            constexpr std::pair<PerformanceOperation, const char*> operations[] = {
                {PerformanceOperation::DatasetOpen, "Diagnostic dataset open"},
                {PerformanceOperation::LayerMetadataRead, "Diagnostic layer metadata"},
                {PerformanceOperation::FeatureGeometryRead, "Diagnostic feature geometry read"},
                {PerformanceOperation::GeometryConversion, "Diagnostic geometry conversion"},
                {PerformanceOperation::SceneBuild, "Diagnostic scene build"},
                {PerformanceOperation::GeometryValidation, "Diagnostic geometry validation"},
                {PerformanceOperation::PolygonTriangulation, "Diagnostic polygon triangulation"},
                {PerformanceOperation::OpenGlGeometrySubmission, "Diagnostic OpenGL geometry submission"},
                {PerformanceOperation::OpenGlGpuBufferUpload, "Diagnostic OpenGL GPU buffer build+upload"},
                {PerformanceOperation::OpenGlContextInitialization, "Diagnostic OpenGL context initialization"},
                {PerformanceOperation::FrameRender, "Diagnostic complete frame render"},
                {PerformanceOperation::FramebufferReadback, "Diagnostic framebuffer readback"}};
            constexpr std::pair<PerformanceCounter, const char*> counters[] = {
                {PerformanceCounter::FeaturesProcessed, "Diagnostic features processed"},
                {PerformanceCounter::GeometriesLoaded, "Diagnostic geometries loaded"},
                {PerformanceCounter::PolygonsTriangulated, "Diagnostic polygons triangulated"},
                {PerformanceCounter::TrianglesGenerated, "Diagnostic triangles generated"},
                {PerformanceCounter::InvalidGeometries, "Diagnostic invalid geometries"},
                {PerformanceCounter::TriangulationFailures, "Diagnostic triangulation failures"},
                {PerformanceCounter::FeaturesSkippedOutsideViewport,
                 "Diagnostic features outside viewport"},
                {PerformanceCounter::SceneCacheHits, "Diagnostic scene cache hits"},
                {PerformanceCounter::SceneCacheMisses, "Diagnostic scene cache misses"}};

            const diagnostics::PerformanceSnapshot snapshot =
                PerformanceDiagnostics::Snapshot();
            for (const auto& [operation, name] : operations)
            {
                const diagnostics::PerformanceTiming& timing = snapshot.Timing(operation);
                Report(name, timing.DurationMilliseconds(), "ms");
                Report((std::string(name) + " calls").c_str(), static_cast<double>(timing.callCount), "");
            }
            for (const auto& [counter, name] : counters)
            {
                Report(name, static_cast<double>(snapshot.Counter(counter)), "");
            }
        }

        /// Draws the first frame through the hosted control. Background mesh
        /// preparation may still be pending (GDI) or already published
        /// (OpenGL), but the first frame must present filled content. Reports
        /// the load time, first-frame time, and diagnostic breakdown.
        void ExpectGdiFirstFrameAndReport(
            HostedControl& hosted,
            const std::string& loadName,
            const double loadMs,
            const HDC surface)
        {
            Microsoft::WRL::ComPtr<IMapWinGISControl2> rendering;
            ASSERT_EQ(hosted.Control().As(&rendering), S_OK);

            ASSERT_EQ(hosted.Resize(kTargetWidth, kTargetHeight), S_OK);

            Clock::time_point start = Clock::now();
            ASSERT_EQ(hosted.Draw(surface, kTargetWidth, kTargetHeight), S_OK);
            const double firstFrameMs = ElapsedMilliseconds(start);

            ATL::CComBSTR backend;
            ASSERT_EQ(rendering->get_RenderingBackend(&backend), S_OK);
            ASSERT_NE(backend.m_str, nullptr);
            EXPECT_TRUE(
                std::wstring_view(backend.m_str) == L"GDI" ||
                std::wstring_view(backend.m_str) == L"OpenGL");

            const diagnostics::PerformanceSnapshot snapshot =
                diagnostics::PerformanceDiagnostics::Snapshot();
            const std::int64_t triangulated = snapshot.Counter(
                diagnostics::PerformanceCounter::PolygonsTriangulated);
            if (std::wstring_view(backend.m_str) == L"OpenGL")
            {
                EXPECT_GT(triangulated, 0)
                    << "OpenGL fill rendering requires a prepared mesh";
            }
            EXPECT_EQ(
                snapshot.Counter(
                    diagnostics::PerformanceCounter::TriangulationFailures),
                0);

            std::size_t paintedSamples = 0;
            for (int y = 0; y < kTargetHeight; y += 16)
            {
                for (int x = 0; x < kTargetWidth; x += 16)
                {
                    paintedSamples += GetPixel(surface, x, y) == kFillColor;
                }
            }
            EXPECT_GT(paintedSamples, 0U)
                << "the first frame must present polygon fills";

            std::wcout << L"End-to-end first frame: " << kTargetWidth << L'x' << kTargetHeight
                       << L" target, active backend "
                       << static_cast<const wchar_t*>(backend.m_str) << L'\n';
            Report(loadName, loadMs, "ms");
            Report("End-to-end first presented frame", firstFrameMs, "ms");
            Report(
                "End-to-end polygons triangulated by the first frame",
                static_cast<double>(triangulated),
                "");
            ReportPerformanceDiagnostics();
        }

        class DiagnosticCapture final
        {
        public:
            DiagnosticCapture()
            {
                diagnostics::PerformanceDiagnostics::SetEnabled(false);
                diagnostics::PerformanceDiagnostics::Reset();
                diagnostics::PerformanceDiagnostics::SetEnabled(true);
            }

            ~DiagnosticCapture()
            {
                diagnostics::PerformanceDiagnostics::SetEnabled(false);
                diagnostics::PerformanceDiagnostics::Reset();
            }
        };
    };
}

TEST_F(RenderingBaseline, MeasuresTriangulationDisabledCosts)
{
    const char* enabled = std::getenv("MWG_RUN_RENDERING_BASELINE");
    if (enabled == nullptr || std::string(enabled) != "1")
    {
        GTEST_SKIP() << "Set MWG_RUN_RENDERING_BASELINE=1 to measure the rendering baseline.";
    }

    DiagnosticCapture diagnostics;
    const int featureCount = FeatureCountFromEnvironment();
    TempDirectory directory;
    const std::filesystem::path path =
        BaselineDatasetPath(directory.Path(), featureCount);

    Clock::time_point start = Clock::now();
    Map map;
    map.AddLayer(VectorDataset::Open(path));
    const double openMs = ElapsedMilliseconds(start);

    start = Clock::now();
    const rendering::MapRenderScene scene =
        rendering::CreateMapRenderScene(
            map,
            rendering::VectorRenderSceneOptions{.prepareFillMeshes = false});
    const double sceneMs = ElapsedMilliseconds(start);
    ASSERT_EQ(scene.layers.size(), 1U);
    ASSERT_TRUE(scene.layers.front().scene->fillMeshes.empty());
    const std::int64_t measuredFeatureCount =
        scene.layers.front().scene->sourceFeatureCount;
    ASSERT_GT(measuredFeatureCount, 0);

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

    Viewport panned = viewport;
    ASSERT_TRUE(panned.Pan(1.0, 1.0));
    start = Clock::now();
    ASSERT_EQ(gdi.Draw(surface.Get(), bounds, scene, panned), S_OK);
    const double gdiPanMs = ElapsedMilliseconds(start);

    Viewport zoomed = viewport;
    ASSERT_TRUE(zoomed.Zoom(2.0));
    start = Clock::now();
    ASSERT_EQ(gdi.Draw(surface.Get(), bounds, scene, zoomed), S_OK);
    const double gdiZoomMs = ElapsedMilliseconds(start);

    activex::OpenGlVectorLayerRenderer openGl;
    start = Clock::now();
    const HRESULT openGlResult = openGl.Draw(
        surface.Get(),
        bounds,
        scene,
        viewport,
        true,
        activex::VectorRenderMode::Outline);
    const double openGlMs = ElapsedMilliseconds(start);
    EXPECT_TRUE(openGlResult == S_OK || openGlResult == S_FALSE);
    if (openGlResult == S_FALSE)
    {
        std::wcout << L"  OpenGL unavailable: " << openGl.GetLastFailureReason() << L'\n';
    }

    std::cout << "Triangulation-disabled baseline: " << measuredFeatureCount
              << " source features (" << scene.layers.front().scene->polygons.size()
              << " polygons), " << kTargetWidth << 'x' << kTargetHeight << " target\n";
    Report("Core open dataset", openMs, "ms");
    Report("Core prepare outline-only scene", sceneMs, "ms");
    Report("GDI first frame", gdiFrames.front(), "ms");
    Report(
        "GDI steady frame median",
        Median(std::vector<double>(gdiFrames.begin() + 1, gdiFrames.end())),
        "ms");
    Report("GDI pan frame", gdiPanMs, "ms");
    Report("GDI zoom frame", gdiZoomMs, "ms");
    Report("OpenGL outline-only first frame", openGlMs, "ms");
    ReportPerformanceDiagnostics();
}

TEST_F(RenderingBaseline, MeasuresCoreSceneAndBackendCosts)
{
    const char* enabled = std::getenv("MWG_RUN_RENDERING_BASELINE");
    if (enabled == nullptr || std::string(enabled) != "1")
    {
        GTEST_SKIP() << "Set MWG_RUN_RENDERING_BASELINE=1 to measure the rendering baseline.";
    }

    DiagnosticCapture diagnostics;
    const int featureCount = FeatureCountFromEnvironment();
    TempDirectory directory;
    const std::filesystem::path path =
        BaselineDatasetPath(directory.Path(), featureCount);

    DWORD memoryError = ERROR_SUCCESS;
    const std::optional<double> memoryBefore = PrivateMiB(memoryError);
    ASSERT_TRUE(memoryBefore.has_value()) << "GetProcessMemoryInfo failed: " << memoryError;
    Clock::time_point start = Clock::now();
    Map map;
    map.AddLayer(VectorDataset::Open(path));
    const double openMs = ElapsedMilliseconds(start);

    start = Clock::now();
    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(
        map,
        rendering::VectorRenderSceneOptions{.prepareFillMeshes = true});
    const double sceneMs = ElapsedMilliseconds(start);
    const std::optional<double> memoryAfterScene = PrivateMiB(memoryError);
    ASSERT_TRUE(memoryAfterScene.has_value()) << "GetProcessMemoryInfo failed: " << memoryError;
    ASSERT_EQ(scene.layers.size(), 1U);
    ASSERT_FALSE(scene.layers.front().scene->polygons.empty());
    const std::int64_t measuredFeatureCount =
        scene.layers.front().scene->sourceFeatureCount;
    ASSERT_GT(measuredFeatureCount, 0);

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

    Viewport pannedGdi = viewport;
    ASSERT_TRUE(pannedGdi.Pan(1.0, 1.0));
    start = Clock::now();
    ASSERT_EQ(gdi.Draw(surface.Get(), bounds, scene, pannedGdi), S_OK);
    const double gdiPanMs = ElapsedMilliseconds(start);

    Viewport zoomedGdi = viewport;
    ASSERT_TRUE(zoomedGdi.Zoom(2.0));
    start = Clock::now();
    ASSERT_EQ(gdi.Draw(surface.Get(), bounds, scene, zoomedGdi), S_OK);
    const double gdiZoomMs = ElapsedMilliseconds(start);

    std::cout << "Rendering baseline: " << measuredFeatureCount
              << " source features (" << scene.layers.front().scene->polygons.size()
              << " polygons), " << kTargetWidth << 'x' << kTargetHeight << " target\n";
    Report("Core open dataset", openMs, "ms");
    Report("Core prepare render scene", sceneMs, "ms");
    Report("Private memory increase for scene", *memoryAfterScene - *memoryBefore, "MiB");
    Report("GDI first frame", gdiFrames.front(), "ms");
    Report(
        "GDI frame (median)",
        Median(std::vector<double>(gdiFrames.begin() + 1, gdiFrames.end())),
        "ms");
    Report("GDI pan frame", gdiPanMs, "ms");
    Report("GDI zoom frame", gdiZoomMs, "ms");

    activex::OpenGlVectorLayerRenderer openGl;
    start = Clock::now();
    const HRESULT firstFrame = openGl.Draw(surface.Get(), bounds, scene, viewport);
    const double firstOpenGlMs = ElapsedMilliseconds(start);
    if (firstFrame == S_FALSE)
    {
        std::wcout << L"  OpenGL unavailable: " << openGl.GetLastFailureReason() << L'\n';
        ReportPerformanceDiagnostics();
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

    Viewport pannedOpenGl = viewport;
    ASSERT_TRUE(pannedOpenGl.Pan(1.0, 1.0));
    start = Clock::now();
    ASSERT_EQ(openGl.Draw(surface.Get(), bounds, scene, pannedOpenGl), S_OK);
    const double openGlPanMs = ElapsedMilliseconds(start);

    Viewport zoomedOpenGl = viewport;
    ASSERT_TRUE(zoomedOpenGl.Zoom(2.0));
    start = Clock::now();
    ASSERT_EQ(openGl.Draw(surface.Get(), bounds, scene, zoomedOpenGl), S_OK);
    const double openGlZoomMs = ElapsedMilliseconds(start);

    Report("OpenGL first frame (context+draw)", firstOpenGlMs, "ms");
    Report("OpenGL frame (median)", Median(openGlFrames), "ms");
    Report("OpenGL pan frame", openGlPanMs, "ms");
    Report("OpenGL zoom frame", openGlZoomMs, "ms");
    const std::optional<double> memoryAfterOpenGl = PrivateMiB(memoryError);
    ASSERT_TRUE(memoryAfterOpenGl.has_value()) << "GetProcessMemoryInfo failed: " << memoryError;
    Report("Private memory increase total", *memoryAfterOpenGl - *memoryBefore, "MiB");
    ReportPerformanceDiagnostics();
}

TEST_F(RenderingBaseline, MeasuresLazyOpenGlFillCosts)
{
    const char* enabled = std::getenv("MWG_RUN_RENDERING_BASELINE");
    if (enabled == nullptr || std::string(enabled) != "1")
    {
        GTEST_SKIP() << "Set MWG_RUN_RENDERING_BASELINE=1 to measure the rendering baseline.";
    }

    rendering::VectorRenderScene probeLayer;
    probeLayer.extent = {0.0, 0.0, 1.0, 1.0};
    probeLayer.sourceFeatureCount = 1;
    probeLayer.polygons = {
        {{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}, {0.0, 0.0}}}};
    probeLayer.fillMeshes.push_back(
        rendering::TriangulatePolygon(probeLayer.polygons.front()));
    rendering::MapRenderScene probeScene;
    probeScene.layers.push_back({
        mwg::LayerId{1},
        std::make_shared<const rendering::VectorRenderScene>(std::move(probeLayer))});
    DibSurface probeSurface(64, 64);
    ASSERT_TRUE(probeSurface.IsValid());
    const RECT probeBounds{0, 0, 64, 64};
    Viewport probeViewport(64, 64);
    ASSERT_TRUE(probeViewport.FitExtent(probeScene.layers.front().scene->extent, 4.0));
    activex::OpenGlVectorLayerRenderer availabilityProbe;
    const HRESULT probeResult =
        availabilityProbe.Draw(
            probeSurface.Get(),
            probeBounds,
            probeScene,
            probeViewport);
    if (probeResult == S_FALSE)
    {
        GTEST_SKIP() << "OpenGL is unavailable; no baseline can be measured.";
    }
    ASSERT_EQ(probeResult, S_OK);

    DiagnosticCapture diagnostics;
    const int featureCount = FeatureCountFromEnvironment();
    TempDirectory directory;
    const std::filesystem::path path =
        BaselineDatasetPath(directory.Path(), featureCount);

    Clock::time_point start = Clock::now();
    Map map;
    map.AddLayer(VectorDataset::Open(path));
    const double openMs = ElapsedMilliseconds(start);

    start = Clock::now();
    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(map);
    const double sceneMs = ElapsedMilliseconds(start);
    ASSERT_EQ(scene.layers.size(), 1U);
    ASSERT_TRUE(scene.layers.front().scene->fillMeshes.empty());
    const std::size_t polygonCount = scene.layers.front().scene->polygons.size();
    ASSERT_GT(polygonCount, 0U);
    const diagnostics::PerformanceSnapshot beforeDraw =
        diagnostics::PerformanceDiagnostics::Snapshot();
    EXPECT_EQ(
        beforeDraw.Counter(
            diagnostics::PerformanceCounter::PolygonsTriangulated),
        0);

    DibSurface surface(kTargetWidth, kTargetHeight);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, kTargetWidth, kTargetHeight};
    Viewport viewport(kTargetWidth, kTargetHeight);
    ASSERT_TRUE(viewport.FitExtent(map.GetExtent(), 12.0));

    activex::OpenGlVectorLayerRenderer openGl;
    start = Clock::now();
    const HRESULT firstFrame =
        openGl.Draw(surface.Get(), bounds, scene, viewport);
    const double firstOpenGlMs = ElapsedMilliseconds(start);
    ASSERT_EQ(firstFrame, S_OK);
    const diagnostics::PerformanceSnapshot afterFirstDraw =
        diagnostics::PerformanceDiagnostics::Snapshot();
    EXPECT_EQ(
        afterFirstDraw.Counter(
            diagnostics::PerformanceCounter::PolygonsTriangulated),
        static_cast<std::int64_t>(polygonCount));

    std::vector<double> steadyFrames;
    for (int frame = 0; frame < kSteadyFrames; ++frame)
    {
        start = Clock::now();
        ASSERT_EQ(openGl.Draw(surface.Get(), bounds, scene, viewport), S_OK);
        steadyFrames.push_back(ElapsedMilliseconds(start));
    }

    Viewport panned = viewport;
    ASSERT_TRUE(panned.Pan(1.0, 1.0));
    start = Clock::now();
    ASSERT_EQ(openGl.Draw(surface.Get(), bounds, scene, panned), S_OK);
    const double panMs = ElapsedMilliseconds(start);

    Viewport zoomed = viewport;
    ASSERT_TRUE(zoomed.Zoom(2.0));
    start = Clock::now();
    ASSERT_EQ(openGl.Draw(surface.Get(), bounds, scene, zoomed), S_OK);
    const double zoomMs = ElapsedMilliseconds(start);

    EXPECT_EQ(
        diagnostics::PerformanceDiagnostics::Snapshot().Counter(
            diagnostics::PerformanceCounter::PolygonsTriangulated),
        static_cast<std::int64_t>(polygonCount));
    EXPECT_TRUE(scene.layers.front().scene->fillMeshes.empty());

    std::cout << "Lazy OpenGL fill baseline: "
              << scene.layers.front().scene->sourceFeatureCount
              << " source features (" << polygonCount << " lazily triangulated), "
              << kTargetWidth << 'x' << kTargetHeight << " target\n";
    Report("Core open dataset", openMs, "ms");
    Report("Core prepare source-only scene", sceneMs, "ms");
    Report("OpenGL lazy-fill first frame", firstOpenGlMs, "ms");
    Report("OpenGL lazy-fill frame median", Median(steadyFrames), "ms");
    Report("OpenGL lazy-fill pan frame", panMs, "ms");
    Report("OpenGL lazy-fill zoom frame", zoomMs, "ms");
    ReportPerformanceDiagnostics();
}

TEST_F(RenderingBaseline, MeasuresEndToEndFirstFrameThroughTheControl)
{
    const char* enabled = std::getenv("MWG_RUN_RENDERING_BASELINE");
    if (enabled == nullptr || std::string(enabled) != "1")
    {
        GTEST_SKIP() << "Set MWG_RUN_RENDERING_BASELINE=1 to measure the rendering baseline.";
    }

    ComApartment apartment;
    ASSERT_TRUE(SUCCEEDED(apartment.Result()));
    HostedControl hosted;
    ASSERT_TRUE(hosted.IsCreated())
        << "MapWinGIS.ocx could not be loaded next to the test executable.";

    DiagnosticCapture diagnostics;
    const int featureCount = FeatureCountFromEnvironment();
    TempDirectory directory;
    const std::filesystem::path path =
        BaselineDatasetPath(directory.Path(), featureCount);

    Microsoft::WRL::ComPtr<IMapWinGISControl4> control;
    ASSERT_EQ(hosted.Control().As(&control), S_OK);

    Clock::time_point start = Clock::now();
    LONGLONG layerId = 0;
    ASSERT_EQ(control->LoadVectorDatasetEx(ATL::CComBSTR(path.c_str()), &layerId), S_OK);
    const double loadMs = ElapsedMilliseconds(start);
    ASSERT_GT(layerId, 0);

    DibSurface surface(kTargetWidth, kTargetHeight);
    ASSERT_TRUE(surface.IsValid());
    ExpectGdiFirstFrameAndReport(
        hosted, "End-to-end load (LoadVectorDatasetEx)", loadMs, surface.Get());
}

TEST_F(RenderingBaseline, MeasuresEndToEndAsyncLoadFirstFrameThroughTheControl)
{
    const char* enabled = std::getenv("MWG_RUN_RENDERING_BASELINE");
    if (enabled == nullptr || std::string(enabled) != "1")
    {
        GTEST_SKIP() << "Set MWG_RUN_RENDERING_BASELINE=1 to measure the rendering baseline.";
    }

    ComApartment apartment;
    ASSERT_TRUE(SUCCEEDED(apartment.Result()));
    HostedControl hosted;
    ASSERT_TRUE(hosted.IsCreated())
        << "MapWinGIS.ocx could not be loaded next to the test executable.";

    DiagnosticCapture diagnostics;
    const int featureCount = FeatureCountFromEnvironment();
    TempDirectory directory;
    const std::filesystem::path path =
        BaselineDatasetPath(directory.Path(), featureCount);

    Clock::time_point start = Clock::now();
    LONGLONG operationId = 0;
    ASSERT_EQ(
        hosted.Control()->BeginLoadVectorDataset(
            ATL::CComBSTR(path.c_str()), &operationId),
        S_OK);
    ASSERT_GT(operationId, 0);

    // EndLoadVectorDataset stays E_PENDING until the worker commits and the
    // completion is delivered; pump this thread's queue to let it finalize.
    VARIANT_BOOL succeeded = VARIANT_FALSE;
    VARIANT_BOOL cancelled = VARIANT_TRUE;
    LONGLONG layerId = 0;
    BSTR summary = nullptr;
    BSTR loadDiagnostics = nullptr;
    HRESULT endResult = E_PENDING;
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < deadline)
    {
        endResult = hosted.Control()->EndLoadVectorDataset(
            operationId,
            &succeeded,
            &cancelled,
            &layerId,
            &summary,
            &loadDiagnostics);
        if (endResult != E_PENDING)
        {
            break;
        }
        PumpDispatchMessages();
        Sleep(1);
    }
    const double loadMs = ElapsedMilliseconds(start);
    ::SysFreeString(summary);
    ::SysFreeString(loadDiagnostics);
    ASSERT_EQ(endResult, S_OK);
    EXPECT_EQ(succeeded, VARIANT_TRUE);
    EXPECT_EQ(cancelled, VARIANT_FALSE);
    ASSERT_GT(layerId, 0);

    DibSurface surface(kTargetWidth, kTargetHeight);
    ASSERT_TRUE(surface.IsValid());
    ExpectGdiFirstFrameAndReport(
        hosted, "End-to-end load (Begin/EndLoadVectorDataset)", loadMs, surface.Get());
}

TEST_F(RenderingBaseline, MeasuresComplexPolygonCosts)
{
    const char* enabled = std::getenv("MWG_RUN_RENDERING_BASELINE");
    if (enabled == nullptr || std::string(enabled) != "1")
    {
        GTEST_SKIP() << "Set MWG_RUN_RENDERING_BASELINE=1 to measure the rendering baseline.";
    }

    DiagnosticCapture diagnostics;
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
    const rendering::MapRenderScene scene = rendering::CreateMapRenderScene(
        map,
        rendering::VectorRenderSceneOptions{.prepareFillMeshes = true});
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
        ASSERT_EQ(layer.scene->fillMeshes.size(), layer.scene->polygons.size());
        polygonCount += layer.scene->polygons.size();
        for (const std::optional<rendering::TriangleMesh>& mesh : layer.scene->fillMeshes)
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
        ReportPerformanceDiagnostics();
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
    ReportPerformanceDiagnostics();
}
