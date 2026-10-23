// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorDatasetTests.cpp
 * @brief Tests vector dataset and layer operations using generated fixtures.
 *
 * Covers layer metadata, feature attributes and geometry, null values,
 * projections, errors, and opening data sources with non-ASCII paths.
 */
#include <gtest/gtest.h>

#include <gdal_priv.h>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    class FlushFailureDataset : public GDALDataset
    {
    public:
        explicit FlushFailureDataset(const bool reportError)
            : _reportError(reportError)
        {
        }

        CPLErr FlushCache(bool) override
        {
            if (_reportError)
                CPLError(CE_Failure, CPLE_FileIO, "%s", "Simulated flush failure");
            return CE_Failure;
        }

    private:
        bool _reportError;
    };

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

    EXPECT_THROW((void)_layer->GeometryWkt(SampleLayer::kFeatureCount), CoreError);
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

TEST(VectorDataset, ReportsFlushFailures)
{
    GdalEnvironment::Initialize();
    const auto deregister = [](GDALDriver* driver)
    {
        GetGDALDriverManager()->DeregisterDriver(driver);
        delete driver;
    };
    const std::unique_ptr<GDALDriver, decltype(deregister)> driver{
        std::make_unique<GDALDriver>().release(), deregister};
    driver->SetDescription("MWGFlushFailure");
    driver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    driver->SetMetadataItem(GDAL_DCAP_OPEN, "YES");
    driver->pfnIdentify = [](GDALOpenInfo* info)
    {
        return std::string_view(info->pszFilename).starts_with("MWG_FLUSH_FAILURE:") ? TRUE : FALSE;
    };
    driver->pfnOpen = [](GDALOpenInfo* info) -> GDALDataset*
    {
        const std::string_view name(info->pszFilename);
        if (!name.starts_with("MWG_FLUSH_FAILURE:")) return nullptr;
        return std::make_unique<FlushFailureDataset>(name.ends_with("detail")).release();
    };
    GetGDALDriverManager()->RegisterDriver(driver.get());

    const GdalEnvironment::QuietScope quiet;
    for (const char* path : {"MWG_FLUSH_FAILURE:detail", "MWG_FLUSH_FAILURE:silent"})
    {
        const std::unique_ptr<VectorDataset> dataset = VectorDataset::Open(path);
        CPLError(CE_Failure, CPLE_AppDefined, "%s", "Stale error");
        try
        {
            dataset->Flush();
            ADD_FAILURE() << "Flush did not report failure";
        }
        catch (const CoreError& error)
        {
            const std::string message(error.what());
            EXPECT_NE(message.find("Cannot flush vector data source:"), std::string::npos) << message;
            EXPECT_NE(message.find(path), std::string::npos) << message;
            EXPECT_EQ(message.find("Stale error"), std::string::npos) << message;
            if (std::string_view(path).ends_with("detail"))
                EXPECT_NE(message.find("Simulated flush failure"), std::string::npos) << message;
        }
    }
}
