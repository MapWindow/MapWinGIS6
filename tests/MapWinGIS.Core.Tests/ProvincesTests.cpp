// SPDX-License-Identifier: MPL-2.0

/**
 * @file ProvincesTests.cpp
 * @brief Tests vector reading and editing against committed reference data.
 *
 * Exercises shapefile and GeoPackage behavior for attributes, geometry,
 * projections, null values, and persisted edits across both formats.
 */
//
// Tests against the committed reference data in tests/data. Unlike the
// generated fixtures in VectorDatasetTests.cpp, these exercise real files that
// carry a projection, a code page and null values.

#include <gtest/gtest.h>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    /// Runs every case against both the shapefile and the GeoPackage, so the
    /// core stays format-agnostic.
    class ProvincesTest : public testing::TestWithParam<const char*>
    {
    protected:
        void SetUp() override
        {
            _dataset = VectorDataset::Open(DataDirectory() / GetParam());
            _layer = _dataset->LayerByName(Provinces::kLayerName);
            ASSERT_NE(_layer, nullptr);
        }

        std::unique_ptr<VectorDataset> _dataset;
        VectorLayer* _layer = nullptr;
    };
}

INSTANTIATE_TEST_SUITE_P(Formats, ProvincesTest,
                         testing::Values("provinces.shp", "provinces.gpkg"),
                         [](const testing::TestParamInfo<const char*>& info)
                         {
                             return std::string(info.param).find("gpkg") != std::string::npos
                                        ? "GeoPackage"
                                        : "Shapefile";
                         });

TEST_P(ProvincesTest, ReportsTheLayer)
{
    EXPECT_EQ(_layer->Type(), ShapeType::Polygon);
    EXPECT_EQ(_layer->FeatureCount(), Provinces::kFeatureCount);
}

TEST_P(ProvincesTest, ReportsTheExtent)
{
    const Extent extent = _layer->GetExtent();
    ASSERT_FALSE(extent.IsEmpty());
    EXPECT_DOUBLE_EQ(extent.xMin, Provinces::kMinX);
    EXPECT_DOUBLE_EQ(extent.yMin, Provinces::kMinY);
    EXPECT_DOUBLE_EQ(extent.xMax, Provinces::kMaxX);
    EXPECT_DOUBLE_EQ(extent.yMax, Provinces::kMaxY);
}

TEST_P(ProvincesTest, ReportsAllMapWinGisFieldTypes)
{
    EXPECT_EQ(_layer->Field(_layer->FieldIndex(Provinces::kIdField)).type, FieldType::Integer);
    EXPECT_EQ(_layer->Field(_layer->FieldIndex(Provinces::kNameField)).type, FieldType::String);
    EXPECT_EQ(_layer->Field(_layer->FieldIndex(Provinces::kAreaField)).type, FieldType::Double);
    EXPECT_EQ(_layer->Field(_layer->FieldIndex(Provinces::kSurveyedField)).type, FieldType::Date);
    EXPECT_EQ(_layer->Field(_layer->FieldIndex(Provinces::kActiveField)).type, FieldType::Boolean);
}

TEST_P(ProvincesTest, ReadsAttributes)
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

TEST_P(ProvincesTest, PreservesNonAsciiAttributeValues)
{
    const int name = _layer->FieldIndex(Provinces::kNameField);
    EXPECT_EQ(_layer->CellString(name, 2), Provinces::kNonAsciiName);
}

TEST_P(ProvincesTest, DistinguishesNullFromEmpty)
{
    const int name = _layer->FieldIndex(Provinces::kNameField);
    const int surveyed = _layer->FieldIndex(Provinces::kSurveyedField);

    EXPECT_FALSE(_layer->IsCellNull(name, 0));
    EXPECT_TRUE(_layer->IsCellNull(name, Provinces::kNullFeature));
    EXPECT_TRUE(_layer->IsCellNull(surveyed, Provinces::kNullFeature));
    EXPECT_FALSE(_layer->IsCellNull(surveyed, 0));
}

