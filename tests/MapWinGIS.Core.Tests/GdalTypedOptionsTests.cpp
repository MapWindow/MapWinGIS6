// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalTypedOptionsTests.cpp
 * @brief Tests conversion and execution of typed GDAL algorithm options.
 *
 * Verifies option names and value types against GDAL metadata and checks
 * vector conversion and clipping, raster conversion, clipping, reprojection,
 * mosaicking, argument validation, and failure reporting.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "mapwingis/gdal/GdalAlgorithmRunner.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/gdal/GdalTypedOptions.h"
#include "TestData.h"

using namespace mwg;

namespace
{
    std::string ToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    std::string LastError(const GdalAlgorithmRunner& runner)
    {
        return runner.Errors().empty() ? std::string{} : runner.Errors().back().message;
    }

    GdalDatasetPtr OpenOutput(const std::filesystem::path& path, const unsigned int flags)
    {
        return GdalDatasetPtr(static_cast<GDALDataset*>(
            GDALOpenEx(ToUtf8(path).c_str(), flags, nullptr, nullptr, nullptr)));
    }

    void ExpectRasterGrid(GDALDataset& dataset, const int width, const int height,
                          const char* epsg, const std::array<double, 6>& expectedTransform)
    {
        EXPECT_EQ(dataset.GetRasterXSize(), width);
        EXPECT_EQ(dataset.GetRasterYSize(), height);
        EXPECT_EQ(dataset.GetRasterCount(), 1);
        const OGRSpatialReference* srs = dataset.GetSpatialRef();
        ASSERT_NE(srs, nullptr);
        EXPECT_STREQ(srs->GetAuthorityName(nullptr), "EPSG");
        EXPECT_STREQ(srs->GetAuthorityCode(nullptr), epsg);

        std::array<double, 6> transform{};
        ASSERT_EQ(dataset.GetGeoTransform(transform.data()), CE_None);
        for (std::size_t i = 0; i < transform.size(); ++i)
            EXPECT_NEAR(transform[i], expectedTransform[i], 1e-8) << i;
    }

    void ExpectPixels(GDALDataset& dataset, const std::vector<std::uint8_t>& expected)
    {
        GDALRasterBand* band = dataset.GetRasterBand(1);
        ASSERT_NE(band, nullptr);
        EXPECT_EQ(band->GetRasterDataType(), GDT_Byte);
        const int width = dataset.GetRasterXSize();
        const int height = dataset.GetRasterYSize();
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) *
                                        static_cast<std::size_t>(height));
        ASSERT_EQ(band->RasterIO(GF_Read, 0, 0, width, height, pixels.data(),
                                width, height, GDT_Byte, 0, 0), CE_None);
        EXPECT_EQ(pixels, expected);
    }

    void ExpectArgumentsExist(const std::string_view algorithmPath,
                              const GdalAlgorithmArguments& arguments)
    {
        GdalAlgorithmRunner runner(algorithmPath);
        ASSERT_FALSE(runner.UsageAsJson().empty()) << algorithmPath;
        for (const GdalAlgorithmArgument& argument : arguments)
        {
            const bool configured = std::visit(
                [&runner, &argument](const auto& value)
                {
                    return runner.SetArgument(argument.name, value);
                },
                argument.value);
            EXPECT_TRUE(configured)
                << algorithmPath << ": " << argument.name << ": "
                << (runner.Errors().empty() ? std::string{} : runner.Errors().back().message);
        }
    }

    void ExpectArgumentsValid(const std::string_view algorithmPath,
                              const GdalAlgorithmArguments& arguments)
    {
        GdalAlgorithmRunner runner(algorithmPath);
        ASSERT_FALSE(runner.UsageAsJson().empty()) << algorithmPath;
        EXPECT_TRUE(ConfigureGdalAlgorithm(runner, arguments))
            << algorithmPath << ": "
            << (runner.Errors().empty() ? std::string{} : runner.Errors().back().message);
    }
}

