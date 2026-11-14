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

#include <cpl_error.h>
#include <gdal.h>

#include "mapwingis/gdal/GdalEnvironment.h"

namespace
{
    std::atomic<int> g_hostHandlerCalls{0};
    std::mutex g_messagesMutex;
    std::vector<std::string> g_messages;

    void CPL_STDCALL HostErrorHandler(CPLErr, CPLErrorNum, const char* message)
    {
        ++g_hostHandlerCalls;
        if (message != nullptr)
        {
            std::lock_guard lock(g_messagesMutex);
            g_messages.emplace_back(message);
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
    {
        std::lock_guard lock(g_messagesMutex);
        g_messages.clear();
    }

    CPLSetErrorHandler(&HostErrorHandler);
    int mapWinGisWarnings = 0;
    mwg::GdalEnvironment::SetLogHandler(
        [&mapWinGisWarnings](mwg::LogLevel level, std::string_view message)
        {
            if (level == mwg::LogLevel::Warning &&
                (message.find("GDAL data directory") != std::string_view::npos ||
                 message.find("PROJ data directory") != std::string_view::npos))
            {
                ++mapWinGisWarnings;
            }
        });

    ASSERT_TRUE(mwg::GdalEnvironment::Initialize(gdalData, projData));
    EXPECT_TRUE(mwg::GdalEnvironment::IsInitialized());
    EXPECT_GE(mapWinGisWarnings, 2);

    CPLError(CE_Warning, CPLE_AppDefined, "%s", "host forwarding check");
    EXPECT_GE(g_hostHandlerCalls.load(), 1);
    {
        std::lock_guard lock(g_messagesMutex);
        EXPECT_TRUE(std::any_of(g_messages.begin(), g_messages.end(),
                                [](const std::string& message)
                                { return message == "host forwarding check"; }));
    }

    EXPECT_FALSE(mwg::GdalEnvironment::Initialize(root / "other-gdal", root / "other-proj"));
    mwg::GdalEnvironment::SetLogHandler({});
}