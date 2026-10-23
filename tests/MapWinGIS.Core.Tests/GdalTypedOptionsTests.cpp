// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalTypedOptionsTests.cpp
 * @brief Tests conversion and execution of typed GDAL algorithm options.
 *
 * Verifies option names and value types against GDAL metadata and checks
 * raster conversion, argument validation, and failure reporting.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gdal_priv.h>

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

    std::filesystem::path MakeRaster(const std::filesystem::path& directory)
    {
        constexpr int kWidth = 12;
        constexpr int kHeight = 8;
        const std::filesystem::path path = directory / std::filesystem::path(u8"typed-入力.tif");
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
        if (driver == nullptr)
            throw std::runtime_error("GTiff driver is not available");

        GdalDatasetPtr dataset(driver->Create(ToUtf8(path).c_str(), kWidth, kHeight,
                                              1, GDT_Byte, nullptr));
        if (dataset == nullptr)
            throw std::runtime_error("Could not create the typed-options test raster");

        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth * kHeight), 42);
        if (dataset->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                                pixels.data(), kWidth, kHeight,
                                                GDT_Byte, 0, 0) != CE_None)
        {
            throw std::runtime_error("Could not write the typed-options test raster");
        }
        return path;
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
    const std::filesystem::path raster = MakeRaster(directory.Path());
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
    const std::filesystem::path input = MakeRaster(directory.Path());
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

TEST(GdalTypedOptions, RejectsUnknownAndMutuallyExclusiveArguments)
{
    GdalEnvironment::Initialize();

    GdalAlgorithmRunner unknown("raster convert");
    EXPECT_FALSE(ConfigureGdalAlgorithm(
        unknown, GdalAlgorithmArguments{{"not-a-gdal-option", std::string("value")}}));
    ASSERT_FALSE(unknown.Errors().empty());

    RasterConvertOptions conflicting;
    conflicting.input = "input.tif";
    conflicting.output = "output.tif";
    conflicting.overwrite = true;
    conflicting.append = true;
    GdalAlgorithmRunner invalid("raster convert");
    EXPECT_FALSE(ConfigureGdalAlgorithm(invalid, conflicting.ToArgs()));
    ASSERT_FALSE(invalid.Errors().empty());
}
