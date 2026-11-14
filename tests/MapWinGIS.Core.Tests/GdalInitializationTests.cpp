// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalInitializationTests.cpp
 * @brief Tests first-call GDAL initialization in an isolated process.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <string>
#include <vector>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal.h>
#include <ogr_srs_api.h>

#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalAlgorithmRunner.h"
#include "mapwingis/gdal/GdalConfig.h"
#include "mapwingis/gdal/GdalDiagnostics.h"
#include "mapwingis/gdal/GdalEnvironment.h"

namespace
{
    std::atomic<int> g_hostHandlerCalls{0};
    std::atomic<int> g_dataDirectoryWarnings{0};
    std::atomic_bool g_attemptReentry{false};
    std::atomic_bool g_reentryRejected{false};
    std::atomic<int> g_lastHostClass{CE_None};
    std::atomic<int> g_lastHostErrorNumber{CPLE_None};
    std::mutex g_messagesMutex;
    std::vector<std::string> g_messages;

    void CPL_STDCALL HostErrorHandler(CPLErr errorClass, CPLErrorNum errorNumber, const char* message)
    {
        ++g_hostHandlerCalls;
        g_lastHostClass = errorClass;
        g_lastHostErrorNumber = errorNumber;
        if (message != nullptr)
        {
            std::lock_guard lock(g_messagesMutex);
            g_messages.emplace_back(message);
        }

        if (g_attemptReentry.exchange(false))
        {
            try
            {
                const mwg::GdalAlgorithmRunner runner("raster info");
            }
            catch (const mwg::CoreError&)
            {
                g_reentryRejected = true;
            }
        }
    }
}

TEST(GdalInitialization, PreservesHostHandlerAndReportsMissingDataDirectories)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "MapWinGIS6-missing-runtime-data";
    const std::filesystem::path gdalData = root / "gdal-data";
    const std::filesystem::path projData = root / "proj";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    g_hostHandlerCalls = 0;
    g_dataDirectoryWarnings = 0;
    {
        std::lock_guard lock(g_messagesMutex);
        g_messages.clear();
    }

    CPLSetErrorHandler(&HostErrorHandler);
    mwg::GdalEnvironment::SetLogHandler(
        [](mwg::LogLevel level, std::string_view message)
        {
            if (level == mwg::LogLevel::Warning &&
                (message.find("GDAL data directory") != std::string_view::npos ||
                 message.find("PROJ data directory") != std::string_view::npos))
            {
                ++g_dataDirectoryWarnings;
            }
        });

    ASSERT_TRUE(mwg::GdalEnvironment::Initialize(gdalData, projData));
    EXPECT_TRUE(mwg::GdalEnvironment::IsInitialized());
    EXPECT_GE(g_dataDirectoryWarnings.load(), 2);

    CPLError(CE_Warning, CPLE_AppDefined, "%s", "host forwarding check");
    EXPECT_GE(g_hostHandlerCalls.load(), 1);
    {
        std::lock_guard lock(g_messagesMutex);
        EXPECT_TRUE(std::any_of(g_messages.begin(), g_messages.end(),
                                [](const std::string& message)
                                { return message == "host forwarding check"; }));
    }

    EXPECT_FALSE(mwg::GdalEnvironment::Initialize(root / "other-gdal", root / "other-proj"));

    // The same arguments only succeed while GDAL still uses those paths.
    const std::string otherPath = (root / "override").string();
    EXPECT_TRUE(mwg::GdalEnvironment::Initialize(gdalData, projData));
    {
        const mwg::ScopedGdalConfigOption globalOverride("GDAL_DATA", otherPath);
        EXPECT_FALSE(mwg::GdalEnvironment::Initialize(gdalData, projData));
    }
    EXPECT_TRUE(mwg::GdalEnvironment::Initialize(gdalData, projData));

    CPLSetThreadLocalConfigOption("GDAL_DATA", otherPath.c_str());
    EXPECT_FALSE(mwg::GdalEnvironment::Initialize(gdalData, projData));
    CPLSetThreadLocalConfigOption("GDAL_DATA", nullptr);
    EXPECT_TRUE(mwg::GdalEnvironment::Initialize(gdalData, projData));

    const std::string projPath = projData.string();
    const char* const otherProjPaths[] = {otherPath.c_str(), nullptr};
    const char* const projPaths[] = {projPath.c_str(), nullptr};
    OSRSetPROJSearchPaths(otherProjPaths);
    EXPECT_FALSE(mwg::GdalEnvironment::Initialize(gdalData, projData));
    OSRSetPROJSearchPaths(projPaths);
    EXPECT_TRUE(mwg::GdalEnvironment::Initialize(gdalData, projData));

    mwg::GdalEnvironment::SetLogHandler({});
}

TEST(GdalInitialization, RejectsGdalWorkFromHostHandler)
{
    // The host handler must be installed before Core so Core forwards to it.
    if (!mwg::GdalEnvironment::IsInitialized())
        CPLSetErrorHandler(&HostErrorHandler);
    mwg::GdalEnvironment::Initialize();
    g_reentryRejected = false;
    g_attemptReentry = true;

    CPLError(CE_Warning, CPLE_AppDefined, "%s", "host re-entry check");

    EXPECT_FALSE(g_attemptReentry.load());
    EXPECT_TRUE(g_reentryRejected.load());
    mwg::GdalEnvironment::RethrowLogException();
}

TEST(GdalInitialization, ForwardsScopedDiagnosticsToHostHandler)
{
    if (!mwg::GdalEnvironment::IsInitialized())
        CPLSetErrorHandler(&HostErrorHandler);
    mwg::GdalEnvironment::Initialize();
    {
        std::lock_guard lock(g_messagesMutex);
        g_messages.clear();
    }
    g_lastHostClass = CE_None;
    g_lastHostErrorNumber = CPLE_None;

    {
        mwg::GdalErrorScope scope;
        CPLError(CE_Failure, CPLE_FileIO, "%s", "scoped forwarding check");
        ASSERT_EQ(scope.Errors().size(), 1U);
        scope.RethrowCaptureException();
    }
    EXPECT_EQ(g_lastHostClass.load(), CE_Failure);
    EXPECT_EQ(g_lastHostErrorNumber.load(), CPLE_FileIO);

    {
        mwg::GdalErrorScope scope;
        const mwg::GdalEnvironment::QuietScope quiet;
        CPLError(CE_Warning, CPLE_AppDefined, "%s", "quiet scoped check");
    }

    std::lock_guard lock(g_messagesMutex);
    EXPECT_EQ(std::count(g_messages.begin(), g_messages.end(), "scoped forwarding check"), 1);
    EXPECT_EQ(std::count(g_messages.begin(), g_messages.end(), "quiet scoped check"), 0);
}