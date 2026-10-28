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

#include "GdiVectorLayerRenderer.h"
#include "OpenGlVectorLayerRenderer.h"
#include "VectorRenderBackend.h"
#include "mapwingis/rendering/VectorRenderScene.h"

namespace mwg::activex
{
    class VectorLayerRenderer final
    {
    public:
        [[nodiscard]] HRESULT Draw(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::VectorRenderScene& scene) noexcept;

        [[nodiscard]] std::optional<VectorRenderBackendKind> GetActiveBackend() const noexcept
        {
            return _activeBackend;
        }

    private:
        OpenGlVectorLayerRenderer _openGlRenderer;
        GdiVectorLayerRenderer _gdiRenderer;
        std::optional<VectorRenderBackendKind> _activeBackend;
    };
}
