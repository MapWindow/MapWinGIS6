// SPDX-License-Identifier: MPL-2.0

/**
 * @file OpenGlVectorLayerRendererTestAccess.h
 * @brief Provides test-only access to the OpenGL renderer's context-loss path.
 */
#pragma once

#include "OpenGlVectorLayerRenderer.h"

namespace mwg::activex::testing
{
    class OpenGlVectorLayerRendererTestAccess final
    {
    public:
        static void SimulateContextLoss(OpenGlVectorLayerRenderer& renderer) noexcept
        {
            renderer.SimulateContextLoss();
        }

        /// Checks scene support without activating an OpenGL context.
        [[nodiscard]] static bool IsSceneSupported(
            const rendering::MapRenderScene& scene) noexcept
        {
            return OpenGlVectorLayerRenderer::IsSceneSupported(scene);
        }
    };
}
