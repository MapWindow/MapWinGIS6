// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalAlgorithmRunner.cpp
 * @brief Implements generic GDAL algorithm configuration and execution.
 *
 * Uses GDAL's global algorithm registry and runtime adapters to configure,
 * execute, finalize, and return datasets from algorithms.
 */
#include "mapwingis/gdal/GdalAlgorithmRunner.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "internal/GdalExecution.h"
#include "internal/GdalProgressAdapterAccess.h"

#include <algorithm>
#include <cpl_error.h>
#include <gdal.h>
#include <gdalalgorithm.h>
#include <gdal_priv.h>

#include <exception>
#include <sstream>
#include <utility>

namespace mwg
{
    namespace
    {
        constexpr int kRunnerErrorCode = CPLE_AppDefined;

        std::vector<std::string> SplitAlgorithmPath(const std::string_view path)
        {
            std::istringstream stream{std::string(path)};
            std::vector<std::string> components;
            for (std::string component; stream >> component;)
                components.push_back(std::move(component));
            return components;
        }

        void AddError(std::vector<GdalError>& errors, std::string message)
        {
            errors.push_back({GdalErrorClass::Failure, kRunnerErrorCode, std::move(message)});
        }

        bool HasFailure(const std::vector<GdalError>& errors)
        {
            return std::any_of(errors.begin(), errors.end(), [](const GdalError& error)
            {
                return error.errorClass >= GdalErrorClass::Failure;
            });
        }

        void AppendErrors(std::vector<GdalError>& target, const GdalErrorScope& source)
        {
            const auto& errors = source.Errors();
            target.insert(target.end(), errors.begin(), errors.end());
        }

        void AddFailureIfNoDiagnostic(std::vector<GdalError>& errors,
                                      const std::size_t previousCount,
                                      std::string message)
        {
            if (std::none_of(errors.begin() + static_cast<std::ptrdiff_t>(previousCount),
                             errors.end(), [](const GdalError& error)
                             {
                                 return error.errorClass >= GdalErrorClass::Failure;
                             }))
                AddError(errors, std::move(message));
        }

        template <typename T>
        bool SetArgumentValue(GDALAlgorithm* algorithm,
                              std::vector<GdalError>& errors,
                              bool& configurationValid,
                              const std::string_view name,
                              T&& value)
        {
            const detail::GdalExecutionLock lock;
            const std::size_t previousCount = errors.size();
            GdalErrorScope scope;
            GDALAlgorithmArg* argument = algorithm != nullptr
                                             ? algorithm->GetArg(std::string(name))
                                             : nullptr;
            const bool succeeded = argument != nullptr && argument->Set(std::forward<T>(value));
            AppendErrors(errors, scope);
            scope.RethrowCaptureException();

            if (!succeeded)
            {
                AddFailureIfNoDiagnostic(errors, previousCount,
                                         "Could not set GDAL algorithm argument '" +
                                             std::string(name) + "'.");
                configurationValid = false;
            }
            return succeeded;
        }
    }

    struct GdalAlgorithmRunner::Impl
    {
        std::unique_ptr<GDALAlgorithm> algorithm;
        GdalDatasetPtr outputDataset;
        std::vector<GdalError> errors;
        bool configurationValid = true;
        bool runAttempted = false;
        bool executionSucceeded = false;
        bool finalizeAttempted = false;
        bool finalizationSucceeded = false;
        bool cancelled = false;
        bool operationSucceeded = false;
    };

    int GdalOperationResult::LastErrorCode() const noexcept
    {
        for (auto error = errors.rbegin(); error != errors.rend(); ++error)
        {
            if (error->errorClass >= GdalErrorClass::Failure)
                return error->code;
        }
        return 0;
    }

    std::string GdalOperationResult::ErrorMsg() const
    {
        for (auto error = errors.rbegin(); error != errors.rend(); ++error)
        {
            if (error->errorClass >= GdalErrorClass::Failure)
                return error->message;
        }
        return {};
    }

    std::string GdalOperationResult::DetailedErrorMsg() const
    {
        std::string details;
        for (const auto& error : errors)
        {
            if (!details.empty())
                details += '\n';
            details += "GDAL error (class " +
                       std::to_string(static_cast<int>(error.errorClass)) +
                       ", code " + std::to_string(error.code) + "): " + error.message;
        }
        return details;
    }

    void GdalDatasetDeleter::operator()(GDALDataset* dataset) const noexcept
    {
        const std::lock_guard lock(detail::GdalExecutionMutex());
        if (dataset != nullptr)
            GDALReleaseDataset(dataset);
    }

    GdalAlgorithmRunner::GdalAlgorithmRunner(const std::string_view algorithmPath)
        : GdalAlgorithmRunner(SplitAlgorithmPath(algorithmPath))
    {
    }

