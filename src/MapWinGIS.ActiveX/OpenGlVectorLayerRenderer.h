// SPDX-License-Identifier: MPL-2.0

/**
 * @file OpenGlVectorLayerRenderer.h
 * @brief Declares the WGL-backed polygon scene renderer.
 *
 * The backend owns a reusable hidden OpenGL context and copies its rendered
 * pixels into the host device context.
 */
#pragma once

#include <Windows.h>

#include <vector>

#include "VectorRenderBackend.h"

namespace mwg::activex
{
    class OpenGlVectorLayerRenderer final : public IVectorRenderBackend
    {
    public:
        OpenGlVectorLayerRenderer() = default;
        ~OpenGlVectorLayerRenderer() override;

        OpenGlVectorLayerRenderer(const OpenGlVectorLayerRenderer&) = delete;
        OpenGlVectorLayerRenderer& operator=(const OpenGlVectorLayerRenderer&) = delete;

        [[nodiscard]] HRESULT Draw(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::VectorRenderScene& scene) noexcept override;

    private:
        [[nodiscard]] bool Initialize() noexcept;
        void Release() noexcept;

        HWND _window{};
        HDC _deviceContext{};
        HGLRC _renderingContext{};
        bool _initializationFailed = false;
        std::vector<unsigned char> _rgbaPixels;
        std::vector<unsigned char> _dibPixels;
    };
}
