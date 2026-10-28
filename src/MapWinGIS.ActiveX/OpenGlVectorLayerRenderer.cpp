// SPDX-License-Identifier: MPL-2.0

/**
 * @file OpenGlVectorLayerRenderer.cpp
 * @brief Renders convex polygon scenes with WGL and copies pixels to GDI.
 *
 * Owns one reusable OpenGL 1.1 context. Scenes that need unsupported polygon
 * features return S_FALSE so the coordinator can use its GDI backend.
 */
#include "OpenGlVectorLayerRenderer.h"

#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>

#pragma comment(lib, "opengl32.lib")

namespace mwg::activex
{
    namespace
    {
        constexpr int kMargin = 12;

        class ScopedCurrentContext final
        {
        public:
            explicit ScopedCurrentContext(HDC deviceContext, HGLRC renderingContext) noexcept
                : _previousDeviceContext(wglGetCurrentDC()),
                  _previousRenderingContext(wglGetCurrentContext()),
                  _valid(wglMakeCurrent(deviceContext, renderingContext) != FALSE)
            {
            }

            ~ScopedCurrentContext()
            {
                if (_valid)
                {
                    wglMakeCurrent(_previousDeviceContext, _previousRenderingContext);
                }
            }

            [[nodiscard]] bool IsValid() const noexcept { return _valid; }

        private:
            HDC _previousDeviceContext;
            HGLRC _previousRenderingContext;
            bool _valid;
        };

        [[nodiscard]] bool IsConvex(const rendering::Ring& ring) noexcept
        {
            std::size_t count = ring.size();
            if (count > 1 &&
                ring.front().x == ring.back().x &&
                ring.front().y == ring.back().y)
            {
                --count;
            }

            if (count < 3)
            {
                return false;
            }

            double orientation = 0.0;
            for (std::size_t index = 0; index < count; ++index)
            {
                const rendering::Point& first = ring[index];
                const rendering::Point& second = ring[(index + 1) % count];
                const rendering::Point& third = ring[(index + 2) % count];
                const double cross =
                    (second.x - first.x) * (third.y - second.y) -
                    (second.y - first.y) * (third.x - second.x);
                if (!std::isfinite(cross))
                {
                    return false;
                }

                if (cross == 0.0)
                {
                    continue;
                }

                if (orientation == 0.0)
                {
                    orientation = cross;
                }
                else if ((orientation > 0.0) != (cross > 0.0))
                {
                    return false;
                }
            }

            return orientation != 0.0;
        }

        [[nodiscard]] bool IsSupported(const rendering::VectorRenderScene& scene) noexcept
        {
            for (const rendering::Polygon& polygon : scene.polygons)
            {
                if (polygon.size() != 1 || !IsConvex(polygon.front()))
                {
                    return false;
                }

                for (const rendering::Point& point : polygon.front())
                {
                    if (!std::isfinite(point.x) || !std::isfinite(point.y))
                    {
                        return false;
                    }
                }
            }

            return true;
        }

        [[nodiscard]] std::int64_t GetWidth(const RECT& bounds) noexcept
        {
            return static_cast<std::int64_t>(bounds.right) - bounds.left;
        }

        [[nodiscard]] std::int64_t GetHeight(const RECT& bounds) noexcept
        {
            return static_cast<std::int64_t>(bounds.bottom) - bounds.top;
        }
    }

    OpenGlVectorLayerRenderer::~OpenGlVectorLayerRenderer()
    {
        Release();
    }

