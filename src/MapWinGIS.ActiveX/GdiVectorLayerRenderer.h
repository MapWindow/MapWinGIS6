// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdiVectorLayerRenderer.h
 * @brief Declares the GDI backend for polygon render scenes.
 *
 * The backend paints supported vector scene geometry into the supplied
 * device context and does not retain host or scene resources.
 */
#pragma once

#include <vector>

#include "VectorRenderBackend.h"

namespace mwg::activex
{
    class GdiVectorLayerRenderer final : public IVectorRenderBackend
    {
    public:
        /// Draws the layers bottom first. A layer with a vertex outside the
        /// GDI coordinate range, or a non-finite vertex, is skipped as a
        /// whole and listed by GetFailedLayers; the other layers are still
        /// drawn. Returns E_INVALIDARG when every layer was skipped, and S_OK
        /// when the scene has no layers.
        [[nodiscard]] HRESULT Draw(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::MapRenderScene& scene,
            const Viewport& viewport) noexcept override;

        /// Layers skipped by the last Draw call, in drawing order.
        [[nodiscard]] const std::vector<LayerId>& GetFailedLayers() const noexcept
        {
            return _failedLayers;
        }

    private:
        std::vector<LayerId> _failedLayers;
    };
}
