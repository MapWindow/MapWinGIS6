// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorLayerRenderer.h
 * @brief Declares the backend coordinator for vector scene rendering.
 *
 * Tries OpenGL rendering first and uses the GDI backend for unsupported
 * geometry or when OpenGL cannot be initialized. With the Automatic
 * preference, a Fill-mode frame whose layers have no fill meshes yet is
 * painted entirely with GDI so the paint path never triangulates. Missing
 * meshes are prepared on a background worker and published to the owner
 * thread; a later frame then selects OpenGL. An explicit OpenGL preference
 * keeps drawing with OpenGL and triangulates lazily on the calling thread.
 * One backend draws the whole frame, so all layers share its viewport
 * transform and pixel conventions.
 */
#pragma once

#include <Windows.h>

#include <optional>
#include <span>
#include <string_view>

#include "GdiVectorLayerRenderer.h"
#include "OpenGlVectorLayerRenderer.h"
#include "VectorRenderBackend.h"
#include "mapwingis/map/Viewport.h"
#include "mapwingis/rendering/MapRenderScene.h"

namespace mwg::activex
{
    class VectorLayerRenderer final
    {
    public:
        void SetPreference(VectorRenderBackendPreference preference) noexcept
        {
            _preference = preference;
        }

        [[nodiscard]] VectorRenderBackendPreference GetPreference() const noexcept
        {
            return _preference;
        }

        /// Draws the scene through viewport into bounds; see
        /// IVectorRenderBackend::Draw. Layers that the selected backend
        /// cannot draw are skipped and listed by GetFailedLayers.
        [[nodiscard]] HRESULT Draw(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::MapRenderScene& scene,
            const Viewport& viewport,
            bool retainStagingBuffer = true,
            VectorRenderMode renderMode = VectorRenderMode::Fill) noexcept;

        /// Layers skipped by the last Draw call; empty when every layer was
        /// drawn.
        [[nodiscard]] std::span<const LayerId> GetFailedLayers() const noexcept
        {
            if (_activeBackend == VectorRenderBackendKind::OpenGL)
            {
                return {};
            }
            return _gdiRenderer.GetFailedLayers();
        }

        [[nodiscard]] std::optional<VectorRenderBackendKind> GetActiveBackend() const noexcept
        {
            return _activeBackend;
        }

        [[nodiscard]] std::wstring_view GetDiagnostic() const noexcept
        {
            return _diagnostic;
        }

        void InvalidateSceneCache() noexcept
        {
            _openGlRenderer.ClearFillMeshCache();
        }

        void InvalidateLayerSceneCache(const LayerId layerId) noexcept
        {
            _openGlRenderer.ClearLayerFillMeshCache(layerId);
        }

        void ConfigureFillMeshPreparation(
            OpenGlVectorLayerRenderer::FillMeshPreparationWakeCallback wakeCallback,
            void* wakeContext) noexcept
        {
            _openGlRenderer.ConfigureFillMeshPreparation(wakeCallback, wakeContext);
        }

        void PrepareFillMeshesInBackground(
            const rendering::MapRenderScene& scene) noexcept
        {
            _openGlRenderer.PrepareFillMeshesInBackground(scene);
        }

        [[nodiscard]] bool PublishPreparedFillMeshes() noexcept
        {
            return _openGlRenderer.PublishPreparedFillMeshes();
        }

        [[nodiscard]] bool HasCompletedFillMeshPreparation() const noexcept
        {
            return _openGlRenderer.HasCompletedFillMeshPreparation();
        }

    private:
        friend class testing::MapWinGISControlTestAccess;

        OpenGlVectorLayerRenderer _openGlRenderer;
        GdiVectorLayerRenderer _gdiRenderer;
        VectorRenderBackendPreference _preference =
            VectorRenderBackendPreference::Automatic;
        std::optional<VectorRenderBackendKind> _activeBackend;
        std::wstring_view _diagnostic = L"Rendering has not been attempted.";
    };
}
