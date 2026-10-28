// SPDX-License-Identifier: MPL-2.0

/**
 * @file OpenGlVectorLayerRenderer.cpp
 * @brief Renders convex polygon scenes with WGL and copies pixels to GDI.
 *
 * Owns one reusable OpenGL context and draws into a framebuffer object so
 * pixel readback is not subject to window pixel-ownership rules. Scenes that
 * need unsupported polygon features, or systems without framebuffer objects,
 * return S_FALSE so the coordinator can use its GDI backend.
 */
#include "OpenGlVectorLayerRenderer.h"

#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "opengl32.lib")

namespace mwg::activex
{
    // OpenGL 3.0 and GL_EXT_framebuffer_object share these enum values.
    namespace
    {
        constexpr GLenum kFramebuffer = 0x8D40;
        constexpr GLenum kRenderbuffer = 0x8D41;
        constexpr GLenum kColorAttachment0 = 0x8CE0;
        constexpr GLenum kFramebufferComplete = 0x8CD5;
        constexpr GLenum kRgba8 = 0x8058;
    }

    struct OpenGlVectorLayerRenderer::FramebufferFunctions final
    {
        using GenObjects = void(APIENTRY*)(GLsizei, GLuint*);
        using BindObject = void(APIENTRY*)(GLenum, GLuint);
        using RenderbufferStorage = void(APIENTRY*)(GLenum, GLenum, GLsizei, GLsizei);
        using FramebufferRenderbuffer = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint);
        using CheckFramebufferStatus = GLenum(APIENTRY*)(GLenum);

        GenObjects genFramebuffers{};
        BindObject bindFramebuffer{};
        GenObjects genRenderbuffers{};
        BindObject bindRenderbuffer{};
        RenderbufferStorage renderbufferStorage{};
        FramebufferRenderbuffer framebufferRenderbuffer{};
        CheckFramebufferStatus checkFramebufferStatus{};

        /// Loads the core entry points or, failing that, the EXT ones.
        /// Requires the owning context to be current.
        [[nodiscard]] bool Load() noexcept
        {
            return LoadSet("") || LoadSet("EXT");
        }

    private:
        template <typename Function>
        [[nodiscard]] static bool LoadFunction(
            Function& function,
            const char* name,
            const char* suffix) noexcept
        {
            char fullName[64]{};
            const int written = std::snprintf(fullName, sizeof(fullName), "%s%s", name, suffix);
            if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(fullName))
            {
                return false;
            }

