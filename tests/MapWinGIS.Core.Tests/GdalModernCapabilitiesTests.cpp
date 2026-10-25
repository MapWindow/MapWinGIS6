// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalModernCapabilitiesTests.cpp
 * @brief Tests COG creation, VSI access, VRT mosaics, and in-memory pipelines.
 *
 * Local tests use generated rasters; the public /vsicurl/ source test is
 * explicitly opt-in so routine test runs remain independent of network access.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include <gdal_priv.h>

#include "TestData.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/gdal/GdalModern.h"

using namespace mwg;

namespace
{
    std::string ToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    std::uint8_t ReadPixel(GDALDataset* dataset, const int x, const int y)
    {
        std::uint8_t value = 0;
        EXPECT_EQ(dataset->GetRasterBand(1)->RasterIO(
                      GF_Read, x, y, 1, 1, &value, 1, 1, GDT_Byte, 0, 0),
                  CE_None);
        return value;
    }

    std::string LastError(const GdalOperationResult& result)
    {
        return result.errors.empty() ? std::string{} : result.errors.back().message;
    }

    std::string LastError(const GdalAlgorithmRunner& runner)
    {
        return runner.Errors().empty() ? std::string{} : runner.Errors().back().message;
    }
}

TEST(GdalModernCapabilities, WritesCogWithPixelContentAndSourceOverviews)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input = test::MakeRaster(directory.Path());

    GdalDatasetPtr source(static_cast<GDALDataset*>(
        GDALOpen(ToUtf8(input).c_str(), GA_Update)));
    ASSERT_NE(source, nullptr);
    constexpr int kOverviewLevels[] = {2, 4};
    ASSERT_EQ(source->BuildOverviews("NEAREST", 2, kOverviewLevels, 0, nullptr,
                                     nullptr, nullptr),
              CE_None);
    source.reset();

    RasterCogOptions options;
    options.input = input;
    options.output = directory.Path() / "output-cog.tif";
    options.creationOptions = {"BLOCKSIZE=128", "COMPRESS=DEFLATE"};
    const GdalOperationResult result = WriteCog(options);
    ASSERT_TRUE(result.Succeeded()) << LastError(result);
    ASSERT_NE(result.outputDataset, nullptr);
    ASSERT_TRUE(std::filesystem::exists(options.output));

    GdalDatasetPtr cog(static_cast<GDALDataset*>(
        GDALOpen(ToUtf8(options.output).c_str(), GA_ReadOnly)));
    ASSERT_NE(cog, nullptr);
    const char* layout = cog->GetMetadataItem("LAYOUT", "IMAGE_STRUCTURE");
    ASSERT_NE(layout, nullptr);
    EXPECT_STREQ(layout, "COG");
    const char* compression = cog->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE");
    ASSERT_NE(compression, nullptr);
    EXPECT_STREQ(compression, "DEFLATE");
    int blockWidth = 0;
    int blockHeight = 0;
    cog->GetRasterBand(1)->GetBlockSize(&blockWidth, &blockHeight);
    EXPECT_EQ(blockWidth, 128);
    EXPECT_EQ(blockHeight, 128);
    EXPECT_GT(cog->GetRasterBand(1)->GetOverviewCount(), 0);
    EXPECT_EQ(ReadPixel(cog.get(), 0, 0), 42);
}

TEST(GdalModernCapabilities, PassesMemoryDatasetsBetweenPipelineSteps)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input = test::MakeRaster(directory.Path());

    GdalPipelineStep first;
    first.algorithmPath = {"raster", "convert"};
    first.arguments = {
        {"input", ToUtf8(input)},
        {"output-format", std::string("MEM")},
    };

    GdalPipelineStep second;
    second.algorithmPath = {"raster", "convert"};
    second.arguments = {{"output-format", std::string("MEM")}};

    const GdalOperationResult result = RunPipeline({first, second});
    ASSERT_TRUE(result.Succeeded()) << LastError(result);
    ASSERT_NE(result.outputDataset, nullptr);
    EXPECT_EQ(result.outputDataset->GetRasterXSize(), 12);
    EXPECT_EQ(result.outputDataset->GetRasterYSize(), 8);
    EXPECT_EQ(ReadPixel(result.outputDataset.get(), 0, 0), 42);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.Path()),
                            std::filesystem::directory_iterator{}),
              1);
}

