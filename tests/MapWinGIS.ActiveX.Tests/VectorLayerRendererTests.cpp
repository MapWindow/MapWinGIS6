// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorLayerRendererTests.cpp
 * @brief Tests OpenGL rendering and the GDI fallback for vector scenes.
 *
 * Also verifies that multi-layer scenes are drawn through one shared
 * viewport and that a layer that cannot be drawn does not stop the others.
 */
#include <Windows.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "OpenGlVectorLayerRenderer.h"
#include "PanBlit.h"
#include "OpenGlVectorLayerRendererTestAccess.h"
#include "VectorLayerRenderer.h"
#include "ViewTransform.h"

#include "mapwingis/core/Error.h"
#include "mapwingis/rendering/PolygonTriangulation.h"

namespace
{
    class BitmapSurface final
    {
    public:
        BitmapSurface(const int width, const int height) noexcept
        {
            _screen = GetDC(nullptr);
            if (_screen == nullptr)
            {
                return;
            }

            _memory = CreateCompatibleDC(_screen);
            if (_memory == nullptr)
            {
                return;
            }

            _bitmap = CreateCompatibleBitmap(_screen, width, height);
            if (_bitmap == nullptr)
            {
                return;
            }

            _previous = SelectObject(_memory, _bitmap);
        }

        ~BitmapSurface()
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
            if (_screen != nullptr)
            {
                ReleaseDC(nullptr, _screen);
            }
        }

        BitmapSurface(const BitmapSurface&) = delete;
        BitmapSurface& operator=(const BitmapSurface&) = delete;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return _memory != nullptr &&
                _bitmap != nullptr &&
                _previous != nullptr &&
                _previous != HGDI_ERROR;
        }

        [[nodiscard]] HDC Get() const noexcept { return _memory; }

    private:
        HDC _screen{};
        HDC _memory{};
        HBITMAP _bitmap{};
        HGDIOBJ _previous{};
    };

    constexpr double kPadding = 12.0;
    constexpr COLORREF kFill = RGB(61, 122, 169);

    /// One polygon layer: a closed axis-aligned rectangle.
    [[nodiscard]] mwg::rendering::VectorRenderScene MakeRectangleLayer(const mwg::Extent& rectangle)
    {
        return {
            rectangle,
            1,
            0,
            {{{{rectangle.xMin, rectangle.yMin},
               {rectangle.xMax, rectangle.yMin},
               {rectangle.xMax, rectangle.yMax},
               {rectangle.xMin, rectangle.yMax},
               {rectangle.xMin, rectangle.yMin}}}}};
    }

    /// Triangulates every polygon of a layer scene, mirroring how Core
    /// prepares meshes when the scene is created. Polygons that cannot be
    /// triangulated keep a null mesh so fallback behavior stays testable.
    void TriangulateScene(mwg::rendering::VectorRenderScene& scene)
    {
        scene.fillMeshes.clear();
        scene.fillMeshes.reserve(scene.polygons.size());
        for (std::size_t index = 0; index < scene.polygons.size(); ++index)
        {
            std::optional<mwg::rendering::TriangleMesh> mesh;
            try
            {
                mesh = mwg::rendering::TriangulatePolygon(scene.polygons[index]);
            }
            catch (const mwg::CoreError&)
            {
            }
            scene.fillMeshes.push_back(std::move(mesh));
        }
    }

    /// Wraps a layer scene in the shared immutable form held by map scenes.
    [[nodiscard]] std::shared_ptr<const mwg::rendering::VectorRenderScene> Share(
        mwg::rendering::VectorRenderScene scene)
    {
        return std::make_shared<const mwg::rendering::VectorRenderScene>(std::move(scene));
    }

    /// Gives a test fixture a private, editable copy of a layer's geometry;
    /// map scenes otherwise share it immutably.
    [[nodiscard]] mwg::rendering::VectorRenderScene& Edit(mwg::rendering::LayerRenderScene& layer)
    {
        auto copy = std::make_shared<mwg::rendering::VectorRenderScene>(*layer.scene);
        mwg::rendering::VectorRenderScene& editable = *copy;
        layer.scene = std::move(copy);
        return editable;
    }

    /// A map scene with these layers, bottom first, with identities 1, 2, ...
    /// Every polygon gets its Core triangulation mesh.
    [[nodiscard]] mwg::rendering::MapRenderScene MakeMapScene(
        std::vector<mwg::rendering::VectorRenderScene> layers)
    {
        mwg::rendering::MapRenderScene scene;
        std::uint64_t id = 0;
        for (mwg::rendering::VectorRenderScene& layer : layers)
        {
            TriangulateScene(layer);
            scene.layers.push_back({mwg::LayerId{++id}, Share(std::move(layer))});
        }
        return scene;
    }

    [[nodiscard]] mwg::rendering::MapRenderScene MakeConvexPolygonScene()
    {
        return MakeMapScene({MakeRectangleLayer({1.0, 1.0, 9.0, 9.0})});
    }

    /// Rebuilds the single-polygon fixture with one additional interior ring.
    void AddInteriorRing(mwg::rendering::MapRenderScene& scene, mwg::rendering::Ring ring)
    {
        auto& buffer = Edit(scene.layers.front()).polygons;
        const auto view = buffer.front();
        mwg::rendering::Polygon polygon;
        for (std::size_t index = 0; index < view.size(); ++index)
        {
            const auto points = view[index];
            polygon.emplace_back(points.begin(), points.end());
        }
        polygon.push_back(std::move(ring));
        buffer = {polygon};
    }

    /// A viewport of the bounds' size fitted to extent, as the control does.
    [[nodiscard]] mwg::Viewport FitViewport(const RECT& bounds, const mwg::Extent& extent)
    {
        mwg::Viewport viewport(bounds.right - bounds.left, bounds.bottom - bounds.top);
        EXPECT_TRUE(viewport.FitExtent(extent, kPadding));
        return viewport;
    }

    [[nodiscard]] mwg::Viewport FitViewport(const RECT& bounds)
    {
        return FitViewport(bounds, mwg::Extent{0.0, 0.0, 10.0, 10.0});
    }

    /// Color of the pixel that contains a map point.
    [[nodiscard]] COLORREF MapPixel(
        const HDC surface,
        const mwg::Viewport& viewport,
        const double x,
        const double y)
    {
        const auto screen = viewport.MapToScreen({x, y});
        const auto pixel = screen.has_value() ? mwg::ToPixel(*screen) : std::nullopt;
        EXPECT_TRUE(pixel.has_value()) << "map point (" << x << ", " << y << ")";
        return pixel.has_value() ? GetPixel(surface, pixel->x, pixel->y) : CLR_INVALID;
    }

    /// Two layers with different extents and a gap between them.
    [[nodiscard]] mwg::rendering::MapRenderScene MakeTwoLayerScene()
    {
        return MakeMapScene({
            MakeRectangleLayer({0.0, 0.0, 2.0, 2.0}),
            MakeRectangleLayer({8.0, 3.0, 10.0, 5.0})});
    }

    constexpr mwg::Extent kTwoLayerExtent{0.0, 0.0, 10.0, 5.0};

    /// Checks the pixels of MakeTwoLayerScene drawn through viewport.
    void ExpectTwoLayerPixels(const HDC surface, const mwg::Viewport& viewport)
    {
        const COLORREF background = GetSysColor(COLOR_WINDOW);
        EXPECT_EQ(MapPixel(surface, viewport, 1.0, 1.0), kFill) << "first layer";
        EXPECT_EQ(MapPixel(surface, viewport, 9.0, 4.0), kFill) << "second layer";
        EXPECT_EQ(MapPixel(surface, viewport, 5.0, 2.5), background) << "gap";
        EXPECT_EQ(MapPixel(surface, viewport, 1.0, 4.0), background) << "above first layer";
        EXPECT_EQ(MapPixel(surface, viewport, 9.0, 1.0), background) << "below second layer";
    }
}

