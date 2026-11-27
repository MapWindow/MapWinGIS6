// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapRenderScene.cpp
 * @brief Builds owned render data for all visible layers of a map.
 */
#include "mapwingis/rendering/MapRenderScene.h"

#include <exception>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include "mapwingis/core/Error.h"
#include "mapwingis/vector/VectorDataset.h"

#include "internal/VectorRenderSceneBuilder.h"

namespace mwg::rendering
{
    namespace
    {
        void TransformPoints(
            const std::span<Point> points,
            const ProjectionTransform& transform)
        {
            if (points.empty())
            {
                return;
            }
            std::vector<double> x(points.size());
            std::vector<double> y(points.size());
            for (std::size_t index = 0; index < points.size(); ++index)
            {
                x[index] = points[index].x;
                y[index] = points[index].y;
            }
            if (!transform.Transform(points.size(), x.data(), y.data()))
            {
                throw CoreError("Cannot transform vector layer geometry");
            }
            for (std::size_t index = 0; index < points.size(); ++index)
            {
                points[index] = {x[index], y[index]};
            }
        }

        void TransformScene(
            VectorRenderScene& scene,
            const ProjectionTransform& forward)
        {
            TransformPoints(scene.polygons.Coordinates(), forward);
            for (std::optional<TriangleMesh>& mesh : scene.fillMeshes)
            {
                if (mesh.has_value())
                {
                    TransformPoints(mesh->vertices, forward);
                }
            }
        }
    }

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
            const std::optional<Projection>& display =
                layer.DisplayProjection();
            if (!display.has_value())
            {
                scene = detail::BuildVectorRenderScene(layer.Source(), options);
            }
            else
            {
                const Projection source = layer.Source().Projection();
                if (source.IsEmpty())
                {
                    throw CoreError(
                        "Cannot transform a scene from a projectionless source");
                }

                if (source.IsSame(*display))
                {
                    scene = detail::BuildVectorRenderScene(layer.Source(), options);
                }
                else
                {
                    VectorRenderSceneOptions sourceOptions = options;
                    if (options.spatialFilter.has_value())
                    {
                        const ProjectionTransform inverse = display->TransformTo(source);
                        Extent sourceFilter;
                        if (!inverse.TransformBounds(
                                *options.spatialFilter, sourceFilter) ||
                            !sourceFilter.IsValid())
                        {
                            throw CoreError("Cannot transform the layer spatial filter");
                        }
                        sourceOptions.spatialFilter = sourceFilter;
                    }

                    scene = detail::BuildVectorRenderScene(
                        layer.Source(), sourceOptions);
                    const ProjectionTransform forward = source.TransformTo(*display);
                    TransformScene(scene, forward);
                    if (scene.extent.IsValid())
                    {
                        Extent transformedExtent;
                        if (!forward.TransformBounds(scene.extent, transformedExtent) ||
                            !transformedExtent.IsValid())
                        {
                            throw CoreError("Cannot transform vector layer scene extent");
                        }
                        scene.extent = transformedExtent;
                    }
                }
            }
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
