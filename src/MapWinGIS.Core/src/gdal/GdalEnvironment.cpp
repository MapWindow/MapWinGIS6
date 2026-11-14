// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalEnvironment.cpp
 * @brief Implements process-wide GDAL setup and diagnostic handling.
 *
 * Configures GDAL and PROJ runtime data, registers drivers, and provides
 * thread-safe initialization, version queries, and scoped error suppression.
 */
#include "mapwingis/gdal/GdalEnvironment.h"

#include <atomic>
#include <exception>
#include <mutex>
#include <system_error>
#include <utility>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_version.h>
#include <geos_c.h>
#include <ogr_spatialref.h>

#include <windows.h>

#include "internal/Utf8.h"
#include "internal/GdalExecution.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalConfig.h"

namespace mwg
{
    using detail::ToUtf8;

    namespace
    {
        std::once_flag g_initFlag;
        std::atomic_bool g_initialized{false};

        // GDAL keeps ErrorHandler registered process-wide. This state must
        // therefore outlive static destruction in both Core and its host.
        // Core is pinned for the process lifetime before registering the callback.
        struct GdalProcessState final
        {
            std::mutex logHandlerMutex;
            LogHandler logHandler;
            std::atomic<CPLErrorHandler> previousErrorHandler{nullptr};
            std::filesystem::path gdalDataDir;
            std::filesystem::path projDataDir;
        };

        GdalProcessState& ProcessState()
        {
            // Deliberately leaked: GDAL may invoke the registered callback until
            // process exit, after ordinary C++ static objects would be destroyed.
            static GdalProcessState* const state = new GdalProcessState();
            return *state;
        }

        thread_local std::string t_lastError;
        thread_local std::exception_ptr t_logException;

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

        std::string DescribePath(const std::filesystem::path& path)
        {
            try
            {
                return ToUtf8(path);
            }
            catch (...)
            {
                return "<path cannot be represented as UTF-8>";
            }
        }

        void CPL_STDCALL ErrorHandler(const CPLErr severity, CPLErrorNum errorNumber,
                                      const char* message) noexcept
        {
            if (message == nullptr) return;

            GdalProcessState& state = ProcessState();
            try
            {
                GdalEnvironment::ReportDiagnostic(ToLogLevel(severity), message);
            }
            catch (...)
            {
                if (!t_logException)
                    t_logException = std::current_exception();
            }

            // Preserve a host-installed process-wide handler. Do not forward
            // to GDAL's default/quiet handlers or recursively to ourselves.
            const CPLErrorHandler previous = state.previousErrorHandler.load(std::memory_order_acquire);
            if (previous != nullptr &&
                previous != &ErrorHandler &&
                previous != CPLDefaultErrorHandler &&
                previous != CPLQuietErrorHandler)
            {
                try
                {
                    previous(severity, errorNumber, message);
                }
                catch (...)
                {
                    if (!t_logException)
                        t_logException = std::current_exception();
                }
            }
        }

        void DoInitialize(const std::filesystem::path& gdalDataDir,
                          const std::filesystem::path& projDataDir)
        {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&DoInitialize), &module))
            {
                throw CoreError("Could not pin MapWinGIS.Core.dll for the lifetime of GDAL's error handler.");
            }

            GdalProcessState& state = ProcessState();
            const std::string gdalPath = gdalDataDir.empty() ? std::string{} : ToUtf8(gdalDataDir);
            const std::string projPath = projDataDir.empty() ? std::string{} : ToUtf8(projDataDir);

            if (!GDALCheckVersion(GDAL_VERSION_MAJOR, GDAL_VERSION_MINOR, "MapWinGIS.Core"))
                throw CoreError("The loaded GDAL runtime is incompatible with the headers used to build MapWinGIS.Core.");

            state.gdalDataDir = gdalDataDir;
            state.projDataDir = projDataDir;
            state.previousErrorHandler.store(CPLSetErrorHandler(&ErrorHandler),
                                             std::memory_order_release);

            std::error_code error;
            const bool hasGdalData = !gdalDataDir.empty() &&
                                     std::filesystem::is_directory(gdalDataDir, error);
            if (hasGdalData && !error)
            {
                CPLSetConfigOption("GDAL_DATA", gdalPath.c_str());
            }
            else
            {
                if (!gdalPath.empty())
                    CPLSetConfigOption("GDAL_DATA", gdalPath.c_str());
                GdalEnvironment::ReportDiagnostic(LogLevel::Warning,
                                 "GDAL data directory is unavailable: " + DescribePath(gdalDataDir) +
                                     ". GDAL_DATA is still set to this path; no alternate data directory is selected.");
            }

            error.clear();
            const bool hasProjData = !projDataDir.empty() &&
                                     std::filesystem::is_directory(projDataDir, error);
            if (!projPath.empty())
            {
                const char* paths[] = {projPath.c_str(), nullptr};
                OSRSetPROJSearchPaths(paths);
            }
            if (!hasProjData || error)
            {
                GdalEnvironment::ReportDiagnostic(LogLevel::Warning,
                                 "PROJ data directory is unavailable: " + DescribePath(projDataDir) +
                                     ". PROJ search is restricted to this configured path.");
            }

            const GdalInitializer initializer;
            (void)initializer;
            g_initialized.store(true, std::memory_order_release);
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
        if (IsInitialized()) return;
        const std::filesystem::path moduleDir = ModuleDirectory();
        (void)Initialize(moduleDir / "gdal-data", moduleDir / "proj");
    }

    bool GdalEnvironment::Initialize(const std::filesystem::path& gdalDataDir,
                                     const std::filesystem::path& projDataDir)
    {
        std::call_once(g_initFlag, DoInitialize, gdalDataDir, projDataDir);
        const GdalProcessState& state = ProcessState();
        const bool samePaths = state.gdalDataDir == gdalDataDir && state.projDataDir == projDataDir;
        if (!samePaths)
        {
            ReportDiagnostic(LogLevel::Warning,
                             "GdalEnvironment is already initialized with different data paths; "
                             "the requested paths were not applied.");
        }
        return samePaths;
    }

    bool GdalEnvironment::IsInitialized() noexcept
    {
        return g_initialized.load(std::memory_order_acquire);
    }

    void GdalEnvironment::SetLogHandler(LogHandler handler)
    {
        GdalProcessState& state = ProcessState();
        std::lock_guard lock(state.logHandlerMutex);
        state.logHandler = std::move(handler);
    }

    void GdalEnvironment::ReportDiagnostic(const LogLevel level, const std::string_view message)
    {
        if (level == LogLevel::Error)
            t_lastError = message;

        LogHandler handler;
        GdalProcessState& state = ProcessState();
        {
            std::lock_guard lock(state.logHandlerMutex);
            handler = state.logHandler;
        }
        if (handler)
        {
            if (detail::GdalCallbackActive())
                throw CoreError("GDAL diagnostic callbacks cannot be re-entered.");
            const detail::GdalCallbackScope callbackScope;
            handler(level, message);
        }
    }

    void GdalEnvironment::RethrowLogException()
    {
        if (const auto exception = std::exchange(t_logException, {}))
            std::rethrow_exception(exception);
    }

    std::string GdalEnvironment::LastErrorMessage()
    {
        if (!t_lastError.empty())
            return t_lastError;
        const char* const message = CPLGetLastErrorMsg();
        return message != nullptr ? message : "";
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
        return GEOSversion();
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
