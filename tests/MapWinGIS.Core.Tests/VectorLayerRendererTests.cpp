// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorLayerRendererTests.cpp
 * @brief Tests OpenGL rendering and the GDI fallback for vector scenes.
 */
#include <Windows.h>

#include <gtest/gtest.h>

#include <limits>
#include <string_view>

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

    [[nodiscard]] mwg::rendering::VectorRenderScene MakeConvexPolygonScene()
    {
        return {
            mwg::Extent{0.0, 0.0, 10.0, 10.0},
            {{{{1.0, 1.0}, {9.0, 1.0}, {9.0, 9.0}, {1.0, 9.0}, {1.0, 1.0}}}}};
    }
}

TEST(VectorLayerRenderer, UsesOpenGlForConvexPolygonWhenAvailable)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));

    const RECT bounds{0, 0, 120, 100};
    const mwg::rendering::VectorRenderScene scene = MakeConvexPolygonScene();

    // Only an unavailable OpenGL target may skip; drawing, readback or
    // presentation failures in the backend must fail the test.
    mwg::activex::OpenGlVectorLayerRenderer openGlRenderer;
    const HRESULT openGlResult = openGlRenderer.Draw(surface.Get(), bounds, scene);
    if (openGlResult == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(openGlResult, S_OK);
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), RGB(61, 122, 169));
    EXPECT_EQ(GetPixel(surface.Get(), 5, 5), GetSysColor(COLOR_WINDOW));

    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    mwg::activex::VectorLayerRenderer renderer;
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene), S_OK);
    EXPECT_EQ(
        renderer.GetActiveBackend(),
        mwg::activex::VectorRenderBackendKind::OpenGL);
    EXPECT_EQ(renderer.GetDiagnostic(), L"OpenGL rendered the frame successfully.");
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), RGB(61, 122, 169));
    EXPECT_EQ(GetPixel(surface.Get(), 5, 5), GetSysColor(COLOR_WINDOW));
}

TEST(VectorLayerRenderer, OpenGlRecreatesLostContextOnLaterDraw)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    const mwg::rendering::VectorRenderScene scene = MakeConvexPolygonScene();

    mwg::activex::OpenGlVectorLayerRenderer renderer;
    const HRESULT firstResult = renderer.Draw(surface.Get(), bounds, scene);
    if (firstResult == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(firstResult, S_OK);

    renderer.SimulateContextLossForTesting();
    EXPECT_EQ(renderer.Draw(surface.Get(), bounds, scene), E_FAIL);
    EXPECT_EQ(
        renderer.GetLastFailureReason(),
        L"OpenGL could not activate its rendering context.");

    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene), S_OK);
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), RGB(61, 122, 169));
    EXPECT_EQ(GetPixel(surface.Get(), 5, 5), GetSysColor(COLOR_WINDOW));
}

TEST(VectorLayerRenderer, OpenGlRejectsConcaveCornerHiddenByDuplicateVertex)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::VectorRenderScene scene = MakeConvexPolygonScene();
    scene.polygons.front().front() = {
        {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {5.0, 5.0}, {5.0, 5.0}, {0.0, 10.0}, {0.0, 0.0}};

    mwg::activex::OpenGlVectorLayerRenderer renderer;
    EXPECT_EQ(renderer.Draw(surface.Get(), bounds, scene), S_FALSE);
}

TEST(VectorLayerRenderer, OpenGlRejectsSelfIntersectingRing)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::VectorRenderScene scene{
        mwg::Extent{-3.0, -3.0, 3.0, 3.0},
        {{{{0.0, 3.0}, {2.0, -3.0}, {-3.0, 1.0}, {3.0, 1.0}, {-2.0, -3.0}, {0.0, 3.0}}}}};

    mwg::activex::OpenGlVectorLayerRenderer renderer;
    EXPECT_EQ(renderer.Draw(surface.Get(), bounds, scene), S_FALSE);
}

TEST(VectorLayerRenderer, FailsWithoutDrawingNonFiniteCoordinates)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 120, 100, BLACKNESS));
    const RECT bounds{0, 0, 120, 100};

    mwg::rendering::VectorRenderScene scene = MakeConvexPolygonScene();
    scene.polygons.push_back(
        {{{1.0, 1.0}, {std::numeric_limits<double>::quiet_NaN(), 5.0}, {9.0, 9.0}, {1.0, 1.0}}});

    mwg::activex::VectorLayerRenderer renderer;
    EXPECT_EQ(renderer.Draw(surface.Get(), bounds, scene), E_INVALIDARG);
    EXPECT_FALSE(renderer.GetActiveBackend().has_value());
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), RGB(0, 0, 0));
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

    mwg::rendering::VectorRenderScene scene = MakeConvexPolygonScene();
    scene.polygons.front().front() = {
        {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}};
    scene.polygons.front().push_back(
        {{3.0, 3.0}, {3.0, 7.0}, {7.0, 7.0}, {7.0, 3.0}, {3.0, 3.0}});
    mwg::activex::VectorLayerRenderer renderer;
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene), S_OK);

    EXPECT_EQ(
        renderer.GetActiveBackend(),
        mwg::activex::VectorRenderBackendKind::Gdi);
    EXPECT_EQ(
        renderer.GetDiagnostic(),
        L"The scene uses geometry that the OpenGL backend does not support.");
    EXPECT_EQ(GetPixel(surface.Get(), 30, 50), RGB(61, 122, 169));
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), GetSysColor(COLOR_WINDOW));
}

