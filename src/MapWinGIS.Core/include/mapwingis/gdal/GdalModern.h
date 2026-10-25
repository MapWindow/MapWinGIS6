// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalModern.h
 * @brief Declares modern GDAL convenience operations and dataset pipelines.
 *
 * Provides COG creation and sequential algorithm execution with ownership-safe
 * dataset transfer between steps.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mapwingis/core/Api.h"
#include "mapwingis/gdal/GdalAlgorithmRunner.h"
#include "mapwingis/gdal/GdalTypedOptions.h"

namespace mwg
{
    /// Result of a GDAL convenience operation.
    struct MWG_CORE_API GdalOperationResult
    {
        bool succeeded = false;
        GdalDatasetPtr outputDataset;
        std::vector<GdalError> errors;

        /// Indicates whether the operation completed successfully.
        [[nodiscard]] bool Succeeded() const noexcept { return succeeded; }
    };

    /// Options for creating a Cloud-Optimized GeoTIFF.
    ///
    /// GDAL 3.12.4 COG driver defaults are used unless overridden through
    /// creationOptions. input and output accept local paths and GDAL VSI paths.
    struct MWG_CORE_API RasterCogOptions
    {
        std::filesystem::path input;
        std::filesystem::path output;
        std::optional<bool> overwrite;
        std::vector<std::string> creationOptions;
        std::vector<std::string> inputFormats;
        std::vector<std::string> openOptions;
    };

    /// One algorithm and its typed arguments in a sequential pipeline.
    struct MWG_CORE_API GdalPipelineStep
    {
        std::vector<std::string> algorithmPath;
        GdalAlgorithmArguments arguments;
    };

    /**
     * @brief Creates a Cloud-Optimized GeoTIFF with GDAL's raster convert algorithm.
     *
     * Leaves format defaults to GDAL and applies supplied COG creation options.
     * VSI paths may be used for inputs or outputs when supported by the driver.
     * The returned dataset is owned by the result and may be null if GDAL does
     * not return an output dataset. Errors are returned in the result.
     *
     * Not thread-safe when callers concurrently modify process-wide GDAL
     * configuration. Allocation failures propagate.
     */
    [[nodiscard]] MWG_CORE_API GdalOperationResult WriteCog(
        const RasterCogOptions& options,
        GdalProgressAdapter::Callback progress = {},
        const GdalCancellationToken* cancellationToken = nullptr);

    /**
     * @brief Runs GDAL algorithms in order, passing each output dataset to the next step.
     *
     * The first step configures its input normally. Later steps must omit the
     * "input" argument; their input is the preceding step's output dataset.
     * Every step must produce an output dataset. Set output-format to "MEM" or
     * "VRT" for in-memory processing where the algorithm and driver support it.
     * Progress is reported over the full pipeline. A failed step stops execution
     * and returns its diagnostics; all intermediate datasets are released.
     *
     * Not thread-safe for shared GDAL datasets or runners. Allocation failures
     * propagate.
     */
    [[nodiscard]] MWG_CORE_API GdalOperationResult RunPipeline(
        const std::vector<GdalPipelineStep>& steps,
        GdalProgressAdapter::Callback progress = {},
        const GdalCancellationToken* cancellationToken = nullptr);
}