TEST(GdalTypedOptions, EmitsMetadataRecognizedArgumentsForPriorityAlgorithms)
{
    GdalEnvironment::Initialize();

    RasterConvertOptions rasterConvert;
    rasterConvert.input = "input.tif";
    rasterConvert.output = "output.tif";
    rasterConvert.outputFormat = "GTiff";
    rasterConvert.overwrite = true;
    rasterConvert.append = false;
    rasterConvert.inputFormats = {"GTiff"};
    rasterConvert.creationOptions = {"TILED=YES"};
    rasterConvert.openOptions = {"NUM_THREADS=1"};
    ExpectArgumentsExist("raster convert", rasterConvert.ToArgs());

    RasterReprojectOptions reproject;
    reproject.input = "input.tif";
    reproject.output = "output.tif";
    reproject.outputFormat = "GTiff";
    reproject.sourceCrs = "EPSG:4326";
    reproject.targetCrs = "EPSG:3857";
    reproject.overwrite = true;
    reproject.append = false;
    reproject.resolution = std::array<double, 2>{1, 1};
    reproject.size = std::array<int, 2>{12, 8};
    reproject.boundingBox = std::array<double, 4>{-1, -1, 1, 1};
    reproject.boundingBoxCrs = "EPSG:3857";
    reproject.resampling = "nearest";
    reproject.inputNoData = std::vector<double>{0};
    reproject.outputNoData = std::vector<double>{255};
    reproject.targetAlignedPixels = true;
    reproject.creationOptions = {"TILED=YES"};
    reproject.inputFormats = {"GTiff"};
    reproject.openOptions = {"NUM_THREADS=1"};
    ExpectArgumentsExist("raster reproject", reproject.ToArgs());

    VectorConvertOptions vectorConvert;
    vectorConvert.input = "input.gpkg";
    vectorConvert.output = "output.geojson";
    vectorConvert.outputFormat = "GeoJSON";
    vectorConvert.inputLayers = {"provinces"};
    vectorConvert.outputLayer = "converted";
    vectorConvert.overwrite = true;
    vectorConvert.update = false;
    vectorConvert.overwriteLayer = false;
    vectorConvert.append = false;
    vectorConvert.skipErrors = false;
    vectorConvert.inputFormats = {"GPKG"};
    vectorConvert.creationOptions = {"RFC7946=YES"};
    vectorConvert.layerCreationOptions = {"COORDINATE_PRECISION=6"};
    vectorConvert.openOptions = {"LIST_ALL_TABLES=YES"};
    ExpectArgumentsExist("vector convert", vectorConvert.ToArgs());

    RasterOverviewAddOptions overviews;
    overviews.input = "input.tif";
    overviews.levels = {2, 4};
    overviews.resampling = "average";
    overviews.minimumSize = 256;
    overviews.creationOptions = {"COMPRESS_OVERVIEW=DEFLATE"};
    overviews.openOptions = {"NUM_THREADS=1"};
    ExpectArgumentsExist("raster overview add", overviews.ToArgs());

    RasterInfoOptions rasterInfo;
    rasterInfo.input = "input.tif";
    rasterInfo.outputFormat = "json";
    rasterInfo.minMax = true;
    rasterInfo.computeStatistics = true;
    rasterInfo.approximateStatistics = false;
    rasterInfo.computeHistogram = false;
    rasterInfo.checksum = true;
    rasterInfo.inputFormats = {"GTiff"};
    rasterInfo.openOptions = {"NUM_THREADS=1"};
    ExpectArgumentsExist("raster info", rasterInfo.ToArgs());

    VectorInfoOptions vectorInfo;
    vectorInfo.input = "input.gpkg";
    vectorInfo.layer = "provinces";
    vectorInfo.summary = true;
    vectorInfo.features = false;
    vectorInfo.limit = 5;
    vectorInfo.where = "ID > 0";
    vectorInfo.sql = "SELECT * FROM provinces";
    vectorInfo.inputFormats = {"GPKG"};
    vectorInfo.openOptions = {"LIST_ALL_TABLES=YES"};
    ExpectArgumentsExist("vector info", vectorInfo.ToArgs());

    RasterMosaicOptions mosaic;
    mosaic.inputs = {"first.tif", "second.tif"};
    mosaic.output = "mosaic.tif";
    mosaic.outputFormat = "GTiff";
    mosaic.overwrite = true;
    mosaic.append = false;
    mosaic.bands = std::vector<int>{1};
    mosaic.resolution = std::array<double, 2>{1, 1};
    mosaic.boundingBox = std::array<double, 4>{0, 0, 10, 10};
    mosaic.inputNoData = std::vector<double>{0};
    mosaic.outputNoData = std::vector<double>{255};
    mosaic.creationOptions = {"TILED=YES"};
    mosaic.inputFormats = {"GTiff"};
    mosaic.openOptions = {"NUM_THREADS=1"};
    ExpectArgumentsExist("raster mosaic", mosaic.ToArgs());

    VectorClipOptions vectorClip;
    vectorClip.input = "input.gpkg";
    vectorClip.output = "clipped.gpkg";
    vectorClip.like = "overlay.gpkg";
    vectorClip.inputLayer = "provinces";
    vectorClip.likeLayer = "boundary";
    vectorClip.likeWhere = "ID > 0";
    vectorClip.outputLayer = "clipped";
    vectorClip.outputFormat = "GPKG";
    vectorClip.overwrite = true;
    vectorClip.update = false;
    vectorClip.overwriteLayer = false;
    vectorClip.append = false;
    vectorClip.boundingBox = std::array<double, 4>{0, 0, 10, 10};
    vectorClip.boundingBoxCrs = "EPSG:28992";
    vectorClip.geometry = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))";
    vectorClip.geometryCrs = "EPSG:28992";
    vectorClip.skipErrors = false;
    vectorClip.creationOptions = {"SPATIAL_INDEX=YES"};
    vectorClip.layerCreationOptions = {"GEOMETRY_NAME=geom"};
    vectorClip.openOptions = {"LIST_ALL_TABLES=YES"};
    ExpectArgumentsExist("vector clip", vectorClip.ToArgs());

    RasterClipOptions rasterClip;
    rasterClip.input = "input.tif";
    rasterClip.output = "clipped.tif";
    rasterClip.outputFormat = "GTiff";
    rasterClip.overwrite = true;
    rasterClip.append = false;
    rasterClip.boundingBox = std::array<double, 4>{0, 0, 10, 10};
    rasterClip.boundingBoxCrs = "EPSG:4326";
    rasterClip.window = std::array<int, 4>{0, 0, 10, 10};
    rasterClip.geometry = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))";
    rasterClip.geometryCrs = "EPSG:4326";
    rasterClip.like = "cutline.gpkg";
    rasterClip.likeLayer = "boundary";
    rasterClip.likeSql = "SELECT * FROM boundary";
    rasterClip.likeWhere = "ID > 0";
    rasterClip.onlyBoundingBox = false;
    rasterClip.allowBoundingBoxOutsideSource = true;
    rasterClip.creationOptions = {"TILED=YES"};
    rasterClip.inputFormats = {"GTiff"};
    rasterClip.openOptions = {"NUM_THREADS=1"};
    ExpectArgumentsExist("raster clip", rasterClip.ToArgs());

    RasterCalcOptions rasterCalc;
    rasterCalc.expressions = {"X + 1", "X - 1"};
    rasterCalc.inputs = {"input.tif"};
    rasterCalc.output = "calculated.tif";
    rasterCalc.outputFormat = "GTiff";
    rasterCalc.outputDataType = "Byte";
    rasterCalc.overwrite = true;
    rasterCalc.append = false;
    rasterCalc.outputNoData = 255;
    rasterCalc.dialect = "builtin";
    rasterCalc.noCheckCrs = false;
    rasterCalc.noCheckExtent = false;
    rasterCalc.propagateNoData = true;
    rasterCalc.flatten = false;
    rasterCalc.creationOptions = {"TILED=YES"};
    rasterCalc.inputFormats = {"GTiff"};
    rasterCalc.openOptions = {"NUM_THREADS=1"};
    ExpectArgumentsExist("raster calc", rasterCalc.ToArgs());
}

