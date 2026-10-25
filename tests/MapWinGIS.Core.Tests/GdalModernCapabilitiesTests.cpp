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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <cpl_error.h>
#include <gdal_priv.h>
#include <gdalalgorithm.h>
#include <vrtdataset.h>

#include "TestData.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/gdal/GdalModern.h"

using namespace mwg;

TEST(GdalModernCapabilities, ExposesStructuredCancellationForCogAndPipelineOperations)
{
    GdalEnvironment::Initialize();
    test::TempDirectory directory;
    const auto input = test::MakeRaster(directory.Path());
    RasterCogOptions options;
    options.input = input;
    options.output = directory.Path() / "cancelled.tif";
    GdalCancellationToken token;
    token.Cancel();
    const auto cog = WriteCog(options, {}, &token);
    EXPECT_FALSE(cog.Succeeded());
    EXPECT_TRUE(cog.WasCancelled());
    EXPECT_NE(cog.LastErrorCode(), 0);
    EXPECT_FALSE(cog.ErrorMsg().empty());
    EXPECT_EQ(cog.outputDataset, nullptr);
    EXPECT_FALSE(std::filesystem::exists(options.output));

    RasterConvertOptions convert;
    convert.input = input;
    convert.outputFormat = "MEM";
    const auto pipeline = RunPipeline(
        {{{"raster", "convert"}, convert.ToArgs()}}, {}, &token);
    EXPECT_FALSE(pipeline.Succeeded());
    EXPECT_TRUE(pipeline.WasCancelled());
    EXPECT_NE(pipeline.LastErrorCode(), 0);
    EXPECT_EQ(pipeline.outputDataset, nullptr);
    EXPECT_TRUE(pipeline.retainedDatasets.empty());

    const auto invalid = RunPipeline({});
    EXPECT_FALSE(invalid.Succeeded());
    EXPECT_FALSE(invalid.WasCancelled());
    EXPECT_NE(invalid.ErrorMsg().find("at least one step"), std::string::npos);
}

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

    class PipelineOutputAlgorithm final : public GDALAlgorithm
    {
    public:
        static constexpr const char* kName = "mwg-test-pipeline-output";
        static GDALDataset* outputDataset;

        PipelineOutputAlgorithm()
            : GDALAlgorithm(kName, "Test pipeline intermediate dataset ownership", "")
        {
            AddArg("output", 0, "Output dataset", &_output);
        }

    protected:
        bool RunImpl(GDALProgressFunc, void*) override
        {
            if (outputDataset == nullptr)
            {
                ReportError(CE_Failure, CPLE_AppDefined, "%s",
                            "pipeline test output dataset was not configured");
                return false;
            }
            _output.Set(outputDataset);
            return true;
        }

    private:
        GDALArgDatasetValue _output;
    };

    GDALDataset* PipelineOutputAlgorithm::outputDataset = nullptr;

    class PipelineVrtAlgorithm final : public GDALAlgorithm
    {
    public:
        static constexpr const char* kName = "mwg-test-pipeline-vrt";

        PipelineVrtAlgorithm()
            : GDALAlgorithm(kName, "Test pipeline VRT source ownership", "")
        {
            AddInputDatasetArg(&_input);
            AddArg("output", 0, "Output dataset", &_output);
        }

    protected:
        bool RunImpl(GDALProgressFunc, void*) override
        {
            GDALDataset* source = _input.GetDatasetRef();
            if (source == nullptr || source->GetRasterCount() == 0)
            {
                ReportError(CE_Failure, CPLE_AppDefined, "%s",
                            "pipeline VRT test input is unavailable");
                return false;
            }

            auto vrt = VRTDataset::CreateVRTDataset(
                "", source->GetRasterXSize(), source->GetRasterYSize(), 1,
                source->GetRasterBand(1)->GetRasterDataType(), nullptr);
            auto* band = vrt != nullptr
                             ? dynamic_cast<VRTSourcedRasterBand*>(vrt->GetRasterBand(1))
                             : nullptr;
            if (band == nullptr ||
                band->AddSimpleSource(source->GetRasterBand(1)) != CE_None)
            {
                ReportError(CE_Failure, CPLE_AppDefined, "%s",
                            "could not create pipeline VRT source");
                return false;
            }

            _output.Set(std::unique_ptr<GDALDataset>(vrt.release()));
            return true;
        }

    private:
        GDALArgDatasetValue _input;
        GDALArgDatasetValue _output;
    };

    class PipelineFailureAlgorithm final : public GDALAlgorithm
    {
    public:
        static constexpr const char* kName = "mwg-test-pipeline-failure";
        static int runCount;

        PipelineFailureAlgorithm()
            : GDALAlgorithm(kName, "Test pipeline failure diagnostics", "")
        {
            AddInputDatasetArg(&_input);
        }

    protected:
        bool RunImpl(GDALProgressFunc, void*) override
        {
            ++runCount;
            ReportError(CE_Failure, CPLE_AppDefined, "%s",
                        "forced failure in later pipeline step");
            return false;
        }

    private:
        GDALArgDatasetValue _input;
    };

    int PipelineFailureAlgorithm::runCount = 0;

    class PipelineAfterFailureAlgorithm final : public GDALAlgorithm
    {
    public:
        static constexpr const char* kName = "mwg-test-pipeline-after-failure";
        static int runCount;

        PipelineAfterFailureAlgorithm()
            : GDALAlgorithm(kName, "Test pipeline short-circuiting", "")
        {
            AddInputDatasetArg(&_input);
        }

    protected:
        bool RunImpl(GDALProgressFunc, void*) override
        {
            ++runCount;
            return true;
        }

    private:
        GDALArgDatasetValue _input;
    };

    int PipelineAfterFailureAlgorithm::runCount = 0;

    void RegisterPipelineTestAlgorithms()
    {
        static std::once_flag registrationFlag;
        std::call_once(registrationFlag, []
        {
            auto& registry = GDALGlobalAlgorithmRegistry::GetSingleton();
            registry.DeclareAlgorithm(
                {"mwg-test", "pipeline-output"},
                [] { return std::make_unique<PipelineOutputAlgorithm>(); });
            registry.DeclareAlgorithm(
                {"mwg-test", "pipeline-vrt"},
                [] { return std::make_unique<PipelineVrtAlgorithm>(); });
            registry.DeclareAlgorithm(
                {"mwg-test", "pipeline-failure"},
                [] { return std::make_unique<PipelineFailureAlgorithm>(); });
            registry.DeclareAlgorithm(
                {"mwg-test", "pipeline-after-failure"},
                [] { return std::make_unique<PipelineAfterFailureAlgorithm>(); });
        });
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

TEST(GdalModernCapabilities, KeepsIntermediateDatasetsAliveForPipelineVrtOutput)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    RegisterPipelineTestAlgorithms();
    test::TempDirectory directory;
    const std::filesystem::path input = test::MakeRaster(directory.Path());

    GdalPipelineStep first;
    first.algorithmPath = {"raster", "convert"};
    first.arguments = {
        {"input", ToUtf8(input)},
        {"output-format", std::string("MEM")},
    };
    GdalPipelineStep second;
    second.algorithmPath = {"mwg-test", "pipeline-vrt"};

    const GdalOperationResult result = RunPipeline({first, second});
    ASSERT_TRUE(result.Succeeded()) << LastError(result);
    ASSERT_NE(result.outputDataset, nullptr);
    ASSERT_EQ(result.retainedDatasets.size(), 1u);
    EXPECT_EQ(ReadPixel(result.outputDataset.get(), 0, 0), 42);
    EXPECT_EQ(ReadPixel(result.retainedDatasets.front().get(), 0, 0), 42);
}