TEST(VectorLayerRenderer, UsesOpenGlForConvexPolygonWhenAvailable)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));

    const RECT bounds{0, 0, 120, 100};
    const mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    const mwg::Viewport viewport = FitViewport(bounds);

    // Only an unavailable OpenGL target may skip; drawing, readback or
    // presentation failures in the backend must fail the test.
    mwg::activex::OpenGlVectorLayerRenderer openGlRenderer;
    const HRESULT openGlResult = openGlRenderer.Draw(surface.Get(), bounds, scene, viewport);
    if (openGlResult == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(openGlResult, S_OK);
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), kFill);
    EXPECT_EQ(GetPixel(surface.Get(), 5, 5), GetSysColor(COLOR_WINDOW));

    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    mwg::activex::VectorLayerRenderer renderer;
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, viewport), S_OK);
    EXPECT_EQ(
        renderer.GetActiveBackend(),
        mwg::activex::VectorRenderBackendKind::OpenGL);
    EXPECT_EQ(renderer.GetDiagnostic(), L"OpenGL rendered the frame successfully.");
    EXPECT_TRUE(renderer.GetFailedLayers().empty());
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), kFill);
    EXPECT_EQ(GetPixel(surface.Get(), 5, 5), GetSysColor(COLOR_WINDOW));
}

TEST(VectorLayerRenderer, OpenGlRecreatesLostContextOnLaterDraw)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    const mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    const mwg::Viewport viewport = FitViewport(bounds);

    mwg::activex::OpenGlVectorLayerRenderer renderer;
    const HRESULT firstResult = renderer.Draw(surface.Get(), bounds, scene, viewport);
    if (firstResult == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(firstResult, S_OK);

    mwg::activex::testing::OpenGlVectorLayerRendererTestAccess::
        SimulateContextLoss(renderer);
    EXPECT_EQ(renderer.Draw(surface.Get(), bounds, scene, viewport), E_FAIL);
    EXPECT_EQ(
        renderer.GetLastFailureReason(),
        L"OpenGL could not activate its rendering context.");

    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, viewport), S_OK);
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), kFill);
    EXPECT_EQ(GetPixel(surface.Get(), 5, 5), GetSysColor(COLOR_WINDOW));
}

TEST(VectorLayerRenderer, OpenGlRendersConcavePolygonWithDuplicateVertex)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    const RECT bounds{0, 0, 120, 100};

    // Concave corner with a consecutive duplicate vertex; Core
    // triangulation tolerates the duplicate and produces the fill mesh.
    mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    Edit(scene.layers.front()).polygons = {{{
        {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {5.0, 5.0}, {5.0, 5.0}, {0.0, 10.0}, {0.0, 0.0}}}};
    TriangulateScene(Edit(scene.layers.front()));

    const mwg::Viewport viewport = FitViewport(bounds);
    mwg::activex::OpenGlVectorLayerRenderer renderer;
    const HRESULT result = renderer.Draw(surface.Get(), bounds, scene, viewport);
    if (result == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(result, S_OK);
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 3.0, 3.0), kFill) << "inside the body";
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 3.0, 9.0), GetSysColor(COLOR_WINDOW))
        << "concave notch stays unfilled";
}

TEST(VectorLayerRenderer, OpenGlRejectsPolygonWithoutFillMesh)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};

    // A triangulation failure leaves the mesh empty while the original
    // rings remain; OpenGL must decline the frame instead of approximating
    // the fill so the coordinator falls back to GDI.
    mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    Edit(scene.layers.front()).fillMeshes.front().reset();

    mwg::activex::OpenGlVectorLayerRenderer renderer;
    EXPECT_EQ(renderer.Draw(surface.Get(), bounds, scene, FitViewport(bounds)), S_FALSE);
    EXPECT_EQ(
        renderer.GetLastFailureReason(),
        L"The OpenGL backend requires a triangulated fill mesh for "
        L"every polygon; this frame was rendered with GDI.");
}

TEST(VectorLayerRenderer, OpenGlRejectsSelfIntersectingRing)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};

    const mwg::rendering::MapRenderScene scene = MakeMapScene({{
        mwg::Extent{-3.0, -3.0, 3.0, 3.0},
        0,
        0,
        {{{{0.0, 3.0}, {2.0, -3.0}, {-3.0, 1.0}, {3.0, 1.0}, {-2.0, -3.0}, {0.0, 3.0}}}}}});

    mwg::activex::OpenGlVectorLayerRenderer renderer;
    EXPECT_EQ(
        renderer.Draw(surface.Get(), bounds, scene, FitViewport(bounds, {-3.0, -3.0, 3.0, 3.0})),
        S_FALSE);
    EXPECT_EQ(
        renderer.GetLastFailureReason(),
        L"The OpenGL backend requires a triangulated fill mesh for "
        L"every polygon; this frame was rendered with GDI.");
}

TEST(VectorLayerRenderer, OpenGlRejectsTheFrameWhenAnyLayerHasNoFillMesh)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::MapRenderScene scene = MakeTwoLayerScene();
    Edit(scene.layers.back()).fillMeshes.front().reset();

    mwg::activex::OpenGlVectorLayerRenderer renderer;
    EXPECT_EQ(
        renderer.Draw(surface.Get(), bounds, scene, FitViewport(bounds, kTwoLayerExtent)),
        S_FALSE);
    EXPECT_EQ(
        renderer.GetLastFailureReason(),
        L"The OpenGL backend requires a triangulated fill mesh for "
        L"every polygon; this frame was rendered with GDI.");
}

TEST(VectorLayerRenderer, FailsWithoutDrawingNonFiniteCoordinates)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    Edit(scene.layers.front()).polygons.AppendPolygon(
        {{{1.0, 1.0}, {std::numeric_limits<double>::quiet_NaN(), 5.0}, {9.0, 9.0}, {1.0, 1.0}}});
    TriangulateScene(Edit(scene.layers.front()));

    mwg::activex::VectorLayerRenderer renderer;
    EXPECT_EQ(renderer.Draw(surface.Get(), bounds, scene, FitViewport(bounds)), E_INVALIDARG);
    EXPECT_FALSE(renderer.GetActiveBackend().has_value());
    ASSERT_EQ(renderer.GetFailedLayers().size(), 1U);
    EXPECT_EQ(renderer.GetFailedLayers().front(), mwg::LayerId{1});
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), RGB(0, 0, 0));
}

