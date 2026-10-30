// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalUtils.h
 * @brief Declares native compatibility methods for the MapWinGIS 5 GdalUtils API.
 *
 * GdalUtils preserves the legacy operation names and error state for native
 * callers while MapWinGIS.Core exposes GDAL without COM dependencies.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mapwingis/core/Api.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalAlgorithmRunner.h"

namespace mwg
{
    /**
     * @brief Runs MapWinGIS 5-compatible GDAL operations from native C++.
     *
     * Operation methods preserve the legacy names and return false on GDAL
     * operation failure. Option strings use GDAL utility command-line syntax
     * and UTF-8 encoding. Shared-connection flags are accepted for source
     * compatibility but have no effect because the native core owns each
     * dataset independently. Prefer GdalAlgorithmRunner and typed options for
     * new code.
     *
     * The object is not thread-safe. Error properties describe the most recent
     * operation. LastErrorCode returns the last GDAL error code for a failure,
     * or zero when the last operation succeeded. ErrorMsg searches diagnostics
     * from that operation.
     *
     * Progress overloads use the same synchronous callback contract as
     * GdalAlgorithmRunner::Run(). They remain supported because they provide
     * progress and cancellation to callers of the legacy utility methods.
     * Result().WasCancelled() distinguishes token/callback cancellation from
     * callback exceptions and other failures. GDAL execution is serialized
     * with runners and convenience operations, not with direct dataset access.
     */
    class MWG_CORE_API GdalUtils
    {
    public:
        GdalUtils();
        ~GdalUtils();

        GdalUtils(const GdalUtils&) = delete;
        GdalUtils& operator=(const GdalUtils&) = delete;

        /// Returns the last failing GDAL diagnostic code, or zero on success.
        [[nodiscard]] int LastErrorCode() const noexcept;

        /// Returns the newest diagnostic with this GDAL code, or an empty string.
        [[nodiscard]] std::string ErrorMsg(int errorCode) const;

        /// Returns all diagnostics from the most recent operation, one per line.
        [[nodiscard]] std::string DetailedErrorMsg() const;

        /// Returns all diagnostics from the most recent operation.
        /// The next operation replaces the contents and invalidates references
        /// to individual diagnostics. Use Result() for an independent snapshot.
        /// The vector reference remains valid until destruction.
        [[nodiscard]] const std::vector<GdalError>& Errors() const noexcept;

        /// Snapshots the most recent operation, including cancellation state.
        [[nodiscard]] GdalOperationResult Result() const;

        /// Converts a raster with legacy GDALTranslate command-line options.
        [[nodiscard, deprecated("Use GdalAlgorithmRunner and typed GDAL options instead.")]]
        bool GdalRasterTranslate(const std::filesystem::path& sourceFilename,
                                 const std::filesystem::path& destinationFilename,
                                 const std::vector<std::string>& options);

        /// Legacy translation with synchronous progress and cancellation.
        [[nodiscard]] bool GdalRasterTranslate(
                                 const std::filesystem::path& sourceFilename,
                                 const std::filesystem::path& destinationFilename,
                                 const std::vector<std::string>& options,
                                 GdalProgressAdapter::Callback progress,
                                 const GdalCancellationToken* cancellationToken = nullptr);

        /// Reprojects a raster with legacy GDALWarp command-line options.
        [[nodiscard, deprecated("Use GdalAlgorithmRunner and typed GDAL options instead.")]]
        bool GdalRasterWarp(const std::filesystem::path& sourceFilename,
                            const std::filesystem::path& destinationFilename,
                            const std::vector<std::string>& options);

        /// Legacy warp with synchronous progress and cancellation.
        [[nodiscard]] bool GdalRasterWarp(
                            const std::filesystem::path& sourceFilename,
                            const std::filesystem::path& destinationFilename,
                            const std::vector<std::string>& options,
                            GdalProgressAdapter::Callback progress,
                            const GdalCancellationToken* cancellationToken = nullptr);

        /// Converts vector data with legacy GDALVectorTranslate command-line options.
        /// @note useSharedConnection is accepted for source compatibility but has no effect.
        [[nodiscard, deprecated("Use GdalAlgorithmRunner and typed GDAL options instead.")]]
        bool GdalVectorTranslate(const std::filesystem::path& sourceFilename,
                                 const std::filesystem::path& destinationFilename,
                                 const std::vector<std::string>& options,
                                 bool useSharedConnection = false);

