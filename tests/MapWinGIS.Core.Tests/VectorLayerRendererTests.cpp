// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorLayerRendererTests.cpp
 * @brief Tests OpenGL rendering and the GDI fallback for vector scenes.
 */
#include <Windows.h>

#include <gtest/gtest.h>

#include "VectorLayerRenderer.h"

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

    mwg::activex::VectorLayerRenderer renderer;
    const RECT bounds{0, 0, 120, 100};
    const mwg::rendering::VectorRenderScene scene = MakeConvexPolygonScene();
    ASSERT_EQ(renderer.Draw(surface.Get(), bounds, scene), S_OK);

    const auto backend = renderer.GetActiveBackend();
    ASSERT_TRUE(backend.has_value());
    if (*backend != mwg::activex::VectorRenderBackendKind::OpenGL)
    {
        GTEST_SKIP() << "OpenGL context is unavailable; GDI fallback is tested separately.";
    }

    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), RGB(61, 122, 169));
    EXPECT_EQ(GetPixel(surface.Get(), 5, 5), GetSysColor(COLOR_WINDOW));
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
    EXPECT_EQ(GetPixel(surface.Get(), 30, 50), RGB(61, 122, 169));
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), GetSysColor(COLOR_WINDOW));
}