TEST(VectorLayerRenderer, SkipsOnlyTheLayerThatCannotBeDrawn)
{
    BitmapSurface surface(200, 120);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 200, 120};
    ASSERT_NE(FillRect(surface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);

    mwg::rendering::MapRenderScene scene = MakeTwoLayerScene();
    Edit(scene.layers.front()).polygons.AppendPolygon(
        {{{0.0, 0.0}, {std::numeric_limits<double>::infinity(), 1.0}, {1.0, 1.0}, {0.0, 0.0}}});
    TriangulateScene(Edit(scene.layers.front()));
    const mwg::Viewport viewport = FitViewport(bounds, kTwoLayerExtent);

    mwg::activex::VectorLayerRenderer renderer;
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, viewport), S_OK);

    EXPECT_EQ(renderer.GetActiveBackend(), mwg::activex::VectorRenderBackendKind::Gdi);
    EXPECT_EQ(
        renderer.GetDiagnostic(),
        L"GDI could not draw one or more layers; the other layers were drawn.");
    ASSERT_EQ(renderer.GetFailedLayers().size(), 1U);
    EXPECT_EQ(renderer.GetFailedLayers().front(), mwg::LayerId{1});
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 1.0, 1.0), GetSysColor(COLOR_WINDOW));
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 9.0, 4.0), kFill);
}

TEST(VectorLayerRenderer, ReportsALayerWithoutASceneAndDrawsTheOthers)
{
    BitmapSurface surface(200, 120);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 200, 120};
    ASSERT_NE(FillRect(surface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);

    mwg::rendering::MapRenderScene scene = MakeTwoLayerScene();
    scene.layers.front().scene.reset();
    const mwg::Viewport viewport = FitViewport(bounds, kTwoLayerExtent);

    mwg::activex::VectorLayerRenderer renderer;
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, viewport), S_OK);

    EXPECT_EQ(renderer.GetActiveBackend(), mwg::activex::VectorRenderBackendKind::Gdi);
    ASSERT_EQ(renderer.GetFailedLayers().size(), 1U);
    EXPECT_EQ(renderer.GetFailedLayers().front(), mwg::LayerId{1});
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 1.0, 1.0), GetSysColor(COLOR_WINDOW));
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 9.0, 4.0), kFill);
}

TEST(VectorLayerRenderer, OpenGlRendersPolygonWithInteriorRing)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    AddInteriorRing(scene,
        {{3.0, 3.0}, {3.0, 7.0}, {7.0, 7.0}, {7.0, 3.0}, {3.0, 3.0}});
    TriangulateScene(Edit(scene.layers.front()));

    mwg::activex::VectorLayerRenderer renderer;
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, FitViewport(bounds)), S_OK);
    if (renderer.GetActiveBackend() != mwg::activex::VectorRenderBackendKind::OpenGL)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    EXPECT_EQ(GetPixel(surface.Get(), 30, 50), kFill);
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), GetSysColor(COLOR_WINDOW));
}

TEST(VectorLayerRenderer, FallsBackToGdiWhenAPolygonHasNoFillMesh)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    Edit(scene.layers.front()).fillMeshes.front().reset();

    mwg::activex::VectorLayerRenderer renderer;
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, FitViewport(bounds)), S_OK);
    EXPECT_EQ(renderer.GetActiveBackend(), mwg::activex::VectorRenderBackendKind::Gdi);
    EXPECT_EQ(
        renderer.GetDiagnostic(),
        L"The OpenGL backend requires a triangulated fill mesh for "
        L"every polygon; this frame was rendered with GDI.");
}

TEST(VectorLayerRenderer, GdiFallbackRendersConcaveHoledPolygonFromRings)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    ASSERT_NE(FillRect(surface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);

    // A scene whose only polygon has no triangulated mesh: the coordinator
    // must fall back to GDI, which fills the concave shape from the original
    // rings and leaves the concave indentation and the interior ring empty.
    mwg::rendering::VectorRenderScene layer;
    layer.extent = {0.0, 0.0, 10.0, 10.0};
    layer.sourceFeatureCount = 1;
    layer.polygons = {{
        {{0.0, 0.0}, {10.0, 0.0}, {10.0, 4.0}, {4.0, 4.0}, {4.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}},
        {{1.0, 1.0}, {1.0, 3.0}, {3.0, 3.0}, {3.0, 1.0}, {1.0, 1.0}},
    }};
    layer.fillMeshes.push_back(std::nullopt);
    mwg::rendering::MapRenderScene scene;
    scene.layers.push_back({mwg::LayerId{1}, Share(std::move(layer))});

    const mwg::Viewport viewport = FitViewport(bounds);
    mwg::activex::VectorLayerRenderer renderer;
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, viewport), S_OK);
    EXPECT_EQ(renderer.GetActiveBackend(), mwg::activex::VectorRenderBackendKind::Gdi);

    EXPECT_EQ(MapPixel(surface.Get(), viewport, 7.0, 2.0), kFill) << "lower arm";
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 2.0, 7.0), kFill) << "left arm";
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 7.0, 7.0), GetSysColor(COLOR_WINDOW))
        << "concave indentation stays unfilled";
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 2.0, 2.0), GetSysColor(COLOR_WINDOW))
        << "interior ring stays unfilled";

    // A later mesh-complete frame must select OpenGL again when the backend
    // is available, so a coordinator that permanently stops trying OpenGL
    // after one missing-mesh frame cannot pass. Probe availability
    // independently of the coordinator, as
    // UsesOpenGlForConvexPolygonWhenAvailable does.
    const mwg::rendering::MapRenderScene meshScene = MakeConvexPolygonScene();
    mwg::activex::OpenGlVectorLayerRenderer openGlProbe;
    const HRESULT probeResult = openGlProbe.Draw(surface.Get(), bounds, meshScene, viewport);
    if (probeResult == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(probeResult, S_OK);
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, meshScene, viewport), S_OK);
    EXPECT_EQ(renderer.GetActiveBackend(), mwg::activex::VectorRenderBackendKind::OpenGL);
}

TEST(VectorLayerRenderer, OpenGlSupportRequiresAMeshForEveryPolygon)
{
    const mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    EXPECT_TRUE(
        mwg::activex::testing::OpenGlVectorLayerRendererTestAccess::IsSceneSupported(scene));

    // No meshes at all.
    mwg::rendering::MapRenderScene withoutMeshes = MakeConvexPolygonScene();
    Edit(withoutMeshes.layers.front()).fillMeshes.clear();
    EXPECT_FALSE(
        mwg::activex::testing::OpenGlVectorLayerRendererTestAccess::IsSceneSupported(
            withoutMeshes));

    // A triangulation failure for one polygon rejects the whole frame.
    mwg::rendering::MapRenderScene nullMesh = MakeConvexPolygonScene();
    Edit(nullMesh.layers.front()).fillMeshes.front().reset();
    EXPECT_FALSE(
        mwg::activex::testing::OpenGlVectorLayerRendererTestAccess::IsSceneSupported(nullMesh));

    // Non-finite coordinates stay unsupported regardless of the mesh.
    mwg::rendering::MapRenderScene nonFinite = MakeConvexPolygonScene();
    Edit(nonFinite.layers.front()).polygons.Coordinates().front().x =
        std::numeric_limits<double>::infinity();
    EXPECT_FALSE(
        mwg::activex::testing::OpenGlVectorLayerRendererTestAccess::IsSceneSupported(nonFinite));
}

