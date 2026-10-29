// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalOperationDispatcherTests.cpp
 * @brief Tests asynchronous GDAL execution and owner-thread notification delivery.
 */
#include <gtest/gtest.h>

#include <gdal_priv.h>
#include <gdalalgorithm.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include "GdalOperationDispatcher.h"
#include "TestData.h"
#include "mapwingis/gdal/GdalAlgorithmRunner.h"
#include "mapwingis/gdal/GdalEnvironment.h"

namespace
{
    using namespace std::chrono_literals;

    [[nodiscard]] std::string ToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    struct DispatchSignal
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool notified = false;

        static bool Wake(void* context) noexcept
        {
            auto& signal = *static_cast<DispatchSignal*>(context);
            {
                std::lock_guard lock(signal.mutex);
                signal.notified = true;
            }
            signal.condition.notify_one();
            return true;
        }
    };

    struct FailedDispatchSignal
    {
        std::atomic<int> attempts = 0;

        static bool Wake(void* context) noexcept
        {
            auto& signal = *static_cast<FailedDispatchSignal*>(context);
            ++signal.attempts;
            return false;
        }
    };

    class DispatcherProgressAlgorithm final : public GDALAlgorithm
    {
    public:
        DispatcherProgressAlgorithm()
            : GDALAlgorithm(
                  "mwg-test-dispatcher-progress",
                  "Test ActiveX operation progress dispatch",
                  "")
        {
        }

    protected:
        bool RunImpl(GDALProgressFunc progress, void* userData) override
        {
            for (int step = 0; step <= 100; ++step)
            {
                if (progress != nullptr &&
                    progress(
                        static_cast<double>(step) / 100.0,
                        "dispatcher progress",
                        userData) == 0)
                {
                    return false;
                }
                std::this_thread::sleep_for(2ms);
            }
            return true;
        }
    };

    class DispatcherDiagnosticsAlgorithm final : public GDALAlgorithm
    {
    public:
        DispatcherDiagnosticsAlgorithm()
            : GDALAlgorithm(
                  "mwg-test-dispatcher-diagnostics",
                  "Test ActiveX operation diagnostics",
                  "")
        {
            AddArg("fail", 0, "Fail after a warning", &_fail);
        }

    protected:
        bool RunImpl(GDALProgressFunc, void*) override
        {
            ReportError(CE_Warning, CPLE_AppDefined, "%s", "dispatcher warning");
            if (_fail)
            {
                ReportError(CE_Failure, CPLE_AppDefined, "%s", "dispatcher failure");
                return false;
            }
            return true;
        }

    private:
        bool _fail = false;
    };

    void RegisterDispatcherAlgorithms()
    {
        static std::once_flag registrationFlag;
        std::call_once(registrationFlag, []
        {
            GDALGlobalAlgorithmRegistry::GetSingleton().DeclareAlgorithm(
                {"mwg-test-dispatcher", "progress"},
                [] { return std::make_unique<DispatcherProgressAlgorithm>(); });
            GDALGlobalAlgorithmRegistry::GetSingleton().DeclareAlgorithm(
                {"mwg-test-dispatcher", "diagnostics"},
                [] { return std::make_unique<DispatcherDiagnosticsAlgorithm>(); });
        });
    }

    [[nodiscard]] std::vector<mwg::activex::GdalOperationNotification>
    WaitForNotifications(
        mwg::activex::GdalOperationDispatcher& dispatcher,
        DispatchSignal& signal,
        const std::chrono::steady_clock::time_point deadline)
    {
        {
            std::unique_lock lock(signal.mutex);
            if (!signal.notified)
            {
                signal.condition.wait_until(lock, deadline, [&signal]
                {
                    return signal.notified;
                });
            }
            signal.notified = false;
        }
        return dispatcher.DrainNotifications();
    }

    [[nodiscard]] bool WaitForCompletion(
        mwg::activex::GdalOperationDispatcher& dispatcher,
        DispatchSignal& signal,
        const std::uint64_t operationId,
        const std::chrono::steady_clock::time_point deadline,
        std::vector<mwg::activex::GdalOperationNotification>* notifications = nullptr)
    {
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto batch = WaitForNotifications(dispatcher, signal, deadline);
            for (auto& notification : batch)
            {
                const bool completed =
                    notification.operationId == operationId &&
                    notification.kind ==
                        mwg::activex::GdalNotificationKind::Completed;
                if (notifications != nullptr)
                {
                    notifications->push_back(std::move(notification));
                }
                if (completed)
                {
                    return true;
                }
            }
        }
        return false;
    }
}

