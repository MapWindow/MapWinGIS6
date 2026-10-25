// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalAlgorithmRunnerTests.cpp
 * @brief Tests generic GDAL algorithm configuration and execution.
 *
 * Covers typed and command-line arguments, structured failures, finalization,
 * output dataset ownership, and raster conversion equivalence.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gdal_priv.h>
#include <gdalalgorithm.h>

#include "mapwingis/gdal/GdalAlgorithmRunner.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "TestData.h"

using namespace mwg;

namespace
{
    using DatasetPtr = GdalDatasetPtr;

    std::string ToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    std::filesystem::path MakeRaster(const std::filesystem::path& directory,
                                     const std::string& fileName)
    {
        constexpr int kWidth = 32;
        constexpr int kHeight = 24;
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
        if (driver == nullptr)
            throw std::runtime_error("GTiff driver is not available");

        const std::filesystem::path path = directory / fileName;
        DatasetPtr dataset(driver->Create(ToUtf8(path).c_str(), kWidth, kHeight, 1, GDT_Byte, nullptr));
        if (dataset == nullptr)
            throw std::runtime_error("Could not create test raster");

        std::vector<std::uint8_t> pixels(kWidth * kHeight);
        for (std::size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = static_cast<std::uint8_t>((i * 17) % 251);

        if (dataset->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                                pixels.data(), kWidth, kHeight,
                                                GDT_Byte, 0, 0) != CE_None)
        {
            throw std::runtime_error("Could not write test raster");
        }

        dataset.reset();
        return path;
    }

    class TypedArgumentsAlgorithm final : public GDALAlgorithm
    {
    public:
        static constexpr const char* kName = "mwg-test-typed-arguments";

        TypedArgumentsAlgorithm()
            : GDALAlgorithm(kName, "Test algorithm runner argument types", "")
        {
            AddArg("text", 0, "String value", &_text);
            AddArg("enabled", 0, "Boolean value", &_enabled);
            AddArg("count", 0, "Integer value", &_count);
            AddArg("ratio", 0, "Real value", &_ratio);
            AddArg("values", 0, "Real list", &_values);
            AddArg("labels", 0, "String list", &_labels);
        }

    protected:
        bool RunImpl(GDALProgressFunc, void*) override
        {
            return _text == "configured" && _enabled && _count == 3 && _ratio == 1.5 &&
                   _values == std::vector<double>{1.0, 2.0} &&
                   _labels == std::vector<std::string>{"one", "two"};
        }

    private:
        std::string _text;
        bool _enabled = false;
        int _count = 0;
        double _ratio = 0.0;
        std::vector<double> _values;
        std::vector<std::string> _labels;
    };

    class FinalizeFailureAlgorithm final : public GDALAlgorithm
    {
    public:
        FinalizeFailureAlgorithm()
            : GDALAlgorithm("mwg-test-finalize-failure", "Test finalization failure", "")
        {
        }

        bool Finalize() override
        {
            ReportError(CE_Failure, CPLE_AppDefined, "%s", "forced finalization failure");
            return false;
        }

    protected:
        bool RunImpl(GDALProgressFunc, void*) override
        {
            return true;
        }
    };

    class ProgressAlgorithm final : public GDALAlgorithm
    {
    public:
        ProgressAlgorithm()
            : GDALAlgorithm("mwg-test-progress", "Test algorithm progress", "")
        {
        }

    protected:
        bool RunImpl(GDALProgressFunc progress, void* userData) override
        {
            return progress == nullptr ||
                   (progress(0.0, "start", userData) != 0 &&
                    progress(1.0, "complete", userData) != 0);
        }
    };

    class InvocationTrackingAlgorithm final : public GDALAlgorithm
    {
    public:
        static constexpr const char* kName = "mwg-test-invocation-tracking";
        static int runCount;

        InvocationTrackingAlgorithm()
            : GDALAlgorithm(kName, "Test algorithm invocation tracking", "")
        {
        }

    protected:
        bool RunImpl(GDALProgressFunc, void*) override
        {
            ++runCount;
            return true;
        }
    };

    int InvocationTrackingAlgorithm::runCount = 0;

    class DiagnosticsAlgorithm final : public GDALAlgorithm
    {
    public:
        static inline std::atomic_int active{0};
        static inline std::atomic_bool overlapped{false};

        DiagnosticsAlgorithm()
            : GDALAlgorithm("mwg-test-diagnostics", "Test isolated operation diagnostics", "")
        {
            AddArg("message", 0, "Diagnostic text", &_message);
            AddArg("fail", 0, "Fail after warning", &_fail);
            AddArg("failure-diagnostic", 0, "Emit a failure diagnostic", &_failureDiagnostic);
        }

