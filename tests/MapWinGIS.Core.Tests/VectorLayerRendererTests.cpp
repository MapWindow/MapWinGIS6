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
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "OpenGlVectorLayerRenderer.h"
#include "VectorLayerRenderer.h"
#include "ViewTransform.h"

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
            {{{{rectangle.xMin, rectangle.yMin},
               {rectangle.xMax, rectangle.yMin},
               {rectangle.xMax, rectangle.yMax},
               {rectangle.xMin, rectangle.yMax},
               {rectangle.xMin, rectangle.yMin}}}}};
    }

    /// A map scene with these layers, bottom first, with identities 1, 2, ...
    [[nodiscard]] mwg::rendering::MapRenderScene MakeMapScene(
        std::vector<mwg::rendering::VectorRenderScene> layers)
    {
        mwg::rendering::MapRenderScene scene;
        std::uint64_t id = 0;
        for (mwg::rendering::VectorRenderScene& layer : layers)
        {
            scene.layers.push_back({mwg::LayerId{++id}, std::move(layer)});
        }
        return scene;
    }

    [[nodiscard]] mwg::rendering::MapRenderScene MakeConvexPolygonScene()
    {
        return MakeMapScene({MakeRectangleLayer({1.0, 1.0, 9.0, 9.0})});
    }

    /// The first polygon of the first layer, for tests that change geometry.
    [[nodiscard]] mwg::rendering::Polygon& FirstPolygon(mwg::rendering::MapRenderScene& scene)
    {
        return scene.layers.front().scene.polygons.front();
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

    renderer.SimulateContextLossForTesting();
    EXPECT_EQ(renderer.Draw(surface.Get(), bounds, scene, viewport), E_FAIL);
    EXPECT_EQ(
        renderer.GetLastFailureReason(),
        L"OpenGL could not activate its rendering context.");

    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, viewport), S_OK);
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), kFill);
    EXPECT_EQ(GetPixel(surface.Get(), 5, 5), GetSysColor(COLOR_WINDOW));
}

TEST(VectorLayerRenderer, OpenGlRejectsConcaveCornerHiddenByDuplicateVertex)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    FirstPolygon(scene).front() = {
        {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {5.0, 5.0}, {5.0, 5.0}, {0.0, 10.0}, {0.0, 0.0}};

    mwg::activex::OpenGlVectorLayerRenderer renderer;
    EXPECT_EQ(renderer.Draw(surface.Get(), bounds, scene, FitViewport(bounds)), S_FALSE);
}

TEST(VectorLayerRenderer, OpenGlRejectsSelfIntersectingRing)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};

    const mwg::rendering::MapRenderScene scene = MakeMapScene({{
        mwg::Extent{-3.0, -3.0, 3.0, 3.0},
        0,
        {{{{0.0, 3.0}, {2.0, -3.0}, {-3.0, 1.0}, {3.0, 1.0}, {-2.0, -3.0}, {0.0, 3.0}}}}}});

    mwg::activex::OpenGlVectorLayerRenderer renderer;
    EXPECT_EQ(
        renderer.Draw(surface.Get(), bounds, scene, FitViewport(bounds, {-3.0, -3.0, 3.0, 3.0})),
        S_FALSE);
}

TEST(VectorLayerRenderer, OpenGlRejectsTheFrameWhenAnyLayerIsUnsupported)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::MapRenderScene scene = MakeTwoLayerScene();
    scene.layers.back().scene.polygons.front().push_back(
        {{8.5, 3.5}, {8.5, 4.5}, {9.5, 4.5}, {9.5, 3.5}, {8.5, 3.5}});

    mwg::activex::OpenGlVectorLayerRenderer renderer;
    EXPECT_EQ(
        renderer.Draw(surface.Get(), bounds, scene, FitViewport(bounds, kTwoLayerExtent)),
        S_FALSE);
    EXPECT_EQ(
        renderer.GetLastFailureReason(),
        L"The scene uses geometry that the OpenGL backend does not support.");
}

TEST(VectorLayerRenderer, FailsWithoutDrawingNonFiniteCoordinates)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    scene.layers.front().scene.polygons.push_back(
        {{{1.0, 1.0}, {std::numeric_limits<double>::quiet_NaN(), 5.0}, {9.0, 9.0}, {1.0, 1.0}}});

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
    scene.layers.front().scene.polygons.push_back(
        {{{0.0, 0.0}, {std::numeric_limits<double>::infinity(), 1.0}, {1.0, 1.0}, {0.0, 0.0}}});
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