TEST(GdalTypedOptions, ValidatesMinimumArgumentsForEachPriorityAlgorithm)
{
    GdalEnvironment::Initialize();

    test::TempDirectory directory;
    const std::filesystem::path raster = test::MakeRaster(directory.Path());
    const std::filesystem::path vector =
        test::MakeProvincesDataset(directory.Path(), "typed.gpkg");

    RasterConvertOptions rasterConvert;
    rasterConvert.input = raster;
    rasterConvert.output = directory.Path() / "converted.tif";
    ExpectArgumentsValid("raster convert", rasterConvert.ToArgs());

    RasterReprojectOptions reproject;
    reproject.input = raster;
    reproject.output = directory.Path() / "reprojected.tif";
    reproject.sourceCrs = "EPSG:4326";
    reproject.targetCrs = "EPSG:3857";
    ExpectArgumentsValid("raster reproject", reproject.ToArgs());

    VectorConvertOptions vectorConvert;
    vectorConvert.input = vector;
    vectorConvert.output = directory.Path() / "converted.geojson";
    ExpectArgumentsValid("vector convert", vectorConvert.ToArgs());

    RasterOverviewAddOptions overviews;
    overviews.input = raster;
    overviews.levels = {2};
    ExpectArgumentsValid("raster overview add", overviews.ToArgs());

    RasterInfoOptions rasterInfo;
    rasterInfo.input = raster;
    ExpectArgumentsValid("raster info", rasterInfo.ToArgs());

    VectorInfoOptions vectorInfo;
    vectorInfo.input = vector;
    ExpectArgumentsValid("vector info", vectorInfo.ToArgs());

    RasterMosaicOptions mosaic;
    mosaic.inputs = {raster};
    mosaic.output = directory.Path() / "mosaic.tif";
    ExpectArgumentsValid("raster mosaic", mosaic.ToArgs());

    VectorClipOptions vectorClip;
    vectorClip.input = vector;
    vectorClip.output = directory.Path() / "clipped.gpkg";
    vectorClip.boundingBox = std::array<double, 4>{100000, 400000, 104000, 401000};
    ExpectArgumentsValid("vector clip", vectorClip.ToArgs());

    RasterClipOptions rasterClip;
    rasterClip.input = raster;
    rasterClip.output = directory.Path() / "raster-clipped.tif";
    rasterClip.boundingBox = std::array<double, 4>{0, 0, 10, 8};
    ExpectArgumentsValid("raster clip", rasterClip.ToArgs());

    RasterCalcOptions rasterCalc;
    rasterCalc.expressions = {"X + 1"};
    rasterCalc.inputs = {raster};
    rasterCalc.output = directory.Path() / "calculated.tif";
    ExpectArgumentsValid("raster calc", rasterCalc.ToArgs());
}

