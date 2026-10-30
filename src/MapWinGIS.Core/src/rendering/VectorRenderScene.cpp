// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderScene.cpp
 * @brief Builds owned polygon scene data from vector layers.
 *
 * Reads vector geometries sequentially and copies supported polygon
 * coordinates into owned scene data before releasing each feature.
 */
#include "mapwingis/rendering/VectorRenderScene.h"

#include <memory>
#include <string>
#include <utility>

#include <ogrsf_frmts.h>

#include "mapwingis/core/Error.h"
#include "mapwingis/vector/VectorDataset.h"

#include "internal/VectorLayerAccess.h"

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

            case wkbCurvePolygon:
            case wkbMultiSurface:
            {
                const std::unique_ptr<OGRGeometry> linear(geometry.getLinearGeometry());
                if (linear == nullptr)
                {
                    throw CoreError("Cannot linearize curved vector geometry");
                }
                AppendGeometry(*linear, scene);
                return;
            }

            default:
                throw CoreError("Vector rendering supports Polygon and MultiPolygon geometries only");
            }
        }
    }

    VectorRenderScene CreateVectorRenderScene(const VectorLayer& layer)
    {
        if (BaseShapeType(layer.Type()) != ShapeType::Polygon)
        {
            throw CoreError("Vector rendering supports polygon layers only");
        }

        VectorRenderScene scene;
        scene.extent = layer.GetExtent();
        detail::VectorLayerAccess::ForEachGeometry(
            layer,
            [&scene](const OGRGeometry& geometry)
            {
                AppendGeometry(geometry, scene);
            });

        if (scene.polygons.empty())
        {
            throw CoreError("Vector layer contains no polygon geometry to render");
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
