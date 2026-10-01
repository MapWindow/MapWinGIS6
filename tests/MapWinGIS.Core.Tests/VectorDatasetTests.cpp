// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorDatasetTests.cpp
 * @brief Tests vector dataset and layer operations using generated fixtures.
 *
 * Covers layer metadata, feature attributes and geometry, null values,
 * projections, errors, and opening data sources with non-ASCII paths.
 */
#include <gtest/gtest.h>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    class VectorDatasetTest : public testing::Test
    {
    protected:
        void SetUp() override
        {
            _shp = MakeSampleShapefile(_dir.Path());
            _dataset = VectorDataset::Open(_shp);
            _layer = &_dataset->Layer(0);
        }

        TempDirectory _dir;
        std::filesystem::path _shp;
        std::unique_ptr<VectorDataset> _dataset;
        VectorLayer* _layer = nullptr;
    };
}

TEST_F(VectorDatasetTest, ReportsTheDriverAndPath)
{
    EXPECT_EQ(_dataset->DriverName(), "ESRI Shapefile");
    EXPECT_EQ(_dataset->Path(), _shp);
}

TEST_F(VectorDatasetTest, HasOneLayer)
{
    EXPECT_EQ(_dataset->LayerCount(), 1);
    EXPECT_EQ(_layer->Name(), "sample");
    EXPECT_NE(_dataset->LayerByName("SAMPLE"), nullptr);
    EXPECT_EQ(_dataset->LayerByName("missing"), nullptr);
}

TEST_F(VectorDatasetTest, ReportsShapeTypeAndFeatureCount)
{
    EXPECT_EQ(_layer->Type(), ShapeType::Polygon);
    EXPECT_EQ(_layer->FeatureCount(), SampleLayer::kFeatureCount);
}

TEST_F(VectorDatasetTest, ReportsTheLayerExtent)
{
    const Extent extent = _layer->GetExtent();
    ASSERT_FALSE(extent.IsEmpty());
    EXPECT_DOUBLE_EQ(extent.xMin, SampleLayer::kMinX);
    EXPECT_DOUBLE_EQ(extent.yMin, SampleLayer::kMinY);
    EXPECT_DOUBLE_EQ(extent.xMax, SampleLayer::kMaxX);
    EXPECT_DOUBLE_EQ(extent.yMax, SampleLayer::kMaxY);
}

TEST_F(VectorDatasetTest, ReportsTheFieldDefinitions)
{
    ASSERT_EQ(_layer->FieldCount(), 3);

    EXPECT_EQ(_layer->Field(0).name, SampleLayer::kIdField);
    EXPECT_EQ(_layer->Field(0).type, FieldType::Integer);

    EXPECT_EQ(_layer->Field(1).name, SampleLayer::kNameField);
    EXPECT_EQ(_layer->Field(1).type, FieldType::String);
    EXPECT_EQ(_layer->Field(1).width, 32);

    EXPECT_EQ(_layer->Field(2).name, SampleLayer::kAreaField);
    EXPECT_EQ(_layer->Field(2).type, FieldType::Double);
    EXPECT_EQ(_layer->Field(2).precision, 4);
}

TEST_F(VectorDatasetTest, FindsFieldsCaseInsensitively)
{
    EXPECT_EQ(_layer->FieldIndex("name"), 1);
    EXPECT_EQ(_layer->FieldIndex("NAME"), 1);
    EXPECT_EQ(_layer->FieldIndex("nope"), -1);
}

TEST_F(VectorDatasetTest, ReadsCellValues)
{
    const int id = _layer->FieldIndex(SampleLayer::kIdField);
    const int name = _layer->FieldIndex(SampleLayer::kNameField);
    const int area = _layer->FieldIndex(SampleLayer::kAreaField);

    EXPECT_EQ(_layer->CellInteger(id, 0), 1);
    EXPECT_EQ(_layer->CellInteger(id, 2), 3);
    EXPECT_EQ(_layer->CellString(name, 0), "first");
    EXPECT_EQ(_layer->CellString(name, 1), "second");
    ASSERT_TRUE(_layer->CellDouble(area, 1).has_value());
    EXPECT_DOUBLE_EQ(*_layer->CellDouble(area, 1), 2.0);
}

TEST_F(VectorDatasetTest, ReportsNullCells)
{
    const int name = _layer->FieldIndex(SampleLayer::kNameField);

    EXPECT_FALSE(_layer->IsCellNull(name, 0));
    EXPECT_TRUE(_layer->IsCellNull(name, 2));
    EXPECT_EQ(_layer->CellString(name, 2), "");
    EXPECT_FALSE(_layer->CellDouble(name, 2).has_value());
}

TEST_F(VectorDatasetTest, RejectsOutOfRangeFieldIndexes)
{
    EXPECT_THROW((void)_layer->CellString(-1, 0), CoreError);
    EXPECT_THROW((void)_layer->CellString(99, 0), CoreError);
    EXPECT_THROW((void)_dataset->Layer(1), CoreError);
}

TEST_F(VectorDatasetTest, ExportsGeometryAsWkt)
{
    const std::string wkt = _layer->GeometryWkt(0);
    EXPECT_TRUE(wkt.starts_with("POLYGON")) << wkt;
    EXPECT_NE(wkt.find("0 0"), std::string::npos) << wkt;

    EXPECT_EQ(_layer->GeometryWkt(SampleLayer::kFeatureCount), "");
}

TEST_F(VectorDatasetTest, ReportsPerFeatureExtents)
{
    const Extent extent = _layer->GeometryExtent(1);
    ASSERT_FALSE(extent.IsEmpty());
    EXPECT_DOUBLE_EQ(extent.xMin, 1.0);
    EXPECT_DOUBLE_EQ(extent.xMax, 2.0);
}

TEST_F(VectorDatasetTest, ReportsTheProjection)
{
    const std::string wkt = _layer->ProjectionWkt();
    ASSERT_FALSE(wkt.empty());
    EXPECT_NE(wkt.find("Amersfoort"), std::string::npos) << wkt;
}

TEST(VectorDataset, ReportsMissingFiles)
{
    const TempDirectory dir;
    const std::filesystem::path missing = dir.Path() / "does-not-exist.shp";

    EXPECT_EQ(VectorDataset::TryOpen(missing), nullptr);
    EXPECT_THROW((void)VectorDataset::Open(missing), OpenFailed);
}

TEST(VectorDataset, OpensNonAsciiPaths)
{
    const TempDirectory dir;
    const std::filesystem::path shp = MakeSampleShapefile(dir.Path(), "\u0412\u043e\u0437\u0434\u0443\u0445");

    const std::unique_ptr<VectorDataset> dataset = VectorDataset::Open(shp);
    ASSERT_EQ(dataset->LayerCount(), 1);
    EXPECT_EQ(dataset->Layer(0).FeatureCount(), SampleLayer::kFeatureCount);
}
