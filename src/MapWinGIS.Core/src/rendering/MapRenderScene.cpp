// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapRenderScene.cpp
 * @brief Builds owned render data for all visible layers of a map.
 */
#include "mapwingis/rendering/MapRenderScene.h"

#include <exception>
#include <new>
#include <utility>

#include "internal/VectorRenderSceneBuilder.h"

namespace mwg::rendering
{
    MapRenderScene CreateMapRenderScene(const Map& map)
    {
        MapRenderScene result;
        for (int position = 0; position < map.LayerCount(); ++position)
        {
            const MapLayer& layer = map.Layer(position);
            if (!layer.IsVisible())
            {
                result.skippedLayers.push_back({layer.Id(), LayerSkipReason::Hidden, {}});
                continue;
            }

            VectorRenderScene scene;
            try
            {
                scene = detail::BuildVectorRenderScene(layer.Source());
            }
            catch (const std::bad_alloc&)
            {
                throw;
            }
            catch (const std::exception& error)
            {
                result.skippedLayers.push_back({layer.Id(), LayerSkipReason::Failed, error.what()});
                continue;
            }

            if (scene.polygons.empty())
            {
                result.skippedLayers.push_back({layer.Id(), LayerSkipReason::NoGeometry, {}});
                continue;
            }
            result.layers.push_back({layer.Id(), std::move(scene)});
        }
        return result;
    }
}
