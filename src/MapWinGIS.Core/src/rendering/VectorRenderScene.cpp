// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderScene.cpp
 * @brief Builds owned polygon scene data from vector layers.
 *
 * Extracts Polygon and MultiPolygon coordinates through the existing vector
 * layer API and releases all feature-owned geometry before returning.
 */
#include "mapwingis/rendering/VectorRenderScene.h"

#include <memory>
#include <string>
#include <utility>

#include <ogrsf_frmts.h>

#include "mapwingis/core/Error.h"
#include "mapwingis/vector/VectorDataset.h"

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

            default:
                throw CoreError("Vector rendering supports Polygon and MultiPolygon geometries only");
            }
        }
    }

    VectorRenderScene CreateVectorRenderScene(const VectorLayer& layer)
    {
        if (layer.Type() != ShapeType::Polygon)
        {
            throw CoreError("Vector rendering supports polygon layers only");
        }

        VectorRenderScene scene;
        scene.extent = layer.GetExtent();
        const std::int64_t featureCount = layer.FeatureCount();
        for (std::int64_t index = 0; index < featureCount; ++index)
        {
            const std::string geometryWkt = layer.GeometryWkt(index);
            if (geometryWkt.empty())
            {
                continue;
            }

            const char* text = geometryWkt.c_str();
            OGRGeometry* parsed = nullptr;
            if (OGRGeometryFactory::createFromWkt(&text, nullptr, &parsed) != OGRERR_NONE ||
                parsed == nullptr)
            {
                throw CoreError("Cannot parse vector feature geometry at index " +
                                std::to_string(index));
            }

            const std::unique_ptr<OGRGeometry> geometry(parsed);
            if (!geometry->IsEmpty())
            {
                AppendGeometry(*geometry, scene);
            }
        }

        if (scene.polygons.empty())
        {
            throw CoreError("Vector layer contains no polygon geometry to render");
        }
        return scene;
    }
}
