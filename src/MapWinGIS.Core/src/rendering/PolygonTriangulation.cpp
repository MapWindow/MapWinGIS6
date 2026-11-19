// SPDX-License-Identifier: MPL-2.0

/**
 * @file PolygonTriangulation.cpp
 * @brief Implements polygon triangulation through OGR and GEOS.
 *
 * Builds a temporary OGR polygon from the backend-neutral rings, validates
 * it, and converts the constrained Delaunay triangulation into an indexed
 * triangle mesh. All OGR geometry is owned through RAII handles.
 */
#include "mapwingis/rendering/PolygonTriangulation.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

#include <ogrsf_frmts.h>

#include "mapwingis/core/Error.h"
#include "mapwingis/diagnostics/PerformanceDiagnostics.h"
#include "mapwingis/gdal/GdalDiagnostics.h"
#include "mapwingis/gdal/GdalEnvironment.h"

namespace mwg::rendering
{
    namespace
    {
        std::unique_ptr<OGRLinearRing> MakeRing(const std::span<const Point> ring)
        {
            auto result = std::make_unique<OGRLinearRing>();
            for (const Point& point : ring)
            {
                result->addPoint(point.x, point.y);
            }
            if (result->getNumPoints() > 1)
            {
                result->closeRings();
            }
            return result;
        }

        void ValidateRing(const std::span<const Point> ring, const char* role)
        {
            if (ring.size() < 3)
            {
                throw InvalidArgument(std::string("Polygon ") + role +
                                      " ring has fewer than three points");
            }
            for (const Point& point : ring)
            {
                if (!std::isfinite(point.x) || !std::isfinite(point.y))
                {
                    throw InvalidArgument(std::string("Polygon ") + role +
                                          " ring contains a non-finite coordinate");
                }
            }
        }

        template <typename PolygonType>
        std::unique_ptr<OGRPolygon> MakeOgrPolygon(const PolygonType& polygon)
        {
            if (polygon.empty())
            {
                throw InvalidArgument("Polygon has no rings");
            }

            ValidateRing(polygon.front(), "exterior");
            auto result = std::make_unique<OGRPolygon>();
            result->addRingDirectly(MakeRing(polygon.front()).release());
            for (std::size_t index = 1; index < polygon.size(); ++index)
            {
                const auto& hole = polygon[index];
                if (hole.size() < 3)
                {
                    continue;
                }
                ValidateRing(hole, "interior");
                result->addRingDirectly(MakeRing(hole).release());
            }
            return result;
        }

        double TwiceSignedArea(const Point& a, const Point& b, const Point& c)
        {
            return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        }

        // Exact coordinate equality: triangulation reuses input vertices bit for bit, and
        // std::hash<double> hashes 0.0 and -0.0 alike, consistent with operator==.
        struct PointHash
        {
            [[nodiscard]] std::size_t operator()(const Point& point) const noexcept
            {
                const std::size_t x = std::hash<double>{}(point.x);
                const std::size_t y = std::hash<double>{}(point.y);
                constexpr std::size_t mixingConstant =
                    static_cast<std::size_t>(0x9e3779b97f4a7c15ULL);
                return x ^ (y + mixingConstant + (x << 6) + (x >> 2));
            }
        };

        struct PointEqual
        {
            [[nodiscard]] bool operator()(const Point& left, const Point& right) const noexcept
            {
                return left.x == right.x && left.y == right.y;
            }
        };

        /// Deduplicates vertices and assembles consistently wound triangles.
        class MeshBuilder
        {
        public:
            void AddTriangle(Point a, Point b, Point c)
            {
                if (TwiceSignedArea(a, b, c) < 0.0)
                {
                    std::swap(b, c);
                }
                _mesh.indices.push_back(AddVertex(a));
                _mesh.indices.push_back(AddVertex(b));
                _mesh.indices.push_back(AddVertex(c));
            }

            [[nodiscard]] TriangleMesh TakeResult()
            {
                TriangleMesh result;
                result.vertices = std::move(_mesh.vertices);
                result.indices = std::move(_mesh.indices);
                return result;
            }

        private:
            [[nodiscard]] std::uint32_t AddVertex(const Point& point)
            {
                const auto found = _vertexLookup.find(point);
                if (found != _vertexLookup.end())
                {
                    return found->second;
                }
                if (_mesh.vertices.size() >=
                    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
                {
                    throw CoreError(
                        "Polygon triangulation produced more distinct vertices than the "
                        "mesh index type supports");
                }
                const auto index = static_cast<std::uint32_t>(_mesh.vertices.size());
                _mesh.vertices.push_back(point);
                _vertexLookup.emplace(point, index);
                return index;
            }

            TriangleMesh _mesh;
            std::unordered_map<Point, std::uint32_t, PointHash, PointEqual> _vertexLookup;
        };

