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
    detail::LayerScene detail::BuildLayerRenderScene(const MapLayer& layer)
    {
        if (!layer.IsVisible())
        {
            return SkippedLayer{layer.Id(), LayerSkipReason::Hidden, {}};
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
            return SkippedLayer{layer.Id(), LayerSkipReason::Failed, error.what()};
        }

        if (scene.polygons.empty())
        {
            return SkippedLayer{layer.Id(), LayerSkipReason::NoGeometry, {}};
        }
        return LayerRenderScene{layer.Id(), std::move(scene)};
    }

    MapRenderScene CreateMapRenderScene(const Map& map)
    {
        MapRenderScene result;
        for (int position = 0; position < map.LayerCount(); ++position)
        {
            detail::LayerScene layerScene = detail::BuildLayerRenderScene(map.Layer(position));
            if (auto* drawable = std::get_if<LayerRenderScene>(&layerScene))
            {
                result.layers.push_back(std::move(*drawable));
            }
            else
            {
                result.skippedLayers.push_back(std::get<SkippedLayer>(std::move(layerScene)));
            }
        }
        return result;
    }
}
