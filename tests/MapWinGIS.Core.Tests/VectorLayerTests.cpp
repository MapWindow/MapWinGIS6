// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorLayerTests.cpp
 * @brief Tests vector layer reading and editing with generated fixtures.
 *
 * Exercises shapefile and GeoPackage behavior for attributes, geometry,
 * projections, null values, and persisted edits across both formats.
 */

#include <gtest/gtest.h>

#include <array>
#include <limits>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    /// Runs every case against both the shapefile and the GeoPackage, so the
    /// core stays format-agnostic.
    class VectorLayerTest : public testing::TestWithParam<const char*>
    {
    protected:
        void SetUp() override
        {
            _dataset = VectorDataset::Open(MakeProvincesDataset(_dir.Path(), GetParam()));
            _layer = _dataset->LayerByName(Provinces::kLayerName);
            ASSERT_NE(_layer, nullptr);
        }

        TempDirectory _dir;
        std::unique_ptr<VectorDataset> _dataset;
        VectorLayer* _layer = nullptr;
    };
}

INSTANTIATE_TEST_SUITE_P(Formats, VectorLayerTest,
                         testing::Values("provinces.shp", "provinces.gpkg"),
                         [](const testing::TestParamInfo<const char*>& info)
                         {
                             return std::string(info.param).find("gpkg") != std::string::npos
                                        ? "GeoPackage"
                                        : "Shapefile";
                         });

TEST_P(VectorLayerTest, ReportsTheLayer)
{
    EXPECT_EQ(_layer->Type(), ShapeType::Polygon);
    EXPECT_EQ(_layer->FeatureCount(), Provinces::kFeatureCount);
}

TEST_P(VectorLayerTest, ReportsTheExtent)
{
    const Extent extent = _layer->GetExtent();
    ASSERT_FALSE(extent.IsEmpty());
    EXPECT_DOUBLE_EQ(extent.xMin, Provinces::kMinX);
    EXPECT_DOUBLE_EQ(extent.yMin, Provinces::kMinY);
    EXPECT_DOUBLE_EQ(extent.xMax, Provinces::kMaxX);
    EXPECT_DOUBLE_EQ(extent.yMax, Provinces::kMaxY);
}

TEST_P(VectorLayerTest, ReportsAllMapWinGisFieldTypes)
{
    EXPECT_EQ(_layer->Field(_layer->FieldIndex(Provinces::kIdField)).type, FieldType::Integer);
    EXPECT_EQ(_layer->Field(_layer->FieldIndex(Provinces::kNameField)).type, FieldType::String);
    EXPECT_EQ(_layer->Field(_layer->FieldIndex(Provinces::kAreaField)).type, FieldType::Double);
    EXPECT_EQ(_layer->Field(_layer->FieldIndex(Provinces::kSurveyedField)).type, FieldType::Date);
    EXPECT_EQ(_layer->Field(_layer->FieldIndex(Provinces::kActiveField)).type, FieldType::Boolean);
}

TEST_P(VectorLayerTest, ReadsAttributes)
{
    const int id = _layer->FieldIndex(Provinces::kIdField);
    const int name = _layer->FieldIndex(Provinces::kNameField);
    const int area = _layer->FieldIndex(Provinces::kAreaField);
    const int active = _layer->FieldIndex(Provinces::kActiveField);

    EXPECT_EQ(_layer->CellInteger(id, 0), 1);
    EXPECT_EQ(_layer->CellString(name, 0), "Noord");
    EXPECT_EQ(_layer->CellString(name, 1), "Oost");
    ASSERT_TRUE(_layer->CellDouble(area, 0).has_value());
    EXPECT_DOUBLE_EQ(*_layer->CellDouble(area, 0), 1.0);
    EXPECT_EQ(_layer->CellInteger(active, 2), 0);
    EXPECT_EQ(_layer->CellInteger(active, 3), 1);
}

TEST_P(VectorLayerTest, PreservesNonAsciiAttributeValues)
{
    const int name = _layer->FieldIndex(Provinces::kNameField);
    EXPECT_EQ(_layer->CellString(name, 2), Provinces::kNonAsciiName);
}

