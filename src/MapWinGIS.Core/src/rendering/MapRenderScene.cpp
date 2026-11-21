// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapRenderScene.cpp
 * @brief Builds owned render data for all visible layers of a map.
 */
#include "mapwingis/rendering/MapRenderScene.h"

#include <exception>
#include <memory>
#include <new>
#include <utility>

#include "internal/VectorRenderSceneBuilder.h"

namespace mwg::rendering
{
    detail::LayerScene detail::BuildLayerRenderScene(
        const MapLayer& layer,
        const VectorRenderSceneOptions& options)
    {
        if (!layer.IsVisible())
        {
            return SkippedLayer{layer.Id(), LayerSkipReason::Hidden, {}};
        }

        VectorRenderScene scene;
        try
        {
            scene = detail::BuildVectorRenderScene(layer.Source(), options);
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
            const LayerSkipReason reason = options.spatialFilter.has_value()
                ? LayerSkipReason::OutsideViewport
                : LayerSkipReason::NoGeometry;
            return SkippedLayer{layer.Id(), reason, {}};
        }
        return LayerRenderScene{layer.Id(), std::make_shared<const VectorRenderScene>(std::move(scene))};
    }

    MapRenderScene CreateMapRenderScene(const Map& map)
    {
        return CreateMapRenderScene(map, {});
    }

    MapRenderScene CreateMapRenderScene(
        const Map& map,
        const VectorRenderSceneOptions& options)
    {
        detail::ValidateVectorRenderSceneOptions(options);
        MapRenderScene result;
        for (int position = 0; position < map.LayerCount(); ++position)
        {
            detail::LayerScene layerScene =
                detail::BuildLayerRenderScene(map.Layer(position), options);
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
