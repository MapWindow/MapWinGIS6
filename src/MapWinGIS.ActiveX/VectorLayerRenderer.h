// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorLayerRenderer.h
 * @brief Declares the initial GDI renderer for backend-neutral vector scenes.
 *
 * The renderer paints polygon scene data into the ActiveX control's host
 * drawing context and does not access vector datasets.
 */
#pragma once

#include <Windows.h>

#include "mapwingis/rendering/VectorRenderScene.h"

namespace mwg::activex
{
    [[nodiscard]] HRESULT DrawVectorRenderScene(
        HDC deviceContext,
        const RECT& bounds,
        const rendering::VectorRenderScene& scene) noexcept;
}