TEST_P(VectorLayerTest, DistinguishesNullFromEmpty)
{
    const int name = _layer->FieldIndex(Provinces::kNameField);
    const int surveyed = _layer->FieldIndex(Provinces::kSurveyedField);

    EXPECT_FALSE(_layer->IsCellNull(name, 0));
    EXPECT_TRUE(_layer->IsCellNull(name, Provinces::kNullFeature));
    EXPECT_TRUE(_layer->IsCellNull(surveyed, Provinces::kNullFeature));
    EXPECT_FALSE(_layer->IsCellNull(surveyed, 0));
}

TEST_P(VectorLayerTest, ReadsDatesAsText)
{
    const int surveyed = _layer->FieldIndex(Provinces::kSurveyedField);
    EXPECT_EQ(_layer->CellString(surveyed, 0), "2020/01/15");
}

TEST_P(VectorLayerTest, ReportsTheProjection)
{
    const std::string wkt = _layer->ProjectionWkt();
    ASSERT_FALSE(wkt.empty());
    EXPECT_NE(wkt.find("Amersfoort"), std::string::npos) << wkt;
    EXPECT_NE(wkt.find("28992"), std::string::npos) << wkt;
}

TEST_P(VectorLayerTest, ReadsGeometry)
{
    const std::string wkt = _layer->GeometryWkt(0);
    EXPECT_TRUE(wkt.starts_with("POLYGON")) << wkt;

    const Extent extent = _layer->GeometryExtent(0);
    ASSERT_FALSE(extent.IsEmpty());
    EXPECT_DOUBLE_EQ(extent.xMin, 100000.0);
    EXPECT_DOUBLE_EQ(extent.xMax, 101000.0);
    EXPECT_DOUBLE_EQ(extent.Width() * extent.Height(), 1000000.0);
}

TEST_P(VectorLayerTest, IsReadOnlyByDefault)
{
    EXPECT_FALSE(_dataset->IsWritable());
    EXPECT_FALSE(_layer->IsWritable());

    const int name = _layer->FieldIndex(Provinces::kNameField);
    EXPECT_THROW(_layer->SetCellString(name, 0, "Zuid"), CoreError);
}

TEST_P(VectorLayerTest, RejectsOutOfRangeFeatureIndexes)
{
    const int name = _layer->FieldIndex(Provinces::kNameField);
    const int area = _layer->FieldIndex(Provinces::kAreaField);
    const int id = _layer->FieldIndex(Provinces::kIdField);

    const std::array<std::int64_t, 4> invalidIndexes{
        -1, _layer->FeatureCount(), std::int64_t{1} << 32, std::numeric_limits<std::int64_t>::max()};
    for (const std::int64_t index : invalidIndexes)
    {
        EXPECT_THROW((void)_layer->IsCellNull(name, index), CoreError);
        EXPECT_THROW((void)_layer->CellString(name, index), CoreError);
        EXPECT_THROW((void)_layer->CellDouble(area, index), CoreError);
        EXPECT_THROW((void)_layer->CellInteger(id, index), CoreError);
        EXPECT_THROW((void)_layer->GeometryWkt(index), CoreError);
        EXPECT_THROW((void)_layer->GeometryExtent(index), CoreError);
    }
}

TEST_P(VectorLayerTest, PreservesNullAttributeResults)
{
    const int name = _layer->FieldIndex(Provinces::kNameField);
    EXPECT_TRUE(_layer->IsCellNull(name, Provinces::kNullFeature));
    EXPECT_TRUE(_layer->CellString(name, Provinces::kNullFeature).empty());
    EXPECT_FALSE(_layer->CellDouble(name, Provinces::kNullFeature).has_value());
    EXPECT_FALSE(_layer->CellInteger(name, Provinces::kNullFeature).has_value());
}

// ------------------------------------------------------------------ editing

namespace
{
    class VectorLayerEditTest : public testing::TestWithParam<const char*>
    {
    protected:
        void SetUp() override
        {
            _path = MakeProvincesDataset(_dir.Path(), GetParam());
        }

        /// Opens the fixture, hands the layer to `edit`, then closes so the
        /// driver flushes everything to disk.
        template <typename TEdit>
        void Edit(TEdit edit)
        {
            std::unique_ptr<VectorDataset> dataset = VectorDataset::Open(_path, OpenMode::Update);
            ASSERT_TRUE(dataset->IsWritable());
            edit(dataset->Layer(0));
        }