    GdalAlgorithmRunner::GdalAlgorithmRunner(std::vector<std::string> algorithmPath)
        : GdalAlgorithmRunner(std::make_unique<Impl>())
    {
        const detail::GdalExecutionLock lock;
        GdalEnvironment::Initialize();

        if (algorithmPath.empty() ||
            std::any_of(algorithmPath.begin(), algorithmPath.end(),
                        [](const std::string& component)
                        {
                            return component.empty() || component.find('\0') != std::string::npos;
                        }))
        {
            AddError(_impl->errors, "GDAL algorithm path must contain non-empty components.");
            _impl->configurationValid = false;
            return;
        }

        GdalErrorScope scope;
        _impl->algorithm = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate(algorithmPath);
        AppendErrors(_impl->errors, scope);
        scope.RethrowCaptureException();
        if (_impl->algorithm == nullptr)
        {
            std::string path;
            for (const std::string& component : algorithmPath)
            {
                if (!path.empty())
                    path += ' ';
                path += component;
            }
            AddError(_impl->errors, "Unknown GDAL algorithm: " + path);
            _impl->configurationValid = false;
        }
    }

    GdalAlgorithmRunner::GdalAlgorithmRunner(std::unique_ptr<Impl> impl)
        : _impl(std::move(impl))
    {
    }

    GdalAlgorithmRunner::~GdalAlgorithmRunner()
    {
        const std::lock_guard lock(detail::GdalExecutionMutex());
        _impl.reset();
    }

    bool GdalAlgorithmRunner::CanConfigure(const std::string_view name)
    {
        const detail::GdalExecutionLock lock;
        if (_impl->runAttempted)
        {
            AddError(_impl->errors, "GDAL algorithm arguments cannot be changed after Run().");
            return false;
        }
        if (name.find('\0') != std::string_view::npos)
        {
            AddError(_impl->errors, "GDAL algorithm argument names must not contain null bytes.");
            _impl->configurationValid = false;
            return false;
        }
        return true;
    }

    bool GdalAlgorithmRunner::SetArgument(const std::string_view name, const std::string_view value)
    {
        if (!CanConfigure(name))
            return false;
        if (value.find('\0') != std::string_view::npos)
        {
            AddError(_impl->errors, "GDAL algorithm argument values must not contain null bytes.");
            _impl->configurationValid = false;
            return false;
        }

        return SetArgumentValue(_impl->algorithm.get(), _impl->errors,
                                _impl->configurationValid, name, std::string(value));
    }

    bool GdalAlgorithmRunner::SetArgument(const std::string_view name, const char* value)
    {
        if (!CanConfigure(name))
            return false;
        if (value == nullptr)
        {
            AddError(_impl->errors, "GDAL algorithm string argument values must not be null.");
            _impl->configurationValid = false;
            return false;
        }
        return SetArgumentValue(_impl->algorithm.get(), _impl->errors,
                                _impl->configurationValid, name, std::string(value));
    }

    bool GdalAlgorithmRunner::SetArgument(const std::string_view name, const bool value)
    {
        if (!CanConfigure(name))
            return false;
        return SetArgumentValue(_impl->algorithm.get(), _impl->errors,
                                _impl->configurationValid, name, value);
    }

    bool GdalAlgorithmRunner::SetArgument(const std::string_view name, const int value)
    {
        if (!CanConfigure(name))
            return false;
        return SetArgumentValue(_impl->algorithm.get(), _impl->errors,
                                _impl->configurationValid, name, value);
    }

    bool GdalAlgorithmRunner::SetArgument(const std::string_view name, const double value)
    {
        if (!CanConfigure(name))
            return false;
        return SetArgumentValue(_impl->algorithm.get(), _impl->errors,
                                _impl->configurationValid, name, value);
    }

    bool GdalAlgorithmRunner::SetArgument(const std::string_view name, const std::vector<double>& value)
    {
        if (!CanConfigure(name))
            return false;
        return SetArgumentValue(_impl->algorithm.get(), _impl->errors,
                                _impl->configurationValid, name, value);
    }

    bool GdalAlgorithmRunner::SetArgument(const std::string_view name, const std::vector<int>& value)
    {
        if (!CanConfigure(name))
            return false;
        return SetArgumentValue(_impl->algorithm.get(), _impl->errors,
                                _impl->configurationValid, name, value);
    }

    bool GdalAlgorithmRunner::SetArgument(const std::string_view name,
                                          const std::vector<std::string>& value)
    {
        if (!CanConfigure(name))
            return false;
        return SetArgumentValue(_impl->algorithm.get(), _impl->errors,
                                _impl->configurationValid, name, value);
    }

