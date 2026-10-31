// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderScene.cpp
 * @brief Builds owned point, line, and polygon scene data from vector layers.
 *
 * Reads vector geometries sequentially and copies supported geometry
 * coordinates into owned scene data before releasing each feature.
 */
#include "mapwingis/rendering/VectorRenderScene.h"

#include <memory>
#include <string>
#include <utility>

#include <ogrsf_frmts.h>

#include "mapwingis/core/Error.h"
#include "mapwingis/vector/VectorDataset.h"

#include "internal/CoordinateTransformation.h"
#include "internal/VectorLayerAccess.h"
#include "internal/VectorRenderSceneBuilder.h"

namespace mwg::rendering
{
    namespace
    {
        void AppendRing(const OGRLinearRing* source, Polygon& polygon)
        {
            if (source == nullptr || source->getNumPoints() < 3)
            {
                return;
            }

            Ring ring;
            const int count = source->getNumPoints();
            ring.reserve(static_cast<std::size_t>(count));
            for (int index = 0; index < count; ++index)
            {
                ring.push_back({source->getX(index), source->getY(index)});
            }
            polygon.push_back(std::move(ring));
        }

        void AppendPolygon(const OGRPolygon& source, VectorRenderScene& scene)
        {
            Polygon polygon;
            AppendRing(source.getExteriorRing(), polygon);
            for (int index = 0; index < source.getNumInteriorRings(); ++index)
            {
                AppendRing(source.getInteriorRing(index), polygon);
            }

            if (!polygon.empty())
            {
                scene.polygons.push_back(std::move(polygon));
            }
        }

        void AppendGeometry(const OGRGeometry& geometry, VectorRenderScene& scene)
        {
            switch (wkbFlatten(geometry.getGeometryType()))
            {
            case wkbPoint:
            {
                const auto& point = static_cast<const OGRPoint&>(geometry);
                if (!point.IsEmpty())
                {
                    scene.points.push_back({point.getX(), point.getY()});
                }
                return;
            }

            case wkbMultiPoint:
            case wkbMultiLineString:
            case wkbMultiCurve:
            case wkbGeometryCollection:
            {
                const auto& collection = static_cast<const OGRGeometryCollection&>(geometry);
                for (int index = 0; index < collection.getNumGeometries(); ++index)
                {
                    const OGRGeometry* child = collection.getGeometryRef(index);
                    if (child != nullptr)
                    {
                        AppendGeometry(*child, scene);
                    }
                }
                return;
            }

            case wkbLineString:
            case wkbCircularString:
            case wkbCompoundCurve:
            {
                const std::unique_ptr<OGRGeometry> linear(geometry.getLinearGeometry());
                if (linear == nullptr)
                {
                    throw CoreError("Cannot linearize curved vector geometry");
                }
                const auto& line = static_cast<const OGRLineString&>(*linear);
                if (line.getNumPoints() >= 2)
                {
                    Ring points;
                    points.reserve(static_cast<std::size_t>(line.getNumPoints()));
                    for (int index = 0; index < line.getNumPoints(); ++index)
                    {
                        points.push_back({line.getX(index), line.getY(index)});
                    }
                    scene.lines.push_back(std::move(points));
                }
                return;
            }

            case wkbPolygon:
                AppendPolygon(static_cast<const OGRPolygon&>(geometry), scene);
                return;

            case wkbMultiPolygon:
            {
                const auto& multiPolygon = static_cast<const OGRMultiPolygon&>(geometry);
                for (int index = 0; index < multiPolygon.getNumGeometries(); ++index)
                {
                    const OGRGeometry* polygon = multiPolygon.getGeometryRef(index);
                    if (polygon != nullptr)
                    {
                        AppendPolygon(static_cast<const OGRPolygon&>(*polygon), scene);
                    }
                }
                return;
            }

            case wkbCurvePolygon:
            {
                const std::unique_ptr<OGRGeometry> linear(geometry.getLinearGeometry());
                if (linear == nullptr)
                {
                    throw CoreError("Cannot linearize curved vector geometry");
                }
                AppendGeometry(*linear, scene);
                return;
            }

            case wkbMultiSurface:
            {
                const auto& surfaces = static_cast<const OGRGeometryCollection&>(geometry);
                for (int index = 0; index < surfaces.getNumGeometries(); ++index)
                {
                    const OGRGeometry* surface = surfaces.getGeometryRef(index);
                    if (surface != nullptr)
                    {
                        AppendGeometry(*surface, scene);
                    }
                }
                return;
            }

            default:
                throw CoreError("Vector rendering does not support this geometry type");
            }
        }
    }

    VectorRenderScene detail::BuildVectorRenderScene(
        const VectorLayer& layer,
        const std::string& mapProjectionWkt)
    {
        const ShapeType shapeType = BaseShapeType(layer.Type());
        if (shapeType != ShapeType::Point && shapeType != ShapeType::MultiPoint &&
            shapeType != ShapeType::PolyLine && shapeType != ShapeType::Polygon)
        {
            throw CoreError("Vector rendering does not support this layer geometry type");
        }

        VectorRenderScene scene;
        scene.extent = layer.GetExtent();
        scene.sourceFeatureCount = layer.FeatureCount();
        const std::string sourceWkt = layer.ProjectionWkt();
        const std::string& targetWkt =
            mapProjectionWkt.empty() ? sourceWkt : mapProjectionWkt;
        const mwg::detail::CoordinateTransformation transform(sourceWkt, targetWkt);
        mwg::detail::VectorLayerAccess::ForEachGeometry(
            layer,
            [&scene, &transform](const OGRGeometry& geometry)
            {
                if (!transform.IsNeeded())
                {
                    AppendGeometry(geometry, scene);
                    return;
                }
                std::unique_ptr<OGRGeometry> transformed(geometry.clone());
                if (transformed == nullptr ||
                    transformed->transform(transform.Get()) != OGRERR_NONE)
                {
                    throw CoreError("Cannot transform vector geometry into the map coordinate system");
                }
                AppendGeometry(*transformed, scene);
            });
        return scene;
    }

    VectorRenderScene CreateVectorRenderScene(const VectorLayer& layer)
    {
        VectorRenderScene scene = detail::BuildVectorRenderScene(layer);
        if (scene.points.empty() && scene.lines.empty() && scene.polygons.empty())
        {
            throw CoreError("Vector layer contains no geometry to render");
        }
        return scene;
    }

    VectorRenderScene CreateVectorRenderScene(const VectorDataset& dataset)
    {
        if (dataset.LayerCount() == 0)
        {
            throw CoreError("Vector dataset contains no layers");
        }

        return CreateVectorRenderScene(dataset.Layer(0));
    }
}