        [[nodiscard]] std::unique_ptr<VectorDataset> Reopen() const
        {
            return VectorDataset::Open(_path);
        }

        TempDirectory _dir;
        std::filesystem::path _path;
    };
}

INSTANTIATE_TEST_SUITE_P(Formats, VectorLayerEditTest,
                         testing::Values("provinces.shp", "provinces.gpkg"),
                         [](const testing::TestParamInfo<const char*>& info)
                         {
                             return std::string(info.param).find("gpkg") != std::string::npos
                                        ? "GeoPackage"
                                        : "Shapefile";
                         });

TEST_P(VectorLayerEditTest, GeneratesEveryPartOfTheDataset)
{
    EXPECT_TRUE(std::filesystem::exists(_path));
    if (_path.extension() == ".shp")
    {
        for (const char* sidecar : {".shx", ".dbf", ".prj", ".cpg"})
        {
            EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(_path).replace_extension(sidecar)))
                << sidecar;
        }
    }
}

TEST_P(VectorLayerEditTest, PersistsAttributeEdits)
{
    Edit([](VectorLayer& layer)
    {
        layer.SetCellString(layer.FieldIndex(Provinces::kNameField), 0, "Zuid-Holland");
        layer.SetCellDouble(layer.FieldIndex(Provinces::kAreaField), 0, 42.5);
        layer.SetCellInteger(layer.FieldIndex(Provinces::kActiveField), 0, 0);
    });

    const std::unique_ptr<VectorDataset> dataset = Reopen();
    VectorLayer& layer = dataset->Layer(0);

    EXPECT_EQ(layer.CellString(layer.FieldIndex(Provinces::kNameField), 0), "Zuid-Holland");
    ASSERT_TRUE(layer.CellDouble(layer.FieldIndex(Provinces::kAreaField), 0).has_value());
    EXPECT_DOUBLE_EQ(*layer.CellDouble(layer.FieldIndex(Provinces::kAreaField), 0), 42.5);
    EXPECT_EQ(layer.CellInteger(layer.FieldIndex(Provinces::kActiveField), 0), 0);

    EXPECT_EQ(layer.FeatureCount(), Provinces::kFeatureCount);
    EXPECT_EQ(layer.CellString(layer.FieldIndex(Provinces::kNameField), 1), "Oost");
}

TEST_P(VectorLayerEditTest, FlushesPendingEdits)
{
    const std::unique_ptr<VectorDataset> dataset = VectorDataset::Open(_path, OpenMode::Update);
    VectorLayer& layer = dataset->Layer(0);
    layer.SetCellString(layer.FieldIndex(Provinces::kNameField), 0, "Flushed");
    EXPECT_NO_THROW(dataset->Flush());

    const std::unique_ptr<VectorDataset> reopened = Reopen();
    EXPECT_EQ(reopened->Layer(0).CellString(
                  reopened->Layer(0).FieldIndex(Provinces::kNameField), 0), "Flushed");
}

TEST_P(VectorLayerEditTest, RejectsOutOfRangeFeatureIndexes)
{
    Edit([](VectorLayer& layer)
    {
        const int name = layer.FieldIndex(Provinces::kNameField);
        const int area = layer.FieldIndex(Provinces::kAreaField);
        const int id = layer.FieldIndex(Provinces::kIdField);
        const std::array<std::int64_t, 4> invalidIndexes{
            -1, layer.FeatureCount(), std::int64_t{1} << 32, std::numeric_limits<std::int64_t>::max()};
        for (const std::int64_t index : invalidIndexes)
        {
            EXPECT_THROW(layer.SetCellString(name, index, "Invalid"), CoreError);
            EXPECT_THROW(layer.SetCellDouble(area, index, 1.0), CoreError);
            EXPECT_THROW(layer.SetCellInteger(id, index, 1), CoreError);
            EXPECT_THROW(layer.SetCellNull(name, index), CoreError);
            EXPECT_THROW(layer.SetGeometryWkt(index, "POLYGON EMPTY"), CoreError);
        }
    });
}

