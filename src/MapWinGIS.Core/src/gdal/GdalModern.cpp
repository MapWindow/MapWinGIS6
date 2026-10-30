// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalModern.cpp
 * @brief Implements COG creation and sequential GDAL algorithm pipelines.
 *
 * Uses the generic algorithm runner to preserve GDAL diagnostics, progress,
 * cancellation, finalization, and dataset ownership.
 */
#include "mapwingis/gdal/GdalModern.h"
#include "internal/GdalExecution.h"

#include <algorithm>
#include <cpl_error.h>
#include <utility>

namespace mwg
{
    namespace
    {
        void AppendRunnerErrors(GdalOperationResult& result,
                                const GdalAlgorithmRunner& runner)
        {
            const auto& errors = runner.Errors();
            result.errors.insert(result.errors.end(), errors.begin(), errors.end());
            result.cancelled = result.cancelled || runner.Result().WasCancelled();
        }

        void AddOperationError(GdalOperationResult& result, std::string message)
        {
            result.errors.push_back(
                {GdalErrorClass::Failure, CPLE_AppDefined, std::move(message)});
        }

        bool HasInputArgument(const GdalPipelineStep& step)
        {
            return std::any_of(step.arguments.begin(), step.arguments.end(),
                               [](const GdalAlgorithmArgument& argument)
                               {
                                   return argument.name == "input";
                               });
        }
    }

    GdalOperationResult WriteCog(const RasterCogOptions& options,
                                 GdalProgressAdapter::Callback progress,
                                 const GdalCancellationToken* cancellationToken)
    {
        const detail::GdalExecutionLock lock;
        RasterConvertOptions conversion;
        conversion.input = options.input;
        conversion.output = options.output;
        conversion.outputFormat = "COG";
        conversion.overwrite = options.overwrite;
        conversion.creationOptions = options.creationOptions;
        conversion.inputFormats = options.inputFormats;
        conversion.openOptions = options.openOptions;

        GdalAlgorithmRunner runner("raster convert");
        GdalOperationResult result;
        if (!ConfigureGdalAlgorithm(runner, conversion.ToArgs()))
        {
            AppendRunnerErrors(result, runner);
            return result;
        }

        (void)runner.Run(std::move(progress), cancellationToken);
        result = runner.Result();
        if (result.succeeded)
            result.outputDataset = runner.TakeOutputDataset();
        return result;
    }

    GdalOperationResult RunPipeline(const std::vector<GdalPipelineStep>& steps,
                                    GdalProgressAdapter::Callback progress,
                                    const GdalCancellationToken* cancellationToken)
    {
        const detail::GdalExecutionLock lock;
        GdalOperationResult result;
        if (steps.empty())
        {
            AddOperationError(result, "A GDAL pipeline must contain at least one step.");
            return result;
        }
        for (std::size_t index = 1; index < steps.size(); ++index)
        {
            if (HasInputArgument(steps[index]))
            {
                AddOperationError(
                    result,
                    "GDAL pipeline steps after the first must omit the 'input' argument.");
                return result;
            }
        }

        GdalDatasetPtr previousOutput;
        std::vector<GdalDatasetPtr> retainedDatasets;
        retainedDatasets.reserve(steps.size() - 1);
        for (std::size_t index = 0; index < steps.size(); ++index)
        {
            const GdalPipelineStep& step = steps[index];
            GdalAlgorithmRunner runner(step.algorithmPath);

            if (index > 0)
            {
                if (!runner.SetDatasetArgument("input", previousOutput))
                {
                    AppendRunnerErrors(result, runner);
                    return result;
                }
            }

            if (!ConfigureGdalAlgorithm(runner, step.arguments))
            {
                AppendRunnerErrors(result, runner);
                return result;
            }

            const auto stepProgress =
                [&progress, index, stepCount = steps.size()](
                    const double fraction, const std::string_view message)
                {
                    if (!progress)
                        return true;
                    return progress((static_cast<double>(index) + fraction) /
                                        static_cast<double>(stepCount),
                                    message);
                };

            if (!runner.Run(stepProgress, cancellationToken))
            {
                AppendRunnerErrors(result, runner);
                return result;
            }

            GdalDatasetPtr output = runner.TakeOutputDataset();
            if (output == nullptr)
            {
                AppendRunnerErrors(result, runner);
                AddOperationError(result,
                                  "A GDAL pipeline step completed without an output dataset.");
                return result;
            }

            if (previousOutput != nullptr)
                retainedDatasets.push_back(std::move(previousOutput));
            previousOutput = std::move(output);
            AppendRunnerErrors(result, runner);
        }

        result.outputDataset = std::move(previousOutput);
        result.retainedDatasets = std::move(retainedDatasets);
        result.succeeded = true;
        return result;
    }
}