    protected:
        bool RunImpl(GDALProgressFunc, void*) override
        {
            if (active.fetch_add(1) != 0)
                overlapped.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ReportError(_failureDiagnostic ? CE_Failure : CE_Warning,
                        CPLE_AppDefined, "%s", _message.c_str());
            active.fetch_sub(1);
            return !_fail;
        }

    private:
        std::string _message = "test warning";
        bool _fail = false;
        bool _failureDiagnostic = false;
    };

    class CallbackLifecycleAlgorithm final : public GDALAlgorithm
    {
    public:
        static inline int finalizeCount = 0;

        CallbackLifecycleAlgorithm()
            : GDALAlgorithm("mwg-test-callback-lifecycle", "Test callback cleanup", "")
        {
        }

        bool Finalize() override
        {
            ++finalizeCount;
            return true;
        }

    protected:
        bool RunImpl(GDALProgressFunc progress, void* userData) override
        {
            if (progress != nullptr)
                (void)progress(1.0, "complete", userData);
            return true;
        }
    };

    void RegisterTestAlgorithms()
    {
        static std::once_flag registrationFlag;
        std::call_once(registrationFlag, []
        {
            auto& registry = GDALGlobalAlgorithmRegistry::GetSingleton();
            registry.DeclareAlgorithm({"mwg-test", "typed-arguments"},
                                      [] { return std::make_unique<TypedArgumentsAlgorithm>(); });
            registry.DeclareAlgorithm({"mwg-test", "finalize-failure"},
                                      [] { return std::make_unique<FinalizeFailureAlgorithm>(); });
            registry.DeclareAlgorithm({"mwg-test", "progress"},
                                      [] { return std::make_unique<ProgressAlgorithm>(); });
            registry.DeclareAlgorithm({"mwg-test", "invocation-tracking"},
                                      [] { return std::make_unique<InvocationTrackingAlgorithm>(); });
            registry.DeclareAlgorithm({"mwg-test", "diagnostics"},
                                      [] { return std::make_unique<DiagnosticsAlgorithm>(); });
            registry.DeclareAlgorithm({"mwg-test", "callback-lifecycle"},
                                      [] { return std::make_unique<CallbackLifecycleAlgorithm>(); });
        });
    }

    std::vector<std::uint8_t> ReadPixels(GDALDataset* dataset)
    {
        const int width = dataset->GetRasterXSize();
        const int height = dataset->GetRasterYSize();
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * height));
        EXPECT_EQ(dataset->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, width, height,
                                                      pixels.data(), width, height,
                                                      GDT_Byte, 0, 0), CE_None);
        return pixels;
    }

    std::string LastError(const GdalAlgorithmRunner& runner)
    {
        return runner.Errors().empty() ? std::string{} : runner.Errors().back().message;
    }
}

TEST(GdalAlgorithmRunner, SetsAllSupportedArgumentTypes)
{
    GdalEnvironment::Initialize();
    RegisterTestAlgorithms();
    GdalAlgorithmRunner runner(std::vector<std::string>{"mwg-test", "typed-arguments"});

    ASSERT_TRUE(runner.SetArgument("text", "configured"));
    ASSERT_TRUE(runner.SetArgument("enabled", true));
    ASSERT_TRUE(runner.SetArgument("count", 3));
    ASSERT_TRUE(runner.SetArgument("ratio", 1.5));
    ASSERT_TRUE(runner.SetArgument("values", std::vector<double>{1.0, 2.0}));
    ASSERT_TRUE(runner.SetArgument("labels", std::vector<std::string>{"one", "two"}));
    EXPECT_TRUE(runner.Run()) << LastError(runner);
    EXPECT_TRUE(runner.Finalize());
}

TEST(GdalAlgorithmRunner, ExposesUsageMetadataAndReportsUnknownAlgorithms)
{
    GdalEnvironment::Initialize();

    GdalAlgorithmRunner known("raster convert");
    EXPECT_NE(known.UsageAsJson().find("input"), std::string::npos);

    GdalAlgorithmRunner unknown("raster algorithm-that-does-not-exist");
    EXPECT_TRUE(unknown.UsageAsJson().empty());
    ASSERT_FALSE(unknown.Errors().empty());
    EXPECT_NE(unknown.Errors().back().message.find("Unknown GDAL algorithm"), std::string::npos);
}