TEST(VectorLayerRenderer, OpenGlSupportsConcaveMultipartAndHoledScenes)
{
    mwg::rendering::VectorRenderScene multipart;
    multipart.extent = {0.0, 0.0, 10.0, 10.0};
    multipart.sourceFeatureCount = 2;
    multipart.polygons = {
        // L-shaped concave polygon.
        {{{0.0, 0.0}, {10.0, 0.0}, {10.0, 4.0}, {4.0, 4.0}, {4.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}},
        // Rectangle with two interior rings.
        {{{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}},
         {{2.0, 2.0}, {2.0, 4.0}, {4.0, 4.0}, {4.0, 2.0}, {2.0, 2.0}},
         {{6.0, 6.0}, {6.0, 8.0}, {8.0, 8.0}, {8.0, 6.0}, {6.0, 6.0}}}};
    TriangulateScene(multipart);

    const mwg::rendering::MapRenderScene scene =
        MakeMapScene({multipart, MakeRectangleLayer({20.0, 20.0, 30.0, 30.0})});
    EXPECT_TRUE(
        mwg::activex::testing::OpenGlVectorLayerRendererTestAccess::IsSceneSupported(scene));

    // A malformed mesh reference also rejects the scene.
    mwg::rendering::MapRenderScene broken = scene;
    Edit(broken.layers.front()).fillMeshes.front()->indices.resize(4);
    EXPECT_FALSE(
        mwg::activex::testing::OpenGlVectorLayerRendererTestAccess::IsSceneSupported(broken));
}

TEST(VectorLayerRenderer, OpenGlRendersLShapedPolygon)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    Edit(scene.layers.front()).polygons = {{{
        {0.0, 0.0}, {10.0, 0.0}, {10.0, 4.0}, {4.0, 4.0}, {4.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}}};
    TriangulateScene(Edit(scene.layers.front()));

    const mwg::Viewport viewport = FitViewport(bounds);
    mwg::activex::OpenGlVectorLayerRenderer renderer;
    const HRESULT result = renderer.Draw(surface.Get(), bounds, scene, viewport);
    if (result == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(result, S_OK);
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 7.0, 2.0), kFill) << "lower arm";
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 2.0, 7.0), kFill) << "left arm";
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 7.0, 7.0), GetSysColor(COLOR_WINDOW))
        << "indentation stays unfilled";
}

TEST(VectorLayerRenderer, OpenGlRendersPolygonWithMultipleInteriorRings)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    AddInteriorRing(scene, {{2.0, 2.0}, {2.0, 4.0}, {4.0, 4.0}, {4.0, 2.0}, {2.0, 2.0}});
    AddInteriorRing(scene, {{6.0, 6.0}, {6.0, 8.0}, {8.0, 8.0}, {8.0, 6.0}, {6.0, 6.0}});
    TriangulateScene(Edit(scene.layers.front()));

    const mwg::Viewport viewport = FitViewport(bounds);
    mwg::activex::OpenGlVectorLayerRenderer renderer;
    const HRESULT result = renderer.Draw(surface.Get(), bounds, scene, viewport);
    if (result == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(result, S_OK);
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 3.0, 3.0), GetSysColor(COLOR_WINDOW))
        << "first hole stays unfilled";
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 7.0, 7.0), GetSysColor(COLOR_WINDOW))
        << "second hole stays unfilled";
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 5.0, 5.0), kFill) << "between the holes";
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 1.0, 5.0), kFill) << "beside the first hole";
}

TEST(VectorLayerRenderer, OpenGlRendersMultipartPolygonAndOutlinesFollowRings)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::VectorRenderScene multipart;
    multipart.extent = {0.0, 0.0, 10.0, 10.0};
    multipart.sourceFeatureCount = 2;
    multipart.polygons = {
        {{{0.0, 0.0}, {6.0, 0.0}, {6.0, 6.0}, {0.0, 6.0}, {0.0, 0.0}}},
        {{{4.0, 4.0}, {10.0, 4.0}, {10.0, 10.0}, {4.0, 10.0}, {4.0, 4.0}},
         {{6.0, 6.0}, {6.0, 8.0}, {8.0, 8.0}, {8.0, 6.0}, {6.0, 6.0}}}};
    const mwg::rendering::MapRenderScene scene = MakeMapScene({multipart});

    const mwg::Viewport viewport = FitViewport(bounds);
    mwg::activex::OpenGlVectorLayerRenderer renderer;
    const HRESULT result = renderer.Draw(surface.Get(), bounds, scene, viewport);
    if (result == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(result, S_OK);
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 2.0, 2.0), kFill) << "first component";
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 9.0, 5.0), kFill) << "second component body";
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 7.0, 7.0), GetSysColor(COLOR_WINDOW))
        << "hole of the second component";
    EXPECT_EQ(MapPixel(surface.Get(), viewport, 8.0, 2.0), GetSysColor(COLOR_WINDOW))
        << "gap between the components";
}

TEST(VectorLayerRenderer, OpenGlDoesNotDrawInternalTriangleEdges)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    const mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    const mwg::Viewport viewport = FitViewport(bounds);

    mwg::activex::OpenGlVectorLayerRenderer renderer;
    const HRESULT result = renderer.Draw(surface.Get(), bounds, scene, viewport);
    if (result == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(result, S_OK);

    // A convex quad triangulates into two triangles whose diagonal crosses
    // the polygon interior. Only the original ring may be outlined, so no
    // interior pixel may have the outline color.
    constexpr COLORREF kOutline = RGB(24, 58, 82);
    for (double t = 0.15; t < 0.9; t += 0.1)
    {
        for (const mwg::rendering::Point diagonal :
                 {mwg::rendering::Point{1.0 + 8.0 * t, 1.0 + 8.0 * t},
                  mwg::rendering::Point{1.0 + 8.0 * t, 9.0 - 8.0 * t}})
        {
            const auto screen = viewport.MapToScreen({diagonal.x, diagonal.y});
            ASSERT_TRUE(screen.has_value());
            const auto pixel = mwg::ToPixel(*screen);
            ASSERT_TRUE(pixel.has_value());
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    EXPECT_NE(GetPixel(surface.Get(), pixel->x + dx, pixel->y + dy), kOutline)
                        << "diagonal point (" << diagonal.x << ", " << diagonal.y << ")";
                }
            }
        }
    }
}