TEST_P(VectorLayerEditTest, PersistsNonAsciiEdits)
{
    Edit([](VectorLayer& layer)
    {
        layer.SetCellString(layer.FieldIndex(Provinces::kNameField), 0, "Sk\u00e2rsterl\u00e2n");
    });

    const std::unique_ptr<VectorDataset> dataset = Reopen();
    EXPECT_EQ(dataset->Layer(0).CellString(
                  dataset->Layer(0).FieldIndex(Provinces::kNameField), 0),
              "Sk\u00e2rsterl\u00e2n");
}

TEST_P(VectorLayerEditTest, PersistsNulls)
{
    Edit([](VectorLayer& layer)
    {
        layer.SetCellNull(layer.FieldIndex(Provinces::kNameField), 0);
        layer.SetCellString(layer.FieldIndex(Provinces::kNameField), Provinces::kNullFeature, "Filled");
    });

    const std::unique_ptr<VectorDataset> dataset = Reopen();
    VectorLayer& layer = dataset->Layer(0);
    const int name = layer.FieldIndex(Provinces::kNameField);

    EXPECT_TRUE(layer.IsCellNull(name, 0));
    EXPECT_FALSE(layer.IsCellNull(name, Provinces::kNullFeature));
    EXPECT_EQ(layer.CellString(name, Provinces::kNullFeature), "Filled");
}

TEST_P(VectorLayerEditTest, PersistsGeometryEdits)
{
    static constexpr const char* kMoved =
        "POLYGON ((200000 500000, 201000 500000, 201000 501000, 200000 501000, 200000 500000))";

    Edit([](VectorLayer& layer) { layer.SetGeometryWkt(0, kMoved); });

    const std::unique_ptr<VectorDataset> dataset = Reopen();
    const Extent extent = dataset->Layer(0).GeometryExtent(0);

    ASSERT_FALSE(extent.IsEmpty());
    EXPECT_DOUBLE_EQ(extent.xMin, 200000.0);
    EXPECT_DOUBLE_EQ(extent.yMax, 501000.0);
}

TEST_P(VectorLayerEditTest, RejectsInvalidWkt)
{
    const GdalEnvironment::QuietScope quiet;
    Edit([](VectorLayer& layer)
    {
        EXPECT_THROW(layer.SetGeometryWkt(0, "NOT WKT"), CoreError);
    });
}

TEST_P(VectorLayerEditTest, ReflectsEditsWithoutReopening)
{
    const std::unique_ptr<VectorDataset> dataset = VectorDataset::Open(_path, OpenMode::Update);
    VectorLayer& layer = dataset->Layer(0);
    const int name = layer.FieldIndex(Provinces::kNameField);
    const int area = layer.FieldIndex(Provinces::kAreaField);

    ASSERT_EQ(layer.CellString(name, 0), "Noord");

    layer.SetCellString(name, 0, "Changed");
    EXPECT_EQ(layer.CellString(name, 0), "Changed");

    // Reading a different feature and coming back must not resurrect the old value.
    EXPECT_EQ(layer.CellString(name, 1), "Oost");
    EXPECT_EQ(layer.CellString(name, 0), "Changed");

    layer.SetCellNull(area, 0);
    EXPECT_TRUE(layer.IsCellNull(area, 0));
}

TEST_P(VectorLayerEditTest, RecomputesTheExtentAfterAGeometryEdit)
{
    static constexpr const char* kMoved =
        "POLYGON ((200000 500000, 201000 500000, 201000 501000, 200000 501000, 200000 500000))";

    const std::unique_ptr<VectorDataset> dataset = VectorDataset::Open(_path, OpenMode::Update);
    VectorLayer& layer = dataset->Layer(0);

    ASSERT_DOUBLE_EQ(layer.GetExtent().xMax, Provinces::kMaxX);

    layer.SetGeometryWkt(0, kMoved);
    EXPECT_DOUBLE_EQ(layer.GetExtent().xMax, 201000.0);
}

TEST_P(VectorLayerEditTest, KeepsGeneratedFixturesIndependent)
{
    Edit([](VectorLayer& layer)
    {
        layer.SetCellString(layer.FieldIndex(Provinces::kNameField), 0, "Changed");
    });

    const TempDirectory otherDir;
    const std::unique_ptr<VectorDataset> other =
        VectorDataset::Open(MakeProvincesDataset(otherDir.Path(), GetParam()));
    EXPECT_EQ(other->Layer(0).CellString(
                  other->Layer(0).FieldIndex(Provinces::kNameField), 0),
              "Noord");
}
