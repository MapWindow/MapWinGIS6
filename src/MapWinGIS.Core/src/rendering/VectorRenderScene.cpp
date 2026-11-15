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
#include "internal/VectorRenderSceneBuilder.h"
#include "mapwingis/rendering/PolygonTriangulation.h"

namespace mwg::rendering
{
    std::span<const Point> PolygonView::operator[](const std::size_t index) const
    {
        if (index >= _rings.size())
        {
            throw InvalidArgument("Polygon ring index out of range");
        }
        const GeometryRange range = _rings[index];
        return _coordinates.subspan(range.offset, range.count);
    }

    PolygonView PolygonBuffer::operator[](const std::size_t index) const
    {
        if (index >= _polygons.size())
        {
            throw InvalidArgument("Polygon index out of range");
        }
        const GeometryRange range = _polygons[index];
        return PolygonView(_coordinates,
            std::span<const GeometryRange>(_rings).subspan(range.offset, range.count));
    }

    PolygonBuffer::PolygonBuffer(const std::initializer_list<Polygon> polygons)
    {
        for (const Polygon& polygon : polygons)
        {
            AppendPolygon(polygon);
        }
    }

    PolygonBuffer& PolygonBuffer::operator=(PolygonBuffer other) noexcept
    {
        _coordinates.swap(other._coordinates);
        _rings.swap(other._rings);
        _polygons.swap(other._polygons);
        return *this;
    }

    void PolygonBuffer::AppendPolygon(const Polygon& polygon)
    {
        std::vector<Point> coordinates;
        std::vector<std::size_t> ringEnds;
        for (const Ring& ring : polygon)
        {
            coordinates.insert(coordinates.end(), ring.begin(), ring.end());
            ringEnds.push_back(coordinates.size());
        }
        AppendPolygon(coordinates, ringEnds);
    }

    void PolygonBuffer::AppendPolygon(const std::span<const Point> coordinates,
                                     const std::span<const std::size_t> ringEnds)
    {
        std::size_t previous = 0;
        for (const std::size_t end : ringEnds)
        {
            if (end < previous || end > coordinates.size())
            {
                throw InvalidArgument("Polygon ring offsets must partition the coordinates");
            }
            previous = end;
        }
        if (previous != coordinates.size())
        {
            throw InvalidArgument("Polygon ring offsets must cover all coordinates");
        }

        const std::size_t pointCount = _coordinates.size();
        const std::size_t ringCount = _rings.size();
        const std::size_t polygonCount = _polygons.size();
        try
        {
            // Insert coordinates first: its size check also prevents overflow
            // when the existing coordinate count is added to a ring offset.
            _coordinates.insert(_coordinates.end(), coordinates.begin(), coordinates.end());
            previous = 0;
            for (const std::size_t end : ringEnds)
            {
                _rings.push_back({pointCount + previous, end - previous});
                previous = end;
            }
            _polygons.push_back({ringCount, ringEnds.size()});
        }
        catch (...)
        {
            _coordinates.resize(pointCount);
            _rings.resize(ringCount);
            _polygons.resize(polygonCount);
            throw;
        }
    }

    namespace
    {
        class GeometryBuilder
        {
        public:
            explicit GeometryBuilder(VectorRenderScene& scene) : _scene(scene) {}

            void AppendGeometry(const OGRGeometry& geometry)
            {
                switch (wkbFlatten(geometry.getGeometryType()))
                {
                case wkbPolygon:
                    AppendPolygon(static_cast<const OGRPolygon&>(geometry));
                    return;

                case wkbMultiPolygon:
                {
                    const auto& multiPolygon = static_cast<const OGRMultiPolygon&>(geometry);
                    for (int index = 0; index < multiPolygon.getNumGeometries(); ++index)
                    {
                        const OGRPolygon* polygon = multiPolygon.getGeometryRef(index);
                        if (polygon != nullptr)
                        {
                            AppendPolygon(*polygon);
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
                    AppendGeometry(*linear);
                    return;
                }

                default:
                    throw CoreError("Vector rendering supports Polygon and MultiPolygon geometries only");
                }
            }

        private:
            void AppendRing(const OGRLinearRing* source)
            {
                if (source == nullptr || source->getNumPoints() < 3)
                {
                    return;
                }
                for (int index = 0; index < source->getNumPoints(); ++index)
                {
                    _coordinates.push_back({source->getX(index), source->getY(index)});
                }
                _ringEnds.push_back(_coordinates.size());
            }

            void AppendPolygon(const OGRPolygon& source)
            {
                _coordinates.clear();
                _ringEnds.clear();
                AppendRing(source.getExteriorRing());
                for (int index = 0; index < source.getNumInteriorRings(); ++index)
                {
                    AppendRing(source.getInteriorRing(index));
                }
                if (!_ringEnds.empty())
                {
                    _scene.polygons.AppendPolygon(_coordinates, _ringEnds);
                }
            }

            VectorRenderScene& _scene;
            std::vector<Point> _coordinates;
            std::vector<std::size_t> _ringEnds;
        };
    }

    VectorRenderScene detail::BuildVectorRenderScene(const VectorLayer& layer)
    {
        if (BaseShapeType(layer.Type()) != ShapeType::Polygon)
        {
            throw CoreError("Vector rendering supports polygon layers only");
        }

        VectorRenderScene scene;
        const bool hasFastExtent = mwg::detail::VectorLayerAccess::HasFastExtent(layer);
        if (hasFastExtent)
        {
            scene.extent = layer.GetExtent();
        }
        GeometryBuilder builder(scene);
        scene.sourceFeatureCount = mwg::detail::VectorLayerAccess::ForEachGeometry(
            layer,
            [&builder](const OGRGeometry& geometry)
            {
                builder.AppendGeometry(geometry);
            },
            hasFastExtent ? nullptr : &scene.extent);

        // Prepare one fill mesh per polygon while the scene is created, so
        // renderers never triangulate per frame. A polygon that cannot be
        // triangulated keeps a null mesh; its rings above remain available
        // for outlines and ring-based backends such as GDI.
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
        return scene;
    }

    VectorRenderScene CreateVectorRenderScene(const VectorLayer& layer)
    {
        VectorRenderScene scene = detail::BuildVectorRenderScene(layer);
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