TEST(GdalAlgorithmRunner, ReportsInvalidArgumentsAndRejectsRepeatedRun)
{
    GdalEnvironment::Initialize();
    RegisterTestAlgorithms();
    GdalAlgorithmRunner invalid(std::vector<std::string>{"mwg-test", "typed-arguments"});
    EXPECT_FALSE(invalid.SetArgument("not-an-argument", "value"));
    ASSERT_FALSE(invalid.Errors().empty());
    EXPECT_FALSE(invalid.Run());

    GdalAlgorithmRunner runner(std::vector<std::string>{"mwg-test", "typed-arguments"});
    ASSERT_TRUE(runner.SetArgument("text", "configured"));
    ASSERT_TRUE(runner.SetArgument("enabled", true));
    ASSERT_TRUE(runner.SetArgument("count", 3));
    ASSERT_TRUE(runner.SetArgument("ratio", 1.5));
    ASSERT_TRUE(runner.SetArgument("values", std::vector<double>{1.0, 2.0}));
    ASSERT_TRUE(runner.SetArgument("labels", std::vector<std::string>{"one", "two"}));
    ASSERT_TRUE(runner.Run());
    EXPECT_FALSE(runner.Run());
    EXPECT_NE(runner.Errors().back().message.find("only be run once"), std::string::npos);
}

TEST(GdalAlgorithmRunner, ReportsFinalizationFailures)
{
    GdalEnvironment::Initialize();
    RegisterTestAlgorithms();
    GdalAlgorithmRunner runner(std::vector<std::string>{"mwg-test", "finalize-failure"});

    EXPECT_FALSE(runner.Run());
    ASSERT_FALSE(runner.Errors().empty());
    EXPECT_NE(runner.Errors().back().message.find("forced finalization failure"), std::string::npos);
    const auto result = runner.Result();
    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.WasCancelled());
    EXPECT_EQ(result.LastErrorCode(), CPLE_AppDefined);
    EXPECT_NE(result.ErrorMsg().find("forced finalization failure"), std::string::npos);
    EXPECT_NE(result.DetailedErrorMsg().find("class 3, code 1"), std::string::npos);
}

TEST(GdalAlgorithmRunner, ReportsProgressAndCancellation)
{
    GdalEnvironment::Initialize();
    RegisterTestAlgorithms();

    GdalAlgorithmRunner runner("mwg-test progress");
    std::vector<double> progress;
    ASSERT_TRUE(runner.Run([&progress](const double fraction, std::string_view)
    {
        progress.push_back(fraction);
        return true;
    }));
    ASSERT_FALSE(progress.empty());
    EXPECT_DOUBLE_EQ(progress.front(), 0.0);
    EXPECT_DOUBLE_EQ(progress.back(), 1.0);

    GdalAlgorithmRunner callbackCancelled("mwg-test progress");
    EXPECT_FALSE(callbackCancelled.Run([](double, std::string_view) { return false; }));
    ASSERT_FALSE(callbackCancelled.Errors().empty());
    EXPECT_NE(callbackCancelled.Errors().back().message.find("cancelled"), std::string::npos);

    GdalAlgorithmRunner cancelled("mwg-test progress");
    GdalCancellationToken cancellation;
    cancellation.Cancel();
    EXPECT_FALSE(cancelled.Run({}, &cancellation));
    EXPECT_TRUE(cancelled.Result().WasCancelled());
    ASSERT_FALSE(cancelled.Errors().empty());
    EXPECT_NE(cancelled.Errors().back().message.find("cancelled"), std::string::npos);
}

TEST(GdalAlgorithmRunner, LogsWarningsWithoutTurningSuccessIntoFailure)
{
    GdalEnvironment::Initialize();
    RegisterTestAlgorithms();
    std::vector<std::string> warnings;
    GdalEnvironment::SetLogHandler([&](const LogLevel level, const std::string_view message)
    {
        if (level == LogLevel::Warning)
            warnings.emplace_back(message);
    });

    GdalAlgorithmRunner runner("mwg-test diagnostics");
    const bool succeeded = runner.Run();
    GdalEnvironment::SetLogHandler({});

    EXPECT_TRUE(succeeded);
    const auto result = runner.Result();
    EXPECT_TRUE(result.Succeeded());
    EXPECT_FALSE(result.WasCancelled());
    EXPECT_EQ(result.LastErrorCode(), 0);
    EXPECT_TRUE(result.ErrorMsg().empty());
    ASSERT_EQ(result.AllErrors().size(), 1);
    EXPECT_EQ(result.AllErrors().front().errorClass, GdalErrorClass::Warning);
    ASSERT_EQ(warnings.size(), 1);
    EXPECT_NE(warnings.front().find("test warning"), std::string::npos);
}