        /// Legacy vector translation with synchronous progress and cancellation.
        /// @note useSharedConnection is accepted for source compatibility but has no effect.
        [[nodiscard]] bool GdalVectorTranslate(
                                 const std::filesystem::path& sourceFilename,
                                 const std::filesystem::path& destinationFilename,
                                 const std::vector<std::string>& options,
                                 bool useSharedConnection,
                                 GdalProgressAdapter::Callback progress,
                                 const GdalCancellationToken* cancellationToken = nullptr);

        /// Reprojects vector data from one EPSG code to another.
        /// @note useSharedConnection is accepted for source compatibility but has no effect.
        [[nodiscard, deprecated("Use GdalAlgorithmRunner and typed GDAL options instead.")]]
        bool GdalVectorReproject(const std::filesystem::path& sourceFilename,
                                 const std::filesystem::path& destinationFilename,
                                 int sourceEpsgCode,
                                 int destinationEpsgCode,
                                 bool useSharedConnection = false);

        /// Legacy vector reprojection with synchronous progress and cancellation.
        /// @note useSharedConnection is accepted for source compatibility but has no effect.
        [[nodiscard]] bool GdalVectorReproject(
                                 const std::filesystem::path& sourceFilename,
                                 const std::filesystem::path& destinationFilename,
                                 int sourceEpsgCode,
                                 int destinationEpsgCode,
                                 bool useSharedConnection,
                                 GdalProgressAdapter::Callback progress,
                                 const GdalCancellationToken* cancellationToken = nullptr);

        /// Builds overview levels; band indexes are one-based, and an empty list selects all bands.
        /// Configuration options are scoped to this call and restored afterward.
        /// They are process-wide during execution and are not isolated from concurrent GDAL calls.
        /// Returns false with diagnostics for null bytes in resamplingMethod
        /// or configuration option names or values.
        /// @throws InvalidArgument if a configuration option name is empty
        /// and the null-byte checks pass.
        [[nodiscard, deprecated("Use GdalAlgorithmRunner and typed GDAL options instead.")]]
        bool GdalBuildOverviews(const std::filesystem::path& sourceFilename,
                                std::string_view resamplingMethod,
                                const std::vector<int>& overviewList,
                                const std::vector<int>& bandList,
                                const std::vector<std::pair<std::string, std::string>>&
                                    configOptions = {});

        /// Legacy overview building with synchronous progress and cancellation.
        /// Returns false with diagnostics for null bytes in resamplingMethod
        /// or configuration option names or values.
        /// @throws InvalidArgument if a configuration option name is empty
        /// and the null-byte checks pass.
        [[nodiscard]] bool GdalBuildOverviews(
                                const std::filesystem::path& sourceFilename,
                                std::string_view resamplingMethod,
                                const std::vector<int>& overviewList,
                                const std::vector<int>& bandList,
                                const std::vector<std::pair<std::string, std::string>>& configOptions,
                                GdalProgressAdapter::Callback progress,
                                const GdalCancellationToken* cancellationToken = nullptr);

        /// Clips subject features by the overlay dataset's geometry.
        /// @note useSharedConnection is accepted for source compatibility but has no effect.
        [[nodiscard, deprecated("Use GdalAlgorithmRunner and typed GDAL options instead.")]]
        bool ClipVectorWithVector(const std::filesystem::path& subjectFilename,
                                  const std::filesystem::path& overlayFilename,
                                  const std::filesystem::path& destinationFilename,
                                  bool useSharedConnection = true);

        /// Legacy clipping with synchronous progress and cancellation.
        /// @note useSharedConnection is accepted for source compatibility but has no effect.
        [[nodiscard]] bool ClipVectorWithVector(
                                  const std::filesystem::path& subjectFilename,
                                  const std::filesystem::path& overlayFilename,
                                  const std::filesystem::path& destinationFilename,
                                  bool useSharedConnection,
                                  GdalProgressAdapter::Callback progress,
                                  const GdalCancellationToken* cancellationToken = nullptr);

    private:
        struct Impl;
        explicit GdalUtils(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> _impl;
    };
}