            const PROC address = wglGetProcAddress(fullName);
            // Some drivers report failure with small sentinel values instead of null.
            const auto value = reinterpret_cast<std::intptr_t>(address);
            if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1)
            {
                function = nullptr;
                return false;
            }

            function = reinterpret_cast<Function>(address);
            return true;
        }

        [[nodiscard]] bool LoadSet(const char* suffix) noexcept
        {
            return LoadFunction(genFramebuffers, "glGenFramebuffers", suffix) &&
                LoadFunction(bindFramebuffer, "glBindFramebuffer", suffix) &&
                LoadFunction(genRenderbuffers, "glGenRenderbuffers", suffix) &&
                LoadFunction(bindRenderbuffer, "glBindRenderbuffer", suffix) &&
                LoadFunction(renderbufferStorage, "glRenderbufferStorage", suffix) &&
                LoadFunction(framebufferRenderbuffer, "glFramebufferRenderbuffer", suffix) &&
                LoadFunction(checkFramebufferStatus, "glCheckFramebufferStatus", suffix);
        }
    };

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
                // A failed wglMakeCurrent still detaches the caller's context,
                // so restore it even when activation did not succeed.
                wglMakeCurrent(_previousDeviceContext, _previousRenderingContext);
            }

            [[nodiscard]] bool IsValid() const noexcept { return _valid; }

        private:
            HDC _previousDeviceContext;
            HGLRC _previousRenderingContext;
            bool _valid;
        };

        [[nodiscard]] bool IsSamePoint(
            const rendering::Point& first,
            const rendering::Point& second) noexcept
        {
            return first.x == second.x && first.y == second.y;
        }

        /// Returns true when the ring is a simple, strictly convex polygon.
        /// Consecutive duplicate vertices are ignored so they cannot hide a
        /// concave corner, and the total turning angle must be one full turn
        /// so self-intersecting rings with consistent turns are rejected.
        [[nodiscard]] bool IsConvex(const rendering::Ring& ring) noexcept
        {
            std::vector<rendering::Point> vertices;
            try
            {
                vertices.reserve(ring.size());
                for (const rendering::Point& point : ring)
                {
                    if (vertices.empty() || !IsSamePoint(vertices.back(), point))
                    {
                        vertices.push_back(point);
                    }
                }
            }
            catch (const std::bad_alloc&)
            {
                return false;
            }

            while (vertices.size() > 1 && IsSamePoint(vertices.front(), vertices.back()))
            {
                vertices.pop_back();
            }

            const std::size_t count = vertices.size();
            if (count < 3)
            {
                return false;
            }

            constexpr double kPi = 3.14159265358979323846;
            double orientation = 0.0;
            double totalTurn = 0.0;
            for (std::size_t index = 0; index < count; ++index)
            {
                const rendering::Point& first = vertices[index];
                const rendering::Point& second = vertices[(index + 1) % count];
                const rendering::Point& third = vertices[(index + 2) % count];
                const double cross =
                    (second.x - first.x) * (third.y - second.y) -
                    (second.y - first.y) * (third.x - second.x);
                const double dot =
                    (second.x - first.x) * (third.x - second.x) +
                    (second.y - first.y) * (third.y - second.y);
                if (!std::isfinite(cross) || !std::isfinite(dot))
                {
                    return false;
                }

                if (cross == 0.0)
                {
                    // Collinear vertices are fine, but a reversal is a spike
                    // whose overlapping edges GL_POLYGON cannot represent.
                    if (dot < 0.0)
                    {
                        return false;
                    }
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

                totalTurn += std::atan2(cross, dot);
            }

            // With consistent turn signs the total is a multiple of 2*pi;
            // only a single full turn describes a simple ring.
            return orientation != 0.0 && std::abs(totalTurn) < 3.0 * kPi;
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

    OpenGlVectorLayerRenderer::OpenGlVectorLayerRenderer() noexcept = default;

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

        if (!InitializeFramebuffer())
        {
            _initializationFailed = true;
            OutputDebugStringW(L"MapWinGIS: OpenGL framebuffer objects are unavailable.\n");
            Release();
            return false;
        }

        return true;
    }

    bool OpenGlVectorLayerRenderer::InitializeFramebuffer() noexcept
    {
        const ScopedCurrentContext currentContext(_deviceContext, _renderingContext);
        if (!currentContext.IsValid())
        {
            return false;
        }

        _framebufferFunctions.reset(new (std::nothrow) FramebufferFunctions{});
        if (_framebufferFunctions == nullptr || !_framebufferFunctions->Load())
        {
            _framebufferFunctions.reset();
            return false;
        }

        while (glGetError() != GL_NO_ERROR)
        {
        }

        _framebufferFunctions->genFramebuffers(1, &_framebuffer);
        _framebufferFunctions->genRenderbuffers(1, &_renderbuffer);
        _framebufferFunctions->bindRenderbuffer(kRenderbuffer, _renderbuffer);
        _framebufferFunctions->bindFramebuffer(kFramebuffer, _framebuffer);
        _framebufferFunctions->framebufferRenderbuffer(
            kFramebuffer,
            kColorAttachment0,
            kRenderbuffer,
            _renderbuffer);
        return glGetError() == GL_NO_ERROR && _framebuffer != 0 && _renderbuffer != 0;
    }

    bool OpenGlVectorLayerRenderer::ResizeFramebuffer(const int width, const int height) noexcept
    {
        _framebufferFunctions->bindFramebuffer(kFramebuffer, _framebuffer);
        _framebufferFunctions->bindRenderbuffer(kRenderbuffer, _renderbuffer);
        if (width != _framebufferWidth || height != _framebufferHeight)
        {
            _framebufferWidth = 0;
            _framebufferHeight = 0;
            while (glGetError() != GL_NO_ERROR)
            {
            }

            _framebufferFunctions->renderbufferStorage(kRenderbuffer, kRgba8, width, height);
            if (glGetError() != GL_NO_ERROR)
            {
                return false;
            }

            _framebufferWidth = width;
            _framebufferHeight = height;
        }

        return _framebufferFunctions->checkFramebufferStatus(kFramebuffer) == kFramebufferComplete;
    }

    void OpenGlVectorLayerRenderer::Release() noexcept
    {
        // The context is not shared, so deleting it also frees the
        // framebuffer and renderbuffer objects.
        _framebufferFunctions.reset();
        _framebuffer = 0;
        _renderbuffer = 0;
        _framebufferWidth = 0;
        _framebufferHeight = 0;

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
        if (!ResizeFramebuffer(viewportWidth, viewportHeight))
        {
            OutputDebugStringW(L"MapWinGIS: no complete OpenGL framebuffer for this size.\n");
            return S_FALSE;
        }

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
        glDrawBuffer(kColorAttachment0);

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
        glReadBuffer(kColorAttachment0);
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