TEST(GdalAlgorithmRunner, AddsFailureDiagnosticWhenGdalOnlyEmitsWarnings)
{
    GdalEnvironment::Initialize();
    RegisterTestAlgorithms();
    GdalAlgorithmRunner runner("mwg-test diagnostics");
    ASSERT_TRUE(runner.SetArgument("fail", true));
    EXPECT_FALSE(runner.Run());
    const auto result = runner.Result();
    EXPECT_EQ(result.LastErrorCode(), CPLE_AppDefined);
    EXPECT_NE(result.ErrorMsg().find("execution failed"), std::string::npos);
    ASSERT_EQ(result.AllErrors().size(), 2);
    EXPECT_EQ(result.AllErrors().front().errorClass, GdalErrorClass::Warning);
}

TEST(GdalAlgorithmRunner, DoesNotReportSuccessWithFailureDiagnostics)
{
    GdalEnvironment::Initialize();
    RegisterTestAlgorithms();
    GdalAlgorithmRunner runner("mwg-test diagnostics");
    ASSERT_TRUE(runner.SetArgument("failure-diagnostic", true));
    EXPECT_FALSE(runner.Run());
    EXPECT_TRUE(runner.Finalize());
    EXPECT_FALSE(runner.Result().Succeeded());
    EXPECT_EQ(runner.Result().LastErrorCode(), CPLE_AppDefined);
}

TEST(GdalAlgorithmRunner, FinalizesSuccessfulGdalRunEvenWhenCallbackCancelsOrThrows)
{
    GdalEnvironment::Initialize();
    RegisterTestAlgorithms();
    CallbackLifecycleAlgorithm::finalizeCount = 0;

    GdalAlgorithmRunner cancelled("mwg-test callback-lifecycle");
    EXPECT_FALSE(cancelled.Run([](double, std::string_view) { return false; }));
    EXPECT_TRUE(cancelled.Result().WasCancelled());
    EXPECT_EQ(CallbackLifecycleAlgorithm::finalizeCount, 1);
    EXPECT_TRUE(cancelled.Finalize());
    EXPECT_EQ(CallbackLifecycleAlgorithm::finalizeCount, 1);

    GdalAlgorithmRunner failed("mwg-test callback-lifecycle");
    EXPECT_FALSE(failed.Run([](double, std::string_view) -> bool
    {
        throw std::runtime_error("progress failed");
    }));
    EXPECT_FALSE(failed.Result().WasCancelled());
    EXPECT_NE(failed.Result().ErrorMsg().find("progress failed"), std::string::npos);
    EXPECT_EQ(CallbackLifecycleAlgorithm::finalizeCount, 2);

    GdalAlgorithmRunner nonStandard("mwg-test callback-lifecycle");
    EXPECT_FALSE(nonStandard.Run([](double, std::string_view) -> bool { throw 42; }));
    EXPECT_FALSE(nonStandard.Result().WasCancelled());
    EXPECT_NE(nonStandard.Result().ErrorMsg().find("non-standard exception"), std::string::npos);
    EXPECT_EQ(CallbackLifecycleAlgorithm::finalizeCount, 3);
}

TEST(GdalAlgorithmRunner, RejectsCallbackReentryAndKeepsCallbacksOnTheExecutingThread)
{
    GdalEnvironment::Initialize();
    RegisterTestAlgorithms();
    GdalAlgorithmRunner outer("mwg-test progress");
    GdalAlgorithmRunner nested("mwg-test progress");
    const auto caller = std::this_thread::get_id();
    std::thread::id callbackThread;
    EXPECT_FALSE(outer.Run([&](double, std::string_view)
    {
        callbackThread = std::this_thread::get_id();
        return nested.Run();
    }));
    EXPECT_EQ(callbackThread, caller);
    EXPECT_FALSE(outer.Result().WasCancelled());
    EXPECT_NE(outer.Result().ErrorMsg().find("cannot be called from a GDAL callback"),
              std::string::npos);
    EXPECT_TRUE(nested.Run());
}

