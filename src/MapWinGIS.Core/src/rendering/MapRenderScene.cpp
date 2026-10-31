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
#include "mapwingis/core/Error.h"

namespace mwg::rendering::detail
{
    LayerRenderSceneResult BuildLayerRenderScene(
        const Map& map,
        const MapLayer& layer,
        const Extent* visibleExtent,
        const int width,
        const int height)
    {
        if (!layer.IsVisible())
        {
            return {{}, SkippedLayer{layer.Id(), LayerSkipReason::Hidden, {}}};
        }

        try
        {
            if (layer.IsRaster())
            {
                RasterImage raster;
                if (visibleExtent != nullptr)
                {
                    raster = layer.RasterSource()->ReadWindow(
                        *visibleExtent,
                        map.CoordinateSystemWkt(),
                        width,
                        height);
                }
                else
                {
                    raster = layer.RasterSource()->ReadImage(map.CoordinateSystemWkt());
                    raster.extent = layer.GetExtent();
                }
                return {
                    LayerRenderScene{layer.Id(), {}, std::move(raster)},
                    {}};
            }

            VectorRenderScene scene = BuildVectorRenderScene(
                layer.Source(),
                map.CoordinateSystemWkt());
            scene.extent = layer.GetExtent();
            if (scene.points.empty() && scene.lines.empty() && scene.polygons.empty())
            {
                return {
                    {},
                    SkippedLayer{layer.Id(), LayerSkipReason::NoGeometry, {}}};
            }
            return {
                LayerRenderScene{layer.Id(), std::move(scene), std::nullopt},
                {}};
        }
        catch (const std::bad_alloc&)
        {
            throw;
        }
        catch (const std::exception& error)
        {
            return {
                {},
                SkippedLayer{layer.Id(), LayerSkipReason::Failed, error.what()}};
        }
    }
}

namespace mwg::rendering
{
    namespace
    {
        MapRenderScene BuildMapRenderScene(
            const Map& map,
            const Extent* visibleExtent,
            const int width,
            const int height)
        {
            MapRenderScene result;
            for (int position = 0; position < map.LayerCount(); ++position)
            {
                detail::LayerRenderSceneResult layer =
                    detail::BuildLayerRenderScene(
                        map,
                        map.Layer(position),
                        visibleExtent,
                        width,
                        height);
                if (layer.layer.has_value())
                {
                    result.layers.push_back(std::move(*layer.layer));
                }
                if (layer.skipped.has_value())
                {
                    result.skippedLayers.push_back(std::move(*layer.skipped));
                }
            }
            return result;
        }
    }

    MapRenderScene CreateMapRenderScene(const Map& map)
    {
        return BuildMapRenderScene(map, nullptr, 0, 0);
    }

    MapRenderScene CreateMapRenderScene(
        const Map& map,
        const Extent& visibleExtent,
        const int width,
        const int height)
    {
        if (!visibleExtent.IsValid() || visibleExtent.Width() <= 0.0 ||
            visibleExtent.Height() <= 0.0 || width <= 0 || height <= 0)
        {
            throw InvalidArgument("Raster scene window must have a valid extent and dimensions");
        }
        return BuildMapRenderScene(map, &visibleExtent, width, height);
    }
}