    bool OpenGlVectorLayerRenderer::Initialize() noexcept
    {
        if (_renderingContext != nullptr)
        {
            return true;
        }
        if (_initializationFailed)
        {
            return false;
        }

        _window = CreateWindowExW(
            0,
            L"STATIC",
            L"MapWinGIS OpenGL render target",
            WS_POPUP,
            0,
            0,
            1,
            1,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (_window == nullptr)
        {
            _initializationFailed = true;
            OutputDebugStringW(L"MapWinGIS: could not create the OpenGL render target window.\n");
            Release();
            return false;
        }

        _deviceContext = GetDC(_window);
        if (_deviceContext == nullptr)
        {
            _initializationFailed = true;
            OutputDebugStringW(L"MapWinGIS: could not acquire the OpenGL render target DC.\n");
            Release();
            return false;
        }

        PIXELFORMATDESCRIPTOR descriptor{};
        descriptor.nSize = sizeof(descriptor);
        descriptor.nVersion = 1;
        descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
        descriptor.iPixelType = PFD_TYPE_RGBA;
        descriptor.cColorBits = 32;
        descriptor.cAlphaBits = 8;
        descriptor.iLayerType = PFD_MAIN_PLANE;

        const int pixelFormat = ChoosePixelFormat(_deviceContext, &descriptor);
        if (pixelFormat == 0 || SetPixelFormat(_deviceContext, pixelFormat, &descriptor) == FALSE)
        {
            _initializationFailed = true;
            OutputDebugStringW(L"MapWinGIS: could not select an OpenGL pixel format.\n");
            Release();
            return false;
        }

        _renderingContext = wglCreateContext(_deviceContext);
        if (_renderingContext == nullptr)
        {
            _initializationFailed = true;
            OutputDebugStringW(L"MapWinGIS: could not create an OpenGL rendering context.\n");
            Release();
            return false;
        }

        return true;
    }

    void OpenGlVectorLayerRenderer::Release() noexcept
    {
        if (_renderingContext != nullptr)
        {
            if (wglGetCurrentContext() == _renderingContext)
            {
                wglMakeCurrent(nullptr, nullptr);
            }
            wglDeleteContext(_renderingContext);
            _renderingContext = nullptr;
        }

        if (_deviceContext != nullptr && _window != nullptr)
        {
            ReleaseDC(_window, _deviceContext);
            _deviceContext = nullptr;
        }

        if (_window != nullptr)
        {
            DestroyWindow(_window);
            _window = nullptr;
        }
    }

    HRESULT OpenGlVectorLayerRenderer::Draw(
        const HDC deviceContext,
        const RECT& bounds,
        const rendering::VectorRenderScene& scene) noexcept
    {
        if (deviceContext == nullptr)
        {
            return E_INVALIDARG;
        }

        if (!IsSupported(scene))
        {
            return S_FALSE;
        }

        if (scene.extent.IsEmpty() ||
            scene.extent.Width() <= 0.0 ||
            scene.extent.Height() <= 0.0)
        {
            return E_INVALIDARG;
        }

        const std::int64_t width = GetWidth(bounds);
        const std::int64_t height = GetHeight(bounds);
        if (width <= 2 * kMargin || height <= 2 * kMargin ||
            width > std::numeric_limits<int>::max() ||
            height > std::numeric_limits<int>::max())
        {
            return S_FALSE;
        }

        const std::size_t pixelWidth = static_cast<std::size_t>(width);
        const std::size_t pixelHeight = static_cast<std::size_t>(height);
        if (pixelWidth > std::numeric_limits<std::size_t>::max() / pixelHeight ||
            pixelWidth * pixelHeight > std::numeric_limits<std::size_t>::max() / 4)
        {
            return E_OUTOFMEMORY;
        }

        if (!Initialize())
        {
            return S_FALSE;
        }

        if (SetWindowPos(
                _window,
                nullptr,
                0,
                0,
                static_cast<int>(width),
                static_cast<int>(height),
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE) == FALSE)
        {
            OutputDebugStringW(L"MapWinGIS: could not resize the OpenGL render target.\n");
            return E_FAIL;
        }

        try
        {
            const std::size_t pixelCount = pixelWidth * pixelHeight;
            _rgbaPixels.resize(pixelCount * 4);
            _dibPixels.resize(pixelCount * 4);
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
        catch (const std::length_error&)
        {
            return E_OUTOFMEMORY;
        }

        const ScopedCurrentContext currentContext(_deviceContext, _renderingContext);
        if (!currentContext.IsValid())
        {
            OutputDebugStringW(L"MapWinGIS: could not activate the OpenGL rendering context.\n");
            return E_FAIL;
        }

        const int viewportWidth = static_cast<int>(width);
        const int viewportHeight = static_cast<int>(height);
        const double availableWidth = static_cast<double>(width - 2 * kMargin);
        const double availableHeight = static_cast<double>(height - 2 * kMargin);
        const double scale = std::min(
            availableWidth / scene.extent.Width(),
            availableHeight / scene.extent.Height());
        if (!std::isfinite(scale) || scale <= 0.0)
        {
            return E_INVALIDARG;
        }

        const double drawingWidth = scene.extent.Width() * scale;
        const double drawingHeight = scene.extent.Height() * scale;
        const double left = (static_cast<double>(width) - drawingWidth) / 2.0;
        const double top = (static_cast<double>(height) - drawingHeight) / 2.0;

        while (glGetError() != GL_NO_ERROR)
        {
        }

        glViewport(0, 0, viewportWidth, viewportHeight);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(width), static_cast<double>(height), 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_DITHER);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
        glDrawBuffer(GL_FRONT);

        const COLORREF background = GetSysColor(COLOR_WINDOW);
        glClearColor(
            static_cast<GLfloat>(GetRValue(background)) / 255.0F,
            static_cast<GLfloat>(GetGValue(background)) / 255.0F,
            static_cast<GLfloat>(GetBValue(background)) / 255.0F,
            1.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        for (const rendering::Polygon& polygon : scene.polygons)
        {
            const rendering::Ring& ring = polygon.front();
            std::size_t count = ring.size();
            if (count > 1 &&
                ring.front().x == ring.back().x &&
                ring.front().y == ring.back().y)
            {
                --count;
            }

            glColor3ub(61, 122, 169);
            glBegin(GL_POLYGON);
            for (std::size_t index = 0; index < count; ++index)
            {
                const rendering::Point& point = ring[index];
                const double x = left + (point.x - scene.extent.xMin) * scale;
                const double y = top + (scene.extent.yMax - point.y) * scale;
                glVertex2d(x, y);
            }
            glEnd();

            glColor3ub(24, 58, 82);
            glBegin(GL_LINE_LOOP);
            for (std::size_t index = 0; index < count; ++index)
            {
                const rendering::Point& point = ring[index];
                const double x = left + (point.x - scene.extent.xMin) * scale;
                const double y = top + (scene.extent.yMax - point.y) * scale;
                glVertex2d(x, y);
            }
            glEnd();
        }

        glFlush();
        glReadBuffer(GL_FRONT);
        glReadPixels(
            0,
            0,
            viewportWidth,
            viewportHeight,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            _rgbaPixels.data());
        if (glGetError() != GL_NO_ERROR)
        {
            OutputDebugStringW(L"MapWinGIS: OpenGL drawing or pixel readback failed.\n");
            return E_FAIL;
        }

        for (std::size_t row = 0; row < pixelHeight; ++row)
        {
            const std::size_t sourceRow = pixelHeight - row - 1;
            for (std::size_t column = 0; column < pixelWidth; ++column)
            {
                const std::size_t source = (sourceRow * pixelWidth + column) * 4;
                const std::size_t target = (row * pixelWidth + column) * 4;
                _dibPixels[target] = _rgbaPixels[source + 2];
                _dibPixels[target + 1] = _rgbaPixels[source + 1];
                _dibPixels[target + 2] = _rgbaPixels[source];
                _dibPixels[target + 3] = 255;
            }
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = viewportWidth;
        bitmapInfo.bmiHeader.biHeight = -viewportHeight;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        const int copiedRows = SetDIBitsToDevice(
            deviceContext,
            bounds.left,
            bounds.top,
            static_cast<DWORD>(viewportWidth),
            static_cast<DWORD>(viewportHeight),
            0,
            0,
            0,
            static_cast<UINT>(viewportHeight),
            _dibPixels.data(),
            &bitmapInfo,
            DIB_RGB_COLORS);
        return copiedRows == viewportHeight ? S_OK : E_FAIL;
    }
}
