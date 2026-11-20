// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorRenderScene.cpp
 * @brief Builds owned polygon source geometry and optional fill meshes.
 *
 * Reads vector geometries sequentially and copies supported polygon
 * coordinates into owned scene data before releasing each feature.
 * Triangulation is a separate, opt-in preparation step after source
 * geometry conversion. Unsupported or degenerate parts are skipped and
 * counted rather than failing the layer.
 */
#include "mapwingis/rendering/VectorRenderScene.h"

#include <memory>
#include <string>
#include <utility>

#include <ogrsf_frmts.h>

#include "mapwingis/core/Error.h"
#include "mapwingis/diagnostics/PerformanceDiagnostics.h"
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
                if (geometry.IsEmpty())
                {
                    return;
                }
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

                case wkbGeometryCollection:
                {
                    const auto& collection = static_cast<const OGRGeometryCollection&>(geometry);
                    for (int index = 0; index < collection.getNumGeometries(); ++index)
                    {
                        if (const OGRGeometry* member = collection.getGeometryRef(index))
                        {
                            AppendGeometry(*member);
                        }
                    }
                    return;
                }

                default:
                    // A polygon layer can still contain stray points or lines; drop them
                    // rather than failing the whole layer.
                    ++_scene.skippedGeometryCount;
                    return;
                }
            }

        private:
            [[nodiscard]] static bool IsUsableRing(const OGRLinearRing* ring)
            {
                return ring != nullptr && ring->getNumPoints() >= 3;
            }

            void AppendRing(const OGRLinearRing& source)
            {
                for (int index = 0; index < source.getNumPoints(); ++index)
                {
                    _coordinates.push_back({source.getX(index), source.getY(index)});
                }
                _ringEnds.push_back(_coordinates.size());
            }

            void AppendPolygon(const OGRPolygon& source)
            {
                if (source.IsEmpty())
                {
                    return;
                }
                // Without a usable exterior ring, a hole would otherwise become ring 0 and
                // be filled as if it were the polygon's exterior.
                const OGRLinearRing* exterior = source.getExteriorRing();
                if (!IsUsableRing(exterior))
                {
                    ++_scene.skippedGeometryCount;
                    return;
                }
                _coordinates.clear();
                _ringEnds.clear();
                AppendRing(*exterior);
                for (int index = 0; index < source.getNumInteriorRings(); ++index)
                {
                    if (const OGRLinearRing* ring = source.getInteriorRing(index); IsUsableRing(ring))
                    {
                        AppendRing(*ring);
                    }
                }
                _scene.polygons.AppendPolygon(_coordinates, _ringEnds);
            }

            VectorRenderScene& _scene;
            std::vector<Point> _coordinates;
            std::vector<std::size_t> _ringEnds;
        };
    }

    VectorRenderScene detail::BuildVectorRenderScene(
        const VectorLayer& layer,
        const VectorRenderSceneOptions& options)
    {
        const diagnostics::PerformanceScope scope(
            diagnostics::PerformanceOperation::SceneBuild);
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
                const diagnostics::PerformanceScope conversionScope(
                    diagnostics::PerformanceOperation::GeometryConversion);
                builder.AppendGeometry(geometry);
            },
            hasFastExtent ? nullptr : &scene.extent);

        if (!options.prepareFillMeshes)
        {
            return scene;
        }

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
        return CreateVectorRenderScene(layer, {});
    }

    VectorRenderScene CreateVectorRenderScene(
        const VectorLayer& layer,
        const VectorRenderSceneOptions& options)
    {
        VectorRenderScene scene = detail::BuildVectorRenderScene(layer, options);
        if (scene.polygons.empty())
        {
            throw CoreError("Vector layer contains no polygon geometry to render");
        }
        return scene;
    }

    VectorRenderScene CreateVectorRenderScene(const VectorDataset& dataset)
    {
        return CreateVectorRenderScene(dataset, {});
    }

    VectorRenderScene CreateVectorRenderScene(
        const VectorDataset& dataset,
        const VectorRenderSceneOptions& options)
    {
        if (dataset.LayerCount() == 0)
        {
            throw CoreError("Vector dataset contains no layers");
        }

        return CreateVectorRenderScene(dataset.Layer(0), options);
    }
}
