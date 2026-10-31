// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderSceneBuilder.h
 * @brief Declares private scene building shared by layer and map scenes.
 *
 * Lets map scene creation tell layers without drawable geometry apart from
 * layers that cannot be rendered, without parsing exception messages.
 */
#pragma once

#include <string>

#include "mapwingis/rendering/VectorRenderScene.h"

namespace mwg::rendering::detail
{
    /// Copies point, line, and polygon geometry from a layer into scene data. Unlike
    /// CreateVectorRenderScene, a layer without drawable geometry gives a
    /// scene with no polygons instead of an error.
    /// \throws CoreError when the layer has an unsupported geometry type or its geometry
    ///         cannot be converted.
    [[nodiscard]] VectorRenderScene BuildVectorRenderScene(
        const VectorLayer& layer,
        const std::string& mapProjectionWkt = {});
}