TEST(VectorLayerRenderer, UsesGdiForPolygonWithInteriorRing)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    const RECT bounds{0, 0, 120, 100};
    ASSERT_NE(
        FillRect(surface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)),
        0);

    mwg::rendering::MapRenderScene scene = MakeConvexPolygonScene();
    FirstPolygon(scene).front() = {
        {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}};
    FirstPolygon(scene).push_back(
        {{3.0, 3.0}, {3.0, 7.0}, {7.0, 7.0}, {7.0, 3.0}, {3.0, 3.0}});
    mwg::activex::VectorLayerRenderer renderer;
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene, FitViewport(bounds)), S_OK);

    EXPECT_EQ(
        renderer.GetActiveBackend(),
        mwg::activex::VectorRenderBackendKind::Gdi);
    EXPECT_EQ(
        renderer.GetDiagnostic(),
        L"The scene uses geometry that the OpenGL backend does not support.");
    EXPECT_EQ(GetPixel(surface.Get(), 30, 50), kFill);
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), GetSysColor(COLOR_WINDOW));
}

TEST(VectorLayerRenderer, DrawsPointAndLineGeometryWithGdi)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    ASSERT_NE(
        FillRect(surface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)),
        0);

    mwg::rendering::MapRenderScene scene;
    mwg::rendering::VectorRenderScene layer;
    layer.extent = {0.0, 0.0, 10.0, 10.0};
    layer.lines.push_back({{1.0, 5.0}, {9.0, 5.0}});
    layer.points.push_back({5.0, 2.0});
    scene.layers.push_back({mwg::LayerId{1}, std::move(layer)});

    mwg::activex::VectorLayerRenderer renderer;
    renderer.SetPreference(mwg::activex::VectorRenderBackendPreference::Gdi);
    ASSERT_EQ(
        renderer.Draw(
            surface.Get(),
            bounds,
            scene,
            FitViewport(bounds, {0.0, 0.0, 10.0, 10.0})),
        S_OK);
    EXPECT_EQ(renderer.GetActiveBackend(), mwg::activex::VectorRenderBackendKind::Gdi);
    EXPECT_TRUE(renderer.GetFailedLayers().empty());
    EXPECT_EQ(MapPixel(surface.Get(), FitViewport(bounds, {0.0, 0.0, 10.0, 10.0}), 5.0, 5.0),
              RGB(24, 58, 82));
    EXPECT_EQ(MapPixel(surface.Get(), FitViewport(bounds, {0.0, 0.0, 10.0, 10.0}), 5.0, 2.0),
              kFill);
}

TEST(VectorLayerRenderer, DrawsRasterLayersWithAlphaBlending)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    ASSERT_NE(
        FillRect(surface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)),
        0);

    mwg::rendering::MapRenderScene scene;
    mwg::RasterImage image;
    image.width = 1;
    image.height = 1;
    image.extent = {0.0, 0.0, 10.0, 10.0};
    image.pixels = {0xFF2A2A2AU};
    scene.layers.push_back({mwg::LayerId{1}, {}, std::move(image)});

    mwg::activex::VectorLayerRenderer renderer;
    renderer.SetPreference(mwg::activex::VectorRenderBackendPreference::Gdi);
    ASSERT_EQ(renderer.Draw(
                  surface.Get(),
                  bounds,
                  scene,
                  FitViewport(bounds, {0.0, 0.0, 10.0, 10.0})),
              S_OK);
    EXPECT_EQ(renderer.GetActiveBackend(), mwg::activex::VectorRenderBackendKind::Gdi);
    EXPECT_EQ(
        MapPixel(surface.Get(), FitViewport(bounds, {0.0, 0.0, 10.0, 10.0}), 5.0, 5.0),
        RGB(42, 42, 42));
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

TEST(VectorLayerRenderer, ClipsOffscreenGeometryBeforeConvertingToGdiCoordinates)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    ASSERT_NE(FillRect(surface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)), 0);
    auto scene = MakeMapScene({
        MakeRectangleLayer({-1.0e12, -1.0e12, 1.0e12, 1.0e12})});
    mwg::rendering::VectorRenderScene lineLayer;
    lineLayer.lines.push_back({{-1.0e12, 5.0}, {1.0e12, 5.0}});
    scene.layers.push_back({mwg::LayerId{2}, std::move(lineLayer)});
    const mwg::Viewport viewport = FitViewport(bounds);

    mwg::activex::GdiVectorLayerRenderer gdi;
    EXPECT_EQ(gdi.Draw(surface.Get(), bounds, scene, viewport), S_OK);
    EXPECT_TRUE(gdi.GetFailedLayers().empty());
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), RGB(24, 58, 82));
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
