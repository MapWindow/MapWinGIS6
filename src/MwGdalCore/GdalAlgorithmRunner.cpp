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

#include <algorithm>
#include <cpl_error.h>
#include <gdal.h>
#include <gdalalgorithm.h>

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

        void AppendErrors(std::vector<GdalError>& target, const GdalErrorScope& source)
        {
            const auto& errors = source.Errors();
            target.insert(target.end(), errors.begin(), errors.end());
        }

        void AddFailureIfNoDiagnostic(std::vector<GdalError>& errors,
                                      const std::size_t previousCount,
                                      std::string message)
        {
            if (errors.size() == previousCount)
                AddError(errors, std::move(message));
        }

        template <typename T>
        bool SetArgumentValue(GDALAlgorithm* algorithm,
                              std::vector<GdalError>& errors,
                              bool& configurationValid,
                              const std::string_view name,
                              T&& value)
        {
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
    };

    void GdalDatasetDeleter::operator()(GDALDataset* dataset) const noexcept
    {
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

    GdalAlgorithmRunner::~GdalAlgorithmRunner() = default;

    bool GdalAlgorithmRunner::CanConfigure(const std::string_view name)
    {
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

    bool GdalAlgorithmRunner::SetArgument(const std::string_view name,
                                          const std::vector<std::string>& value)
    {
        if (!CanConfigure(name))
            return false;
        return SetArgumentValue(_impl->algorithm.get(), _impl->errors,
                                _impl->configurationValid, name, value);
    }

    bool GdalAlgorithmRunner::ParseCommandLineArgs(const std::vector<std::string>& args)
    {
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

    bool GdalAlgorithmRunner::Run(GdalProgressAdapter::Callback progress,
                                  const GdalCancellationToken* cancellationToken)
    {
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
            AddError(_impl->errors, "GDAL algorithm execution was cancelled.");
            return false;
        }

        GdalProgressAdapter adapter(std::move(progress), cancellationToken);
        GdalErrorScope scope;
        const std::size_t previousCount = _impl->errors.size();
        const bool succeeded = _impl->algorithm->Run(adapter.Function(), adapter.UserData());
        AppendErrors(_impl->errors, scope);
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

        if (adapter.WasCancelled())
        {
            AddError(_impl->errors, "GDAL algorithm execution was cancelled.");
            return false;
        }
        if (!succeeded || callbackFailed)
        {
            AddFailureIfNoDiagnostic(_impl->errors, previousCount, "GDAL algorithm execution failed.");
            return false;
        }

        _impl->executionSucceeded = true;
        GDALAlgorithmArg* output = _impl->algorithm->GetArg("output");
        if (output != nullptr && output->GetType() == GAAT_DATASET)
        {
            _impl->outputDataset.reset(
                output->Get<GDALArgDatasetValue>().GetDatasetIncreaseRefCount());
        }

        const bool finalized = Finalize();
        if (!finalized)
            _impl->outputDataset.reset();
        return finalized;
    }

    bool GdalAlgorithmRunner::Finalize()
    {
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
        return _impl->finalizationSucceeded ? std::move(_impl->outputDataset) : GdalDatasetPtr{};
    }

    const std::vector<GdalError>& GdalAlgorithmRunner::Errors() const noexcept
    {
        return _impl->errors;
    }

    std::string GdalAlgorithmRunner::UsageAsJson() const
    {
        return _impl->algorithm != nullptr ? _impl->algorithm->GetUsageAsJSON() : std::string{};
    }
}
