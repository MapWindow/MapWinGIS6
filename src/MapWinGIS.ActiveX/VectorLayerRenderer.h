// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorLayerRenderer.h
 * @brief Declares the backend coordinator for vector scene rendering.
 *
 * Tries OpenGL rendering first and uses the GDI backend for unsupported
 * geometry or when OpenGL cannot be initialized. One backend draws the whole
 * frame, so all layers share its viewport transform and pixel conventions.
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
            const Viewport& viewport) noexcept;

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

    private:
        OpenGlVectorLayerRenderer _openGlRenderer;
        GdiVectorLayerRenderer _gdiRenderer;
        VectorRenderBackendPreference _preference =
            VectorRenderBackendPreference::Automatic;
        std::optional<VectorRenderBackendKind> _activeBackend;
        std::wstring_view _diagnostic = L"Rendering has not been attempted.";
    };
}