TEST(GdalAlgorithmRunner, SerializesConcurrentRunnersAndIsolatesTheirResults)
{
    GdalEnvironment::Initialize();
    RegisterTestAlgorithms();
    DiagnosticsAlgorithm::overlapped.store(false);
    std::barrier start(2);
    std::array<GdalOperationResult, 2> results;
    const auto execute = [&](const std::size_t index)
    {
        GdalAlgorithmRunner runner("mwg-test diagnostics");
        (void)runner.SetArgument("message", index == 0 ? "first operation" : "second operation");
        (void)runner.SetArgument("fail", index == 1);
        start.arrive_and_wait();
        (void)runner.Run();
        results[index] = runner.Result();
    };
    std::thread first(execute, 0);
    std::thread second(execute, 1);
    first.join();
    second.join();

    EXPECT_FALSE(DiagnosticsAlgorithm::overlapped.load());
    EXPECT_TRUE(results[0].Succeeded());
    EXPECT_FALSE(results[1].Succeeded());
    EXPECT_EQ(results[0].AllErrors().size(), 1);
    EXPECT_EQ(results[1].AllErrors().size(), 2);
    EXPECT_EQ(results[0].DetailedErrorMsg().find("second operation"), std::string::npos);
    EXPECT_EQ(results[1].DetailedErrorMsg().find("first operation"), std::string::npos);
}

TEST(GdalAlgorithmRunner, PreservesLoggerExceptionsUntilAfterFinalization)
{
    GdalEnvironment::Initialize();
    RegisterTestAlgorithms();
    GdalEnvironment::SetLogHandler([](LogLevel, std::string_view)
    {
        throw std::runtime_error("logger failed");
    });
    GdalAlgorithmRunner runner("mwg-test diagnostics");
    EXPECT_THROW((void)runner.Run(), std::runtime_error);
    GdalEnvironment::SetLogHandler({});
    EXPECT_TRUE(runner.Finalize());
    EXPECT_FALSE(runner.Result().Succeeded());
    EXPECT_FALSE(runner.Result().AllErrors().empty());
}

TEST(GdalAlgorithmRunner, RejectsPreCancelledRunBeforeInvokingAlgorithm)
{
    GdalEnvironment::Initialize();
    RegisterTestAlgorithms();
    InvocationTrackingAlgorithm::runCount = 0;

    GdalAlgorithmRunner runner("mwg-test invocation-tracking");
    GdalCancellationToken cancellation;
    cancellation.Cancel();

    EXPECT_FALSE(runner.Run({}, &cancellation));
    EXPECT_EQ(InvocationTrackingAlgorithm::runCount, 0);
    ASSERT_FALSE(runner.Errors().empty());
    EXPECT_NE(runner.Errors().back().message.find("cancelled"), std::string::npos);
}

TEST(GdalAlgorithmRunner, ConvertsGeoTiffEquivalentlyFromTypedAndCommandLineArguments)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    mwg::test::TempDirectory directory;
    const std::filesystem::path input = MakeRaster(directory.Path(), "input.tif");
    const std::filesystem::path typedOutput = directory.Path() / "typed.png";
    const std::filesystem::path parsedOutput = directory.Path() / "parsed.png";

    GdalDatasetPtr output;
    {
        GdalAlgorithmRunner typed("raster convert");
        ASSERT_TRUE(typed.SetArgument("input", ToUtf8(input)));
        ASSERT_TRUE(typed.SetArgument("output", ToUtf8(typedOutput)));
        ASSERT_TRUE(typed.SetArgument("output-format", "PNG"));
        ASSERT_TRUE(typed.Run()) << LastError(typed);
        output = typed.TakeOutputDataset();
    }
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(output->GetRasterXSize(), 32);
    EXPECT_EQ(output->GetRasterYSize(), 24);
    output.reset();

    GdalAlgorithmRunner parsed("raster convert");
    ASSERT_TRUE(parsed.ParseCommandLineArgs({
        "--input", ToUtf8(input),
        "--output", ToUtf8(parsedOutput),
        "--output-format", "PNG"
    })) << LastError(parsed);
    ASSERT_TRUE(parsed.Run()) << LastError(parsed);

    DatasetPtr typedDataset(static_cast<GDALDataset*>(
                                GDALOpen(ToUtf8(typedOutput).c_str(), GA_ReadOnly)));
    DatasetPtr parsedDataset(static_cast<GDALDataset*>(
                                 GDALOpen(ToUtf8(parsedOutput).c_str(), GA_ReadOnly)));
    ASSERT_NE(typedDataset, nullptr);
    ASSERT_NE(parsedDataset, nullptr);
    EXPECT_EQ(typedDataset->GetRasterXSize(), parsedDataset->GetRasterXSize());
    EXPECT_EQ(typedDataset->GetRasterYSize(), parsedDataset->GetRasterYSize());
    EXPECT_EQ(ReadPixels(typedDataset.get()), ReadPixels(parsedDataset.get()));
}
