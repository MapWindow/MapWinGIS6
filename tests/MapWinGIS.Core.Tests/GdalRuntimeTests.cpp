// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalRuntimeTests.cpp
 * @brief Tests GDAL foundation helpers for configuration, errors and progress.
 *
 * Exercises scoped configuration restoration, diagnostic capture, repeated
 * initialization, progress reporting, and cancellation through GDAL callbacks.
 */
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <cpl_error.h>
#include <gdal_priv.h>

#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/gdal/GdalRuntime.h"
#include "TestData.h"

using namespace mwg;

namespace
{
    using DatasetPtr = std::unique_ptr<GDALDataset, decltype([](GDALDataset* dataset)
    {
        if (dataset != nullptr)
            GDALClose(dataset);
    })>;

    DatasetPtr MakeSourceRaster()
    {
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("MEM");
        if (driver == nullptr)
            return {};
        return DatasetPtr(driver->Create("", 128, 128, 1, GDT_Byte, nullptr));
    }
}

TEST(GdalRuntime, RepeatedInitializationKeepsRegisteredDriversAvailable)
{
    GdalEnvironment::Initialize();
    const int driverCount = GdalEnvironment::RasterDriverCount();

    for (int i = 0; i < 3; ++i)
    {
        const GdalInitializer initializer;
        EXPECT_TRUE(GdalEnvironment::HasRasterDriver("GTiff"));
        EXPECT_EQ(GdalEnvironment::RasterDriverCount(), driverCount);
    }
}

TEST(GdalRuntime, ScopedConfigurationRestoresPreviousValue)
{
    constexpr std::string_view optionName = "MWG_TEST_SCOPED_CONFIG_OPTION";
    GdalConfig::SetOption(optionName, "before");

    {
        const ScopedGdalConfigOption option(optionName, "during");
        EXPECT_STREQ(CPLGetConfigOption("MWG_TEST_SCOPED_CONFIG_OPTION", nullptr), "during");
    }

    EXPECT_STREQ(CPLGetConfigOption("MWG_TEST_SCOPED_CONFIG_OPTION", nullptr), "before");
    GdalConfig::ClearOption(optionName);
}

TEST(GdalRuntime, ScopedConfigurationClearsPreviouslyUnsetOption)
{
    constexpr std::string_view optionName = "MWG_TEST_SCOPED_CONFIG_UNSET";
    GdalConfig::ClearOption(optionName);

    {
        const ScopedGdalConfigOption option(optionName, "temporary");
        EXPECT_STREQ(CPLGetConfigOption("MWG_TEST_SCOPED_CONFIG_UNSET", nullptr), "temporary");
    }

    EXPECT_EQ(CPLGetConfigOption("MWG_TEST_SCOPED_CONFIG_UNSET", nullptr), nullptr);
}

TEST(GdalRuntime, RejectsEmptyConfigurationName)
{
    EXPECT_THROW(GdalConfig::ClearOption(""), std::invalid_argument);
}

TEST(GdalRuntime, CapturesStructuredGdalErrors)
{
    GdalEnvironment::Initialize();
    GdalErrorScope errors;

    CPLError(CE_Failure, CPLE_AppDefined, "%s", "forced failure");

    ASSERT_EQ(errors.Errors().size(), 1);
    EXPECT_EQ(errors.Errors().front().errorClass, GdalErrorClass::Failure);
    EXPECT_EQ(errors.LastErrorCode(), CPLE_AppDefined);
    EXPECT_EQ(errors.LastErrorMessage(), "forced failure");
    EXPECT_NE(errors.DetailedErrorMessage().find("forced failure"), std::string::npos);
}

TEST(GdalRuntime, ReportsProgressFromStartToCompletion)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    mwg::test::TempDirectory directory;
    DatasetPtr source = MakeSourceRaster();
    ASSERT_NE(source, nullptr);

    GDALDriver* outputDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(outputDriver, nullptr);

    std::vector<double> progress;
    GdalProgressAdapter adapter([&progress](const double fraction, std::string_view)
    {
        progress.push_back(fraction);
        return true;
    });

    const std::string outputPath = (directory.Path() / "progress.tif").string();
    DatasetPtr output(outputDriver->CreateCopy(
        outputPath.c_str(), source.get(), 0, nullptr, adapter.Function(), adapter.UserData()));

    ASSERT_NE(output, nullptr);
    ASSERT_FALSE(progress.empty());
    EXPECT_DOUBLE_EQ(progress.front(), 0.0);
    EXPECT_DOUBLE_EQ(progress.back(), 1.0);
    EXPECT_FALSE(adapter.WasCancelled());
    adapter.RethrowCallbackException();
}

TEST(GdalRuntime, CancelsCopyWhenTokenIsRequested)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    mwg::test::TempDirectory directory;
    DatasetPtr source = MakeSourceRaster();
    ASSERT_NE(source, nullptr);

    GDALDriver* outputDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(outputDriver, nullptr);

    GdalCancellationToken cancellation;
    GdalProgressAdapter adapter({}, &cancellation);
    cancellation.Cancel();

    const std::string outputPath = (directory.Path() / "cancelled.tif").string();
    DatasetPtr output(outputDriver->CreateCopy(
        outputPath.c_str(), source.get(), 0, nullptr, adapter.Function(), adapter.UserData()));

    EXPECT_EQ(output, nullptr);
    EXPECT_TRUE(adapter.WasCancelled());
}

TEST(GdalRuntime, ProgressCallbackCancellationStopsOperation)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    mwg::test::TempDirectory directory;
    DatasetPtr source = MakeSourceRaster();
    ASSERT_NE(source, nullptr);

    GDALDriver* outputDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(outputDriver, nullptr);

    GdalProgressAdapter adapter([](double, std::string_view) { return false; });
    const std::string outputPath = (directory.Path() / "callback-cancelled.tif").string();
    DatasetPtr output(outputDriver->CreateCopy(
        outputPath.c_str(), source.get(), 0, nullptr, adapter.Function(), adapter.UserData()));

    EXPECT_EQ(output, nullptr);
    EXPECT_TRUE(adapter.WasCancelled());
}

TEST(GdalRuntime, RethrowsProgressCallbackExceptionAfterGdalReturns)
{
    GdalProgressAdapter adapter([](double, std::string_view) -> bool
    {
        throw std::runtime_error("callback failure");
    });

    EXPECT_EQ(adapter.Function()(0.0, "", adapter.UserData()), 0);
    EXPECT_TRUE(adapter.WasCancelled());
    EXPECT_THROW(adapter.RethrowCallbackException(), std::runtime_error);
}