TEST(VectorLayerRenderer, GdiAndOpenGlAgreeOnConcaveHoledScene)
{
    BitmapSurface gdiSurface(120, 100);
    BitmapSurface openGlSurface(120, 100);
    ASSERT_TRUE(gdiSurface.IsValid());
    ASSERT_TRUE(openGlSurface.IsValid());
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::VectorRenderScene layer;
    layer.extent = {0.0, 0.0, 10.0, 10.0};
    layer.sourceFeatureCount = 2;
    layer.polygons = {
        {{{0.0, 0.0}, {10.0, 0.0}, {10.0, 4.0}, {4.0, 4.0}, {4.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}},
        {{{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}},
         {{3.0, 3.0}, {3.0, 7.0}, {7.0, 7.0}, {7.0, 3.0}, {3.0, 3.0}}}};
    const mwg::rendering::MapRenderScene scene = MakeMapScene({layer});
    const mwg::Viewport viewport = FitViewport(bounds);

    mwg::activex::GdiVectorLayerRenderer gdi;
    ASSERT_NE(FillRect(gdiSurface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);
    ASSERT_EQ(gdi.Draw(gdiSurface.Get(), bounds, scene, viewport), S_OK);

    mwg::activex::OpenGlVectorLayerRenderer openGl;
    const HRESULT result = openGl.Draw(openGlSurface.Get(), bounds, scene, viewport);
    if (result == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(result, S_OK);

    // Sample points on a coarse grid that stays at least one map unit from
    // every polygon and hole boundary; both backends must classify them
    // identically.
    for (double y = 2.0; y <= 8.0; y += 3.0)
    {
        for (double x = 2.0; x <= 8.0; x += 3.0)
        {
            const auto screen = viewport.MapToScreen({x, y});
            ASSERT_TRUE(screen.has_value());
            const auto pixel = mwg::ToPixel(*screen);
            ASSERT_TRUE(pixel.has_value());
            EXPECT_EQ(GetPixel(gdiSurface.Get(), pixel->x, pixel->y),
                      GetPixel(openGlSurface.Get(), pixel->x, pixel->y))
                << "map point (" << x << ", " << y << ")";
        }
    }
}

TEST(VectorLayerRenderer, FlatSceneCopyPreservesPixelsAfterTheOriginalIsReleased)
{
    BitmapSurface originalGdi(120, 100);
    BitmapSurface copiedGdi(120, 100);
    BitmapSurface originalOpenGl(120, 100);
    BitmapSurface copiedOpenGl(120, 100);
    ASSERT_TRUE(originalGdi.IsValid());
    ASSERT_TRUE(copiedGdi.IsValid());
    ASSERT_TRUE(originalOpenGl.IsValid());
    ASSERT_TRUE(copiedOpenGl.IsValid());
    const RECT bounds{0, 0, 120, 100};
    const auto viewport = FitViewport(bounds);
    auto source = MakeMapScene({
        MakeRectangleLayer({1, 1, 9, 9}), MakeRectangleLayer({7, 7, 10, 10})});
    AddInteriorRing(source, {{3, 3}, {3, 6}, {6, 6}, {6, 3}, {3, 3}});
    TriangulateScene(Edit(source.layers.front()));
    ASSERT_TRUE(
        mwg::activex::testing::OpenGlVectorLayerRendererTestAccess::IsSceneSupported(source));
    ASSERT_NE(FillRect(originalGdi.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);
    ASSERT_NE(FillRect(copiedGdi.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);

    mwg::activex::GdiVectorLayerRenderer gdi;
    mwg::activex::OpenGlVectorLayerRenderer openGl;
    ASSERT_EQ(gdi.Draw(originalGdi.Get(), bounds, source, viewport), S_OK);
    const HRESULT firstOpenGl = openGl.Draw(originalOpenGl.Get(), bounds, source, viewport);
    ASSERT_TRUE(firstOpenGl == S_OK || firstOpenGl == S_FALSE) << firstOpenGl;

    auto copy = source;
    source = {};
    ASSERT_EQ(gdi.Draw(copiedGdi.Get(), bounds, copy, viewport), S_OK);
    if (firstOpenGl == S_OK)
    {
        ASSERT_EQ(openGl.Draw(copiedOpenGl.Get(), bounds, copy, viewport), S_OK);
    }

    std::size_t gdiDifferences = 0;
    std::size_t openGlDifferences = 0;
    for (int y = 0; y < 100; ++y)
    {
        for (int x = 0; x < 120; ++x)
        {
            gdiDifferences += GetPixel(originalGdi.Get(), x, y) != GetPixel(copiedGdi.Get(), x, y);
            if (firstOpenGl == S_OK)
            {
                openGlDifferences +=
                    GetPixel(originalOpenGl.Get(), x, y) != GetPixel(copiedOpenGl.Get(), x, y);
            }
        }
    }
    EXPECT_EQ(gdiDifferences, 0U);
    EXPECT_EQ(openGlDifferences, 0U);
}

TEST(VectorLayerRenderer, HonorsCpuPreferenceAndReportsTheSelectedBackend)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    const mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    mwg::activex::VectorLayerRenderer renderer;
    renderer.SetPreference(mwg::activex::VectorRenderBackendPreference::Gdi);

    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, FitViewport(bounds)), S_OK);

    EXPECT_EQ(renderer.GetPreference(), mwg::activex::VectorRenderBackendPreference::Gdi);
    EXPECT_EQ(renderer.GetActiveBackend(), mwg::activex::VectorRenderBackendKind::Gdi);
    EXPECT_EQ(renderer.GetDiagnostic(), L"GDI was selected by the rendering preference.");
}

TEST(VectorLayerRenderer, FallsBackToCpuWhenOpenGlStagingBudgetIsExceeded)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 5000, 5000};
    const mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    mwg::activex::VectorLayerRenderer renderer;

    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, FitViewport(bounds)), S_OK);

    EXPECT_EQ(renderer.GetActiveBackend(), mwg::activex::VectorRenderBackendKind::Gdi);
    EXPECT_EQ(
        renderer.GetDiagnostic(),
        L"The OpenGL frame exceeds the 64 MiB CPU staging-buffer budget.");
}

TEST(VectorLayerRenderer, AppliesDetectedFramebufferLimitWithoutRequiringOpenGl)
{
    EXPECT_FALSE(mwg::activex::OpenGlVectorLayerRenderer::GetTargetRejectionReason(
        1280,
        720,
        4096,
        4096).has_value());

    const auto framebufferRejection =
        mwg::activex::OpenGlVectorLayerRenderer::GetTargetRejectionReason(
        2048,
        720,
        1024,
        4096);
    ASSERT_TRUE(framebufferRejection.has_value());
    EXPECT_EQ(
        *framebufferRejection,
        L"The OpenGL target exceeds the detected maximum framebuffer or viewport size.");

    // A viewport limit smaller than the renderbuffer limit must also reject.
    EXPECT_TRUE(mwg::activex::OpenGlVectorLayerRenderer::GetTargetRejectionReason(
        1024,
        900,
        4096,
        800).has_value());
}

TEST(VectorLayerRenderer, SharedTransformMatchesTheCoreViewport)
{
    mwg::Viewport viewport(120, 100);
    viewport.SetView({5.0, 5.0}, 0.125);
    const auto transform = mwg::activex::ViewTransform::Create(viewport);
    ASSERT_TRUE(transform.has_value());

    const mwg::rendering::Point points[] = {
        {5.0, 5.0}, {0.0, 10.0}, {10.0, 0.0}, {-3.25, 7.5}, {1.0e6, -1.0e6}};
    for (const mwg::rendering::Point& point : points)
    {
        const auto expected = viewport.MapToScreen({point.x, point.y});
        ASSERT_TRUE(expected.has_value());
        EXPECT_DOUBLE_EQ(transform->ToDeviceX(point), expected->x);
        EXPECT_DOUBLE_EQ(transform->ToDeviceY(point), expected->y);
    }
    EXPECT_DOUBLE_EQ(transform->ToDeviceX({5.0, 5.0}), 60.0);
    EXPECT_DOUBLE_EQ(transform->ToDeviceY({5.0, 5.0}), 50.0);
    EXPECT_DOUBLE_EQ(transform->ToDeviceX({0.0, 10.0}), 20.0);
    EXPECT_DOUBLE_EQ(transform->ToDeviceY({0.0, 10.0}), 10.0);
}

