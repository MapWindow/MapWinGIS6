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

#include "ViewTransform.h"

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
        constexpr GLenum kMaxRenderbufferSize = 0x84E8;
        constexpr std::size_t kMaxStagingBufferBytes = 64U * 1024U * 1024U;
        constexpr std::size_t kBytesPerPixel = 4;
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

    std::optional<std::wstring_view> OpenGlVectorLayerRenderer::GetTargetRejectionReason(
        const int width,
        const int height,
        const int maximumWidth,
        const int maximumHeight) noexcept
    {
        if (width <= 0 || height <= 0)
        {
            return L"The OpenGL target dimensions must be positive.";
        }

        if (maximumWidth <= 0 || maximumHeight <= 0)
        {
            return L"OpenGL did not report a usable maximum framebuffer size.";
        }

        if (width > maximumWidth || height > maximumHeight)
        {
            return L"The OpenGL target exceeds the detected maximum framebuffer or viewport size.";
        }

        const std::size_t pixelWidth = static_cast<std::size_t>(width);
        const std::size_t pixelHeight = static_cast<std::size_t>(height);
        if (pixelWidth > std::numeric_limits<std::size_t>::max() / pixelHeight ||
            pixelWidth * pixelHeight > kMaxStagingBufferBytes / kBytesPerPixel)
        {
            return L"The OpenGL frame exceeds the 64 MiB CPU staging-buffer budget.";
        }

        return std::nullopt;
    }

    bool OpenGlVectorLayerRenderer::Initialize() noexcept
    {
        if (_renderingContext != nullptr)
        {
            return true;
        }
        if (_initializationFailed)
        {
            _lastFailureReason = _initializationFailureReason;
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
            _lastFailureReason = L"OpenGL could not create its render target window.";
            _initializationFailureReason = _lastFailureReason;
            OutputDebugStringW(L"MapWinGIS: could not create the OpenGL render target window.\n");
            Release();
            return false;
        }

        _deviceContext = GetDC(_window);
        if (_deviceContext == nullptr)
        {
            _initializationFailed = true;
            _lastFailureReason = L"OpenGL could not acquire its render target device context.";
            _initializationFailureReason = _lastFailureReason;
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
            _lastFailureReason = L"OpenGL could not select a compatible pixel format.";
            _initializationFailureReason = _lastFailureReason;
            OutputDebugStringW(L"MapWinGIS: could not select an OpenGL pixel format.\n");
            Release();
            return false;
        }

        _renderingContext = wglCreateContext(_deviceContext);
        if (_renderingContext == nullptr)
        {
            _initializationFailed = true;
            _lastFailureReason = L"OpenGL could not create a rendering context.";
            _initializationFailureReason = _lastFailureReason;
            OutputDebugStringW(L"MapWinGIS: could not create an OpenGL rendering context.\n");
            Release();
            return false;
        }

        if (!InitializeFramebuffer())
        {
            _initializationFailed = true;
            if (_lastFailureReason == L"OpenGL was not selected for this frame.")
            {
                _lastFailureReason =
                    L"OpenGL framebuffer objects are unavailable on this system.";
            }
            _initializationFailureReason = _lastFailureReason;
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

        try
        {
            _framebufferFunctions = std::make_unique<FramebufferFunctions>();
        }
        catch (const std::bad_alloc&)
        {
            _lastFailureReason =
                L"OpenGL could not allocate its framebuffer function table.";
            return false;
        }

        if (!_framebufferFunctions->Load())
        {
            _framebufferFunctions.reset();
            _lastFailureReason =
                L"The OpenGL framebuffer-object entry points are unavailable.";
            return false;
        }

        while (glGetError() != GL_NO_ERROR)
        {
        }

        GLint maximumFramebufferSize{};
        glGetIntegerv(kMaxRenderbufferSize, &maximumFramebufferSize);
        GLint maximumViewportDimensions[2]{};
        glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maximumViewportDimensions);
        if (glGetError() != GL_NO_ERROR || maximumFramebufferSize <= 0 ||
            maximumViewportDimensions[0] <= 0 || maximumViewportDimensions[1] <= 0)
        {
            _lastFailureReason =
                L"OpenGL did not report a usable maximum framebuffer size.";
            return false;
        }
        _maximumTargetWidth = std::min(maximumFramebufferSize, maximumViewportDimensions[0]);
        _maximumTargetHeight = std::min(maximumFramebufferSize, maximumViewportDimensions[1]);

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
        _maximumTargetWidth = 0;
        _maximumTargetHeight = 0;

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

    OpenGlVectorLayerRenderer::TargetValidation
    OpenGlVectorLayerRenderer::ValidateTarget(
        const HDC deviceContext,
        const RECT& bounds,
        const rendering::VectorRenderScene& scene) noexcept
    {
        if (deviceContext == nullptr)
        {
            _lastFailureReason = L"The OpenGL target device context is invalid.";
            return {E_INVALIDARG, std::nullopt};
        }

        if (!IsSupported(scene))
        {
            _lastFailureReason =
                L"The scene uses geometry that the OpenGL backend does not support.";
            return {S_FALSE, std::nullopt};
        }

        if (scene.extent.IsEmpty() || scene.extent.Width() <= 0.0 ||
            scene.extent.Height() <= 0.0)
        {
            _lastFailureReason = L"The scene extent is invalid for OpenGL rendering.";
            return {E_INVALIDARG, std::nullopt};
        }

        const std::int64_t width = GetWidth(bounds);
        const std::int64_t height = GetHeight(bounds);
        if (width <= 2 * static_cast<std::int64_t>(ViewTransform::kMargin) ||
            height <= 2 * static_cast<std::int64_t>(ViewTransform::kMargin) ||
            width > std::numeric_limits<int>::max() ||
            height > std::numeric_limits<int>::max())
        {
            _lastFailureReason = L"The OpenGL target has invalid or too-small dimensions.";
            return {S_FALSE, std::nullopt};
        }

        const auto rejectionReason = GetTargetRejectionReason(
            static_cast<int>(width),
            static_cast<int>(height),
            std::numeric_limits<int>::max(),
            std::numeric_limits<int>::max());
        if (rejectionReason.has_value())
        {
            _lastFailureReason = *rejectionReason;
            return {S_FALSE, std::nullopt};
        }

        return {
            S_OK,
            Target{
                static_cast<int>(width),
                static_cast<int>(height),
                static_cast<std::size_t>(width),
                static_cast<std::size_t>(height)}};
    }

    bool OpenGlVectorLayerRenderer::EnsureStaging(
        const std::size_t stagingBytes) noexcept
    {
        try
        {
            if (_rgbaPixels.capacity() < stagingBytes)
            {
                std::vector<unsigned char>().swap(_rgbaPixels);
                _rgbaPixels.reserve(stagingBytes);
            }
            _rgbaPixels.resize(stagingBytes);
        }
        catch (const std::bad_alloc&)
        {
            _lastFailureReason = L"OpenGL could not allocate its CPU staging buffers.";
            return false;
        }
        catch (const std::length_error&)
        {
            _lastFailureReason = L"The OpenGL CPU staging-buffer size is not representable.";
            return false;
        }
        return true;
    }

    HRESULT OpenGlVectorLayerRenderer::RenderScene(
        const rendering::VectorRenderScene& scene,
        const Target& target) noexcept
    {
        const auto transform = ViewTransform::Create(
            scene.extent,
            static_cast<double>(target.width),
            static_cast<double>(target.height));
        if (!transform.has_value())
        {
            _lastFailureReason =
                L"The scene extent cannot be represented in the OpenGL viewport.";
            return E_INVALIDARG;
        }

        const ScopedCurrentContext currentContext(_deviceContext, _renderingContext);
        if (!currentContext.IsValid())
        {
            _lastFailureReason = L"OpenGL could not activate its rendering context.";
            OutputDebugStringW(L"MapWinGIS: could not activate the OpenGL rendering context.\n");
            _contextInvalid = true;
            return E_FAIL;
        }

        if (!ResizeFramebuffer(target.width, target.height))
        {
            _lastFailureReason =
                L"OpenGL could not allocate a complete framebuffer for this target.";
            OutputDebugStringW(L"MapWinGIS: no complete OpenGL framebuffer for this size.\n");
            return S_FALSE;
        }

        while (glGetError() != GL_NO_ERROR)
        {
        }

        glViewport(0, 0, target.width, target.height);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(
            0.0,
            static_cast<double>(target.width),
            static_cast<double>(target.height),
            0.0,
            -1.0,
            1.0);
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
                glVertex2d(transform->ToDeviceX(point), transform->ToDeviceY(point));
            }
            glEnd();

            glColor3ub(24, 58, 82);
            glBegin(GL_LINE_LOOP);
            for (std::size_t index = 0; index < count; ++index)
            {
                const rendering::Point& point = ring[index];
                glVertex2d(transform->ToDeviceX(point), transform->ToDeviceY(point));
            }
            glEnd();
        }

        glFlush();
        glReadBuffer(kColorAttachment0);
        glReadPixels(
            0,
            0,
            target.width,
            target.height,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            _rgbaPixels.data());
        if (glGetError() != GL_NO_ERROR)
        {
            _lastFailureReason =
                L"OpenGL drawing or pixel readback failed; the context will be recreated.";
            OutputDebugStringW(L"MapWinGIS: OpenGL drawing or pixel readback failed.\n");
            _contextInvalid = true;
            return E_FAIL;
        }

        return S_OK;
    }

    HRESULT OpenGlVectorLayerRenderer::CopyToDeviceContext(
        const HDC deviceContext,
        const RECT& bounds,
        const Target& target) noexcept
    {
        const std::size_t pixelCount = target.pixelWidth * target.pixelHeight;
        for (std::size_t row = 0; row < target.pixelHeight / 2; ++row)
        {
            for (std::size_t column = 0; column < target.pixelWidth; ++column)
            {
                const std::size_t topPixel =
                    (row * target.pixelWidth + column) * kBytesPerPixel;
                const std::size_t bottomPixel =
                    ((target.pixelHeight - row - 1) * target.pixelWidth + column) *
                    kBytesPerPixel;
                for (std::size_t channel = 0; channel < kBytesPerPixel; ++channel)
                {
                    std::swap(
                        _rgbaPixels[topPixel + channel],
                        _rgbaPixels[bottomPixel + channel]);
                }
            }
        }

        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel)
        {
            const std::size_t offset = pixel * kBytesPerPixel;
            std::swap(_rgbaPixels[offset], _rgbaPixels[offset + 2]);
            _rgbaPixels[offset + 3] = 255;
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = target.width;
        bitmapInfo.bmiHeader.biHeight = -target.height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        const int copiedRows = SetDIBitsToDevice(
            deviceContext,
            bounds.left,
            bounds.top,
            static_cast<DWORD>(target.width),
            static_cast<DWORD>(target.height),
            0,
            0,
            0,
            static_cast<UINT>(target.height),
            _rgbaPixels.data(),
            &bitmapInfo,
            DIB_RGB_COLORS);
        if (copiedRows != target.height)
        {
            _lastFailureReason =
                L"OpenGL rendered the frame, but Windows could not present its pixels.";
            return E_FAIL;
        }
        return S_OK;
    }

