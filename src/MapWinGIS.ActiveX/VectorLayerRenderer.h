// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorLayerRenderer.h
 * @brief Declares the backend coordinator for vector scene rendering.
 *
 * Tries OpenGL rendering first and uses the GDI backend for unsupported
 * geometry or when OpenGL cannot be initialized.
 */
#pragma once

#include <Windows.h>

#include <optional>
#include <string_view>

#include "GdiVectorLayerRenderer.h"
#include "OpenGlVectorLayerRenderer.h"
#include "VectorRenderBackend.h"
#include "mapwingis/rendering/VectorRenderScene.h"

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

        [[nodiscard]] HRESULT Draw(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::VectorRenderScene& scene) noexcept;

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