TEST(GdalTypedOptions, ConvertsRasterToPngAndReturnsTheOutputDataset)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input =
        test::MakeRaster(directory.Path(), std::filesystem::path(u8"typed-入力.tif"));
    const std::filesystem::path output =
        directory.Path() / std::filesystem::path(u8"typed-出力.png");

    RasterConvertOptions options;
    options.input = input;
    options.output = output;
    options.outputFormat = "PNG";

    GdalAlgorithmRunner runner("raster convert");
    ASSERT_TRUE(ConfigureGdalAlgorithm(runner, options.ToArgs()));
    ASSERT_TRUE(runner.Run());

    GdalDatasetPtr dataset = runner.TakeOutputDataset();
    ASSERT_NE(dataset, nullptr);
    EXPECT_EQ(dataset->GetRasterXSize(), 12);
    EXPECT_EQ(dataset->GetRasterYSize(), 8);
    std::uint8_t pixel = 0;
    ASSERT_EQ(dataset->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1,
                                                  &pixel, 1, 1, GDT_Byte, 0, 0),
              CE_None);
    EXPECT_EQ(pixel, 42);
    EXPECT_TRUE(std::filesystem::exists(output));
}

TEST(GdalTypedOptions, ReportsUnavailableOutputDriver)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;

    RasterConvertOptions options;
    options.input = test::MakeRaster(directory.Path());
    options.output = directory.Path() / "unavailable-driver.out";
    options.outputFormat = "MWG_DRIVER_NOT_INSTALLED";

    GdalAlgorithmRunner runner("raster convert");
    const bool configured = ConfigureGdalAlgorithm(runner, options.ToArgs());
    if (configured)
        EXPECT_FALSE(runner.Run());

    EXPECT_FALSE(runner.Result().Succeeded());
    ASSERT_FALSE(runner.Errors().empty());
    EXPECT_FALSE(runner.Errors().back().message.empty());
    EXPECT_FALSE(std::filesystem::exists(options.output));
}

