// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapRenderScene.cpp
 * @brief Builds owned render data for all visible layers of a map.
 */
#include "mapwingis/rendering/MapRenderScene.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include <ogrsf_frmts.h>

#include "mapwingis/core/Error.h"
#include "mapwingis/rendering/PolygonTriangulation.h"
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

        [[nodiscard]] Ring UnwrapRing(const std::span<const Point> ring)
        {
            Ring result;
            result.reserve(ring.size());
            for (const Point& point : ring)
            {
                Point unwrapped = point;
                if (!result.empty())
                {
                    while (unwrapped.x - result.back().x > 180.0)
                    {
                        unwrapped.x -= 360.0;
                    }
                    while (unwrapped.x - result.back().x < -180.0)
                    {
                        unwrapped.x += 360.0;
                    }
                }
                result.push_back(unwrapped);
            }
            return result;
        }

        [[nodiscard]] std::pair<double, double> XRange(const Ring& ring)
        {
            double xMin = std::numeric_limits<double>::max();
            double xMax = std::numeric_limits<double>::lowest();
            for (const Point& point : ring)
            {
                xMin = std::min(xMin, point.x);
                xMax = std::max(xMax, point.x);
            }
            return {xMin, xMax};
        }

        [[nodiscard]] Polygon UnwrapPolygon(const PolygonView& source)
        {
            Polygon result;
            result.reserve(source.size());
            for (std::size_t index = 0; index < source.size(); ++index)
            {
                Ring ring = UnwrapRing(source[index]);
                if (index > 0 && !result.empty() && !ring.empty())
                {
                    const auto [exteriorMin, exteriorMax] = XRange(result.front());
                    const auto [ringMin, ringMax] = XRange(ring);
                    const double exteriorCenter = (exteriorMin + exteriorMax) / 2.0;
                    const double ringCenter = (ringMin + ringMax) / 2.0;
                    const double shift =
                        360.0 * std::round((exteriorCenter - ringCenter) / 360.0);
                    for (Point& point : ring)
                    {
                        point.x += shift;
                    }
                }
                result.push_back(std::move(ring));
            }
            return result;
        }

        [[nodiscard]] std::unique_ptr<OGRPolygon> MakeOgrPolygon(
            const Polygon& polygon)
        {
            auto result = std::make_unique<OGRPolygon>();
            for (std::size_t ringIndex = 0;
                 ringIndex < polygon.size();
                 ++ringIndex)
            {
                const Ring& source = polygon[ringIndex];
                auto ring = std::make_unique<OGRLinearRing>();
                for (const Point& point : source)
                {
                    ring->addPoint(point.x, point.y);
                }
                ring->closeRings();
                if (result->addRing(ring.get()) != OGRERR_NONE)
                {
                    throw CoreError("Cannot prepare polygon for longitude-seam clipping");
                }
            }
            return result;
        }

        void AppendOgrPolygon(
            PolygonBuffer& destination,
            const OGRPolygon& source,
            const double longitudeShift)
        {
            const OGRLinearRing* exterior = source.getExteriorRing();
            if (exterior == nullptr || exterior->getNumPoints() < 3)
            {
                return;
            }

            Polygon polygon;
            const auto appendRing = [&](const OGRLinearRing& ring)
            {
                Ring points;
                points.reserve(static_cast<std::size_t>(ring.getNumPoints()));
                for (int index = 0; index < ring.getNumPoints(); ++index)
                {
                    points.push_back({
                        ring.getX(index) - longitudeShift,
                        ring.getY(index)});
                }
                polygon.push_back(std::move(points));
            };

            appendRing(*exterior);
            for (int index = 0; index < source.getNumInteriorRings(); ++index)
            {
                const OGRLinearRing* hole = source.getInteriorRing(index);
                if (hole != nullptr && hole->getNumPoints() >= 3)
                {
                    appendRing(*hole);
                }
            }
            destination.AppendPolygon(polygon);
        }

        void AppendPolygonalGeometry(
            PolygonBuffer& destination,
            const OGRGeometry& geometry,
            const double longitudeShift)
        {
            switch (wkbFlatten(geometry.getGeometryType()))
            {
            case wkbPolygon:
                AppendOgrPolygon(
                    destination,
                    static_cast<const OGRPolygon&>(geometry),
                    longitudeShift);
                return;

            case wkbMultiPolygon:
            case wkbGeometryCollection:
            {
                const auto& collection =
                    static_cast<const OGRGeometryCollection&>(geometry);
                for (int index = 0; index < collection.getNumGeometries(); ++index)
                {
                    const OGRGeometry* child = collection.getGeometryRef(index);
                    if (child != nullptr)
                    {
                        AppendPolygonalGeometry(
                            destination, *child, longitudeShift);
                    }
                }
                return;
            }

            default:
                return;
            }
        }

        [[nodiscard]] std::unique_ptr<OGRPolygon> MakeLongitudeWindow(
            const double xMin,
            const double xMax,
            const double yMin,
            const double yMax)
        {
            auto window = std::make_unique<OGRPolygon>();
            auto ring = std::make_unique<OGRLinearRing>();
            ring->addPoint(xMin, yMin);
            ring->addPoint(xMax, yMin);
            ring->addPoint(xMax, yMax);
            ring->addPoint(xMin, yMax);
            ring->closeRings();
            if (window->addRing(ring.get()) != OGRERR_NONE)
            {
                throw CoreError("Cannot create longitude-seam clipping window");
            }
            return window;
        }

        void SplitPolygonAtLongitudeSeam(
            PolygonBuffer& destination,
            const PolygonView& source)
        {
            const Polygon unwrapped = UnwrapPolygon(source);
            if (unwrapped.empty() || unwrapped.front().empty())
            {
                return;
            }

            const auto [xMin, xMax] = XRange(unwrapped.front());
            if (xMin >= -180.0 && xMax <= 180.0)
            {
                Polygon polygon;
                polygon.reserve(source.size());
                for (std::size_t index = 0; index < source.size(); ++index)
                {
                    polygon.emplace_back(
                        source[index].begin(),
                        source[index].end());
                }
                destination.AppendPolygon(polygon);
                return;
            }

            const std::unique_ptr<OGRPolygon> polygon =
                MakeOgrPolygon(unwrapped);
            OGREnvelope envelope;
            polygon->getEnvelope(&envelope);
            const int firstWorld = static_cast<int>(
                std::floor((xMin + 180.0) / 360.0));
            const int lastWorld = static_cast<int>(
                std::floor((xMax + 180.0) / 360.0));
            const std::size_t initialSize = destination.size();
            for (int world = firstWorld; world <= lastWorld; ++world)
            {
                const double shift = world * 360.0;
                const std::unique_ptr<OGRPolygon> window =
                    MakeLongitudeWindow(
                        shift - 180.0,
                        shift + 180.0,
                        envelope.MinY,
                        envelope.MaxY);
                OGRGeometryUniquePtr clipped(polygon->Intersection(window.get()));
                if (clipped == nullptr)
                {
                    throw CoreError("Cannot split polygon at the longitude seam");
                }
                AppendPolygonalGeometry(destination, *clipped, shift);
            }
            if (destination.size() == initialSize)
            {
                throw CoreError("Longitude-seam clipping removed the polygon");
            }
        }

        void TransformScene(
            VectorRenderScene& scene,
            const ProjectionTransform& forward,
            const bool prepareFillMeshes,
            const bool splitLongitudeSeam)
        {
            TransformPoints(scene.polygons.Coordinates(), forward);
            if (splitLongitudeSeam)
            {
                PolygonBuffer split;
                for (std::size_t index = 0; index < scene.polygons.size(); ++index)
                {
                    SplitPolygonAtLongitudeSeam(split, scene.polygons[index]);
                }
                scene.polygons = std::move(split);
            }
            if (!prepareFillMeshes)
            {
                scene.fillMeshes.clear();
                return;
            }

            scene.fillMeshes.clear();
            scene.fillMeshes.reserve(scene.polygons.size());
            for (std::size_t index = 0; index < scene.polygons.size(); ++index)
            {
                std::optional<TriangleMesh> mesh;
                try
                {
                    mesh = TriangulatePolygon(scene.polygons[index]);
                }
                catch (const CoreError&)
                {
                }
                scene.fillMeshes.push_back(std::move(mesh));
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
                    sourceOptions.prepareFillMeshes = false;
                    const ProjectionTransform forward =
                        source.TransformTo(*display);
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

                        bool canFilterConservatively = false;
                        const Extent sourceExtent = layer.Source().GetExtent();
                        Extent displayExtent;
                        if (sourceExtent.IsValid() &&
                            forward.TransformBounds(sourceExtent, displayExtent) &&
                            displayExtent.IsValid())
                        {
                            const bool coversLongitudeRange =
                                display->IsGeographic() &&
                                displayExtent.Width() >= 360.0 - 1e-9;
                            if (!displayExtent.Intersects(*options.spatialFilter))
                            {
                                canFilterConservatively = true;
                            }
                            else
                            {
                                canFilterConservatively =
                                    !coversLongitudeRange &&
                                    sourceFilter.Intersects(sourceExtent);
                            }
                        }
                        sourceOptions.spatialFilter =
                            canFilterConservatively
                                ? std::optional<Extent>{sourceFilter}
                                : std::nullopt;
                    }

                    scene = detail::BuildVectorRenderScene(
                        layer.Source(), sourceOptions);
                    TransformScene(
                        scene,
                        forward,
                        options.prepareFillMeshes,
                        source.IsProjected() && display->IsGeographic());
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
