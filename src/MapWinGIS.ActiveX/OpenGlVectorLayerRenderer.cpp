// SPDX-License-Identifier: MPL-2.0

/**
 * @file OpenGlVectorLayerRenderer.cpp
 * @brief Renders polygon fills or outlines with WGL and copies pixels to GDI.
 *
 * Owns one reusable OpenGL context and draws into a framebuffer object so
 * pixel readback is not subject to window pixel-ownership rules. Polygon
 * fills use indexed triangle meshes supplied by Core or generated lazily and
 * cached for repeated draws of the same scene. Outlines follow the original
 * rings and do not require meshes. Each polygon is drawn completely before
 * the next one, matching the GDI backend's painter order. Fill scenes whose
 * mesh preparation fails, or systems without framebuffer objects, return
 * S_FALSE so the coordinator can use GDI.
 */
#include "OpenGlVectorLayerRenderer.h"

#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "opengl32.lib")

#include "ViewTransform.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/diagnostics/PerformanceDiagnostics.h"
#include "mapwingis/rendering/PolygonTriangulation.h"

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
        constexpr GLenum kBgra = 0x80E1;
        constexpr unsigned int kInitializationRetryDraws = 60;
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

        /// Returns true when the mesh can feed glVertexPointer and
        /// glDrawElements safely: complete triangles, in-range indices and
        /// finite vertex coordinates.
        [[nodiscard]] bool IsMeshUsable(const rendering::TriangleMesh& mesh) noexcept
        {
            if (mesh.vertices.empty() ||
                mesh.indices.empty() ||
                mesh.indices.size() % 3U != 0U ||
                mesh.indices.size() >
                    static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()))
            {
                return false;
            }

            for (const std::uint32_t index : mesh.indices)
            {
                if (index >= mesh.vertices.size())
                {
                    return false;
                }
            }

            for (const rendering::Point& point : mesh.vertices)
            {
                if (!std::isfinite(point.x) || !std::isfinite(point.y))
                {
                    return false;
                }
            }

            return true;
        }

        [[nodiscard]] bool AreFillMeshesUsable(
            const std::span<const std::optional<rendering::TriangleMesh>> fillMeshes,
            const std::size_t polygonCount) noexcept
        {
            if (fillMeshes.size() != polygonCount)
            {
                return false;
            }
            for (const std::optional<rendering::TriangleMesh>& mesh : fillMeshes)
            {
                if (!mesh.has_value() || !IsMeshUsable(*mesh))
                {
                    return false;
                }
            }
            return true;
        }

        /// Returns true when the scene has finite ring coordinates and, in
        /// fill mode, one usable mesh per polygon.
        [[nodiscard]] bool IsSupported(
            const rendering::MapRenderScene& scene,
            const VectorRenderMode renderMode) noexcept
        {
            for (const rendering::LayerRenderScene& layer : scene.layers)
            {
                if (layer.scene == nullptr)
                {
                    return false;
                }
                const rendering::VectorRenderScene& vectorScene = *layer.scene;
                if (renderMode == VectorRenderMode::Fill &&
                    !vectorScene.fillMeshes.empty())
                {
                    if (vectorScene.fillMeshes.size() != vectorScene.polygons.size())
                    {
                        return false;
                    }
                    for (std::size_t index = 0;
                         index < vectorScene.polygons.size();
                         ++index)
                    {
                        const std::optional<rendering::TriangleMesh>& mesh =
                            vectorScene.fillMeshes[index];
                        if (!mesh.has_value() || !IsMeshUsable(*mesh))
                        {
                            return false;
                        }
                    }
                }
                for (const rendering::Point& point : vectorScene.polygons.Coordinates())
                {
                    if (!std::isfinite(point.x) || !std::isfinite(point.y))
                    {
                        return false;
                    }
                }
            }

            return true;
        }

        /// Draws one polygon's triangulated fill as indexed triangles through
        /// the client-side vertex-array API. The caller must have enabled
        /// GL_VERTEX_ARRAY, and IsSupported must have accepted the scene.
        [[nodiscard]] HRESULT DrawPolygonFill(
            const rendering::TriangleMesh& mesh,
            const ViewTransform& transform,
            std::vector<GLdouble>& deviceVertices) noexcept
        {
            // Reuse one transformed-vertex allocation for every polygon in
            // the layer instead of allocating in the inner draw loop.
            try
            {
                deviceVertices.clear();
                if (mesh.vertices.size() > deviceVertices.max_size() / 2U)
                {
                    return E_OUTOFMEMORY;
                }
                deviceVertices.reserve(mesh.vertices.size() * 2U);
                for (const rendering::Point& point : mesh.vertices)
                {
                    deviceVertices.push_back(transform.ToDeviceX(point));
                    deviceVertices.push_back(transform.ToDeviceY(point));
                }
            }
            catch (const std::bad_alloc&)
            {
                return E_OUTOFMEMORY;
            }
            catch (const std::length_error&)
            {
                return E_OUTOFMEMORY;
            }

            glVertexPointer(2, GL_DOUBLE, 0, deviceVertices.data());
            glDrawElements(
                GL_TRIANGLES,
                static_cast<GLsizei>(mesh.indices.size()),
                GL_UNSIGNED_INT,
                mesh.indices.data());
            return S_OK;
        }

        /// Draws one polygon's original ring boundaries as outlines.
        /// Triangle edges are not polygon boundaries and are never drawn.
        void DrawPolygonOutlines(
            const rendering::PolygonView polygon,
            const ViewTransform& transform) noexcept
        {
            for (std::size_t ringIndex = 0; ringIndex < polygon.size(); ++ringIndex)
            {
                const std::span<const rendering::Point> ring = polygon[ringIndex];
                std::size_t count = ring.size();
                if (count > 1 &&
                    ring.front().x == ring.back().x &&
                    ring.front().y == ring.back().y)
                {
                    --count;
                }

                if (count < 3)
                {
                    continue;
                }

                glBegin(GL_LINE_LOOP);
                for (std::size_t vertex = 0; vertex < count; ++vertex)
                {
                    const rendering::Point& point = ring[vertex];
                    glVertex2d(transform.ToDeviceX(point), transform.ToDeviceY(point));
                }
                glEnd();
            }
        }

        /// Draws one layer polygon by polygon. Fill mode uses prepared meshes;
        /// both modes outline the original rings. Overlapping polygons retain
        /// the same painter order as the GDI backend.
        [[nodiscard]] HRESULT DrawLayer(
            const rendering::VectorRenderScene& scene,
            const std::span<const std::optional<rendering::TriangleMesh>> fillMeshes,
            const ViewTransform& transform,
            const VectorRenderMode renderMode) noexcept
        {
            const diagnostics::PerformanceScope submissionScope(
                diagnostics::PerformanceOperation::OpenGlGeometrySubmission);
            if (renderMode == VectorRenderMode::Fill)
            {
                glEnableClientState(GL_VERTEX_ARRAY);
            }
            std::vector<GLdouble> deviceVertices;
            for (std::size_t index = 0; index < scene.polygons.size(); ++index)
            {
                if (renderMode == VectorRenderMode::Fill)
                {
                    glColor3ub(61, 122, 169);
                    const HRESULT fillResult =
                        DrawPolygonFill(*fillMeshes[index], transform, deviceVertices);
                    if (FAILED(fillResult))
                    {
                        glDisableClientState(GL_VERTEX_ARRAY);
                        return fillResult;
                    }
                }

                glColor3ub(24, 58, 82);
                DrawPolygonOutlines(scene.polygons[index], transform);
            }
            if (renderMode == VectorRenderMode::Fill)
            {
                glDisableClientState(GL_VERTEX_ARRAY);
            }
            return S_OK;
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

    struct OpenGlVectorLayerRenderer::FillMeshCache final
    {
        struct Layer final
        {
            std::weak_ptr<const rendering::VectorRenderScene> scene;
            std::vector<std::optional<rendering::TriangleMesh>> generatedMeshes;
            bool failed = false;
            std::uint64_t preparationToken = 0;
        };

        std::map<LayerId, Layer> layers;
        std::uint64_t generation = 0;
        std::uint64_t nextPreparationToken = 0;
    };

    bool OpenGlVectorLayerRenderer::IsSceneSupported(
        const rendering::MapRenderScene& scene,
        const VectorRenderMode renderMode) noexcept
    {
        return IsSupported(scene, renderMode);
    }

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
        const rendering::MapRenderScene& scene,
        const Viewport& viewport,
        const VectorRenderMode renderMode) noexcept
    {
        if (deviceContext == nullptr)
        {
            _lastFailureReason = L"The OpenGL target device context is invalid.";
            return {E_INVALIDARG, std::nullopt};
        }

        if (!MatchesViewport(bounds, viewport))
        {
            _lastFailureReason =
                L"The viewport is not valid or does not match the OpenGL target size.";
            return {E_INVALIDARG, std::nullopt};
        }

        if (!IsSupported(scene, renderMode))
        {
            _lastFailureReason = renderMode == VectorRenderMode::Fill
                ? L"The OpenGL backend requires a triangulated fill mesh for "
                  L"every polygon; this frame was rendered with GDI."
                : L"The OpenGL backend requires finite polygon coordinates for "
                  L"outline rendering; this frame was rendered with GDI.";
            return {S_FALSE, std::nullopt};
        }

        // MatchesViewport limits both dimensions to Viewport::kMaxDimension.
        const std::int64_t width = GetWidth(bounds);
        const std::int64_t height = GetHeight(bounds);

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
            if (_pixelBuffer.capacity() < stagingBytes)
            {
                // Drop retained capacity before reserve to avoid overlapping
                // allocations that exceed the 64 MiB staging budget.
                std::vector<unsigned char>().swap(_pixelBuffer);
                _pixelBuffer.reserve(stagingBytes);
            }
            _pixelBuffer.resize(stagingBytes);
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

    HRESULT OpenGlVectorLayerRenderer::PrepareFillMeshes(
        const rendering::MapRenderScene& scene,
        const bool pruneCache) noexcept
    {
        if (pruneCache && !PruneFillMeshCache(scene))
        {
            return S_FALSE;
        }

        std::shared_ptr<FillMeshCache> cache;
        try
        {
            if (_fillMeshCache == nullptr)
            {
                _fillMeshCache = std::make_shared<FillMeshCache>();
            }
            cache = _fillMeshCache;
            for (const rendering::LayerRenderScene& layer : scene.layers)
            {
                if (layer.scene == nullptr || !layer.scene->fillMeshes.empty())
                {
                    continue;
                }

                const auto cached = cache->layers.find(layer.layerId);
                if (cached != cache->layers.end())
                {
                    continue;
                }

                auto [pending, inserted] = cache->layers.try_emplace(layer.layerId);
                if (!inserted)
                {
                    continue;
                }
                const std::uint64_t cacheGeneration = cache->generation;
                const std::uint64_t preparationToken = ++cache->nextPreparationToken;
                pending->second.scene = layer.scene;
                pending->second.preparationToken = preparationToken;

                std::vector<std::optional<rendering::TriangleMesh>> generatedMeshes;
                generatedMeshes.reserve(layer.scene->polygons.size());
                bool failed = false;
                for (std::size_t index = 0;
                     index < layer.scene->polygons.size();
                     ++index)
                {
                    try
                    {
                        rendering::TriangleMesh mesh =
                            rendering::TriangulatePolygon(layer.scene->polygons[index]);
                        if (!IsMeshUsable(mesh))
                        {
                            failed = true;
                            break;
                        }
                        generatedMeshes.push_back(std::move(mesh));
                    }
                    catch (const mwg::CoreError&)
                    {
                        failed = true;
                        break;
                    }
                    catch (const std::bad_alloc&)
                    {
                        throw;
                    }
                    catch (const std::length_error&)
                    {
                        throw;
                    }
                    catch (const std::exception&)
                    {
                        failed = true;
                        break;
                    }
                    catch (...)
                    {
                        failed = true;
                        break;
                    }
                }
                pending = cache->layers.find(layer.layerId);
                if (_fillMeshCache != cache ||
                    cache->generation != cacheGeneration ||
                    pending == cache->layers.end() ||
                    pending->second.preparationToken != preparationToken ||
                    pending->second.scene.lock() != layer.scene)
                {
                    _lastFailureReason =
                        L"The OpenGL fill-mesh cache changed during triangulation; "
                        L"this frame was rendered with GDI.";
                    return S_FALSE;
                }
                if (failed)
                {
                    std::vector<std::optional<rendering::TriangleMesh>>()
                        .swap(generatedMeshes);
                }
                pending->second.generatedMeshes = std::move(generatedMeshes);
                pending->second.failed = failed;
            }
        }
        catch (const std::bad_alloc&)
        {
            _lastFailureReason =
                L"The OpenGL backend could not allocate lazily generated fill meshes.";
            return S_FALSE;
        }
        catch (const std::length_error&)
        {
            _lastFailureReason =
                L"The OpenGL backend could not represent its lazily generated fill meshes.";
            return S_FALSE;
        }

        for (const rendering::LayerRenderScene& layer : scene.layers)
        {
            if (layer.scene == nullptr || !layer.scene->fillMeshes.empty())
            {
                continue;
            }
            const auto cached = cache->layers.find(layer.layerId);
            if (cached == cache->layers.end() ||
                cached->second.failed ||
                cached->second.scene.lock() != layer.scene ||
                !AreFillMeshesUsable(
                    cached->second.generatedMeshes,
                    layer.scene->polygons.size()))
            {
                _lastFailureReason =
                    L"The OpenGL backend could not triangulate a polygon for fill "
                    L"rendering; this frame was rendered with GDI.";
                return S_FALSE;
            }
        }
        return S_OK;
    }

    std::span<const std::optional<rendering::TriangleMesh>>
    OpenGlVectorLayerRenderer::GetFillMeshes(
        const LayerId layerId,
        const rendering::VectorRenderScene& scene) const noexcept
    {
        if (!scene.fillMeshes.empty())
        {
            return AreFillMeshesUsable(scene.fillMeshes, scene.polygons.size())
                ? std::span<const std::optional<rendering::TriangleMesh>>(scene.fillMeshes)
                : std::span<const std::optional<rendering::TriangleMesh>>();
        }
        if (_fillMeshCache == nullptr)
        {
            return {};
        }
        const auto cached = _fillMeshCache->layers.find(layerId);
        if (cached == _fillMeshCache->layers.end() ||
            cached->second.scene.lock().get() != &scene ||
            cached->second.failed ||
            !AreFillMeshesUsable(
                cached->second.generatedMeshes,
                scene.polygons.size()))
        {
            return {};
        }
        return cached->second.generatedMeshes;
    }

    bool OpenGlVectorLayerRenderer::PruneFillMeshCache(
        const rendering::MapRenderScene& scene) noexcept
    {
        if (_fillMeshCache == nullptr)
        {
            return true;
        }

        try
        {
            std::map<
                LayerId,
                std::shared_ptr<const rendering::VectorRenderScene>> drawableLayers;
            std::set<LayerId> hiddenLayers;
            for (const rendering::LayerRenderScene& layer : scene.layers)
            {
                drawableLayers.insert_or_assign(layer.layerId, layer.scene);
            }
            for (const rendering::SkippedLayer& layer : scene.skippedLayers)
            {
                if (layer.reason == rendering::LayerSkipReason::Hidden)
                {
                    hiddenLayers.insert(layer.layerId);
                }
            }

            for (auto cached = _fillMeshCache->layers.begin();
                 cached != _fillMeshCache->layers.end();)
            {
                const auto drawable = drawableLayers.find(cached->first);
                if (drawable != drawableLayers.end())
                {
                    if (!cached->second.scene.expired() &&
                        cached->second.scene.lock() == drawable->second)
                    {
                        ++cached;
                        continue;
                    }
                    cached = _fillMeshCache->layers.erase(cached);
                    continue;
                }

                if (hiddenLayers.contains(cached->first) &&
                    !cached->second.scene.expired())
                {
                    ++cached;
                    continue;
                }
                cached = _fillMeshCache->layers.erase(cached);
            }
        }
        catch (const std::bad_alloc&)
        {
            _lastFailureReason =
                L"The OpenGL backend could not allocate its fill-mesh cache index.";
            return false;
        }
        catch (const std::length_error&)
        {
            _lastFailureReason =
                L"The OpenGL backend could not represent its fill-mesh cache index.";
            return false;
        }
        return true;
    }

    void OpenGlVectorLayerRenderer::ClearFillMeshCache() noexcept
    {
        if (_fillMeshCache == nullptr)
        {
            return;
        }
        _fillMeshCache->layers.clear();
        ++_fillMeshCache->generation;
    }

    void OpenGlVectorLayerRenderer::ClearLayerFillMeshCache(
        const LayerId layerId) noexcept
    {
        if (_fillMeshCache == nullptr)
        {
            return;
        }
        (void)_fillMeshCache->layers.erase(layerId);
    }

    std::size_t OpenGlVectorLayerRenderer::FillMeshCacheSize() const noexcept
    {
        return _fillMeshCache == nullptr ? 0U : _fillMeshCache->layers.size();
    }

    std::size_t OpenGlVectorLayerRenderer::CachedGeneratedMeshStorage() const noexcept
    {
        if (_fillMeshCache == nullptr)
        {
            return 0U;
        }
        std::size_t storage = 0;
        for (const auto& cached : _fillMeshCache->layers)
        {
            storage += cached.second.generatedMeshes.size();
        }
        return storage;
    }

    std::size_t
    OpenGlVectorLayerRenderer::CachedGeneratedMeshStorageBytes() const noexcept
    {
        if (_fillMeshCache == nullptr)
        {
            return 0U;
        }

        std::size_t storage = 0;
        for (const auto& cached : _fillMeshCache->layers)
        {
            const FillMeshCache::Layer& layer = cached.second;
            storage += layer.generatedMeshes.capacity() *
                sizeof(std::optional<rendering::TriangleMesh>);
            for (const std::optional<rendering::TriangleMesh>& mesh :
                 layer.generatedMeshes)
            {
                if (mesh.has_value())
                {
                    storage += mesh->vertices.capacity() * sizeof(rendering::Point);
                    storage += mesh->indices.capacity() * sizeof(std::uint32_t);
                }
            }
        }
        return storage;
    }

    HRESULT OpenGlVectorLayerRenderer::RenderScene(
        const rendering::MapRenderScene& scene,
        const Viewport& viewport,
        const Target& target,
        const VectorRenderMode renderMode) noexcept
    {
        const auto transform = ViewTransform::Create(viewport);
        if (!transform.has_value())
        {
            _lastFailureReason = L"The viewport is not valid for OpenGL rendering.";
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

        for (std::size_t index = 0; index < scene.layers.size(); ++index)
        {
            // IsSupported above rejected null layer scenes.
            const rendering::LayerRenderScene& layer = scene.layers[index];
            const std::span<const std::optional<rendering::TriangleMesh>> fillMeshes =
                renderMode == VectorRenderMode::Fill
                    ? GetFillMeshes(layer.layerId, *layer.scene)
                    : std::span<const std::optional<rendering::TriangleMesh>>();
            if (renderMode == VectorRenderMode::Fill &&
                !AreFillMeshesUsable(fillMeshes, layer.scene->polygons.size()))
            {
                _lastFailureReason =
                    L"The OpenGL fill-mesh cache no longer matches the scene; "
                    L"this frame was rendered with GDI.";
                return S_FALSE;
            }
            const HRESULT layerResult =
                DrawLayer(
                    *layer.scene,
                    fillMeshes,
                    *transform,
                    renderMode);
            if (FAILED(layerResult))
            {
                _lastFailureReason = L"OpenGL could not allocate its vertex buffers.";
                return layerResult;
            }
        }

        glFlush();
        glReadBuffer(kColorAttachment0);
        {
            const diagnostics::PerformanceScope readbackScope(
                diagnostics::PerformanceOperation::FramebufferReadback);
            glReadPixels(
                0,
                0,
                target.width,
                target.height,
                kBgra,
                GL_UNSIGNED_BYTE,
                _pixelBuffer.data());
        }
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
                        _pixelBuffer[topPixel + channel],
                        _pixelBuffer[bottomPixel + channel]);
                }
            }
        }

        // GL_BGRA matches the little-endian 32-bit BI_RGB DIB byte
        // order; only the OpenGL bottom-up row order needs reversing.

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
            _pixelBuffer.data(),
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

    void OpenGlVectorLayerRenderer::SimulateContextLoss() noexcept
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
    HRESULT OpenGlVectorLayerRenderer::Draw(
        const HDC deviceContext,
        const RECT& bounds,
        const rendering::MapRenderScene& scene,
        const Viewport& viewport,
        const bool retainStagingBuffer,
        const VectorRenderMode renderMode) noexcept
    {
        if (!retainStagingBuffer)
        {
            // Keep both CPU staging and GPU targets of the screen renderer
            // unchanged. The temporary renderer releases all scaled resources,
            // while retry bookkeeping survives between temporary draws.
            OpenGlVectorLayerRenderer temporaryRenderer;
            if (_fillMeshCache == nullptr)
            {
                try
                {
                    _fillMeshCache = std::make_shared<FillMeshCache>();
                }
                catch (const std::bad_alloc&)
                {
                    _lastFailureReason =
                        L"The OpenGL backend could not allocate its fill-mesh cache.";
                    return S_FALSE;
                }
            }
            temporaryRenderer._fillMeshCache = _fillMeshCache;
            temporaryRenderer._initializationFailed = _initializationFailed;
            temporaryRenderer._initializationFailureReason = _initializationFailureReason;
            temporaryRenderer._retryTargetWidth = _retryTargetWidth;
            temporaryRenderer._retryTargetHeight = _retryTargetHeight;
            temporaryRenderer._initializationRetryDrawsRemaining =
                _initializationRetryDrawsRemaining;
            const HRESULT result = temporaryRenderer.Draw(
                deviceContext,
                bounds,
                scene,
                viewport,
                true,
                renderMode);
            _lastFailureReason = temporaryRenderer.GetLastFailureReason();
            _initializationFailed = temporaryRenderer._initializationFailed;
            _initializationFailureReason = temporaryRenderer._initializationFailureReason;
            _retryTargetWidth = temporaryRenderer._retryTargetWidth;
            _retryTargetHeight = temporaryRenderer._retryTargetHeight;
            _initializationRetryDrawsRemaining =
                temporaryRenderer._initializationRetryDrawsRemaining;
            return result;
        }
        rendering::MapRenderScene stableScene;
        try
        {
            stableScene = scene;
        }
        catch (const std::bad_alloc&)
        {
            _lastFailureReason = L"The OpenGL backend could not snapshot the render scene.";
            return E_OUTOFMEMORY;
        }
        catch (const std::length_error&)
        {
            _lastFailureReason =
                L"The OpenGL backend could not represent its render-scene snapshot.";
            return S_FALSE;
        }
        if (!PruneFillMeshCache(stableScene))
        {
            return S_FALSE;
        }
        const diagnostics::PerformanceScope frameScope(
            diagnostics::PerformanceOperation::FrameRender);
        _lastFailureReason = L"OpenGL did not render this frame.";
        const TargetValidation validation =
            ValidateTarget(deviceContext, bounds, stableScene, viewport, renderMode);
        if (!validation.target.has_value())
        {
            return validation.result;
        }
        const Target& target = *validation.target;

        // Release a lost context once. If replacement initialization fails,
        // preserve its failure state so later draws respect the retry delay.
        if (_contextInvalid)
        {
            Release();
            _contextInvalid = false;
        }

        if (_initializationFailed &&
            (target.width != _retryTargetWidth || target.height != _retryTargetHeight))
        {
            // A resize is a useful retry boundary: drivers can reject one
            // context/target combination yet support a later target.
            Release();
            _initializationFailed = false;
            _initializationFailureReason = {};
            _initializationRetryDrawsRemaining = 0;
        }
        if (_initializationFailed)
        {
            if (_initializationRetryDrawsRemaining > 0)
            {
                --_initializationRetryDrawsRemaining;
                _lastFailureReason = _initializationFailureReason;
                return S_FALSE;
            }
            _initializationFailed = false;
            _initializationFailureReason = {};
        }
        if (!Initialize())
        {
            _retryTargetWidth = target.width;
            _retryTargetHeight = target.height;
            _initializationRetryDrawsRemaining = kInitializationRetryDraws;
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

        if (renderMode == VectorRenderMode::Fill &&
            PrepareFillMeshes(stableScene, false) != S_OK)
        {
            return S_FALSE;
        }

        const HRESULT renderResult =
            RenderScene(stableScene, viewport, target, renderMode);
        if (FAILED(renderResult) || renderResult == S_FALSE)
        {
            return renderResult;
        }

        const HRESULT copyResult =
            CopyToDeviceContext(deviceContext, bounds, target);
        return copyResult;
    }
}
