// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorLayerRenderer.cpp
 * @brief Coordinates OpenGL and GDI rendering for vector scenes.
 *
 * Keeps backend selection internal to the ActiveX rendering layer and retries
 * with GDI when OpenGL is unavailable or cannot represent the scene.
 */
#include "VectorLayerRenderer.h"

namespace mwg::activex
{
    HRESULT VectorLayerRenderer::Draw(
        const HDC deviceContext,
        const RECT& bounds,
        const rendering::VectorRenderScene& scene) noexcept
    {
        _activeBackend.reset();

        const HRESULT openGlResult = _openGlRenderer.Draw(deviceContext, bounds, scene);
        if (openGlResult == S_OK)
        {
            _activeBackend = VectorRenderBackendKind::OpenGL;
            return S_OK;
        }

        if (FAILED(openGlResult))
        {
            ::OutputDebugStringW(
                L"MapWinGIS: OpenGL rendering failed; using the GDI backend.\n");
        }

        const HRESULT gdiResult = _gdiRenderer.Draw(deviceContext, bounds, scene);
        if (SUCCEEDED(gdiResult))
        {
            _activeBackend = VectorRenderBackendKind::Gdi;
        }

        return gdiResult;
    }
}
