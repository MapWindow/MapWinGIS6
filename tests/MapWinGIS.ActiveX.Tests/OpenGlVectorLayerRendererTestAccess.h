// SPDX-License-Identifier: MPL-2.0

/**
 * @file OpenGlVectorLayerRendererTestAccess.h
 * @brief Provides test-only access to OpenGL recovery and retained buffers.
 */
#pragma once

#include "OpenGlVectorLayerRenderer.h"

namespace mwg::activex::testing
{
    class OpenGlVectorLayerRendererTestAccess final
    {
    public:
        [[nodiscard]] static std::size_t StagingCapacity(
            const OpenGlVectorLayerRenderer& renderer) noexcept
        {
            return renderer._pixelBuffer.capacity();
        }

        [[nodiscard]] static const unsigned char* StagingData(
            const OpenGlVectorLayerRenderer& renderer) noexcept
        {
            return renderer._pixelBuffer.data();
        }

        static void SimulateFailedRecovery(
            OpenGlVectorLayerRenderer& renderer, const int width, const int height) noexcept
        {
            renderer.Release();
            renderer._contextInvalid = true;
            renderer._initializationFailed = true;
            renderer._initializationFailureReason = L"Simulated context initialization failure.";
            renderer._retryTargetWidth = width;
            renderer._retryTargetHeight = height;
            renderer._initializationRetryDrawsRemaining = 60;
        }

        [[nodiscard]] static unsigned int RetryDrawsRemaining(
            const OpenGlVectorLayerRenderer& renderer) noexcept
        {
            return renderer._initializationRetryDrawsRemaining;
        }

        static void SimulateContextLoss(OpenGlVectorLayerRenderer& renderer) noexcept
        {
            renderer.SimulateContextLoss();
        }

        [[nodiscard]] static bool HasGpuBufferCache(
            const OpenGlVectorLayerRenderer& renderer) noexcept
        {
            return renderer.HasGpuBufferCache();
        }

        [[nodiscard]] static std::size_t GpuBufferCacheSize(
            const OpenGlVectorLayerRenderer& renderer) noexcept
        {
            return renderer.GpuBufferCacheSize();
        }

        [[nodiscard]] static std::size_t GpuBufferUploadCount(
            const OpenGlVectorLayerRenderer& renderer) noexcept
        {
            return renderer.GpuBufferUploadCount();
        }

        /// Checks scene support without activating an OpenGL context.
        [[nodiscard]] static bool IsSceneSupported(
            const rendering::MapRenderScene& scene,
            const VectorRenderMode renderMode = VectorRenderMode::Fill) noexcept
        {
            return OpenGlVectorLayerRenderer::IsSceneSupported(scene, renderMode);
        }

        [[nodiscard]] static HRESULT PrepareFillMeshes(
            OpenGlVectorLayerRenderer& renderer,
            const rendering::MapRenderScene& scene) noexcept
        {
            return renderer.PrepareFillMeshes(scene);
        }

        [[nodiscard]] static std::size_t FillMeshCacheSize(
            const OpenGlVectorLayerRenderer& renderer) noexcept
        {
            return renderer.FillMeshCacheSize();
        }

        [[nodiscard]] static std::size_t CachedGeneratedMeshStorage(
            const OpenGlVectorLayerRenderer& renderer) noexcept
        {
            return renderer.CachedGeneratedMeshStorage();
        }

        [[nodiscard]] static std::size_t CachedGeneratedMeshStorageBytes(
            const OpenGlVectorLayerRenderer& renderer) noexcept
        {
            return renderer.CachedGeneratedMeshStorageBytes();
        }

        static void InvalidateLayerSceneCache(
            OpenGlVectorLayerRenderer& renderer, const LayerId layerId) noexcept
        {
            renderer.ClearLayerFillMeshCache(layerId);
        }

        static void InvalidateSceneCache(
            OpenGlVectorLayerRenderer& renderer) noexcept
        {
            renderer.ClearFillMeshCache();
        }
    };
}
