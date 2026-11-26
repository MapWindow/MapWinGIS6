// SPDX-License-Identifier: MPL-2.0

/**
 * @file PolygonTriangulation.cpp
 * @brief Implements fast polygon triangulation with a GEOS fallback.
 *
 * Converts backend-neutral ring coordinates directly with earcut. When that
 * path cannot produce a mesh, it builds a temporary OGR polygon and converts
 * its constrained Delaunay triangulation into an indexed triangle mesh.
 */
#include "mapwingis/rendering/PolygonTriangulation.h"

#include <algorithm>
#include <array>
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
#include <vector>

#include <mapbox/earcut.hpp>
#include <geos_c.h>
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

        class ExteriorCoverage final
        {
        public:
            explicit ExteriorCoverage(const std::span<const Point> exterior)
                : _context(OGRGeometry::createGEOSContext())
            {
                if (_context == nullptr)
                {
                    throw CoreError("Cannot create a GEOS context for ring validation");
                }
                auto polygon = std::make_unique<OGRPolygon>();
                polygon->addRingDirectly(MakeRing(exterior).release());
                _geometry =
                    polygon->exportToGEOS(_context.get(), true);
                if (_geometry == nullptr)
                {
                    throw CoreError("Cannot convert the exterior ring for validation");
                }
            }

            ExteriorCoverage(const ExteriorCoverage&) = delete;
            ExteriorCoverage& operator=(const ExteriorCoverage&) = delete;

            ~ExteriorCoverage()
            {
                for (const std::unique_ptr<Hole>& hole : _holes)
                {
                    GEOSGeom_destroy_r(_context.get(), hole->geometry);
                }
                if (_geometry != nullptr)
                {
                    GEOSGeom_destroy_r(_context.get(), _geometry);
                }
            }

            [[nodiscard]] bool AddHole(const std::span<const Point> ring)
            {
                auto polygon = std::make_unique<OGRPolygon>();
                polygon->addRingDirectly(MakeRing(ring).release());
                GEOSGeom geometry =
                    polygon->exportToGEOS(_context.get(), true);
                if (geometry == nullptr)
                {
                    throw CoreError(
                        "Cannot convert an interior ring for validation");
                }
                const char covered =
                    GEOSCovers_r(_context.get(), _geometry, geometry);
                if (covered == 2)
                {
                    GEOSGeom_destroy_r(_context.get(), geometry);
                    throw CoreError("Cannot validate an interior ring");
                }
                if (covered == 0)
                {
                    GEOSGeom_destroy_r(_context.get(), geometry);
                    return false;
                }
                try
                {
                    auto hole = std::make_unique<Hole>();
                    hole->geometry = geometry;
                    hole->index = _holes.size();
                    _holes.push_back(std::move(hole));
                }
                catch (...)
                {
                    GEOSGeom_destroy_r(_context.get(), geometry);
                    throw;
                }
                return true;
            }

            [[nodiscard]] bool ValidateHoles()
            {
                if (_holes.size() < 2)
                {
                    return true;
                }

                GEOSSTRtree* const tree =
                    GEOSSTRtree_create_r(_context.get(), 10);
                if (tree == nullptr)
                {
                    throw CoreError(
                        "Cannot create a spatial index for interior ring validation");
                }
                try
                {
                    for (const std::unique_ptr<Hole>& hole : _holes)
                    {
                        GEOSSTRtree_insert_r(
                            _context.get(), tree, hole->geometry, hole.get());
                    }
                    if (GEOSSTRtree_build_r(_context.get(), tree) == 0)
                    {
                        throw CoreError(
                            "Cannot build the spatial index for interior ring validation");
                    }

                    for (const std::unique_ptr<Hole>& hole : _holes)
                    {
                        HoleQuery query{_context.get(), hole.get()};
                        GEOSSTRtree_query_r(
                            _context.get(),
                            tree,
                            hole->geometry,
                            &QueryHole,
                            &query);
                        if (query.validationFailed)
                        {
                            throw CoreError(
                                "Cannot compare interior rings for validation");
                        }
                        if (query.overlaps)
                        {
                            GEOSSTRtree_destroy_r(_context.get(), tree);
                            return false;
                        }
                    }
                }
                catch (...)
                {
                    GEOSSTRtree_destroy_r(_context.get(), tree);
                    throw;
                }
                GEOSSTRtree_destroy_r(_context.get(), tree);
                return true;
            }

        private:
            struct Hole final
            {
                GEOSGeom geometry = nullptr;
                std::size_t index = 0;
            };

            struct HoleQuery final
            {
                GEOSContextHandle_t context = nullptr;
                const Hole* source = nullptr;
                bool overlaps = false;
                bool validationFailed = false;
            };

            struct ContextDeleter final
            {
                void operator()(GEOSContextHandle_t context) const noexcept
                {
                    OGRGeometry::freeGEOSContext(context);
                }
            };

            static void QueryHole(void* item, void* data)
            {
                auto& query = *static_cast<HoleQuery*>(data);
                const Hole& candidate = *static_cast<Hole*>(item);
                if (query.overlaps || query.validationFailed ||
                    candidate.index <= query.source->index)
                {
                    return;
                }

                const char overlaps = GEOSRelatePattern_r(
                    query.context,
                    query.source->geometry,
                    candidate.geometry,
                    "T********");
                if (overlaps == 2)
                {
                    query.validationFailed = true;
                }
                else if (overlaps == 1)
                {
                    query.overlaps = true;
                }
            }

            std::unique_ptr<
                std::remove_pointer_t<GEOSContextHandle_t>,
                ContextDeleter> _context;
            GEOSGeom _geometry = nullptr;
            std::vector<std::unique_ptr<Hole>> _holes;
        };

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

        template <typename PolygonType>
        std::vector<std::vector<std::array<double, 2>>> MakeEarcutPolygon(
            const PolygonType& polygon,
            std::vector<Point>& flattenedPoints)
        {
            if (polygon.empty())
            {
                throw InvalidArgument("Polygon has no rings");
            }

            ValidateRing(polygon.front(), "exterior");
            if (polygon.size() > 1)
            {
                GdalEnvironment::Initialize();
            }
            const std::unique_ptr<ExteriorCoverage> exterior =
                polygon.size() > 1
                    ? std::make_unique<ExteriorCoverage>(polygon.front())
                    : nullptr;
            std::vector<std::vector<std::array<double, 2>>> result;
            result.reserve(polygon.size());
            for (std::size_t ringIndex = 0; ringIndex < polygon.size(); ++ringIndex)
            {
                const auto& ring = polygon[ringIndex];
                if (ringIndex != 0 && ring.size() < 3)
                {
                    continue;
                }
                if (ringIndex != 0)
                {
                    ValidateRing(ring, "interior");
                }

                std::vector<std::array<double, 2>> converted;
                std::vector<Point> convertedPoints;
                converted.reserve(ring.size());
                convertedPoints.reserve(ring.size());
                for (const Point& point : ring)
                {
                    if (!convertedPoints.empty() &&
                        convertedPoints.back().x == point.x &&
                        convertedPoints.back().y == point.y)
                    {
                        continue;
                    }
                    converted.push_back({point.x, point.y});
                    convertedPoints.push_back(point);
                }
                if (converted.size() > 1 &&
                    converted.front() == converted.back())
                {
                    converted.pop_back();
                    convertedPoints.pop_back();
                }
                if (converted.size() < 3)
                {
                    if (ringIndex == 0)
                    {
                        throw InvalidArgument(
                            "Polygon exterior ring has fewer than three distinct points");
                    }
                    continue;
                }
                if (ringIndex != 0 && !exterior->AddHole(convertedPoints))
                {
                    throw InvalidArgument(
                        "Polygon interior ring lies outside the exterior ring");
                }
                flattenedPoints.insert(
                    flattenedPoints.end(),
                    convertedPoints.begin(),
                    convertedPoints.end());
                result.push_back(std::move(converted));
            }
            if (exterior != nullptr && !exterior->ValidateHoles())
            {
                throw InvalidArgument(
                    "Polygon interior rings overlap");
            }
            return result;
        }

        template <typename PolygonType>
        [[nodiscard]] double ExpectedFillArea(const PolygonType& polygon)
        {
            double total = 0.0;
            for (std::size_t ringIndex = 0; ringIndex < polygon.size(); ++ringIndex)
            {
                const auto& ring = polygon[ringIndex];
                if (ringIndex != 0 && ring.size() < 3)
                {
                    continue;
                }
                const Point& origin = ring.front();
                double twiceArea = 0.0;
                for (std::size_t pointIndex = 0;
                     pointIndex < ring.size();
                     ++pointIndex)
                {
                    const Point& point = ring[pointIndex];
                    const Point& next = ring[(pointIndex + 1U) % ring.size()];
                    twiceArea +=
                        (point.x - origin.x) * (next.y - origin.y) -
                        (next.x - origin.x) * (point.y - origin.y);
                }
                const double area = std::abs(twiceArea) * 0.5;
                total += ringIndex == 0 ? area : -area;
            }
            return total;
        }

        [[nodiscard]] double MeshArea(const TriangleMesh& mesh)
        {
            double result = 0.0;
            for (std::size_t index = 0; index < mesh.indices.size(); index += 3U)
            {
                const Point& a = mesh.vertices[mesh.indices[index]];
                const Point& b = mesh.vertices[mesh.indices[index + 1U]];
                const Point& c = mesh.vertices[mesh.indices[index + 2U]];
                result += std::abs(TwiceSignedArea(a, b, c)) * 0.5;
            }
            return result;
        }

        template <typename PolygonType>
        [[nodiscard]] bool TryTriangulateWithEarcut(
            const PolygonType& polygon,
            TriangleMesh& mesh)
        {
            std::vector<Point> flattenedPoints;
            flattenedPoints.reserve(polygon.empty() ? 0U : polygon.front().size());
            const auto rings = MakeEarcutPolygon(polygon, flattenedPoints);
            std::vector<std::uint32_t> indices =
                mapbox::earcut<std::uint32_t>(rings);
            if (indices.empty())
            {
                return false;
            }
            if (indices.size() % 3U != 0U)
            {
                return false;
            }

            MeshBuilder builder;
            for (std::size_t index = 0; index < indices.size(); index += 3U)
            {
                const std::uint32_t a = indices[index];
                const std::uint32_t b = indices[index + 1U];
                const std::uint32_t c = indices[index + 2U];
                if (a >= flattenedPoints.size() ||
                    b >= flattenedPoints.size() ||
                    c >= flattenedPoints.size())
                {
                    return false;
                }
                if (TwiceSignedArea(
                        flattenedPoints[a],
                        flattenedPoints[b],
                        flattenedPoints[c]) == 0.0)
                {
                    continue;
                }
                builder.AddTriangle(
                    flattenedPoints[a],
                    flattenedPoints[b],
                    flattenedPoints[c]);
            }
            mesh = builder.TakeResult();
            if (mesh.indices.empty())
            {
                return false;
            }
            const double expectedArea = ExpectedFillArea(polygon);
            const double meshArea = MeshArea(mesh);
            const double tolerance =
                std::abs(expectedArea) * 1e-9;
            return expectedArea > 0.0 &&
                std::abs(meshArea - expectedArea) <= tolerance;
        }

        template <typename PolygonType>
        TriangleMesh TriangulateWithGeos(const PolygonType& polygon)
        {
            GdalEnvironment::Initialize();
            const std::unique_ptr<OGRPolygon> source = MakeOgrPolygon(polygon);
            GdalErrorScope scope;
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
            return mesh;
        }
    }

    template <typename PolygonType>
    TriangleMesh Triangulate(const PolygonType& polygon)
    {
        TriangulationFailureScope failureScope;
        TriangleMesh mesh;
        bool meshed = false;
        {
            const diagnostics::PerformanceScope triangulationScope(
                diagnostics::PerformanceOperation::PolygonTriangulation);
            try
            {
                meshed = TryTriangulateWithEarcut(polygon, mesh);
            }
            catch (const InvalidArgument&)
            {
                throw;
            }
            catch (const std::bad_alloc&)
            {
                throw;
            }
            catch (const std::exception&)
            {
                meshed = false;
            }
            catch (...)
            {
                meshed = false;
            }
            if (!meshed)
            {
                mesh = TriangulateWithGeos(polygon);
            }
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