TEST(VectorLayerRenderer, HonorsCpuPreferenceAndReportsTheSelectedBackend)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    const mwg::rendering::VectorRenderScene scene = MakeConvexPolygonScene();
    mwg::activex::VectorLayerRenderer renderer;
    renderer.SetPreference(mwg::activex::VectorRenderBackendPreference::Gdi);

    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene), S_OK);

    EXPECT_EQ(renderer.GetPreference(), mwg::activex::VectorRenderBackendPreference::Gdi);
    EXPECT_EQ(renderer.GetActiveBackend(), mwg::activex::VectorRenderBackendKind::Gdi);
    EXPECT_EQ(renderer.GetDiagnostic(), L"GDI was selected by the rendering preference.");
}

TEST(VectorLayerRenderer, FallsBackToCpuWhenOpenGlStagingBudgetIsExceeded)
{
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    const RECT bounds{0, 0, 5000, 5000};
    const mwg::rendering::VectorRenderScene scene = MakeConvexPolygonScene();
    mwg::activex::VectorLayerRenderer renderer;

    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene), S_OK);

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

TEST(VectorLayerRenderer, CreatesTheSameSharedTransformForBothBackends)
{
    const mwg::Extent extent{0.0, 0.0, 10.0, 10.0};
    const auto gdiTransform =
        mwg::activex::ViewTransform::Create(extent, 120.0, 100.0);
    const auto openGlTransform =
        mwg::activex::ViewTransform::Create(extent, 120.0, 100.0);

    ASSERT_TRUE(gdiTransform.has_value());
    ASSERT_TRUE(openGlTransform.has_value());
    EXPECT_DOUBLE_EQ(gdiTransform->scale, openGlTransform->scale);
    EXPECT_DOUBLE_EQ(gdiTransform->offsetX, openGlTransform->offsetX);
    EXPECT_DOUBLE_EQ(gdiTransform->offsetY, openGlTransform->offsetY);
    EXPECT_DOUBLE_EQ(gdiTransform->ToDeviceX({0.0, 10.0}), 22.0);
    EXPECT_DOUBLE_EQ(gdiTransform->ToDeviceY({0.0, 10.0}), 12.0);
    EXPECT_DOUBLE_EQ(openGlTransform->ToDeviceX({10.0, 0.0}), 98.0);
    EXPECT_DOUBLE_EQ(openGlTransform->ToDeviceY({10.0, 0.0}), 88.0);
}

TEST(VectorLayerRenderer, GdiAndOpenGlDrawAtTheSharedTransformCoordinates)
{
    BitmapSurface gdiSurface(120, 100);
    BitmapSurface openGlSurface(120, 100);
    ASSERT_TRUE(gdiSurface.IsValid());
    ASSERT_TRUE(openGlSurface.IsValid());
    const RECT bounds{0, 0, 120, 100};
    const auto scene = MakeConvexPolygonScene();
    ASSERT_NE(
        FillRect(gdiSurface.Get(), &bounds, GetSysColorBrush(COLOR_WINDOW)),
        0);

    mwg::activex::GdiVectorLayerRenderer gdiRenderer;
    ASSERT_EQ(gdiRenderer.Draw(gdiSurface.Get(), bounds, scene), S_OK);

    mwg::activex::OpenGlVectorLayerRenderer openGlRenderer;
    const HRESULT openGlResult = openGlRenderer.Draw(
        openGlSurface.Get(),
        bounds,
        scene);
    if (openGlResult == S_FALSE)
    {
        GTEST_SKIP() << "No OpenGL context with framebuffer objects is available.";
    }
    ASSERT_EQ(openGlResult, S_OK);

    const POINT samplePoints[] = {
        {60, 50},
        {85, 50},
        {25, 50},
        {95, 50}};
    for (const POINT point : samplePoints)
    {
        EXPECT_EQ(
            GetPixel(gdiSurface.Get(), point.x, point.y),
            GetPixel(openGlSurface.Get(), point.x, point.y))
            << "at (" << point.x << ", " << point.y << ")";
    }
    EXPECT_EQ(GetPixel(gdiSurface.Get(), 60, 50), RGB(61, 122, 169));
    EXPECT_EQ(GetPixel(gdiSurface.Get(), 95, 50), GetSysColor(COLOR_WINDOW));
}

TEST(VectorLayerRenderer, RejectsDegenerateExtentsAndZeroAreaTargets)
{
    EXPECT_FALSE(mwg::activex::ViewTransform::Create(
        mwg::Extent{1.0, 0.0, 1.0, 10.0},
        120.0,
        100.0).has_value());
    EXPECT_FALSE(mwg::activex::ViewTransform::Create(
        mwg::Extent{0.0, 1.0, 10.0, 1.0},
        120.0,
        100.0).has_value());
    EXPECT_FALSE(mwg::activex::ViewTransform::Create(
        mwg::Extent{0.0, 0.0, 10.0, 10.0},
        0.0,
        100.0).has_value());
    EXPECT_FALSE(mwg::activex::ViewTransform::Create(
        mwg::Extent{0.0, 0.0, 10.0, 10.0},
        120.0,
        0.0).has_value());
}