TEST(GdalOperationDispatcher, RunsRasterConversionAndDeliversCompletionOnOwnerThread)
{
    mwg::GdalEnvironment::Initialize();
    mwg::test::TempDirectory directory;
    const std::filesystem::path input =
        mwg::test::MakeRaster(directory.Path(), "source.tif");
    const std::filesystem::path output = directory.Path() / "converted.png";
    const std::thread::id ownerThread = std::this_thread::get_id();

    DispatchSignal signal;
    mwg::activex::GdalOperationDispatcher dispatcher;
    dispatcher.SetWakeCallback(&DispatchSignal::Wake, &signal);
    const std::uint64_t operationId = dispatcher.Begin(
        "raster convert",
        {
            "--input", ToUtf8(input),
            "--output", ToUtf8(output),
            "--output-format", "PNG",
        });

    bool completedOnOwnerThread = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!completedOnOwnerThread &&
           std::chrono::steady_clock::now() < deadline)
    {
        for (const auto& notification :
             WaitForNotifications(dispatcher, signal, deadline))
        {
            EXPECT_EQ(notification.operationId, operationId);
            if (notification.kind ==
                mwg::activex::GdalNotificationKind::Completed)
            {
                completedOnOwnerThread = std::this_thread::get_id() == ownerThread;
            }
        }
    }
    ASSERT_TRUE(completedOnOwnerThread);

    mwg::activex::GdalOperationSnapshot result;
    ASSERT_EQ(
        dispatcher.End(operationId, result),
        mwg::activex::GdalEndResult::Ended);
    ASSERT_TRUE(result.succeeded) << result.diagnostics;
    EXPECT_FALSE(result.cancelled);

    mwg::GdalDatasetPtr converted(static_cast<GDALDataset*>(
        GDALOpen(ToUtf8(output).c_str(), GA_ReadOnly)));
    ASSERT_NE(converted, nullptr);
    std::uint8_t pixel = 0;
    ASSERT_EQ(
        converted->GetRasterBand(1)->RasterIO(
            GF_Read,
            0,
            0,
            1,
            1,
            &pixel,
            1,
            1,
            GDT_Byte,
            0,
            0),
        CE_None);
    EXPECT_EQ(pixel, 42);
}

TEST(GdalOperationDispatcher, CancellationIsIdempotentAndReportedSeparately)
{
    mwg::GdalEnvironment::Initialize();
    RegisterDispatcherAlgorithms();

    DispatchSignal signal;
    mwg::activex::GdalOperationDispatcher dispatcher;
    dispatcher.SetWakeCallback(&DispatchSignal::Wake, &signal);
    const std::uint64_t operationId =
        dispatcher.Begin("mwg-test-dispatcher progress", {});

    bool sawProgress = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!sawProgress && std::chrono::steady_clock::now() < deadline)
    {
        for (const auto& notification :
             WaitForNotifications(dispatcher, signal, deadline))
        {
            if (notification.kind ==
                mwg::activex::GdalNotificationKind::Progress)
            {
                sawProgress = true;
            }
        }
    }
    ASSERT_TRUE(sawProgress);
    EXPECT_EQ(
        dispatcher.Cancel(operationId),
        mwg::activex::GdalCancelResult::Requested);
    EXPECT_EQ(
        dispatcher.Cancel(operationId),
        mwg::activex::GdalCancelResult::AlreadyRequested);

    bool completed = false;
    while (!completed && std::chrono::steady_clock::now() < deadline)
    {
        for (const auto& notification :
             WaitForNotifications(dispatcher, signal, deadline))
        {
            if (notification.kind ==
                mwg::activex::GdalNotificationKind::Completed)
            {
                completed = true;
            }
        }
    }
    ASSERT_TRUE(completed);

    mwg::activex::GdalOperationSnapshot result;
    ASSERT_EQ(
        dispatcher.End(operationId, result),
        mwg::activex::GdalEndResult::Ended);
    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(result.cancelled);
}

