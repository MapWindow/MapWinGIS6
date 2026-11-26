// SPDX-License-Identifier: MPL-2.0

/**
 * @file OpenGlVectorLayerRenderer.h
 * @brief Declares the WGL-backed polygon fill and outline renderer.
 *
 * The backend owns a reusable OpenGL context, created on a hidden window that
 * is never presented. Immutable scene geometry is cached in GPU buffers, then
 * rendered into a framebuffer object before the required pixels are copied to
 * the host device context. Missing fill meshes can be prepared on a background
 * worker and published without giving that worker access to the cache.
 */
#pragma once

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "VectorRenderBackend.h"

namespace mwg::activex
{
    namespace testing
    {
        class OpenGlVectorLayerRendererTestAccess;
        class MapWinGISControlTestAccess;
    }
    struct ViewTransform;
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

        /// Reports whether Fill-mode drawing can proceed for this scene
        /// without generating geometry on the calling thread. Ready means
        /// every drawable layer has meshes from the scene or the cache;
        /// Pending means at least one layer still needs preparation; Failed
        /// means the frame must stay on GDI (a missing scene, unusable
        /// meshes, or a failed triangulation).
        enum class FillMeshReadiness : int
        {
            Ready,
            Pending,
            Failed
        };

        /// Background-preparation state for one layer/scene pair.
        enum class FillMeshPreparationState : int
        {
            NotStarted,
            Pending,
            Ready,
            Failed
        };

        using FillMeshPreparationWakeCallback = bool(*)(void*) noexcept;

        [[nodiscard]] FillMeshReadiness GetFillMeshReadiness(
            const rendering::MapRenderScene& scene) const noexcept;

        void ConfigureFillMeshPreparation(
            FillMeshPreparationWakeCallback wakeCallback,
            void* wakeContext) noexcept;

        /// Queues missing meshes without doing triangulation on the caller.
        /// Cached scene entries and an earlier preparation for the same
        /// immutable scene are reused.
        void PrepareFillMeshesInBackground(
            const rendering::MapRenderScene& scene) noexcept;

        /// Publishes completed worker results from the control's owner thread.
        /// Returns true when a pending state changed.
        [[nodiscard]] bool PublishPreparedFillMeshes() noexcept;
        [[nodiscard]] bool HasCompletedFillMeshPreparation() const noexcept;

        /// Returns why the scene cannot be drawn in the requested mode
        /// without generating geometry, or nullopt when it is drawable.
        /// Does not create a context or touch the fill-mesh cache.
        [[nodiscard]] static std::optional<std::wstring_view>
        GetSceneRejectionReason(
            const rendering::MapRenderScene& scene,
            VectorRenderMode renderMode) noexcept;

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
        void QueueDeletedGpuBuffer(unsigned int buffer) noexcept;
        void FlushDeletedGpuBuffers() noexcept;
        [[nodiscard]] HRESULT DrawLayerWithGpuBuffers(
            LayerId layerId,
            const std::shared_ptr<const rendering::VectorRenderScene>& scene,
            std::span<const std::optional<rendering::TriangleMesh>> fillMeshes,
            const ViewTransform& transform,
            VectorRenderMode renderMode) noexcept;
        [[nodiscard]] bool PruneGpuBufferCache(
            const rendering::MapRenderScene& scene) noexcept;
        void ClearGpuBufferCache() noexcept;
        void ClearLayerGpuBufferCache(LayerId layerId) noexcept;
        [[nodiscard]] bool HasGpuBufferCache() const noexcept;
        [[nodiscard]] std::size_t GpuBufferCacheSize() const noexcept;
        [[nodiscard]] std::size_t GpuBufferUploadCount() const noexcept;
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
        [[nodiscard]] FillMeshPreparationState GetFillMeshPreparationState(
            LayerId layerId,
            const std::shared_ptr<const rendering::VectorRenderScene>& scene)
            const noexcept;
        [[nodiscard]] bool PruneFillMeshCache(
            const rendering::MapRenderScene& scene) noexcept;
        void ClearFillMeshCache() noexcept;
        void ClearLayerFillMeshCache(LayerId layerId) noexcept;
        [[nodiscard]] std::size_t FillMeshCacheSize() const noexcept;
        [[nodiscard]] std::size_t CachedGeneratedMeshStorage() const noexcept;
        [[nodiscard]] std::size_t CachedGeneratedMeshStorageBytes() const noexcept;
        [[nodiscard]] bool WaitForFillMeshPreparation(
            std::chrono::milliseconds timeout) noexcept;
        void CancelLayerFillMeshPreparation(LayerId layerId) noexcept;
        void CancelAllFillMeshPreparation() noexcept;
        void StopFillMeshPreparation() noexcept;
        struct FillMeshPreparation;
        static void RunFillMeshPreparation(
            const std::shared_ptr<FillMeshPreparation>& preparation) noexcept;
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
        struct BufferFunctions;
        std::unique_ptr<BufferFunctions> _bufferFunctions;
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
        std::shared_ptr<FillMeshPreparation> _fillMeshPreparation;
        std::thread _fillMeshPreparationWorker;
        FillMeshPreparationWakeCallback _fillMeshWakeCallback = nullptr;
        void* _fillMeshWakeContext = nullptr;
        struct GpuBufferCache;
        std::unique_ptr<GpuBufferCache> _gpuBufferCache;
        std::size_t _gpuBufferUploadCount = 0;
        int _retryTargetWidth = 0;
        int _retryTargetHeight = 0;
        unsigned int _initializationRetryDrawsRemaining = 0;
    };
}
