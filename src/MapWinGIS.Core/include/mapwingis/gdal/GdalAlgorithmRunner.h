// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalAlgorithmRunner.h
 * @brief Declares the generic GDAL algorithm execution API.
 *
 * GdalAlgorithmRunner configures and executes algorithms registered with GDAL,
 * captures diagnostics, and transfers ownership of output datasets.
 */
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mapwingis/core/Api.h"
#include "mapwingis/gdal/GdalRuntime.h"

class GDALDataset;

namespace mwg
{
    /// Releases a dataset reference returned by GdalAlgorithmRunner.
    struct MWG_CORE_API GdalDatasetDeleter
    {
        void operator()(GDALDataset* dataset) const noexcept;
    };

    /// Owns a GDAL dataset transferred from a GdalAlgorithmRunner.
    using GdalDatasetPtr = std::unique_ptr<GDALDataset, GdalDatasetDeleter>;

    /// Configures and runs one GDAL algorithm instance.
    ///
    /// Construct a new runner for each execution. Run() may be called once and
    /// finalizes successful executions before returning. The runner is not
    /// thread-safe.
    class MWG_CORE_API GdalAlgorithmRunner
    {
    public:
        explicit GdalAlgorithmRunner(std::string_view algorithmPath);
        explicit GdalAlgorithmRunner(std::vector<std::string> algorithmPath);
        ~GdalAlgorithmRunner();

        GdalAlgorithmRunner(const GdalAlgorithmRunner&) = delete;
        GdalAlgorithmRunner& operator=(const GdalAlgorithmRunner&) = delete;

        /// Sets a string-valued algorithm argument.
        [[nodiscard]] bool SetArgument(std::string_view name, std::string_view value);
        /// Sets a string-valued algorithm argument from a null-terminated string.
        [[nodiscard]] bool SetArgument(std::string_view name, const char* value);
        /// Sets a boolean-valued algorithm argument.
        [[nodiscard]] bool SetArgument(std::string_view name, bool value);
        /// Sets an integer-valued algorithm argument.
        [[nodiscard]] bool SetArgument(std::string_view name, int value);
        /// Sets a real-valued algorithm argument.
        [[nodiscard]] bool SetArgument(std::string_view name, double value);
        /// Sets a list of real-valued algorithm argument values.
        [[nodiscard]] bool SetArgument(std::string_view name, const std::vector<double>& value);
        /// Sets a list of integer-valued algorithm argument values.
        [[nodiscard]] bool SetArgument(std::string_view name, const std::vector<int>& value);
        /// Sets a list of string-valued algorithm argument values.
        [[nodiscard]] bool SetArgument(std::string_view name, const std::vector<std::string>& value);

        /// Validates configured values using the instantiated GDAL algorithm's
        /// argument metadata and constraints.
        [[nodiscard]] bool ValidateArguments();

        /// Parses GDAL-style command-line flags for this algorithm.
        [[nodiscard]] bool ParseCommandLineArgs(const std::vector<std::string>& args);

        /// Runs and finalizes the configured algorithm, reporting progress and
        /// cancellation through the existing GDAL callback adapter.
        [[nodiscard]] bool Run(GdalProgressAdapter::Callback progress = {},
                               const GdalCancellationToken* cancellationToken = nullptr);

        /// Finalizes a successfully run algorithm. Run() calls this
        /// automatically; a later call is harmless and returns the same result.
        [[nodiscard]] bool Finalize();

        /// Transfers the output dataset to the caller, if the algorithm
        /// produced one. The caller owns the returned dataset.
        [[nodiscard]] GdalDatasetPtr TakeOutputDataset();

        /// Returns all diagnostics captured since construction.
        [[nodiscard]] const std::vector<GdalError>& Errors() const noexcept;

        /// Returns the GDAL usage metadata as JSON, or an empty string when
        /// the algorithm path could not be instantiated.
        [[nodiscard]] std::string UsageAsJson() const;

    private:
        struct Impl;
        explicit GdalAlgorithmRunner(std::unique_ptr<Impl> impl);
        [[nodiscard]] bool CanConfigure(std::string_view name);
        std::unique_ptr<Impl> _impl;
    };
}