TEST(GdalTypedOptions, ConvertsVectorLayersAndPreservesAttributes)
{
    test::TempDirectory directory;
    VectorConvertOptions options;
    options.input = test::MakeProvincesDataset(directory.Path(), "input.gpkg");
    options.output = directory.Path() / "converted.geojson";
    options.outputFormat = "GeoJSON";
    options.inputLayers = {test::Provinces::kLayerName};
    options.outputLayer = "converted";

    {
        GdalAlgorithmRunner runner("vector convert");
        ASSERT_TRUE(ConfigureGdalAlgorithm(runner, options.ToArgs())) << LastError(runner);
        ASSERT_TRUE(runner.Run()) << LastError(runner);
    }

    EXPECT_TRUE(std::filesystem::exists(options.output));
    GdalDatasetPtr dataset = OpenOutput(options.output, GDAL_OF_VECTOR);
    ASSERT_NE(dataset, nullptr);
    EXPECT_STREQ(dataset->GetDriver()->GetDescription(), "GeoJSON");
    EXPECT_EQ(dataset->GetLayerCount(), 1);
    OGRLayer* layer = dataset->GetLayerByName("converted");
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(layer->GetFeatureCount(), test::Provinces::kFeatureCount);
    ASSERT_NE(layer->GetSpatialRef(), nullptr);
    EXPECT_STREQ(layer->GetSpatialRef()->GetAuthorityCode(nullptr), "28992");
    EXPECT_EQ(layer->GetLayerDefn()->GetFieldCount(), 5);

    for (int i = 0; i < test::Provinces::kFeatureCount; ++i)
    {
        OGRFeatureUniquePtr feature(layer->GetNextFeature());
        ASSERT_NE(feature, nullptr);
        EXPECT_EQ(feature->GetFieldAsInteger(test::Provinces::kIdField), i + 1);
        EXPECT_DOUBLE_EQ(feature->GetFieldAsDouble(test::Provinces::kAreaField), 1.0);
        if (i == 2)
            EXPECT_STREQ(feature->GetFieldAsString(test::Provinces::kNameField),
                         test::Provinces::kNonAsciiName);
        if (i == test::Provinces::kNullFeature)
            EXPECT_FALSE(feature->IsFieldSetAndNotNull(
                feature->GetFieldIndex(test::Provinces::kNameField)));

        OGRGeometry* geometry = feature->GetGeometryRef();
        ASSERT_NE(geometry, nullptr);
        EXPECT_NEAR(OGR_G_Area(OGRGeometry::ToHandle(geometry)), 1000000.0, 1e-6);
    }

    OGREnvelope extent;
    ASSERT_EQ(layer->GetExtent(&extent), OGRERR_NONE);
    EXPECT_DOUBLE_EQ(extent.MinX, test::Provinces::kMinX);
    EXPECT_DOUBLE_EQ(extent.MinY, test::Provinces::kMinY);
    EXPECT_DOUBLE_EQ(extent.MaxX, test::Provinces::kMaxX);
    EXPECT_DOUBLE_EQ(extent.MaxY, test::Provinces::kMaxY);
}

