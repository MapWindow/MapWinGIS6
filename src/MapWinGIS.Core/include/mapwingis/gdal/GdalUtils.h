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
#include "mapwingis/gdal/GdalRuntime.h"

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

        /// Returns all warning and failure diagnostics from the most recent operation.
        [[nodiscard]] std::vector<GdalError> Errors() const;

        /// Converts a raster with legacy GDALTranslate command-line options.
        [[nodiscard, deprecated("Use GdalAlgorithmRunner and typed GDAL options instead.")]]
        bool GdalRasterTranslate(const std::filesystem::path& sourceFilename,
                                 const std::filesystem::path& destinationFilename,
                                 const std::vector<std::string>& options);

        /// Reprojects a raster with legacy GDALWarp command-line options.
        [[nodiscard, deprecated("Use GdalAlgorithmRunner and typed GDAL options instead.")]]
        bool GdalRasterWarp(const std::filesystem::path& sourceFilename,
                            const std::filesystem::path& destinationFilename,
                            const std::vector<std::string>& options);

        /// Converts vector data with legacy GDALVectorTranslate command-line options.
        [[nodiscard, deprecated("Use GdalAlgorithmRunner and typed GDAL options instead.")]]
        bool GdalVectorTranslate(const std::filesystem::path& sourceFilename,
                                 const std::filesystem::path& destinationFilename,
                                 const std::vector<std::string>& options,
                                 bool useSharedConnection = false);

        /// Reprojects vector data from one EPSG code to another.
        [[nodiscard, deprecated("Use GdalAlgorithmRunner and typed GDAL options instead.")]]
        bool GdalVectorReproject(const std::filesystem::path& sourceFilename,
                                 const std::filesystem::path& destinationFilename,
                                 int sourceEpsgCode,
                                 int destinationEpsgCode,
                                 bool useSharedConnection = false);

        /// Builds overview levels; band indexes are one-based, and an empty list selects all bands.
        /// Configuration options are scoped to this call and restored afterward.
        /// They are process-wide during execution and are not isolated from concurrent GDAL calls.
        [[nodiscard, deprecated("Use GdalAlgorithmRunner and typed GDAL options instead.")]]
        bool GdalBuildOverviews(const std::filesystem::path& sourceFilename,
                                std::string_view resamplingMethod,
                                const std::vector<int>& overviewList,
                                const std::vector<int>& bandList,
                                const std::vector<std::pair<std::string, std::string>>&
                                    configOptions = {});

        /// Clips subject features by the overlay dataset's geometry.
        [[nodiscard, deprecated("Use GdalAlgorithmRunner and typed GDAL options instead.")]]
        bool ClipVectorWithVector(const std::filesystem::path& subjectFilename,
                                  const std::filesystem::path& overlayFilename,
                                  const std::filesystem::path& destinationFilename,
                                  bool useSharedConnection = true);

    private:
        struct Impl;
        explicit GdalUtils(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> _impl;
    };
}
