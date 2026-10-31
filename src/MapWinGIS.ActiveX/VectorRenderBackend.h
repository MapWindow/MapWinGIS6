// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderBackend.h
 * @brief Defines the internal backend contract for vector scene drawing.
 *
 * Backends consume owned Core map render scenes and draw them through a Core
 * viewport into a Windows device context, without accessing maps, datasets,
 * or COM interfaces and without keeping view state of their own.
 */
#pragma once

#include <Windows.h>

#include "mapwingis/map/Viewport.h"
#include "mapwingis/rendering/MapRenderScene.h"

namespace mwg::activex
{
    enum class VectorRenderBackendKind : int
    {
        OpenGL,
        Gdi
    };

    enum class VectorRenderBackendPreference : int
    {
        Automatic,
        OpenGL,
        Gdi
    };

    class IVectorRenderBackend
    {
    public:
        virtual ~IVectorRenderBackend() = default;

        /// Draws all scene layers, bottom first, through the same viewport.
        /// bounds is the device rectangle of the viewport; its size must
        /// equal the viewport size, and the viewport must be valid.
        /// Returns S_FALSE when the backend cannot render this scene or target.
        [[nodiscard]] virtual HRESULT Draw(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::MapRenderScene& scene,
            const Viewport& viewport) noexcept = 0;
    };

    /// True when bounds has the viewport's size and the viewport is valid.
    [[nodiscard]] inline bool MatchesViewport(const RECT& bounds, const Viewport& viewport) noexcept
    {
        return viewport.IsValid() &&
            static_cast<long long>(bounds.right) - bounds.left == viewport.Width() &&
            static_cast<long long>(bounds.bottom) - bounds.top == viewport.Height();
    }
}