TEST(GdalTypedOptions, ClipsVectorGeometriesAndPreservesTheirAttributes)
{
    test::TempDirectory directory;
    VectorClipOptions options;
    options.input = test::MakeProvincesDataset(directory.Path(), "input.gpkg");
    options.output = directory.Path() / "clipped.gpkg";
    options.outputFormat = "GPKG";
    options.inputLayer = test::Provinces::kLayerName;
    options.outputLayer = "clipped";
    options.boundingBox = std::array<double, 4>{100500, 400250, 101500, 400750};
    options.boundingBoxCrs = "EPSG:28992";

    {
        GdalAlgorithmRunner runner("vector clip");
        ASSERT_TRUE(ConfigureGdalAlgorithm(runner, options.ToArgs())) << LastError(runner);
        ASSERT_TRUE(runner.Run()) << LastError(runner);
    }

    EXPECT_TRUE(std::filesystem::exists(options.output));
    GdalDatasetPtr dataset = OpenOutput(options.output, GDAL_OF_VECTOR);
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName("clipped");
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(layer->GetFeatureCount(), 2);
    ASSERT_NE(layer->GetSpatialRef(), nullptr);
    EXPECT_STREQ(layer->GetSpatialRef()->GetAuthorityCode(nullptr), "28992");
    for (int i = 0; i < 2; ++i)
    {
        OGRFeatureUniquePtr feature(layer->GetNextFeature());
        ASSERT_NE(feature, nullptr);
        EXPECT_EQ(feature->GetFieldAsInteger(test::Provinces::kIdField), i + 1);
        EXPECT_STREQ(feature->GetFieldAsString(test::Provinces::kNameField),
                     i == 0 ? "Noord" : "Oost");
        OGRGeometry* geometry = feature->GetGeometryRef();
        ASSERT_NE(geometry, nullptr);
        EXPECT_NEAR(OGR_G_Area(OGRGeometry::ToHandle(geometry)), 250000.0, 1e-6);
    }

    OGREnvelope extent;
    ASSERT_EQ(layer->GetExtent(&extent), OGRERR_NONE);
    EXPECT_DOUBLE_EQ(extent.MinX, 100500);
    EXPECT_DOUBLE_EQ(extent.MinY, 400250);
    EXPECT_DOUBLE_EQ(extent.MaxX, 101500);
    EXPECT_DOUBLE_EQ(extent.MaxY, 400750);
}

TEST(GdalTypedOptions, ClipsRasterToAPixelWindow)
{
    test::TempDirectory directory;
    RasterClipOptions options;
    options.input = test::MakeRaster(directory.Path());
    options.output = directory.Path() / "clipped.tif";
    options.outputFormat = "GTiff";
    options.window = std::array<int, 4>{2, 1, 5, 3};
    options.creationOptions = {"COMPRESS=LZW"};

    {
        GdalAlgorithmRunner runner("raster clip");
        ASSERT_TRUE(ConfigureGdalAlgorithm(runner, options.ToArgs())) << LastError(runner);
        ASSERT_TRUE(runner.Run()) << LastError(runner);
    }

    EXPECT_TRUE(std::filesystem::exists(options.output));
    GdalDatasetPtr dataset = OpenOutput(options.output, GDAL_OF_RASTER);
    ASSERT_NE(dataset, nullptr);
    ExpectRasterGrid(*dataset, 5, 3, "4326", {2, 1, 0, 7, 0, -1});
    EXPECT_STREQ(dataset->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "LZW");
    ExpectPixels(*dataset, std::vector<std::uint8_t>(15, 42));
}

