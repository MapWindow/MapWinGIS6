// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdiVectorLayerRenderer.h
 * @brief Declares the GDI backend for polygon render scenes.
 *
 * The backend paints supported vector scene geometry into the supplied
 * device context and does not retain host or scene resources.
 */
#pragma once

#include "VectorRenderBackend.h"

namespace mwg::activex
{
    class GdiVectorLayerRenderer final : public IVectorRenderBackend
    {
    public:
        [[nodiscard]] HRESULT Draw(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::VectorRenderScene& scene) noexcept override;
    };
}