TEST(GdalModernCapabilities, RunnerRetainsDatasetArgumentUntilRunAndReleasesIt)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input = test::MakeRaster(directory.Path());

    GdalDatasetPtr source(static_cast<GDALDataset*>(
        GDALOpen(ToUtf8(input).c_str(), GA_ReadOnly)));
    ASSERT_NE(source, nullptr);
    GDALDataset* retainedSource = source.get();
    const int initialReferenceCount = retainedSource->GetRefCount();
    retainedSource->Reference();
    GdalDatasetPtr referenceGuard(retainedSource);

    GdalDatasetPtr output;
    {
        GdalAlgorithmRunner runner("raster convert");
        ASSERT_TRUE(runner.SetDatasetArgument("input", source.get()));
        ASSERT_TRUE(runner.SetArgument("output-format", "MEM"));
        EXPECT_EQ(retainedSource->GetRefCount(), initialReferenceCount + 2);

        source.reset();
        EXPECT_EQ(retainedSource->GetRefCount(), initialReferenceCount + 1);
        ASSERT_TRUE(runner.Run()) << LastError(runner);
        output = runner.TakeOutputDataset();
    }

    ASSERT_NE(output, nullptr);
    EXPECT_EQ(ReadPixel(output.get(), 0, 0), 42);
    output.reset();
    EXPECT_EQ(retainedSource->GetRefCount(), initialReferenceCount);
}