TEST(GdalTypedOptions, ReprojectsRasterToTheRequestedCrsBoundsAndSize)
{
    test::TempDirectory directory;
    RasterReprojectOptions options;
    options.input = test::MakeRaster(directory.Path());
    options.output = directory.Path() / "reprojected.tif";
    options.outputFormat = "GTiff";
    options.targetCrs = "EPSG:3857";
    options.boundingBox = std::array<double, 4>{0, 0, 1200000, 800000};
    options.boundingBoxCrs = "EPSG:3857";
    options.size = std::array<int, 2>{6, 4};
    options.resampling = "nearest";
    options.outputNoData = std::vector<double>{255};

    {
        GdalAlgorithmRunner runner("raster reproject");
        ASSERT_TRUE(ConfigureGdalAlgorithm(runner, options.ToArgs())) << LastError(runner);
        ASSERT_TRUE(runner.Run()) << LastError(runner);
    }

    EXPECT_TRUE(std::filesystem::exists(options.output));
    GdalDatasetPtr dataset = OpenOutput(options.output, GDAL_OF_RASTER);
    ASSERT_NE(dataset, nullptr);
    ExpectRasterGrid(*dataset, 6, 4, "3857", {0, 200000, 0, 800000, 0, -200000});
    ExpectPixels(*dataset, std::vector<std::uint8_t>(24, 42));
    GDALRasterBand* band = dataset->GetRasterBand(1);
    ASSERT_NE(band, nullptr);
    int hasNoData = false;
    EXPECT_DOUBLE_EQ(band->GetNoDataValue(&hasNoData), 255);
    EXPECT_TRUE(hasNoData);
}

TEST(GdalTypedOptions, MosaicsAdjacentRastersAtTheRequestedResolution)
{
    test::TempDirectory directory;
    RasterMosaicOptions options;
    options.inputs = {test::MakeRaster(directory.Path(), "left.tif"),
                      test::MakeRaster(directory.Path(), "right.tif", 12, 84)};
    options.output = directory.Path() / "mosaic.tif";
    options.outputFormat = "GTiff";
    options.bands = std::vector<int>{1};
    options.resolution = std::array<double, 2>{0.5, 0.5};
    options.boundingBox = std::array<double, 4>{0, 0, 24, 8};

    {
        GdalAlgorithmRunner runner("raster mosaic");
        ASSERT_TRUE(ConfigureGdalAlgorithm(runner, options.ToArgs())) << LastError(runner);
        ASSERT_TRUE(runner.Run()) << LastError(runner);
    }

    EXPECT_TRUE(std::filesystem::exists(options.output));
    GdalDatasetPtr dataset = OpenOutput(options.output, GDAL_OF_RASTER);
    ASSERT_NE(dataset, nullptr);
    ExpectRasterGrid(*dataset, 48, 16, "4326", {0, 0.5, 0, 8, 0, -0.5});
    std::vector<std::uint8_t> expected;
    expected.reserve(48 * 16);
    for (int row = 0; row < 16; ++row)
    {
        for (int column = 0; column < 48; ++column)
            expected.push_back(column < 24 ? 42 : 84);
    }
    ExpectPixels(*dataset, expected);
}

TEST(GdalTypedOptions, RejectsUnknownAndMutuallyExclusiveArguments)
{
    GdalEnvironment::Initialize();
    test::TempDirectory directory;

    GdalAlgorithmRunner unknown("raster convert");
    EXPECT_FALSE(ConfigureGdalAlgorithm(
        unknown, GdalAlgorithmArguments{{"not-a-gdal-option", std::string("value")}}));
    ASSERT_FALSE(unknown.Errors().empty());

    RasterConvertOptions conflicting;
    conflicting.input = test::MakeRaster(directory.Path());
    conflicting.output = directory.Path() / "conflicting.tif";
    conflicting.overwrite = true;
    conflicting.append = true;
    GdalAlgorithmRunner invalid("raster convert");
    EXPECT_FALSE(ConfigureGdalAlgorithm(invalid, conflicting.ToArgs()));
    ASSERT_FALSE(invalid.Errors().empty());
}
