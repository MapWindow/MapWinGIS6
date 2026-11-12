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
        const rendering::MapRenderScene& scene,
        const Viewport& viewport) noexcept
    {
        _activeBackend.reset();

        if (_preference != VectorRenderBackendPreference::Gdi)
        {
            const HRESULT openGlResult =
                _openGlRenderer.Draw(deviceContext, bounds, scene, viewport);
            if (openGlResult == S_OK)
            {
                _activeBackend = VectorRenderBackendKind::OpenGL;
                _diagnostic = L"OpenGL rendered the frame successfully.";
                return S_OK;
            }

            _diagnostic = _openGlRenderer.GetLastFailureReason();
            if (FAILED(openGlResult))
            {
                ::OutputDebugStringW(
                    L"MapWinGIS: OpenGL rendering failed; using the GDI backend.\n");
            }
        }
        else
        {
            _diagnostic = L"GDI was selected by the rendering preference.";
        }

        const HRESULT gdiResult = _gdiRenderer.Draw(deviceContext, bounds, scene, viewport);
        if (SUCCEEDED(gdiResult))
        {
            _activeBackend = VectorRenderBackendKind::Gdi;
            if (!_gdiRenderer.GetFailedLayers().empty())
            {
                _diagnostic = L"GDI could not draw one or more layers; the other layers were drawn.";
            }
        }
        else
        {
            _diagnostic = L"The selected CPU renderer could not draw this frame.";
        }

        return gdiResult;
    }
}