    bool GdalAlgorithmRunner::SetDatasetArgument(const std::string_view name,
                                                 const GdalDatasetPtr& value)
    {
        const detail::GdalExecutionLock lock;
        if (!CanConfigure(name))
            return false;
        if (value == nullptr)
        {
            AddError(_impl->errors, "GDAL dataset argument values must not be null.");
            _impl->configurationValid = false;
            return false;
        }

        GDALAlgorithmArg* argument =
            _impl->algorithm != nullptr ? _impl->algorithm->GetArg(std::string(name)) : nullptr;
        if (argument == nullptr ||
            (argument->GetType() != GAAT_DATASET &&
             argument->GetType() != GAAT_DATASET_LIST))
        {
            AddError(_impl->errors,
                     "GDAL algorithm argument '" + std::string(name) +
                         "' is not a dataset or dataset-list argument.");
            _impl->configurationValid = false;
            return false;
        }

        const std::size_t previousCount = _impl->errors.size();
        GdalErrorScope scope;
        bool succeeded = false;
        if (argument->GetType() == GAAT_DATASET)
        {
            succeeded = argument->Set(value.get());
        }
        else
        {
            std::vector<GDALArgDatasetValue> datasets;
            datasets.emplace_back(value.get());
            succeeded = argument->Set(std::move(datasets));
        }
        AppendErrors(_impl->errors, scope);
        scope.RethrowCaptureException();

        if (!succeeded)
        {
            AddFailureIfNoDiagnostic(_impl->errors, previousCount,
                                     "Could not set GDAL dataset argument '" +
                                         std::string(name) + "'.");
            _impl->configurationValid = false;
        }
        return succeeded;
    }

    bool GdalAlgorithmRunner::ParseCommandLineArgs(const std::vector<std::string>& args)
    {
        const detail::GdalExecutionLock lock;
        if (_impl->algorithm == nullptr || !_impl->configurationValid || _impl->runAttempted)
        {
            AddError(_impl->errors, "GDAL command-line arguments cannot be parsed for this runner.");
            _impl->configurationValid = false;
            return false;
        }
        if (std::any_of(args.begin(), args.end(),
                        [](const std::string& arg) { return arg.find('\0') != std::string::npos; }))
        {
            AddError(_impl->errors, "GDAL command-line arguments must not contain null bytes.");
            _impl->configurationValid = false;
            return false;
        }

        GdalErrorScope scope;
        const bool succeeded = _impl->algorithm->ParseCommandLineArguments(args);
        const std::size_t previousCount = _impl->errors.size();
        AppendErrors(_impl->errors, scope);
        scope.RethrowCaptureException();
        if (!succeeded)
        {
            AddFailureIfNoDiagnostic(_impl->errors, previousCount,
                                     "Could not parse GDAL algorithm command-line arguments.");
            _impl->configurationValid = false;
        }
        return succeeded;
    }

    bool GdalAlgorithmRunner::ValidateArguments()
    {
        const detail::GdalExecutionLock lock;
        if (_impl->runAttempted)
        {
            AddError(_impl->errors, "GDAL algorithm arguments cannot be validated after Run().");
            return false;
        }
        if (_impl->algorithm == nullptr || !_impl->configurationValid)
            return false;

        GdalErrorScope scope;
        const std::size_t previousCount = _impl->errors.size();
        const bool succeeded = _impl->algorithm->ValidateArguments();
        AppendErrors(_impl->errors, scope);
        scope.RethrowCaptureException();
        if (!succeeded)
        {
            AddFailureIfNoDiagnostic(_impl->errors, previousCount,
                                     "GDAL algorithm argument validation failed.");
            _impl->configurationValid = false;
        }
        return succeeded;
    }