TEST(GdalModernCapabilities, MosaicsRastersIntoAVrtDataset)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;

    RasterMosaicOptions options;
    options.inputs = {
        test::MakeRaster(directory.Path(), "left.tif", 0.0, 42),
        test::MakeRaster(directory.Path(), "right.tif", 12.0, 84),
    };
    options.output = directory.Path() / "mosaic.vrt";
    options.outputFormat = "VRT";

    GdalAlgorithmRunner runner("raster mosaic");
    ASSERT_TRUE(ConfigureGdalAlgorithm(runner, options.ToArgs()));
    ASSERT_TRUE(runner.Run()) << LastError(runner);
    GdalDatasetPtr mosaic = runner.TakeOutputDataset();
    ASSERT_NE(mosaic, nullptr);
    ASSERT_NE(mosaic->GetDriver(), nullptr);
    EXPECT_STREQ(mosaic->GetDriver()->GetDescription(), "VRT");
    EXPECT_EQ(mosaic->GetRasterXSize(), 24);
    EXPECT_EQ(ReadPixel(mosaic.get(), 0, 0), 42);
    EXPECT_EQ(ReadPixel(mosaic.get(), 12, 0), 84);
}

TEST(GdalModernCapabilities, ReadsPublicCogThroughVsiCurlWhenEnabled)
{
    const char* networkTests = std::getenv("MWG_RUN_NETWORK_GDAL_TESTS");
    if (networkTests == nullptr || std::string(networkTests) != "1")
        GTEST_SKIP() << "Set MWG_RUN_NETWORK_GDAL_TESTS=1 to enable network tests.";

    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    constexpr char kCogUrl[] =
        "/vsicurl/https://sentinel-cogs.s3.us-west-2.amazonaws.com/"
        "sentinel-s2-l2a-cogs/36/Q/WD/2020/7/"
        "S2A_36QWD_20200701_0_L2A/TCI.tif";
    const std::string vsiPath = ToUtf8(std::filesystem::path(kCogUrl));
    GdalDatasetPtr dataset(static_cast<GDALDataset*>(GDALOpen(vsiPath.c_str(), GA_ReadOnly)));
    ASSERT_NE(dataset, nullptr) << GdalEnvironment::LastErrorMessage();
    ASSERT_GT(dataset->GetRasterXSize(), 0);
    ASSERT_GT(dataset->GetRasterYSize(), 0);
    ASSERT_NE(dataset->GetRasterBand(1), nullptr);
    EXPECT_GT(dataset->GetRasterBand(1)->GetOverviewCount(), 0);
    (void)ReadPixel(dataset.get(), 0, 0);
}

TEST(GdalModernCapabilities, RejectsEmptyPipelineAndExplicitInputOnLaterStep)
{
    GdalEnvironment::Initialize();

    const GdalOperationResult empty = RunPipeline({});
    EXPECT_FALSE(empty.Succeeded());
    ASSERT_FALSE(empty.errors.empty());
    EXPECT_NE(empty.errors.back().message.find("at least one step"), std::string::npos);

    GdalPipelineStep first;
    first.algorithmPath = {"raster", "convert"};
    first.arguments = {
        {"input", std::string("not-opened-before-validation.tif")},
        {"output-format", std::string("MEM")},
    };
    GdalPipelineStep second;
    second.algorithmPath = {"raster", "convert"};
    second.arguments = {{"input", std::string("must-be-rejected")}};
    const GdalOperationResult explicitInput = RunPipeline({first, second});
    EXPECT_FALSE(explicitInput.Succeeded());
    ASSERT_FALSE(explicitInput.errors.empty());
    EXPECT_NE(explicitInput.errors.back().message.find("must omit the 'input'"),
              std::string::npos);
}
