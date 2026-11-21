// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorLayerRenderer.cpp
 * @brief Coordinates OpenGL and GDI rendering for vector scenes.
 *
 * Keeps backend selection internal to the ActiveX rendering layer and retries
 * with GDI when OpenGL is unavailable or cannot represent the scene.
 */
#include "VectorLayerRenderer.h"

#include <new>
#include <stdexcept>

namespace mwg::activex
{
    HRESULT VectorLayerRenderer::Draw(
        const HDC deviceContext,
        const RECT& bounds,
        const rendering::MapRenderScene& scene,
        const Viewport& viewport,
        const bool retainStagingBuffer,
        const VectorRenderMode renderMode) noexcept
    {
        _activeBackend.reset();

        rendering::MapRenderScene stableScene;
        try
        {
            stableScene = scene;
        }
        catch (const std::bad_alloc&)
        {
            _diagnostic = L"The renderer could not snapshot the render scene.";
            return E_OUTOFMEMORY;
        }
        catch (const std::length_error&)
        {
            _diagnostic = L"The renderer could not represent its render-scene snapshot.";
            return S_FALSE;
        }

        _openGlRenderer.PruneFillMeshCache(stableScene);

        if (_preference != VectorRenderBackendPreference::Gdi)
        {
            const HRESULT openGlResult =
                _openGlRenderer.Draw(
                    deviceContext,
                    bounds,
                    stableScene,
                    viewport,
                    retainStagingBuffer,
                    renderMode);
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

        const HRESULT gdiResult =
            _gdiRenderer.Draw(
                deviceContext,
                bounds,
                stableScene,
                viewport,
                true,
                renderMode);
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