TEST(VectorLayerRenderer, SharedTransformRequiresAValidViewport)
{
    EXPECT_FALSE(mwg::activex::ViewTransform::Create(mwg::Viewport{}).has_value());
    EXPECT_FALSE(mwg::activex::ViewTransform::Create(mwg::Viewport(120, 100)).has_value());

    mwg::Viewport zeroWidth(0, 100);
    zeroWidth.SetView({0.0, 0.0}, 1.0);
    EXPECT_FALSE(mwg::activex::ViewTransform::Create(zeroWidth).has_value());
}

TEST(VectorLayerRenderer, BackendsRejectAViewportThatDoesNotMatchTheBounds)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    const mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    const mwg::Viewport otherSize = FitViewport({0, 0, 100, 100});

    mwg::activex::GdiVectorLayerRenderer gdi;
    EXPECT_EQ(gdi.Draw(surface.Get(), bounds, scene, otherSize), E_INVALIDARG);
    EXPECT_EQ(gdi.Draw(surface.Get(), bounds, scene, mwg::Viewport(120, 100)), E_INVALIDARG);

    mwg::activex::OpenGlVectorLayerRenderer openGl;
    EXPECT_EQ(openGl.Draw(surface.Get(), bounds, scene, otherSize), E_INVALIDARG);
    EXPECT_EQ(
        openGl.GetLastFailureReason(),
        L"The viewport is not valid or does not match the OpenGL target size.");
}

TEST(VectorLayerRenderer, DrawsAnEmptySceneAsBackgroundOnly)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    ASSERT_NE(FillRect(surface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);

    mwg::activex::GdiVectorLayerRenderer gdi;
    EXPECT_EQ(gdi.Draw(surface.Get(), bounds, {}, FitViewport(bounds)), S_OK);
    EXPECT_TRUE(gdi.GetFailedLayers().empty());
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), GetSysColor(COLOR_WINDOW));
}

