// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorLayerRenderer.cpp
 * @brief Coordinates OpenGL and GDI rendering for vector scenes.
 *
 * Keeps backend selection internal to the ActiveX rendering layer, queues
 * background fill-mesh preparation, publishes ready meshes on draw unless an
 * owner wake callback publishes them for Automatic Fill frames, and
 * retries with GDI when OpenGL cannot represent the scene.
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

        const bool automaticFill =
            _preference == VectorRenderBackendPreference::Automatic &&
            renderMode == VectorRenderMode::Fill;
        // Publishing here would let this paint select OpenGL before the owner
        // has warmed the target, moving context and framebuffer allocation
        // back onto the paint path.
        if (!(automaticFill && _ownerPublishesFillMeshes))
        {
            (void)_openGlRenderer.PublishPreparedFillMeshes();
        }
        if (automaticFill)
        {
            _openGlRenderer.PrepareFillMeshesInBackground(stableScene);
        }

        if (_preference != VectorRenderBackendPreference::Gdi)
        {
            const auto readiness = automaticFill
                ? _openGlRenderer.GetFillMeshReadiness(stableScene)
                : OpenGlVectorLayerRenderer::FillMeshReadiness::Ready;

            if (automaticFill &&
                readiness != OpenGlVectorLayerRenderer::FillMeshReadiness::Ready)
            {
                // Drawing with OpenGL here would either triangulate missing
                // meshes on this thread before presenting a pixel (Pending)
                // or reuse a failed preparation while triangulating other
                // layers (Failed). Paint the frame with GDI instead; a later
                // frame retries once every layer's meshes exist. One frame
                // still uses a single backend.
                if (!_openGlRenderer.PruneFillMeshCache(stableScene))
                {
                    _diagnostic = _openGlRenderer.GetLastFailureReason();
                }
                else if (
                    readiness ==
                    OpenGlVectorLayerRenderer::FillMeshReadiness::Pending)
                {
                    _diagnostic =
                        L"GDI rendered the frame; the Automatic preference "
                        L"paints with GDI until the OpenGL fill meshes are "
                        L"prepared.";
                }
                else
                {
                    const auto reason =
                        OpenGlVectorLayerRenderer::GetSceneRejectionReason(
                            stableScene, renderMode);
                    _diagnostic = reason.has_value()
                        ? *reason
                        : L"The OpenGL backend could not triangulate a "
                          L"polygon for fill rendering; this frame was "
                          L"rendered with GDI.";
                }
            }
            else
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
        }
        else
        {
            if (!_openGlRenderer.PruneFillMeshCache(stableScene))
            {
                _diagnostic = _openGlRenderer.GetLastFailureReason();
            }
            else
            {
                _diagnostic = L"GDI was selected by the rendering preference.";
            }
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