    bool GdalAlgorithmRunner::Run(GdalProgressAdapter::Callback progress,
                                  const GdalCancellationToken* cancellationToken)
    {
        const detail::GdalExecutionLock lock;
        _impl->operationSucceeded = false;
        if (_impl->runAttempted)
        {
            AddError(_impl->errors, "A GDAL algorithm runner can only be run once.");
            return false;
        }
        _impl->runAttempted = true;

        if (_impl->algorithm == nullptr || !_impl->configurationValid)
            return false;

        if (cancellationToken != nullptr && cancellationToken->IsCancellationRequested())
        {
            _impl->cancelled = true;
            AddError(_impl->errors, "GDAL algorithm execution was cancelled.");
            return false;
        }

        GdalProgressAdapter adapter(std::move(progress), cancellationToken);
        GdalErrorScope scope;
        const std::size_t previousCount = _impl->errors.size();
        const bool succeeded = _impl->algorithm->Run(
            detail::GdalProgressAdapterAccess::Function(adapter),
            detail::GdalProgressAdapterAccess::UserData(adapter));
        AppendErrors(_impl->errors, scope);

        // A successful GDAL Run must be finalized even if a callback cancelled
        // or threw after GDAL had already produced its output.
        _impl->executionSucceeded = succeeded;
        bool finalized = false;
        if (succeeded)
        {
            GDALAlgorithmArg* output = _impl->algorithm != nullptr
                                           ? _impl->algorithm->GetArg("output")
                                           : nullptr;
            if (output != nullptr && output->GetType() == GAAT_DATASET)
            {
                _impl->outputDataset.reset(
                    output->Get<GDALArgDatasetValue>().GetDatasetIncreaseRefCount());
            }
            finalized = Finalize();
        }
        if (_impl->outputDataset != nullptr)
        {
            GdalErrorScope outputScope;
            const std::size_t outputErrorCount = _impl->errors.size();
            const CPLErr flushResult = _impl->outputDataset->FlushCache();
            AppendErrors(_impl->errors, outputScope);
            outputScope.RethrowCaptureException();
            if (flushResult != CE_None)
            {
                AddFailureIfNoDiagnostic(
                    _impl->errors,
                    outputErrorCount,
                    "GDAL algorithm output could not be flushed.");
            }
        }
        scope.RethrowCaptureException();

        bool callbackFailed = false;
        try
        {
            adapter.RethrowCallbackException();
        }
        catch (const std::exception& error)
        {
            AddError(_impl->errors, "GDAL progress callback failed: " + std::string(error.what()));
            callbackFailed = true;
        }
        catch (...)
        {
            AddError(_impl->errors, "GDAL progress callback failed with a non-standard exception.");
            callbackFailed = true;
        }

        if (adapter.WasCancelled() && !callbackFailed)
        {
            _impl->cancelled = true;
            AddError(_impl->errors, "GDAL algorithm execution was cancelled.");
        }
        if (!succeeded && !adapter.WasCancelled())
        {
            AddFailureIfNoDiagnostic(_impl->errors, previousCount, "GDAL algorithm execution failed.");
        }

        _impl->operationSucceeded =
            succeeded && finalized && !adapter.WasCancelled() && !callbackFailed &&
            !HasFailure(_impl->errors);
        if (!_impl->operationSucceeded)
        {
            GdalErrorScope closeScope;
            const std::size_t closeErrorCount = _impl->errors.size();
            _impl->outputDataset.reset();
            GDALAlgorithmArg* output = _impl->algorithm != nullptr
                                           ? _impl->algorithm->GetArg("output")
                                           : nullptr;
            bool outputClosed = true;
            if (output != nullptr && output->GetType() == GAAT_DATASET)
            {
                outputClosed = output->Get<GDALArgDatasetValue>().Close();
            }
            AppendErrors(_impl->errors, closeScope);
            if (!outputClosed)
            {
                AddFailureIfNoDiagnostic(
                    _impl->errors,
                    closeErrorCount,
                    "GDAL algorithm output could not be closed.");
            }
            closeScope.RethrowCaptureException();
        }
        return _impl->operationSucceeded;
    }

    bool GdalAlgorithmRunner::Finalize()
    {
        const detail::GdalExecutionLock lock;
        if (_impl->finalizeAttempted)
            return _impl->finalizationSucceeded;

        if (!_impl->executionSucceeded || _impl->algorithm == nullptr)
        {
            AddError(_impl->errors, "GDAL algorithm cannot be finalized before a successful run.");
            return false;
        }

        _impl->finalizeAttempted = true;
        GdalErrorScope scope;
        const std::size_t previousCount = _impl->errors.size();
        _impl->finalizationSucceeded = _impl->algorithm->Finalize();
        AppendErrors(_impl->errors, scope);
        scope.RethrowCaptureException();

        if (!_impl->finalizationSucceeded)
            AddFailureIfNoDiagnostic(_impl->errors, previousCount, "GDAL algorithm finalization failed.");
        return _impl->finalizationSucceeded;
    }

    GdalDatasetPtr GdalAlgorithmRunner::TakeOutputDataset()
    {
        const detail::GdalExecutionLock lock;
        return _impl->operationSucceeded ? std::move(_impl->outputDataset) : GdalDatasetPtr{};
    }

    const std::vector<GdalError>& GdalAlgorithmRunner::Errors() const noexcept
    {
        return _impl->errors;
    }

    GdalOperationResult GdalAlgorithmRunner::Result() const
    {
        GdalOperationResult result;
        result.succeeded = _impl->operationSucceeded && !HasFailure(_impl->errors);
        result.cancelled = _impl->cancelled;
        result.errors = _impl->errors;
        return result;
    }

    std::string GdalAlgorithmRunner::UsageAsJson() const
    {
        const detail::GdalExecutionLock lock;
        return _impl->algorithm != nullptr ? _impl->algorithm->GetUsageAsJSON() : std::string{};
    }
}