TEST(GdalOperationDispatcher, PreservesWarningsAndReportsInvalidAndFailedOperations)
{
    mwg::GdalEnvironment::Initialize();
    RegisterDispatcherAlgorithms();

    DispatchSignal signal;
    mwg::activex::GdalOperationDispatcher dispatcher;
    dispatcher.SetWakeCallback(&DispatchSignal::Wake, &signal);

    const auto runToCompletion = [&](const std::string& path,
                                     const std::vector<std::string>& arguments)
    {
        const std::uint64_t operationId = dispatcher.Begin(path, arguments);
        EXPECT_TRUE(WaitForCompletion(
            dispatcher,
            signal,
            operationId,
            std::chrono::steady_clock::now() + 10s));
        mwg::activex::GdalOperationSnapshot result;
        EXPECT_EQ(
            dispatcher.End(operationId, result),
            mwg::activex::GdalEndResult::Ended);
        return result;
    };

    const auto invalid = runToCompletion("raster convert", {"--unknown-option"});
    EXPECT_FALSE(invalid.succeeded);
    EXPECT_FALSE(invalid.diagnostics.empty());

    const auto warning = runToCompletion(
        "mwg-test-dispatcher diagnostics",
        {});
    EXPECT_TRUE(warning.succeeded) << warning.diagnostics;
    ASSERT_EQ(warning.warnings.size(), 1u);
    EXPECT_NE(
        warning.warnings.front().find("dispatcher warning"),
        std::string::npos);

    const auto failure = runToCompletion(
        "mwg-test-dispatcher diagnostics",
        {"--fail"});
    EXPECT_FALSE(failure.succeeded);
    EXPECT_FALSE(failure.cancelled);
    EXPECT_NE(failure.errorCode, 0);
    EXPECT_NE(failure.diagnostics.find("dispatcher failure"), std::string::npos);
}

TEST(GdalOperationDispatcher, CoalescesProgressAndKeepsConcurrentOperationIdsDistinct)
{
    mwg::GdalEnvironment::Initialize();
    RegisterDispatcherAlgorithms();

    DispatchSignal signal;
    mwg::activex::GdalOperationDispatcher dispatcher;
    dispatcher.SetWakeCallback(&DispatchSignal::Wake, &signal);
    const std::uint64_t first = dispatcher.Begin(
        "mwg-test-dispatcher progress",
        {});
    const std::uint64_t second = dispatcher.Begin(
        "mwg-test-dispatcher progress",
        {});
    ASSERT_NE(first, second);

    std::unordered_map<std::uint64_t, int> completions;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while ((completions[first] == 0 || completions[second] == 0) &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::unordered_map<std::uint64_t, bool> progressInBatch;
        for (const auto& notification :
             WaitForNotifications(dispatcher, signal, deadline))
        {
            ASSERT_TRUE(notification.operationId == first ||
                        notification.operationId == second);
            if (notification.kind ==
                mwg::activex::GdalNotificationKind::Progress)
            {
                EXPECT_FALSE(progressInBatch[notification.operationId]);
                progressInBatch[notification.operationId] = true;
            }
            else
            {
                ++completions[notification.operationId];
            }
        }
    }
    EXPECT_EQ(completions[first], 1);
    EXPECT_EQ(completions[second], 1);

    mwg::activex::GdalOperationSnapshot result;
    EXPECT_EQ(dispatcher.End(first, result), mwg::activex::GdalEndResult::Ended);
    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(dispatcher.End(second, result), mwg::activex::GdalEndResult::Ended);
    EXPECT_TRUE(result.succeeded);
}

TEST(GdalOperationDispatcher, ShutdownCancelsWorkersWithoutWaitingForOwnerDispatch)
{
    mwg::GdalEnvironment::Initialize();
    RegisterDispatcherAlgorithms();

    DispatchSignal signal;
    mwg::activex::GdalOperationDispatcher dispatcher;
    dispatcher.SetWakeCallback(&DispatchSignal::Wake, &signal);
    const std::uint64_t operationId = dispatcher.Begin(
        "mwg-test-dispatcher progress",
        {});
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    bool sawProgress = false;
    while (!sawProgress && std::chrono::steady_clock::now() < deadline)
    {
        for (const auto& notification :
             WaitForNotifications(dispatcher, signal, deadline))
        {
            sawProgress = sawProgress ||
                notification.kind == mwg::activex::GdalNotificationKind::Progress;
        }
    }
    ASSERT_TRUE(sawProgress);

    const auto shutdownStart = std::chrono::steady_clock::now();
    dispatcher.Shutdown();
    EXPECT_LT(
        std::chrono::steady_clock::now() - shutdownStart,
        100ms);

    mwg::activex::GdalOperationSnapshot result;
    const auto completionDeadline = std::chrono::steady_clock::now() + 10s;
    auto endResult = dispatcher.GetResult(operationId, result);
    while (endResult == mwg::activex::GdalEndResult::Pending &&
           std::chrono::steady_clock::now() < completionDeadline)
    {
        std::this_thread::sleep_for(1ms);
        endResult = dispatcher.GetResult(operationId, result);
    }
    ASSERT_EQ(endResult, mwg::activex::GdalEndResult::Ended);
    EXPECT_EQ(dispatcher.End(operationId, result), mwg::activex::GdalEndResult::Ended);
    EXPECT_TRUE(result.cancelled);
    EXPECT_TRUE(dispatcher.DrainNotifications().empty());
}

