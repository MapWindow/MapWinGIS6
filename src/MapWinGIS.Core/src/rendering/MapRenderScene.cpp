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
                if (layer.IsRaster())
                {
                    RasterImage raster =
                        layer.RasterSource()->ReadImage(map.CoordinateSystemWkt());
                    raster.extent = layer.GetExtent();
                    result.layers.push_back({layer.Id(), {}, std::move(raster)});
                    continue;
                }

                scene = detail::BuildVectorRenderScene(
                    layer.Source(),
                    map.CoordinateSystemWkt());
                scene.extent = layer.GetExtent();
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

            if (scene.points.empty() && scene.lines.empty() && scene.polygons.empty())
            {
                result.skippedLayers.push_back({layer.Id(), LayerSkipReason::NoGeometry, {}});
                continue;
            }
            result.layers.push_back({layer.Id(), std::move(scene), std::nullopt});
        }
        return result;
    }
}
