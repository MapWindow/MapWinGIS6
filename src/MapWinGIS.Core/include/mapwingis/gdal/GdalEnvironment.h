// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalEnvironment.h
 * @brief Declares process-wide GDAL initialization and diagnostics APIs.
 *
 * GdalEnvironment configures runtime data paths, registers drivers, and
 * exposes version, driver, and error reporting utilities.
 */
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "mapwingis/core/Api.h"

namespace mwg
{
    enum class LogLevel : int
    {
        Debug,
        Warning,
        Error,
    };

    /// Receives GDAL diagnostics on the thread that raised them.
    /// Must not re-enter GDAL or wait for GDAL work on another thread.
    /// Capture owned/shared state rather than borrowed
    /// state when other threads can log concurrently.
    using LogHandler = std::function<void(LogLevel level, std::string_view message)>;

    /// Process-wide GDAL setup.
    ///
    /// Everything that touches GDAL goes through Initialize() first. It is
    /// idempotent and thread-safe, so callers do not have to coordinate.
    class MWG_CORE_API GdalEnvironment
    {
    public:
        GdalEnvironment() = delete;

        /// Registers the drivers and points GDAL at the data files deployed
        /// beside MapWinGIS.Core.dll. Runs at most once per process.
        static void Initialize();

        /// Same, but with explicit data locations. Only has an effect on the
        /// first call.
        static void Initialize(const std::filesystem::path& gdalDataDir,
                               const std::filesystem::path& projDataDir);

        [[nodiscard]] static bool IsInitialized() noexcept;

        /// Installs a handler for GDAL diagnostics. Pass {} to remove it.
        /// An invocation already in progress retains its copy of the handler.
        static void SetLogHandler(LogHandler handler);

        /// Routes a captured diagnostic through the installed logger and updates
        /// the calling thread's last error for Error level. Handler exceptions
        /// propagate; GdalErrorScope preserves them until GDAL returns.
        static void ReportDiagnostic(LogLevel level, std::string_view message);

        /// Rethrows and clears the first logger exception captured by the
        /// default GDAL error handler on this thread. Call after direct GDAL
        /// use outside GdalErrorScope. Exceptions never cross GDAL's callback ABI.
        static void RethrowLogException();

        /// Last GDAL error raised on the calling thread, or "" if there was none.
        [[nodiscard]] static std::string LastErrorMessage();
        static void ClearLastError();

        [[nodiscard]] static std::string GdalVersion();
        [[nodiscard]] static std::string GeosVersion();
        [[nodiscard]] static std::string ProjVersion();

        [[nodiscard]] static int RasterDriverCount();
        [[nodiscard]] static int VectorDriverCount();
        [[nodiscard]] static bool HasRasterDriver(const std::string& name);
        [[nodiscard]] static bool HasVectorDriver(const std::string& name);

        /// Directory the core DLL was loaded from. Runtime data lives here.
        [[nodiscard]] static std::filesystem::path ModuleDirectory();

        /// Suppresses GDAL diagnostics on the calling thread for its lifetime.
        /// Uses GDAL's per-thread handler stack, so other threads are unaffected.
        /// Construct and destroy on the same thread, in reverse construction
        /// order. Restores the previous handler even when an exception unwinds.
        class MWG_CORE_API QuietScope
        {
        public:
            QuietScope();
            ~QuietScope();
            QuietScope(const QuietScope&) = delete;
            QuietScope& operator=(const QuietScope&) = delete;
        };
    };
}