        void AppendTriangle(const OGRPolygon& triangle, MeshBuilder& builder)
        {
            const OGRLinearRing* ring = triangle.getExteriorRing();
            if (ring == nullptr || ring->getNumPoints() < 3)
            {
                return;
            }
            const Point a{ring->getX(0), ring->getY(0)};
            const Point b{ring->getX(1), ring->getY(1)};
            const Point c{ring->getX(2), ring->getY(2)};
            if (TwiceSignedArea(a, b, c) == 0.0)
            {
                return;
            }
            builder.AddTriangle(a, b, c);
        }

        void AppendTriangles(const OGRGeometry& geometry, MeshBuilder& builder)
        {
            switch (wkbFlatten(geometry.getGeometryType()))
            {
            case wkbPolygon:
                AppendTriangle(static_cast<const OGRPolygon&>(geometry), builder);
                return;

            case wkbMultiPolygon:
            case wkbGeometryCollection:
            {
                const auto& collection = static_cast<const OGRGeometryCollection&>(geometry);
                for (int index = 0; index < collection.getNumGeometries(); ++index)
                {
                    const OGRGeometry* child = collection.getGeometryRef(index);
                    if (child != nullptr)
                    {
                        AppendTriangles(*child, builder);
                    }
                }
                return;
            }

            default:
                throw CoreError("Triangulation returned an unexpected geometry type");
            }
        }

        class TriangulationFailureScope final
        {
        public:
            void MarkSucceeded() noexcept
            {
                _succeeded = true;
            }

            ~TriangulationFailureScope()
            {
                if (!_succeeded)
                {
                    diagnostics::PerformanceDiagnostics::IncrementCounter(
                        diagnostics::PerformanceCounter::TriangulationFailures);
                }
            }

        private:
            bool _succeeded = false;
        };
    }

    template <typename PolygonType>
    TriangleMesh Triangulate(const PolygonType& polygon)
    {
        TriangulationFailureScope failureScope;
        GdalEnvironment::Initialize();
        const std::unique_ptr<OGRPolygon> source = MakeOgrPolygon(polygon);

        GdalErrorScope scope;
        bool valid = false;
        {
            const diagnostics::PerformanceScope validationScope(
                diagnostics::PerformanceOperation::GeometryValidation);
            valid = source->IsValid();
        }
        scope.RethrowCaptureException();
        if (!valid)
        {
            diagnostics::PerformanceDiagnostics::IncrementCounter(
                diagnostics::PerformanceCounter::InvalidGeometries);
            throw CoreError(
                "Polygon triangulation requires a valid polygon; the input is "
                "self-intersecting or otherwise invalid");
        }

        const diagnostics::PerformanceScope triangulationScope(
            diagnostics::PerformanceOperation::PolygonTriangulation);
        std::unique_ptr<OGRGeometry> triangulation;
        try
        {
            triangulation.reset(source->ConstrainedDelaunayTriangulation());
        }
        catch (const std::bad_alloc&)
        {
            throw;
        }
        catch (const std::exception& error)
        {
            throw CoreError(std::string("Polygon triangulation failed: ") + error.what());
        }
        scope.RethrowCaptureException();
        if (triangulation == nullptr)
        {
            std::string message = "Polygon triangulation failed";
            const std::string detail = scope.DetailedErrorMessage();
            if (!detail.empty())
            {
                message += ": " + detail;
            }
            throw CoreError(message);
        }

        MeshBuilder builder;
        AppendTriangles(*triangulation, builder);
        TriangleMesh mesh = builder.TakeResult();
        if (mesh.indices.empty())
        {
            throw CoreError("Polygon triangulation produced no triangles");
        }
        diagnostics::PerformanceDiagnostics::IncrementCounter(
            diagnostics::PerformanceCounter::PolygonsTriangulated);
        diagnostics::PerformanceDiagnostics::IncrementCounter(
            diagnostics::PerformanceCounter::TrianglesGenerated,
            static_cast<std::int64_t>(mesh.indices.size() / 3U));
        failureScope.MarkSucceeded();
        return mesh;
    }

    TriangleMesh TriangulatePolygon(const Polygon& polygon)
    {
        return Triangulate(polygon);
    }

    TriangleMesh TriangulatePolygon(const PolygonView polygon)
    {
        return Triangulate(polygon);
    }
}
