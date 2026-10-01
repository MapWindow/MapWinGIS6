// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalEnvironment.cpp
 * @brief Implements process-wide GDAL setup and diagnostic handling.
 *
 * Configures GDAL and PROJ runtime data, registers drivers, and provides
 * thread-safe initialization, version queries, and scoped error suppression.
 */
#include "mapwingis/gdal/GdalEnvironment.h"

#include <mutex>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_version.h>
#include <geos_c.h>
#include <ogr_spatialref.h>

#include <windows.h>

#include "internal/Utf8.h"

namespace mwg
{
    using detail::ToUtf8;

    namespace
    {
        std::once_flag g_initFlag;
        bool g_initialized = false;

        std::mutex g_logHandlerMutex;
        LogHandler g_logHandler;

        thread_local std::string t_lastError;

        LogLevel ToLogLevel(const CPLErr severity) noexcept
        {
            switch (severity)
            {
            case CE_Warning:
                return LogLevel::Warning;
            case CE_Failure:
            case CE_Fatal:
                return LogLevel::Error;
            default:
                return LogLevel::Debug;
            }
        }

        void CPL_STDCALL ErrorHandler(const CPLErr severity, CPLErrorNum, const char* message)
        {
            if (message == nullptr) return;

            if (severity == CE_Failure || severity == CE_Fatal)
                t_lastError = message;

            LogHandler handler;
            {
                std::lock_guard lock(g_logHandlerMutex);
                handler = g_logHandler;
            }
            if (handler) handler(ToLogLevel(severity), message);
        }

        std::string ToUtf8(const std::filesystem::path& path)
        {
            const std::u8string utf8 = path.u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }

        void DoInitialize(const std::filesystem::path& gdalDataDir, const std::filesystem::path& projDataDir)
        {
            CPLSetErrorHandler(&ErrorHandler);

            if (!gdalDataDir.empty() && std::filesystem::is_directory(gdalDataDir))
                CPLSetConfigOption("GDAL_DATA", ToUtf8(gdalDataDir).c_str());

            if (!projDataDir.empty() && std::filesystem::is_directory(projDataDir))
            {
                const std::string proj = ToUtf8(projDataDir);
                const char* paths[] = {proj.c_str(), nullptr};
                OSRSetPROJSearchPaths(paths);
            }

            GDALAllRegister();
            g_initialized = true;
        }

        GDALDriverH FindDriver(const std::string& name, const unsigned int kind)
        {
            GdalEnvironment::Initialize();
            GDALDriverH driver = GDALGetDriverByName(name.c_str());
            if (driver == nullptr) return nullptr;

            const char* value = GDALGetMetadataItem(
                driver, kind == GDAL_OF_RASTER ? GDAL_DCAP_RASTER : GDAL_DCAP_VECTOR, nullptr);
            return (value != nullptr && CPLTestBool(value)) ? driver : nullptr;
        }

        int CountDrivers(const unsigned int kind)
        {
            GdalEnvironment::Initialize();
            int count = 0;
            for (int i = 0; i < GDALGetDriverCount(); ++i)
            {
                GDALDriverH driver = GDALGetDriver(i);
                const char* value = GDALGetMetadataItem(
                    driver, kind == GDAL_OF_RASTER ? GDAL_DCAP_RASTER : GDAL_DCAP_VECTOR, nullptr);
                if (value != nullptr && CPLTestBool(value)) ++count;
            }
            return count;
        }
    }

    void GdalEnvironment::Initialize()
    {
        const std::filesystem::path moduleDir = ModuleDirectory();
        Initialize(moduleDir / "gdal-data", moduleDir / "proj");
    }

    void GdalEnvironment::Initialize(const std::filesystem::path& gdalDataDir,
                                     const std::filesystem::path& projDataDir)
    {
        std::call_once(g_initFlag, DoInitialize, gdalDataDir, projDataDir);
    }

    bool GdalEnvironment::IsInitialized() noexcept
    {
        return g_initialized;
    }

    void GdalEnvironment::SetLogHandler(LogHandler handler)
    {
        std::lock_guard lock(g_logHandlerMutex);
        g_logHandler = std::move(handler);
    }

    std::string GdalEnvironment::LastErrorMessage()
    {
        return t_lastError;
    }

    void GdalEnvironment::ClearLastError()
    {
        t_lastError.clear();
        CPLErrorReset();
    }

    std::string GdalEnvironment::GdalVersion()
    {
        Initialize();
        const char* version = GDALVersionInfo("RELEASE_NAME");
        return version != nullptr ? version : std::string{};
    }

    std::string GdalEnvironment::GeosVersion()
    {
        const char* version = GEOSversion();
        return version != nullptr ? version : std::string{};
    }

    std::string GdalEnvironment::ProjVersion()
    {
        Initialize();
        int major = 0;
        int minor = 0;
        int patch = 0;
        OSRGetPROJVersion(&major, &minor, &patch);
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    int GdalEnvironment::RasterDriverCount() { return CountDrivers(GDAL_OF_RASTER); }
    int GdalEnvironment::VectorDriverCount() { return CountDrivers(GDAL_OF_VECTOR); }

    bool GdalEnvironment::HasRasterDriver(const std::string& name)
    {
        return FindDriver(name, GDAL_OF_RASTER) != nullptr;
    }

    bool GdalEnvironment::HasVectorDriver(const std::string& name)
    {
        return FindDriver(name, GDAL_OF_VECTOR) != nullptr;
    }

    std::filesystem::path GdalEnvironment::ModuleDirectory()
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module))
        {
            return {};
        }

        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) return {};
            if (length < buffer.size())
            {
                buffer.resize(length);
                break;
            }
            buffer.resize(buffer.size() * 2);
        }

        return std::filesystem::path(buffer).parent_path();
    }

    GdalEnvironment::QuietScope::QuietScope()
    {
        GdalEnvironment::Initialize();
        CPLPushErrorHandler(CPLQuietErrorHandler);
    }

    GdalEnvironment::QuietScope::~QuietScope()
    {
        CPLPopErrorHandler();
    }
}