TEST(GdalModernCapabilities, RunnerRetainsDatasetListArgumentUntilRunAndReleasesIt)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input = test::MakeRaster(directory.Path());

    GdalDatasetPtr source(static_cast<GDALDataset*>(
        GDALOpen(ToUtf8(input).c_str(), GA_ReadOnly)));
    ASSERT_NE(source, nullptr);
    GDALDataset* retainedSource = source.get();
    const int initialReferenceCount = retainedSource->GetRefCount();
    retainedSource->Reference();
    GdalDatasetPtr referenceGuard(retainedSource);

    const std::filesystem::path mosaicPath = directory.Path() / "mosaic.vrt";
    GdalDatasetPtr output;
    {
        GdalAlgorithmRunner runner("raster mosaic");
        ASSERT_TRUE(runner.SetDatasetArgument("input", source.get()));
        ASSERT_TRUE(runner.SetArgument("output", ToUtf8(mosaicPath)));
        ASSERT_TRUE(runner.SetArgument("output-format", "VRT"));
        EXPECT_EQ(retainedSource->GetRefCount(), initialReferenceCount + 2);

        source.reset();
        EXPECT_EQ(retainedSource->GetRefCount(), initialReferenceCount + 1);
        ASSERT_TRUE(runner.Run()) << LastError(runner);
        output = runner.TakeOutputDataset();
    }

    ASSERT_NE(output, nullptr);
    EXPECT_EQ(ReadPixel(output.get(), 0, 0), 42);
    output.reset();
    EXPECT_EQ(retainedSource->GetRefCount(), initialReferenceCount);
}

TEST(GdalModernCapabilities, PipelineFailureReportsDiagnosticsAndReleasesIntermediateDataset)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    RegisterPipelineTestAlgorithms();

    test::TempDirectory directory;
    const std::filesystem::path input = test::MakeRaster(directory.Path());
    GdalDatasetPtr source(static_cast<GDALDataset*>(
        GDALOpen(ToUtf8(input).c_str(), GA_ReadOnly)));
    ASSERT_NE(source, nullptr);
    GDALDataset* retainedSource = source.get();
    retainedSource->Reference();
    GdalDatasetPtr referenceGuard(retainedSource);
    const int expectedReferenceCount = retainedSource->GetRefCount();

    PipelineOutputAlgorithm::outputDataset = retainedSource;
    PipelineFailureAlgorithm::runCount = 0;
    PipelineAfterFailureAlgorithm::runCount = 0;

    GdalPipelineStep first;
    first.algorithmPath = {"mwg-test", "pipeline-output"};
    GdalPipelineStep failure;
    failure.algorithmPath = {"mwg-test", "pipeline-failure"};
    GdalPipelineStep afterFailure;
    afterFailure.algorithmPath = {"mwg-test", "pipeline-after-failure"};

    const GdalOperationResult result = RunPipeline({first, failure, afterFailure});
    PipelineOutputAlgorithm::outputDataset = nullptr;

    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(PipelineFailureAlgorithm::runCount, 1);
    EXPECT_EQ(PipelineAfterFailureAlgorithm::runCount, 0);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_NE(LastError(result).find("forced failure in later pipeline step"),
              std::string::npos);
    EXPECT_EQ(retainedSource->GetRefCount(), expectedReferenceCount);
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