TEST_P(ProvincesTest, ReadsDatesAsText)
{
    const int surveyed = _layer->FieldIndex(Provinces::kSurveyedField);
    EXPECT_EQ(_layer->CellString(surveyed, 0), "2020/01/15");
}

TEST_P(ProvincesTest, ReportsTheProjection)
{
    const std::string wkt = _layer->ProjectionWkt();
    ASSERT_FALSE(wkt.empty());
    EXPECT_NE(wkt.find("Amersfoort"), std::string::npos) << wkt;
    EXPECT_NE(wkt.find("28992"), std::string::npos) << wkt;
}

TEST_P(ProvincesTest, ReadsGeometry)
{
    const std::string wkt = _layer->GeometryWkt(0);
    EXPECT_TRUE(wkt.starts_with("POLYGON")) << wkt;

    const Extent extent = _layer->GeometryExtent(0);
    ASSERT_FALSE(extent.IsEmpty());
    EXPECT_DOUBLE_EQ(extent.xMin, 100000.0);
    EXPECT_DOUBLE_EQ(extent.xMax, 101000.0);
    EXPECT_DOUBLE_EQ(extent.Width() * extent.Height(), 1000000.0);
}

TEST_P(ProvincesTest, IsReadOnlyByDefault)
{
    EXPECT_FALSE(_dataset->IsWritable());
    EXPECT_FALSE(_layer->IsWritable());

    const int name = _layer->FieldIndex(Provinces::kNameField);
    EXPECT_THROW(_layer->SetCellString(name, 0, "Zuid"), CoreError);
}

// ------------------------------------------------------------------ editing

namespace
{
    class ProvincesEditTest : public testing::TestWithParam<const char*>
    {
    protected:
        void SetUp() override
        {
            _path = CopyDataset(_dir.Path(), GetParam());
        }

        /// Opens the working copy, hands the layer to `edit`, then closes so the
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

INSTANTIATE_TEST_SUITE_P(Formats, ProvincesEditTest,
                         testing::Values("provinces.shp", "provinces.gpkg"),
                         [](const testing::TestParamInfo<const char*>& info)
                         {
                             return std::string(info.param).find("gpkg") != std::string::npos
                                        ? "GeoPackage"
                                        : "Shapefile";
                         });

TEST_P(ProvincesEditTest, CopiesEveryPartOfTheDataset)
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

TEST_P(ProvincesEditTest, PersistsAttributeEdits)
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

TEST_P(ProvincesEditTest, PersistsNonAsciiEdits)
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

TEST_P(ProvincesEditTest, PersistsNulls)
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

TEST_P(ProvincesEditTest, PersistsGeometryEdits)
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

TEST_P(ProvincesEditTest, RejectsInvalidWkt)
{
    Edit([](VectorLayer& layer)
    {
        EXPECT_THROW(layer.SetGeometryWkt(0, "NOT WKT"), CoreError);
    });
}

TEST_P(ProvincesEditTest, ReflectsEditsWithoutReopening)
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

TEST_P(ProvincesEditTest, RecomputesTheExtentAfterAGeometryEdit)
{
    static constexpr const char* kMoved =
        "POLYGON ((200000 500000, 201000 500000, 201000 501000, 200000 501000, 200000 500000))";

    const std::unique_ptr<VectorDataset> dataset = VectorDataset::Open(_path, OpenMode::Update);
    VectorLayer& layer = dataset->Layer(0);

    ASSERT_DOUBLE_EQ(layer.GetExtent().xMax, Provinces::kMaxX);

    layer.SetGeometryWkt(0, kMoved);
    EXPECT_DOUBLE_EQ(layer.GetExtent().xMax, 201000.0);
}

TEST_P(ProvincesEditTest, LeavesTheReferenceDataUntouched)
{
    Edit([](VectorLayer& layer)
    {
        layer.SetCellString(layer.FieldIndex(Provinces::kNameField), 0, "Changed");
    });

    const std::unique_ptr<VectorDataset> original =
        VectorDataset::Open(DataDirectory() / GetParam());
    EXPECT_EQ(original->Layer(0).CellString(
                  original->Layer(0).FieldIndex(Provinces::kNameField), 0),
              "Noord");
}
