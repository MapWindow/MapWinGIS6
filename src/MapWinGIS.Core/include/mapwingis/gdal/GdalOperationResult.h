// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalOperationResult.h
 * @brief Declares ownership-safe GDAL algorithm results and datasets.
 *
 * Bundles execution diagnostics with datasets whose lifetime may depend on
 * other results retained by the operation.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mapwingis/core/Api.h"
#include "mapwingis/gdal/GdalDiagnostics.h"

class GDALDataset;

namespace mwg
{
    /// Releases a dataset reference returned by a GDAL operation.
    struct MWG_CORE_API GdalDatasetDeleter
    {
        void operator()(GDALDataset* dataset) const noexcept;
    };

    /// Owns a GDAL dataset reference transferred to a caller.
    using GdalDatasetPtr = std::unique_ptr<GDALDataset, GdalDatasetDeleter>;

    /// Owns an operation's diagnostics and any returned datasets.
    /// Keep the result alive while using outputs that depend on retained datasets.
    /// Not thread-safe for shared instances. Diagnostic queries copy strings.
    struct MWG_CORE_API GdalOperationResult
    {
        bool succeeded = false;
        GdalDatasetPtr outputDataset;
        std::vector<GdalDatasetPtr> retainedDatasets;
        std::vector<GdalError> errors;
        bool cancelled = false;

        [[nodiscard]] bool Succeeded() const noexcept { return succeeded; }
        [[nodiscard]] bool WasCancelled() const noexcept { return cancelled; }
        /// Last failure/fatal code, or zero when there are only warnings.
        [[nodiscard]] int LastErrorCode() const noexcept;
        /// Last failure/fatal message, or an empty string.
        [[nodiscard]] std::string ErrorMsg() const;
        /// All diagnostics with severity and code, one per line.
        [[nodiscard]] std::string DetailedErrorMsg() const;
        [[nodiscard]] const std::vector<GdalError>& AllErrors() const noexcept { return errors; }
    };
}
