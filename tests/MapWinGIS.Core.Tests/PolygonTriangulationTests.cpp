// SPDX-License-Identifier: MPL-2.0

/**
 * @file PolygonTriangulationTests.cpp
 * @brief Tests GEOS-backed polygon triangulation and scene mesh integration.
 *
 * Verifies that convex, concave, holed, and multipart polygons become valid
 * indexed triangle meshes whose triangles cover exactly the polygon area,
 * respect holes, do not overlap, and stay aligned with the scene polygons.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <ogrsf_frmts.h>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/rendering/PolygonTriangulation.h"
#include "mapwingis/rendering/VectorRenderScene.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    using Coordinate = std::pair<double, double>;
    using HoleList = std::initializer_list<std::initializer_list<Coordinate>>;

    rendering::Ring MakeRing(std::initializer_list<Coordinate> points)
    {
        rendering::Ring ring;
        ring.reserve(points.size());
        for (const auto& [x, y] : points)
        {
            ring.push_back({x, y});
        }
        return ring;
    }

    rendering::Polygon MakePolygon(std::initializer_list<Coordinate> exterior)
    {
        return {MakeRing(exterior)};
    }

    rendering::Polygon MakePolygon(std::initializer_list<Coordinate> exterior,
                                   const HoleList& holes)
    {
        rendering::Polygon polygon{MakeRing(exterior)};
        for (const auto& hole : holes)
        {
            polygon.push_back(MakeRing(hole));
        }
        return polygon;
    }

    double TwiceSignedArea(const rendering::Point& a,
                           const rendering::Point& b,
                           const rendering::Point& c)
    {
        return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    }

    double RingArea(const rendering::Ring& ring)
    {
        double twiceArea = 0.0;
        for (std::size_t index = 0; index < ring.size(); ++index)
        {
            const rendering::Point& current = ring[index];
            const rendering::Point& next = ring[(index + 1) % ring.size()];
            twiceArea += current.x * next.y - next.x * current.y;
        }
        return std::abs(twiceArea) * 0.5;
    }

    double PolygonArea(const rendering::Polygon& polygon)
    {
        double area = RingArea(polygon.front());
        for (std::size_t index = 1; index < polygon.size(); ++index)
        {
            area -= RingArea(polygon[index]);
        }
        return area;
    }

    double MeshArea(const rendering::TriangleMesh& mesh)
    {
        double area = 0.0;
        for (std::size_t index = 0; index < mesh.indices.size(); index += 3)
        {
            const rendering::Point& a = mesh.vertices[mesh.indices[index]];
            const rendering::Point& b = mesh.vertices[mesh.indices[index + 1]];
            const rendering::Point& c = mesh.vertices[mesh.indices[index + 2]];
            area += std::abs(TwiceSignedArea(a, b, c)) * 0.5;
        }
        return area;
    }

    std::unique_ptr<OGRPolygon> MakeOgrTriangle(const rendering::Point& a,
                                                const rendering::Point& b,
                                                const rendering::Point& c)
    {
        auto triangle = std::make_unique<OGRPolygon>();
        auto ring = std::make_unique<OGRLinearRing>();
        ring->addPoint(a.x, a.y);
        ring->addPoint(b.x, b.y);
        ring->addPoint(c.x, c.y);
        ring->addPoint(a.x, a.y);
        triangle->addRingDirectly(ring.release());
        return triangle;
    }

    std::unique_ptr<OGRPolygon> MakeOgrPolygon(const rendering::Polygon& polygon)
    {
        auto result = std::make_unique<OGRPolygon>();
        for (const rendering::Ring& ring : polygon)
        {
            auto ogrRing = std::make_unique<OGRLinearRing>();
            for (const rendering::Point& point : ring)
            {
                ogrRing->addPoint(point.x, point.y);
            }
            ogrRing->closeRings();
            result->addRingDirectly(ogrRing.release());
        }
        return result;
    }

    /// Asserts the mesh-level invariants: grouped indices, valid references,
    /// unique vertices, non-degenerate triangles, and consistent winding.
    void ExpectValidMesh(const rendering::TriangleMesh& mesh)
    {
        ASSERT_FALSE(mesh.indices.empty());
        EXPECT_EQ(mesh.indices.size() % 3, 0u);
        for (const std::uint32_t index : mesh.indices)
        {
            EXPECT_LT(index, mesh.vertices.size());
        }

        std::map<std::pair<double, double>, int> vertexCounts;
        for (const rendering::Point& vertex : mesh.vertices)
        {
            ++vertexCounts[{vertex.x, vertex.y}];
        }
        for (const auto& [coordinate, count] : vertexCounts)
        {
            EXPECT_EQ(count, 1) << "Duplicate vertex at " << coordinate.first
                                << ", " << coordinate.second;
        }

        for (std::size_t index = 0; index < mesh.indices.size(); index += 3)
        {
            const rendering::Point& a = mesh.vertices[mesh.indices[index]];
            const rendering::Point& b = mesh.vertices[mesh.indices[index + 1]];
            const rendering::Point& c = mesh.vertices[mesh.indices[index + 2]];
            EXPECT_GT(TwiceSignedArea(a, b, c), 0.0)
                << "Triangle is degenerate or not counter-clockwise";
        }
    }

    /// Asserts that the mesh covers exactly the polygon: areas match, every
    /// triangle stays inside, triangles do not overlap, and no triangle
    /// covers an interior hole.
    void ExpectMeshCoversPolygon(const rendering::TriangleMesh& mesh,
                                 const rendering::Polygon& polygon)
    {
        ExpectValidMesh(mesh);
        const std::unique_ptr<OGRPolygon> source = MakeOgrPolygon(polygon);

        EXPECT_NEAR(MeshArea(mesh), PolygonArea(polygon), 1e-9);

        std::vector<std::unique_ptr<OGRPolygon>> triangles;
        triangles.reserve(mesh.indices.size() / 3);
        for (std::size_t index = 0; index < mesh.indices.size(); index += 3)
        {
            triangles.push_back(MakeOgrTriangle(mesh.vertices[mesh.indices[index]],
                                                mesh.vertices[mesh.indices[index + 1]],
                                                mesh.vertices[mesh.indices[index + 2]]));
        }

        for (const auto& triangle : triangles)
        {
            EXPECT_TRUE(triangle->Within(source.get()))
                << "Triangle extends outside the polygon";
        }

        for (std::size_t first = 0; first < triangles.size(); ++first)
        {
            for (std::size_t second = first + 1; second < triangles.size(); ++second)
            {
                const std::unique_ptr<OGRGeometry> overlap(
                    triangles[first]->Intersection(triangles[second].get()));
                ASSERT_NE(overlap, nullptr);
                // Disjoint triangles give an empty result. Adjacent triangles
                // may share an edge or a point (lower dimension), but never
                // a two-dimensional region.
                EXPECT_TRUE(overlap->IsEmpty() || overlap->getDimension() < 2)
                    << "Triangles " << first << " and " << second << " overlap";
            }
        }

        for (std::size_t hole = 1; hole < polygon.size(); ++hole)
        {
            rendering::Polygon holeOnly{polygon[hole]};
            const std::unique_ptr<OGRPolygon> holePolygon = MakeOgrPolygon(holeOnly);
            for (const auto& triangle : triangles)
            {
                const std::unique_ptr<OGRGeometry> intersection(
                    triangle->Intersection(holePolygon.get()));
                ASSERT_NE(intersection, nullptr);
                EXPECT_TRUE(intersection->IsEmpty() || intersection->getDimension() < 2)
                    << "Triangles cover interior hole " << hole;
            }
        }
    }

    /// Asserts that two meshes have identical vertices and indices.
    void ExpectSameMesh(const rendering::TriangleMesh& actual,
                        const rendering::TriangleMesh& expected)
    {
        ASSERT_EQ(actual.vertices.size(), expected.vertices.size());
        for (std::size_t index = 0; index < actual.vertices.size(); ++index)
        {
            EXPECT_DOUBLE_EQ(actual.vertices[index].x, expected.vertices[index].x);
            EXPECT_DOUBLE_EQ(actual.vertices[index].y, expected.vertices[index].y);
        }
        EXPECT_EQ(actual.indices, expected.indices);
    }

    std::filesystem::path MakeWktDataset(const std::filesystem::path& directory,
                                         const char* fileName,
                                         const OGRwkbGeometryType layerType,
                                         const char* geometryWkt)
    {
        GdalEnvironment::Initialize();
        const std::filesystem::path path = directory / fileName;
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
        if (driver == nullptr)
        {
            throw std::runtime_error("GPKG driver is not available");
        }

        const std::u8string utf8 = path.u8string();
        GDALDatasetUniquePtr output(driver->Create(
            reinterpret_cast<const char*>(utf8.data()), 0, 0, 0, GDT_Unknown, nullptr));
        if (output == nullptr)
        {
            throw std::runtime_error("Cannot create polygon dataset");
        }

        OGRLayer* layer = output->CreateLayer("polygons", nullptr, layerType, nullptr);
        if (layer == nullptr)
        {
            throw std::runtime_error("Cannot create polygon layer");
        }

        const char* text = geometryWkt;
        OGRGeometry* geometry = nullptr;
        if (OGRGeometryFactory::createFromWkt(&text, nullptr, &geometry) != OGRERR_NONE ||
            geometry == nullptr)
        {
            throw std::runtime_error("Cannot parse polygon geometry");
        }

        OGRFeatureUniquePtr feature(OGRFeature::CreateFeature(layer->GetLayerDefn()));
        const OGRErr geometryResult = feature->SetGeometry(geometry);
        OGRGeometryFactory::destroyGeometry(geometry);
        if (geometryResult != OGRERR_NONE || layer->CreateFeature(feature.get()) != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot write polygon feature");
        }

        output.reset();
        return path;
    }
}

// --- Basic polygons ---

TEST(PolygonTriangulation, TriangulatesTriangle)
{
    const rendering::TriangleMesh mesh =
        rendering::TriangulatePolygon(MakePolygon({{0, 0}, {2, 0}, {0, 2}}));

    ExpectValidMesh(mesh);
    EXPECT_EQ(mesh.vertices.size(), 3u);
    EXPECT_EQ(mesh.indices.size(), 3u);
    EXPECT_NEAR(MeshArea(mesh), 2.0, 1e-12);
}

TEST(PolygonTriangulation, TriangulatesConvexRectangleWithSharedVertices)
{
    const rendering::Polygon polygon = MakePolygon({{0, 0}, {2, 0}, {2, 1}, {0, 1}});
    const rendering::TriangleMesh mesh = rendering::TriangulatePolygon(polygon);

    ExpectMeshCoversPolygon(mesh, polygon);
    EXPECT_EQ(mesh.vertices.size(), 4u) << "Both triangles should share the diagonal";
    EXPECT_EQ(mesh.indices.size(), 6u);
}

TEST(PolygonTriangulation, TriangulatesConvexPolygonWithSeveralVertices)
{
    const rendering::Polygon polygon =
        MakePolygon({{0, 0}, {3, 0}, {4, 1.5}, {2.5, 3}, {1, 2.5}, {-0.5, 1}});
    const rendering::TriangleMesh mesh = rendering::TriangulatePolygon(polygon);

    ExpectMeshCoversPolygon(mesh, polygon);
    EXPECT_GE(mesh.indices.size(), 12u);
}

// --- Concave polygons ---

TEST(PolygonTriangulation, TriangulatesSimpleConcavePolygon)
{
    const rendering::Polygon polygon =
        MakePolygon({{0, 0}, {4, 0}, {4, 4}, {2, 2}, {0, 4}});
    const rendering::TriangleMesh mesh = rendering::TriangulatePolygon(polygon);

    ExpectMeshCoversPolygon(mesh, polygon);
    EXPECT_NEAR(MeshArea(mesh), 12.0, 1e-9);
}

TEST(PolygonTriangulation, TriangulatesLShapedPolygon)
{
    const rendering::Polygon polygon =
        MakePolygon({{0, 0}, {3, 0}, {3, 1}, {1, 1}, {1, 3}, {0, 3}});
    const rendering::TriangleMesh mesh = rendering::TriangulatePolygon(polygon);

    ExpectMeshCoversPolygon(mesh, polygon);
    EXPECT_NEAR(MeshArea(mesh), 5.0, 1e-9);
}

TEST(PolygonTriangulation, TriangulatesComplexConcavePolygon)
{
    const rendering::Polygon polygon = MakePolygon(
        {{1, 0}, {2, 0}, {2, 1}, {3, 1}, {3, 2}, {2, 2}, {2, 3}, {1, 3}, {1, 2}, {0, 2}, {0, 1}, {1, 1}});
    const rendering::TriangleMesh mesh = rendering::TriangulatePolygon(polygon);

    ExpectMeshCoversPolygon(mesh, polygon);
    EXPECT_NEAR(MeshArea(mesh), 5.0, 1e-9);
}

// --- Interior rings ---

TEST(PolygonTriangulation, TriangulatesPolygonWithOneHole)
{
    const rendering::Polygon polygon =
        MakePolygon({{0, 0}, {3, 0}, {3, 3}, {0, 3}}, {{{1, 1}, {2, 1}, {2, 2}, {1, 2}}});
    const rendering::TriangleMesh mesh = rendering::TriangulatePolygon(polygon);

    ExpectMeshCoversPolygon(mesh, polygon);
    EXPECT_NEAR(MeshArea(mesh), 8.0, 1e-9);
}

TEST(PolygonTriangulation, TriangulatesPolygonWithMultipleHoles)
{
    const rendering::Polygon polygon = MakePolygon(
        {{0, 0}, {5, 0}, {5, 4}, {0, 4}},
        {{{1, 1}, {2, 1}, {2, 2}, {1, 2}}, {{3, 2}, {4, 2}, {4, 3}, {3, 3}}});
    const rendering::TriangleMesh mesh = rendering::TriangulatePolygon(polygon);

    ExpectMeshCoversPolygon(mesh, polygon);
    EXPECT_NEAR(MeshArea(mesh), 18.0, 1e-9);
}

TEST(PolygonTriangulation, TriangulatesConcavePolygonWithHole)
{
    const rendering::Polygon polygon = MakePolygon(
        {{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}},
        {{{1, 1}, {1.5, 1}, {1.5, 1.5}, {1, 1.5}}});
    const rendering::TriangleMesh mesh = rendering::TriangulatePolygon(polygon);

    ExpectMeshCoversPolygon(mesh, polygon);
}

// --- Input validation ---

TEST(PolygonTriangulation, RejectsPolygonWithoutRings)
{
    EXPECT_THROW((void)rendering::TriangulatePolygon({}), InvalidArgument);
}

TEST(PolygonTriangulation, RejectsDegenerateExteriorRing)
{
    EXPECT_THROW(
        (void)rendering::TriangulatePolygon(MakePolygon({{0, 0}, {1, 0}})),
        InvalidArgument);
}

TEST(PolygonTriangulation, RejectsCollinearExteriorRing)
{
    EXPECT_THROW(
        (void)rendering::TriangulatePolygon(MakePolygon({{0, 0}, {1, 0}, {2, 0}})),
        CoreError);
}

TEST(PolygonTriangulation, RejectsSelfIntersectingPolygon)
{
    EXPECT_THROW(
        (void)rendering::TriangulatePolygon(MakePolygon({{0, 0}, {2, 2}, {2, 0}, {0, 2}})),
        CoreError);
}

TEST(PolygonTriangulation, RejectsNonFiniteCoordinates)
{
    EXPECT_THROW(
        (void)rendering::TriangulatePolygon(
            MakePolygon({{0, 0}, {1, 0}, {0, std::numeric_limits<double>::quiet_NaN()}})),
        InvalidArgument);
}

TEST(PolygonTriangulation, AcceptsUnclosedAndDuplicateVertices)
{
    const rendering::Polygon polygon =
        MakePolygon({{0, 0}, {2, 0}, {2, 0}, {2, 2}, {0, 2}, {0, 0}, {0, 0}});
    const rendering::TriangleMesh mesh = rendering::TriangulatePolygon(polygon);

    ExpectValidMesh(mesh);
    EXPECT_NEAR(MeshArea(mesh), 4.0, 1e-9);
}

TEST(PolygonTriangulation, IgnoresInteriorRingWithFewerThanThreePoints)
{
    const rendering::Polygon polygon =
        MakePolygon({{0, 0}, {2, 0}, {2, 2}, {0, 2}}, {{{1, 1}, {1.5, 1.5}}});
    const rendering::TriangleMesh mesh = rendering::TriangulatePolygon(polygon);

    ExpectValidMesh(mesh);
    EXPECT_NEAR(MeshArea(mesh), 4.0, 1e-9);
}

TEST(PolygonTriangulation, RepeatedCallsProduceIdenticalMeshes)
{
    const rendering::Polygon polygon =
        MakePolygon({{0, 0}, {3, 0}, {3, 2}, {1, 2}, {1, 4}, {0, 4}},
                    {{{2, 0.5}, {2.5, 0.5}, {2.5, 1}, {2, 1}}});

    const rendering::TriangleMesh first = rendering::TriangulatePolygon(polygon);
    const rendering::TriangleMesh second = rendering::TriangulatePolygon(polygon);

    ExpectMeshCoversPolygon(first, polygon);
    ExpectMeshCoversPolygon(second, polygon);
    ExpectSameMesh(second, first);
}

// --- Render scene integration ---

TEST(PolygonTriangulation, SceneMeshesAlignWithPolygons)
{
    TempDirectory directory;
    const std::vector<Extent> rectangles = {
        {0.0, 0.0, 2.0, 1.0}, {5.0, 0.0, 7.0, 3.0}, {10.0, 0.0, 11.0, 1.0}};
    const auto dataset =
        VectorDataset::Open(MakeRectanglesDataset(directory.Path(), "rects.gpkg", rectangles));

    const rendering::VectorRenderScene scene = rendering::CreateVectorRenderScene(dataset->Layer(0));

    ASSERT_EQ(scene.polygons.size(), rectangles.size());
    ASSERT_EQ(scene.fillMeshes.size(), scene.polygons.size());
    for (std::size_t index = 0; index < scene.polygons.size(); ++index)
    {
        ASSERT_TRUE(scene.fillMeshes[index].has_value()) << "Polygon " << index;
        const rendering::TriangleMesh& mesh = *scene.fillMeshes[index];
        ExpectValidMesh(mesh);
        const double width = rectangles[index].xMax - rectangles[index].xMin;
        const double height = rectangles[index].yMax - rectangles[index].yMin;
        EXPECT_NEAR(MeshArea(mesh), width * height, 1e-9);
        // Original rings stay available for outlines and GDI.
        ASSERT_EQ(scene.polygons[index].size(), 1u);
        EXPECT_EQ(scene.polygons[index].front().size(), 5u);
    }
}

TEST(PolygonTriangulation, SceneTriangulatesMultipartGeometryWithHole)
{
    TempDirectory directory;
    const auto dataset = VectorDataset::Open(MakeWktDataset(
        directory.Path(),
        "multipart.gpkg",
        wkbMultiPolygon,
        "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
        "((10 10, 14 10, 14 14, 10 14, 10 10), (11 11, 12 11, 12 12, 11 12, 11 11)))"));

    const rendering::VectorRenderScene scene = rendering::CreateVectorRenderScene(dataset->Layer(0));

    ASSERT_EQ(scene.polygons.size(), 2u);
    ASSERT_EQ(scene.fillMeshes.size(), 2u);
    for (std::size_t index = 0; index < scene.polygons.size(); ++index)
    {
        ASSERT_TRUE(scene.fillMeshes[index].has_value()) << "Polygon " << index;
        ExpectMeshCoversPolygon(*scene.fillMeshes[index], scene.polygons[index]);
    }
    EXPECT_NEAR(MeshArea(*scene.fillMeshes[0]), 16.0, 1e-9);
    EXPECT_NEAR(MeshArea(*scene.fillMeshes[1]), 15.0, 1e-9);
}

TEST(PolygonTriangulation, FailedTriangulationKeepsOriginalRings)
{
    TempDirectory directory;
    const auto dataset = VectorDataset::Open(MakeWktDataset(
        directory.Path(),
        "invalid.gpkg",
        wkbPolygon,
        "POLYGON ((0 0, 2 2, 2 0, 0 2, 0 0))"));

    const rendering::VectorRenderScene scene = rendering::CreateVectorRenderScene(dataset->Layer(0));

    ASSERT_EQ(scene.polygons.size(), 1u);
    ASSERT_EQ(scene.fillMeshes.size(), 1u);
    EXPECT_FALSE(scene.fillMeshes.front().has_value())
        << "The self-intersecting polygon must not receive a mesh";
    // GDI can still draw the original rings.
    ASSERT_EQ(scene.polygons.front().size(), 1u);
    EXPECT_EQ(scene.polygons.front().front().size(), 5u);
}
