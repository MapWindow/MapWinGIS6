// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderSceneBuilder.h
 * @brief Declares private scene building shared by layer and map scenes.
 *
 * Lets map scene creation tell layers without polygon geometry apart from
 * layers that cannot be rendered, without parsing exception messages.
 */
#pragma once

#include "mapwingis/rendering/VectorRenderScene.h"

namespace mwg::rendering::detail
{
    /// Copies polygon geometry from a layer into scene data. Unlike
    /// CreateVectorRenderScene, a layer without polygon geometry gives a
    /// scene with no polygons instead of an error.
    /// \throws CoreError when the layer is not polygonal or its geometry
    ///         cannot be converted.
    [[nodiscard]] VectorRenderScene BuildVectorRenderScene(const VectorLayer& layer);
}
