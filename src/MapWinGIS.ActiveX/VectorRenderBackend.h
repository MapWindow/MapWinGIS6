// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderBackend.h
 * @brief Defines the internal backend contract for vector scene drawing.
 *
 * Backends consume owned Core render scenes and draw them into a Windows
 * device context without accessing datasets or COM interfaces.
 */
#pragma once

#include <Windows.h>

#include "mapwingis/rendering/VectorRenderScene.h"

namespace mwg::activex
{
    enum class VectorRenderBackendKind
    {
        OpenGL,
        Gdi
    };

    class IVectorRenderBackend
    {
    public:
        virtual ~IVectorRenderBackend() = default;

        /// Returns S_FALSE when the backend cannot render this scene or target.
        [[nodiscard]] virtual HRESULT Draw(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::VectorRenderScene& scene) noexcept = 0;
    };
}