TEST(GdalOperationDispatcher, NotificationDeliveryDefersEndUntilCallbacksReturn)
{
    mwg::GdalEnvironment::Initialize();
    RegisterDispatcherAlgorithms();

    DispatchSignal signal;
    mwg::activex::GdalOperationDispatcher dispatcher;
    dispatcher.SetWakeCallback(&DispatchSignal::Wake, &signal);
    const std::uint64_t operationId =
        dispatcher.Begin("mwg-test-dispatcher diagnostics", {});
    ASSERT_TRUE(WaitForCompletion(
        dispatcher,
        signal,
        operationId,
        std::chrono::steady_clock::now() + 10s));

    ASSERT_TRUE(dispatcher.BeginNotificationDelivery(operationId));
    mwg::activex::GdalOperationSnapshot result;
    EXPECT_EQ(
        dispatcher.End(operationId, result),
        mwg::activex::GdalEndResult::Pending);
    constexpr int callbackError = -1;
    dispatcher.RecordCallbackError(
        operationId,
        callbackError,
        "event callback failed");
    dispatcher.EndNotificationDelivery(operationId);
    EXPECT_EQ(
        dispatcher.End(operationId, result),
        mwg::activex::GdalEndResult::Ended);
    EXPECT_EQ(result.callbackErrorCode, callbackError);
    EXPECT_EQ(result.callbackError, "event callback failed");
}

TEST(GdalOperationDispatcher, CompletionRemainsAvailableWhenWakeCallbackFails)
{
    mwg::GdalEnvironment::Initialize();
    RegisterDispatcherAlgorithms();

    FailedDispatchSignal signal;
    mwg::activex::GdalOperationDispatcher dispatcher;
    dispatcher.SetWakeCallback(&FailedDispatchSignal::Wake, &signal);
    const std::uint64_t operationId =
        dispatcher.Begin("mwg-test-dispatcher diagnostics", {});

    bool completed = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!completed && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(10ms);
        for (const auto& notification : dispatcher.DrainNotifications())
        {
            completed = completed ||
                (notification.operationId == operationId &&
                 notification.kind ==
                     mwg::activex::GdalNotificationKind::Completed);
        }
    }
    ASSERT_TRUE(completed);
    EXPECT_GT(signal.attempts.load(), 0);

    mwg::activex::GdalOperationSnapshot result;
    EXPECT_EQ(
        dispatcher.End(operationId, result),
        mwg::activex::GdalEndResult::Ended);
    EXPECT_TRUE(result.succeeded);
}

TEST(GdalOperationDispatcher, ResumesNotificationsWithoutCancellingActiveOperations)
{
    mwg::GdalEnvironment::Initialize();
    RegisterDispatcherAlgorithms();

    DispatchSignal signal;
    mwg::activex::GdalOperationDispatcher dispatcher;
    dispatcher.SetWakeCallback(&DispatchSignal::Wake, &signal);
    const std::uint64_t first = dispatcher.Begin(
        "mwg-test-dispatcher progress",
        {});

    dispatcher.Suspend();
    EXPECT_FALSE(dispatcher.IsDispatchActive());
    EXPECT_TRUE(dispatcher.DrainNotifications().empty());

    dispatcher.SetWakeCallback(&DispatchSignal::Wake, &signal);
    EXPECT_TRUE(dispatcher.IsDispatchActive());
    ASSERT_TRUE(WaitForCompletion(
        dispatcher,
        signal,
        first,
        std::chrono::steady_clock::now() + 10s));

    const std::uint64_t second = dispatcher.Begin(
        "mwg-test-dispatcher diagnostics",
        {});
    EXPECT_NE(first, second);
    EXPECT_TRUE(WaitForCompletion(
        dispatcher,
        signal,
        second,
        std::chrono::steady_clock::now() + 10s));

    mwg::activex::GdalOperationSnapshot result;
    EXPECT_EQ(
        dispatcher.End(first, result),
        mwg::activex::GdalEndResult::Ended);
    EXPECT_TRUE(result.succeeded);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(
        dispatcher.End(second, result),
        mwg::activex::GdalEndResult::Ended);
    EXPECT_TRUE(result.succeeded);
}
