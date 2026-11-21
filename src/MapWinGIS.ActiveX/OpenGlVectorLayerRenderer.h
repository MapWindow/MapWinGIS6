// SPDX-License-Identifier: MPL-2.0

/**
 * @file OpenGlVectorLayerRenderer.h
 * @brief Declares the WGL-backed polygon fill and outline renderer.
 *
 * The backend owns a reusable OpenGL context, created on a hidden window that
 * is never presented, and renders into a framebuffer object before copying
 * the pixels into the host device context.
 */
#pragma once

#include <Windows.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "VectorRenderBackend.h"

namespace mwg::activex
{
    namespace testing
    {
        class OpenGlVectorLayerRendererTestAccess;
        class MapWinGISControlTestAccess;
    }
    class VectorLayerRenderer;

    class OpenGlVectorLayerRenderer final : public IVectorRenderBackend
    {
    public:
        OpenGlVectorLayerRenderer() noexcept;
        ~OpenGlVectorLayerRenderer() override;

        OpenGlVectorLayerRenderer(const OpenGlVectorLayerRenderer&) = delete;
        OpenGlVectorLayerRenderer& operator=(const OpenGlVectorLayerRenderer&) = delete;

        /**
         * @brief Returns why a target exceeds detected size or staging limits.
         *
         * @p maximumWidth and @p maximumHeight are the effective per-axis
         * limits: the smaller of the renderbuffer size and the viewport
         * dimensions, because `glViewport` silently clamps larger targets.
         * Returns an empty optional when the target is acceptable.
         */
        [[nodiscard]] static std::optional<std::wstring_view> GetTargetRejectionReason(
            int width,
            int height,
            int maximumWidth,
            int maximumHeight) noexcept;

        /**
         * @brief Draws polygon fills or outlines through OpenGL.
         *
         * Fill mode uses a Core-prepared triangle mesh when one is present,
         * otherwise triangulating the polygon on demand and reusing the
         * generated mesh for later draws of the same scene. It draws
         * boundaries from the original rings. Outline mode draws the original
         * rings without requiring fill meshes. Polygons retain the GDI
         * backend's painter order within a layer.
         * Draws all layers into one frame, bottom first. Returns S_FALSE
         * without drawing when fill-mesh preparation fails or when no OpenGL
         * context with framebuffer-object support is available, so callers
         * can fall back to another backend for the whole frame.
         * When retainStagingBuffer is false, a temporary renderer owns the
         * draw's CPU and GPU buffers, leaving the screen resources unchanged.
         */
        [[nodiscard]] HRESULT Draw(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::MapRenderScene& scene,
            const Viewport& viewport,
            bool retainStagingBuffer = true,
            VectorRenderMode renderMode = VectorRenderMode::Fill) noexcept override;

        [[nodiscard]] std::wstring_view GetLastFailureReason() const noexcept
        {
            return _lastFailureReason;
        }

    private:
        friend class testing::OpenGlVectorLayerRendererTestAccess;
        friend class testing::MapWinGISControlTestAccess;
        friend class VectorLayerRenderer;

        /// True when the scene can be drawn in the requested mode. Outline
        /// mode needs finite ring coordinates; fill mode may lazily prepare
        /// missing meshes but rejects unusable explicitly prepared meshes.
        /// Exposed for tests.
        [[nodiscard]] static bool IsSceneSupported(
            const rendering::MapRenderScene& scene,
            VectorRenderMode renderMode) noexcept;

        void SimulateContextLoss() noexcept;

        struct FramebufferFunctions;
        struct Target final
        {
            int width = 0;
            int height = 0;
            std::size_t pixelWidth = 0;
            std::size_t pixelHeight = 0;
        };
        struct TargetValidation final
        {
            HRESULT result = E_INVALIDARG;
            std::optional<Target> target;
        };

        [[nodiscard]] bool Initialize() noexcept;
        [[nodiscard]] bool InitializeFramebuffer() noexcept;
        [[nodiscard]] bool ResizeFramebuffer(int width, int height) noexcept;
        void Release() noexcept;
        [[nodiscard]] TargetValidation ValidateTarget(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::MapRenderScene& scene,
            const Viewport& viewport,
            VectorRenderMode renderMode) noexcept;
        [[nodiscard]] bool EnsureStaging(std::size_t stagingBytes) noexcept;
        [[nodiscard]] HRESULT PrepareFillMeshes(
            const rendering::MapRenderScene& scene,
            bool pruneCache = true) noexcept;
        [[nodiscard]] std::span<const std::optional<rendering::TriangleMesh>>
            GetFillMeshes(
                LayerId layerId,
                const rendering::VectorRenderScene& scene) const noexcept;
        [[nodiscard]] bool PruneFillMeshCache(
            const rendering::MapRenderScene& scene) noexcept;
        void ClearFillMeshCache() noexcept;
        void ClearLayerFillMeshCache(LayerId layerId) noexcept;
        [[nodiscard]] std::size_t FillMeshCacheSize() const noexcept;
        [[nodiscard]] std::size_t CachedGeneratedMeshStorage() const noexcept;
        [[nodiscard]] std::size_t CachedGeneratedMeshStorageBytes() const noexcept;
        [[nodiscard]] HRESULT RenderScene(
            const rendering::MapRenderScene& scene,
            const Viewport& viewport,
            const Target& target,
            VectorRenderMode renderMode) noexcept;
        [[nodiscard]] HRESULT CopyToDeviceContext(
            HDC deviceContext,
            const RECT& bounds,
            const Target& target) noexcept;

        HWND _window{};
        HDC _deviceContext{};
        HGLRC _renderingContext{};
        std::unique_ptr<FramebufferFunctions> _framebufferFunctions;
        unsigned int _framebuffer{};
        unsigned int _renderbuffer{};
        int _framebufferWidth{};
        int _framebufferHeight{};
        int _maximumTargetWidth{};
        int _maximumTargetHeight{};
        bool _initializationFailed = false;
        bool _contextInvalid = false;
        std::wstring_view _lastFailureReason =
            L"OpenGL was not selected for this frame.";
        std::wstring_view _initializationFailureReason;
        std::vector<unsigned char> _pixelBuffer;
        struct FillMeshCache;
        std::shared_ptr<FillMeshCache> _fillMeshCache;
        int _retryTargetWidth = 0;
        int _retryTargetHeight = 0;
        unsigned int _initializationRetryDrawsRemaining = 0;
    };
}