#ifdef MWG_ENABLE_RENDERER_TEST_HOOKS
    void OpenGlVectorLayerRenderer::SimulateContextLossForTesting() noexcept
    {
        // Keep the stale handles so Draw observes the failure itself; Release
        // later tolerates the already-released DC and destroyed window.
        if (_window != nullptr)
        {
            if (_deviceContext != nullptr)
            {
                ReleaseDC(_window, _deviceContext);
            }
            DestroyWindow(_window);
        }
    }
#endif

    HRESULT OpenGlVectorLayerRenderer::Draw(
        const HDC deviceContext,
        const RECT& bounds,
        const rendering::VectorRenderScene& scene) noexcept
    {
        _lastFailureReason = L"OpenGL did not render this frame.";
        const TargetValidation validation = ValidateTarget(deviceContext, bounds, scene);
        if (!validation.target.has_value())
        {
            return validation.result;
        }
        const Target& target = *validation.target;

        // Keep the flag set until a replacement context initializes, so a
        // transient recreation failure is retried on later draws.
        if (_contextInvalid)
        {
            Release();
            _initializationFailed = false;
            _initializationFailureReason = {};
        }

        if (!Initialize())
        {
            return S_FALSE;
        }
        _contextInvalid = false;

        const auto rejectionReason = GetTargetRejectionReason(
            target.width,
            target.height,
            _maximumTargetWidth,
            _maximumTargetHeight);
        if (rejectionReason.has_value())
        {
            _lastFailureReason = *rejectionReason;
            return S_FALSE;
        }

        const std::size_t stagingBytes =
            target.pixelWidth * target.pixelHeight * kBytesPerPixel;
        if (!EnsureStaging(stagingBytes))
        {
            return S_FALSE;
        }

        const HRESULT renderResult = RenderScene(scene, target);
        if (FAILED(renderResult) || renderResult == S_FALSE)
        {
            return renderResult;
        }

        return CopyToDeviceContext(deviceContext, bounds, target);
    }
}