TEST(VectorLayerRenderer, GdiDrawsLayersWithDifferentExtentsThroughOneViewport)
{
    BitmapSurface surface(200, 120);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 200, 120};
    ASSERT_NE(FillRect(surface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);
    const mwg::rendering::MapRenderScene scene = MakeTwoLayerScene();
    const mwg::Viewport viewport = FitViewport(bounds, kTwoLayerExtent);

    mwg::activex::GdiVectorLayerRenderer gdi;
    ASSERT_EQ(gdi.Draw(surface.Get(), bounds, scene, viewport), S_OK);
    EXPECT_TRUE(gdi.GetFailedLayers().empty());
    ExpectTwoLayerPixels(surface.Get(), viewport);

    // Moving the shared view moves both layers by the same amount.
    mwg::Viewport moved = viewport;
    moved.SetView({viewport.Center()->x + 1.5, viewport.Center()->y - 0.5}, *viewport.Resolution());
    ASSERT_NE(FillRect(surface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);
    ASSERT_EQ(gdi.Draw(surface.Get(), bounds, scene, moved), S_OK);
    ExpectTwoLayerPixels(surface.Get(), moved);
}

TEST(VectorLayerRenderer, GdiAndOpenGlDrawMultipleLayersAtTheSameViewportPositions)
{
    BitmapSurface gdiSurface(200, 120);
    BitmapSurface openGlSurface(200, 120);
    ASSERT_TRUE(gdiSurface.IsValid());
    ASSERT_TRUE(openGlSurface.IsValid());
    const RECT bounds{0, 0, 200, 120};
    ASSERT_NE(FillRect(gdiSurface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);
    const mwg::rendering::MapRenderScene scene = MakeTwoLayerScene();
    const mwg::Viewport viewport = FitViewport(bounds, kTwoLayerExtent);

    mwg::activex::GdiVectorLayerRenderer gdiRenderer;
    ASSERT_EQ(gdiRenderer.Draw(gdiSurface.Get(), bounds, scene, viewport), S_OK);

    mwg::activex::OpenGlVectorLayerRenderer openGlRenderer;
    const HRESULT openGlResult =
        openGlRenderer.Draw(openGlSurface.Get(), bounds, scene, viewport);
    if (openGlResult == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(openGlResult, S_OK);
    ExpectTwoLayerPixels(openGlSurface.Get(), viewport);
    ExpectTwoLayerPixels(gdiSurface.Get(), viewport);
}

TEST(VectorLayerRenderer, DrawsIntoBoundsWithAnOffsetOrigin)
{
    BitmapSurface surface(220, 140);
    ASSERT_TRUE(surface.IsValid());
    const RECT surfaceBounds{0, 0, 220, 140};
    ASSERT_NE(FillRect(surface.Get(), &surfaceBounds, GetSysColorBrush(COLOR_WINDOW)), 0);
    const RECT bounds{20, 20, 220, 140};
    const mwg::Viewport viewport = FitViewport(bounds, kTwoLayerExtent);

    mwg::activex::GdiVectorLayerRenderer gdi;
    ASSERT_EQ(gdi.Draw(surface.Get(), bounds, MakeTwoLayerScene(), viewport), S_OK);

    const auto first = mwg::ToPixel(*viewport.MapToScreen({1.0, 1.0}));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(GetPixel(surface.Get(), first->x + 20, first->y + 20), kFill);
    EXPECT_EQ(GetPixel(surface.Get(), 10, 10), GetSysColor(COLOR_WINDOW));
}

TEST(VectorLayerRenderer, GdiClipsCrossingPolygonsAndRestoresTheHostClip)
{
    for (const bool hasHostClip : {false, true})
    {
        SCOPED_TRACE(hasHostClip);
        BitmapSurface surface(200, 160);
        ASSERT_TRUE(surface.IsValid());
        ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 200, 160, WHITENESS));
        if (hasHostClip)
        {
            ASSERT_NE(IntersectClipRect(surface.Get(), 20, 20, 120, 150), ERROR);
        }
        RECT originalClip{};
        ASSERT_NE(GetClipBox(surface.Get(), &originalClip), ERROR);

        const RECT bounds{40, 30, 160, 130};
        mwg::Viewport viewport(120, 100);
        viewport.SetView({60.0, 50.0}, 1.0);
        const auto scene = MakeMapScene({MakeRectangleLayer({-50.0, -50.0, 170.0, 150.0})});
        mwg::activex::GdiVectorLayerRenderer gdi;
        ASSERT_EQ(gdi.Draw(surface.Get(), bounds, scene, viewport), S_OK);

        RECT restoredClip{};
        ASSERT_NE(GetClipBox(surface.Get(), &restoredClip), ERROR);
        EXPECT_TRUE(EqualRect(&originalClip, &restoredClip));
        // Remove the host clip so samples can also inspect protected pixels.
        ASSERT_NE(SelectClipRgn(surface.Get(), nullptr), ERROR);
        EXPECT_EQ(GetPixel(surface.Get(), 60, 60), kFill);
        EXPECT_EQ(GetPixel(surface.Get(), 39, 60), RGB(255, 255, 255));
        EXPECT_EQ(GetPixel(surface.Get(), 160, 60), RGB(255, 255, 255));
        EXPECT_EQ(GetPixel(surface.Get(), 60, 29), RGB(255, 255, 255));
        EXPECT_EQ(GetPixel(surface.Get(), 60, 130), RGB(255, 255, 255));
        EXPECT_EQ(GetPixel(surface.Get(), 125, 60), hasHostClip ? RGB(255, 255, 255) : kFill);
    }
}

TEST(VectorLayerRenderer, GdiSkipsLayerTraversalWhenTheHostClipDoesNotIntersectBounds)
{
    BitmapSurface surface(160, 160);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_NE(IntersectClipRect(surface.Get(), 140, 140, 160, 160), ERROR);
    RECT originalClip{};
    ASSERT_NE(GetClipBox(surface.Get(), &originalClip), ERROR);
    const RECT bounds{0, 0, 120, 100};
    const mwg::Viewport viewport = FitViewport(bounds);
    auto scene = MakeConvexPolygonScene();
    Edit(scene.layers.front()).polygons.Coordinates().front().x =
        std::numeric_limits<double>::infinity();
    const int saved = SaveDC(surface.Get());
    ASSERT_NE(saved, 0);
    ASSERT_NE(IntersectClipRect(surface.Get(), bounds.left, bounds.top, bounds.right, bounds.bottom),
              ERROR);
    RECT intersection{};
    EXPECT_EQ(GetClipBox(surface.Get(), &intersection), NULLREGION);
    ASSERT_TRUE(RestoreDC(surface.Get(), saved));
    mwg::activex::GdiVectorLayerRenderer renderer;
    EXPECT_EQ(renderer.Draw(surface.Get(), bounds, scene, viewport), S_OK);
    EXPECT_TRUE(renderer.GetFailedLayers().empty());
    RECT restoredClip{};
    ASSERT_NE(GetClipBox(surface.Get(), &restoredClip), ERROR);
    EXPECT_TRUE(EqualRect(&originalClip, &restoredClip));
}

TEST(VectorLayerRenderer, GdiAndOpenGlAgreeAtFractionalRectangleEdges)
{
    BitmapSurface gdiSurface(120, 100);
    BitmapSurface openGlSurface(120, 100);
    ASSERT_TRUE(gdiSurface.IsValid());
    ASSERT_TRUE(openGlSurface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    mwg::Viewport viewport(120, 100);
    viewport.SetView({60.0, 50.0}, 1.0);
    mwg::activex::GdiVectorLayerRenderer gdi;
    mwg::activex::OpenGlVectorLayerRenderer openGl;
    for (const double fraction : {0.25, 0.75})
    {
        const auto scene = MakeMapScene({
            MakeRectangleLayer({20.0 + fraction, 20.0 + fraction,
                                90.0 + fraction, 80.0 + fraction})});
        ASSERT_NE(FillRect(gdiSurface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);
        ASSERT_EQ(gdi.Draw(gdiSurface.Get(), bounds, scene, viewport), S_OK);
        const HRESULT result = openGl.Draw(openGlSurface.Get(), bounds, scene, viewport);
        if (result == S_FALSE)
        {
            GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
        }
        ASSERT_EQ(result, S_OK);
        for (int offset = -2; offset <= 2; ++offset)
        {
            for (const mwg::PixelPoint pixel : {
                     mwg::PixelPoint{20 + offset, 50}, {90 + offset, 50},
                     {60, 19 + offset}, {60, 79 + offset}})
            {
                EXPECT_EQ(GetPixel(gdiSurface.Get(), pixel.x, pixel.y),
                          GetPixel(openGlSurface.Get(), pixel.x, pixel.y))
                    << pixel.x << ", " << pixel.y << ", fraction " << fraction;
            }
        }
    }
}

TEST(VectorLayerRenderer, OverlappingLayersDrawBottomFirst)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    mwg::Viewport viewport(120, 100);
    viewport.SetView({60.0, 50.0}, 1.0);
    auto scene = MakeMapScene({
        MakeRectangleLayer({40.25, 30.25, 80.25, 70.25}),
        MakeRectangleLayer({20.25, 10.25, 100.25, 90.25})});
    mwg::activex::GdiVectorLayerRenderer gdi;
    ASSERT_NE(FillRect(surface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);
    ASSERT_EQ(gdi.Draw(surface.Get(), bounds, scene, viewport), S_OK);
    // The top layer's fill must cover the bottom layer's outline.
    EXPECT_EQ(GetPixel(surface.Get(), 40, 50), kFill);
    std::swap(scene.layers[0], scene.layers[1]);
    ASSERT_EQ(gdi.Draw(surface.Get(), bounds, scene, viewport), S_OK);
    EXPECT_EQ(GetPixel(surface.Get(), 40, 50), RGB(24, 58, 82));

    mwg::activex::OpenGlVectorLayerRenderer openGl;
    const HRESULT result = openGl.Draw(surface.Get(), bounds, scene, viewport);
    if (result == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(result, S_OK);
    EXPECT_EQ(GetPixel(surface.Get(), 40, 50), RGB(24, 58, 82));
    std::swap(scene.layers[0], scene.layers[1]);
    ASSERT_EQ(openGl.Draw(surface.Get(), bounds, scene, viewport), S_OK);
    EXPECT_EQ(GetPixel(surface.Get(), 40, 50), kFill);
}

TEST(VectorLayerRenderer, GdiAndOpenGlAgreeOnOverlappingPolygonsInOneLayer)
{
    BitmapSurface gdiSurface(120, 100);
    BitmapSurface openGlSurface(120, 100);
    ASSERT_TRUE(gdiSurface.IsValid());
    ASSERT_TRUE(openGlSurface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    mwg::Viewport viewport(120, 100);
    viewport.SetView({60.0, 50.0}, 1.0);

    // One layer with two overlapping polygons. The second polygon is drawn
    // after the first, so its fill must cover the first polygon's outlines
    // inside the overlap, exactly as the GDI backend paints them.
    mwg::rendering::VectorRenderScene layer;
    layer.extent = {20.25, 10.25, 90.25, 70.25};
    layer.sourceFeatureCount = 2;
    layer.polygons = {
        {{{20.25, 10.25}, {60.25, 10.25}, {60.25, 50.25}, {20.25, 50.25}, {20.25, 10.25}}},
        {{{40.25, 30.25}, {90.25, 30.25}, {90.25, 70.25}, {40.25, 70.25}, {40.25, 30.25}}}};
    const mwg::rendering::MapRenderScene scene = MakeMapScene({layer});

    mwg::activex::GdiVectorLayerRenderer gdi;
    ASSERT_NE(FillRect(gdiSurface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);
    ASSERT_EQ(gdi.Draw(gdiSurface.Get(), bounds, scene, viewport), S_OK);

    mwg::activex::OpenGlVectorLayerRenderer openGl;
    const HRESULT result = openGl.Draw(openGlSurface.Get(), bounds, scene, viewport);
    if (result == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(result, S_OK);

    // The first polygon's right edge (pixel column 60) and top edge (pixel
    // row 49) stay visible only outside the second polygon; inside the
    // overlap the second polygon's fill covers them in both backends.
    constexpr COLORREF kOutline = RGB(24, 58, 82);
    for (const mwg::PixelPoint pixel : {mwg::PixelPoint{60, 60}, {50, 49}})
    {
        EXPECT_EQ(GetPixel(gdiSurface.Get(), pixel.x, pixel.y), kFill)
            << "GDI at " << pixel.x << ", " << pixel.y;
        EXPECT_EQ(GetPixel(openGlSurface.Get(), pixel.x, pixel.y), kFill)
            << "OpenGL at " << pixel.x << ", " << pixel.y;
    }
    for (const mwg::PixelPoint pixel : {mwg::PixelPoint{60, 80}, {40, 60}})
    {
        EXPECT_EQ(GetPixel(gdiSurface.Get(), pixel.x, pixel.y), kOutline)
            << "GDI at " << pixel.x << ", " << pixel.y;
        EXPECT_EQ(GetPixel(openGlSurface.Get(), pixel.x, pixel.y), kOutline)
            << "OpenGL at " << pixel.x << ", " << pixel.y;
    }

    // Away from the boundaries both backends must agree as well.
    for (const mwg::PixelPoint pixel :
             {mwg::PixelPoint{30, 70}, {70, 40}, {10, 10}})
    {
        EXPECT_EQ(GetPixel(gdiSurface.Get(), pixel.x, pixel.y),
                  GetPixel(openGlSurface.Get(), pixel.x, pixel.y))
            << pixel.x << ", " << pixel.y;
    }
}

TEST(VectorLayerRenderer, DrawsVerySmallAndVeryElongatedTargets)
{
    BitmapSurface surface(2000, 400);
    ASSERT_TRUE(surface.IsValid());
    const mwg::rendering::MapRenderScene scene = MakeTwoLayerScene();
    const RECT targets[] = {{0, 0, 1, 1}, {0, 0, 2000, 3}, {0, 0, 3, 400}};
    for (const RECT& bounds : targets)
    {
        const mwg::Viewport viewport = FitViewport(bounds, kTwoLayerExtent);
        mwg::activex::GdiVectorLayerRenderer gdi;
        EXPECT_EQ(gdi.Draw(surface.Get(), bounds, scene, viewport), S_OK)
            << bounds.right << " x " << bounds.bottom;

        mwg::activex::VectorLayerRenderer renderer;
        EXPECT_EQ(renderer.Draw(surface.Get(), bounds, scene, viewport), S_OK)
            << bounds.right << " x " << bounds.bottom;
    }
}

TEST(PanBlitTest, AcceptsOnlyWholePixelShiftsInsideTheFrame)
{
    LONG shift = 99;
    EXPECT_TRUE(mwg::activex::TryGetWholePixelPanShift(12.0, 100, shift));
    EXPECT_EQ(shift, 12);

    EXPECT_TRUE(mwg::activex::TryGetWholePixelPanShift(-7.0 + 5e-7, 100, shift));
    EXPECT_EQ(shift, -7);

    shift = 99;
    EXPECT_FALSE(mwg::activex::TryGetWholePixelPanShift(2.25, 100, shift));
    EXPECT_EQ(shift, 99);
    EXPECT_FALSE(mwg::activex::TryGetWholePixelPanShift(99.9999999, 100, shift));
    EXPECT_FALSE(mwg::activex::TryGetWholePixelPanShift(100.0, 100, shift));
    EXPECT_FALSE(mwg::activex::TryGetWholePixelPanShift(-100.0, 100, shift));
    EXPECT_FALSE(mwg::activex::TryGetWholePixelPanShift(
        std::numeric_limits<double>::infinity(), 100, shift));
    EXPECT_FALSE(mwg::activex::TryGetWholePixelPanShift(
        std::numeric_limits<double>::quiet_NaN(), 100, shift));
    EXPECT_FALSE(mwg::activex::TryGetWholePixelPanShift(0.0, 0, shift));
    EXPECT_FALSE(mwg::activex::TryGetWholePixelPanShift(
        std::numeric_limits<double>::max(), 100, shift));
}

TEST(VectorLayerRenderer, AlternateSizeDrawPreservesScreenStaging)
{
    using Access = mwg::activex::testing::OpenGlVectorLayerRendererTestAccess;
    BitmapSurface surface(240, 201);
    ASSERT_TRUE(surface.IsValid());
    const auto scene = MakeTwoLayerScene();
    const RECT screenBounds{0, 0, 120, 100};
    const auto screenViewport = FitViewport(screenBounds, kTwoLayerExtent);
    mwg::activex::OpenGlVectorLayerRenderer renderer;
    const HRESULT first = renderer.Draw(surface.Get(), screenBounds, scene, screenViewport);
    if (first == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(first, S_OK);
    const auto capacity = Access::StagingCapacity(renderer);
    const auto* data = Access::StagingData(renderer);
    ASSERT_GT(capacity, 0U);

    const RECT scaledBounds{0, 0, 240, 201};
    const auto scaledViewport = FitViewport(scaledBounds, kTwoLayerExtent);
    ASSERT_EQ(renderer.Draw(surface.Get(), scaledBounds, scene, scaledViewport, false), S_OK);
    ExpectTwoLayerPixels(surface.Get(), scaledViewport);
    EXPECT_EQ(Access::StagingCapacity(renderer), capacity);
    EXPECT_EQ(Access::StagingData(renderer), data);
    EXPECT_EQ(renderer.Draw(nullptr, scaledBounds, scene, scaledViewport, false), E_INVALIDARG);
    EXPECT_EQ(Access::StagingCapacity(renderer), capacity);
    EXPECT_EQ(Access::StagingData(renderer), data);
    ASSERT_EQ(renderer.Draw(surface.Get(), screenBounds, scene, screenViewport), S_OK);
    ExpectTwoLayerPixels(surface.Get(), screenViewport);
}

TEST(VectorLayerRenderer, FailedContextRecoveryBacksOffAndRetriesAfterTheDelayOrResize)
{
    using Access = mwg::activex::testing::OpenGlVectorLayerRendererTestAccess;
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const auto scene = MakeConvexPolygonScene();
    const RECT bounds{0, 0, 120, 100};
    const auto viewport = FitViewport(bounds);
    mwg::activex::OpenGlVectorLayerRenderer renderer;
    Access::SimulateFailedRecovery(renderer, 120, 100);
    for (unsigned int remaining = 60; remaining > 0; --remaining)
    {
        ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, viewport), S_FALSE);
        EXPECT_EQ(renderer.GetLastFailureReason(), L"Simulated context initialization failure.");
        EXPECT_EQ(Access::RetryDrawsRemaining(renderer), remaining - 1);
    }
    EXPECT_TRUE(SUCCEEDED(renderer.Draw(surface.Get(), bounds, scene, viewport)));
    EXPECT_NE(renderer.GetLastFailureReason(), L"Simulated context initialization failure.");

    Access::SimulateFailedRecovery(renderer, 119, 100);
    EXPECT_TRUE(SUCCEEDED(renderer.Draw(surface.Get(), bounds, scene, viewport)));
    EXPECT_NE(renderer.GetLastFailureReason(), L"Simulated context initialization failure.");
}

TEST(VectorLayerRenderer, NonRetainingDrawsPreserveInitializationRetryBackoff)
{
    using Access = mwg::activex::testing::OpenGlVectorLayerRendererTestAccess;
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const auto scene = MakeConvexPolygonScene();
    const RECT bounds{0, 0, 120, 100};
    const auto viewport = FitViewport(bounds);
    mwg::activex::OpenGlVectorLayerRenderer renderer;
    Access::SimulateFailedRecovery(renderer, 120, 100);

    for (unsigned int remaining = 59; remaining > 56; --remaining)
    {
        ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, viewport, false), S_FALSE);
        EXPECT_EQ(renderer.GetLastFailureReason(), L"Simulated context initialization failure.");
        EXPECT_EQ(Access::RetryDrawsRemaining(renderer), remaining);
    }
}
