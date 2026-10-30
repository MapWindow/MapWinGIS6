// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalUtils.cpp
 * @brief Implements native compatibility methods for legacy GDAL operations.
 *
 * Uses GDAL utility APIs for legacy command-line option compatibility and the
 * GDAL algorithm runner for vector clipping.
 */
#include "mapwingis/gdal/GdalUtils.h"

#include "internal/Utf8.h"
#include "internal/GdalExecution.h"
#include "mapwingis/gdal/GdalAlgorithmRunner.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/gdal/GdalTypedOptions.h"

#include <cpl_error.h>
#include <gdal_priv.h>
#include <gdal_utils.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace mwg
{
    namespace
    {
        constexpr int kCompatibilityErrorCode = CPLE_AppDefined;

        bool HasNullByte(const std::vector<std::string>& values)
        {
            return std::any_of(values.begin(), values.end(),
                               [](const std::string& value)
                               {
                                   return value.find('\0') != std::string::npos;
                               });
        }

        bool RecordFailure(std::vector<GdalError>& errors, std::string message)
        {
            errors.clear();
            errors.push_back(
                {GdalErrorClass::Failure, kCompatibilityErrorCode, std::move(message)});
            return false;
        }

        struct DatasetCloser
        {
            void operator()(GDALDataset* dataset) const noexcept
            {
                if (dataset != nullptr)
                    GDALClose(dataset);
            }
        };

        struct TranslateOptionsDeleter
        {
            void operator()(GDALTranslateOptions* options) const noexcept
            {
                GDALTranslateOptionsFree(options);
            }
        };

        struct WarpOptionsDeleter
        {
            void operator()(GDALWarpAppOptions* options) const noexcept
            {
                GDALWarpAppOptionsFree(options);
            }
        };

        struct VectorTranslateOptionsDeleter
        {
            void operator()(GDALVectorTranslateOptions* options) const noexcept
            {
                GDALVectorTranslateOptionsFree(options);
            }
        };

        using DatasetPtr = std::unique_ptr<GDALDataset, DatasetCloser>;
        using TranslateOptionsPtr =
            std::unique_ptr<GDALTranslateOptions, TranslateOptionsDeleter>;
        using WarpOptionsPtr = std::unique_ptr<GDALWarpAppOptions, WarpOptionsDeleter>;
        using VectorTranslateOptionsPtr =
            std::unique_ptr<GDALVectorTranslateOptions, VectorTranslateOptionsDeleter>;

        struct ArgumentList
        {
            std::vector<std::string> storage;
            std::vector<char*> pointers;
        };

        ArgumentList MakeArguments(const std::vector<std::string>& arguments)
        {
            ArgumentList result;
            result.storage = arguments;
            result.pointers.reserve(result.storage.size() + 1);
            for (std::string& argument : result.storage)
                result.pointers.push_back(argument.data());
            result.pointers.push_back(nullptr);
            return result;
        }

        DatasetPtr OpenDataset(const std::filesystem::path& path, const unsigned int flags)
        {
            const std::string utf8Path = detail::ToUtf8(path);
            return DatasetPtr(static_cast<GDALDataset*>(
                GDALOpenEx(utf8Path.c_str(), flags, nullptr, nullptr, nullptr)));
        }

        bool CloseDataset(DatasetPtr& dataset) noexcept
        {
            GDALDataset* rawDataset = dataset.release();
            return rawDataset == nullptr || GDALClose(rawDataset) == CE_None;
        }

        class ScopedGdalConfigOptions
        {
        public:
            ~ScopedGdalConfigOptions()
            {
                for (auto option = _options.rbegin(); option != _options.rend(); ++option)
                    option->reset();
            }

            void Add(const std::string& name, const std::string& value)
            {
                _options.push_back(std::make_unique<ScopedGdalConfigOption>(name, value));
            }

        private:
            std::vector<std::unique_ptr<ScopedGdalConfigOption>> _options;
        };

        bool HasFailure(const std::vector<GdalError>& errors)
        {
            return std::any_of(errors.begin(), errors.end(),
                               [](const GdalError& error)
                               {
                                   return error.errorClass == GdalErrorClass::Failure ||
                                          error.errorClass == GdalErrorClass::Fatal;
                               });
        }

        template <typename Operation>
        bool RunUtilityOperation(std::vector<GdalError>& errors, bool& cancelled,
                                 GdalProgressAdapter::Callback progress,
                                 const GdalCancellationToken* cancellationToken,
                                 Operation&& operation)
        {
            const detail::GdalExecutionLock lock;
            errors.clear();
            GdalEnvironment::Initialize();
            if (cancellationToken != nullptr && cancellationToken->IsCancellationRequested())
            {
                cancelled = true;
                return RecordFailure(errors, "GDAL compatibility operation was cancelled.");
            }

            GdalProgressAdapter adapter(std::move(progress), cancellationToken);
            GdalErrorScope scope;
            const bool succeeded = operation(adapter);
            errors = scope.Errors();
            scope.RethrowCaptureException();

            try
            {
                adapter.RethrowCallbackException();
            }
            catch (const std::exception& error)
            {
                errors.push_back({GdalErrorClass::Failure, kCompatibilityErrorCode,
                                  "GDAL progress callback failed: " + std::string(error.what())});
                return false;
            }
            catch (...)
            {
                errors.push_back({GdalErrorClass::Failure, kCompatibilityErrorCode,
                                  "GDAL progress callback failed with a non-standard exception."});
                return false;
            }
            cancelled = adapter.WasCancelled();
            if (cancelled)
            {
                errors.push_back({GdalErrorClass::Failure, kCompatibilityErrorCode,
                                  "GDAL compatibility operation was cancelled."});
                return false;
            }
            if (!succeeded && !HasFailure(errors))
            {
                errors.push_back({GdalErrorClass::Failure, kCompatibilityErrorCode,
                                  "The GDAL compatibility operation failed."});
            }
            return succeeded && !HasFailure(errors);
        }

        std::string JoinErrors(const std::vector<GdalError>& errors)
        {
            std::ostringstream message;
            for (const GdalError& error : errors)
            {
                if (message.tellp() > 0)
                    message << '\n';
                message << error.message;
            }
            return message.str();
        }

        bool RunClipAlgorithm(std::vector<GdalError>& errors,
                              const std::filesystem::path& subjectFilename,
                              const std::filesystem::path& overlayFilename,
                              const std::filesystem::path& destinationFilename,
                              bool& cancelled, GdalProgressAdapter::Callback progress,
                              const GdalCancellationToken* cancellationToken)
        {
            GdalAlgorithmRunner runner("vector clip");
            VectorClipOptions options;
            options.input = subjectFilename;
            options.output = destinationFilename;
            options.like = overlayFilename;
            options.overwrite = true;

            try
            {
                const bool configured = ConfigureGdalAlgorithm(runner, options.ToArgs());
                const bool succeeded = configured && runner.Run(std::move(progress), cancellationToken);
                errors = runner.Errors();
                cancelled = runner.Result().WasCancelled();
                if (!succeeded && !HasFailure(errors))
                {
                    errors.push_back({GdalErrorClass::Failure, kCompatibilityErrorCode,
                                      "The GDAL vector clipping operation failed."});
                }
                return succeeded;
            }
            catch (...)
            {
                errors = runner.Errors();
                cancelled = runner.Result().WasCancelled();
                throw;
            }
        }

        bool RunVectorTranslateOperation(
            std::vector<GdalError>& errors,
            const std::filesystem::path& sourceFilename,
            const std::filesystem::path& destinationFilename,
            const std::vector<std::string>& options, bool& cancelled,
            GdalProgressAdapter::Callback progress,
            const GdalCancellationToken* cancellationToken)
        {
            return RunUtilityOperation(
                errors, cancelled, std::move(progress), cancellationToken,
                [&](GdalProgressAdapter& adapter)
                {
                    const std::string destination = detail::ToUtf8(destinationFilename);
                    DatasetPtr input =
                        OpenDataset(sourceFilename, GDAL_OF_VECTOR | GDAL_OF_READONLY);
                    if (input == nullptr)
                        return false;

                    ArgumentList arguments = MakeArguments(options);
                    VectorTranslateOptionsPtr translateOptions(
                        GDALVectorTranslateOptionsNew(arguments.pointers.data(), nullptr));
                    if (translateOptions == nullptr)
                        return false;
                    GDALVectorTranslateOptionsSetProgress(
                        translateOptions.get(), adapter.Function(), adapter.UserData());

                    GDALDatasetH sources[] = {input.get()};
                    int usageError = FALSE;
                    DatasetPtr output(static_cast<GDALDataset*>(
                        GDALVectorTranslate(destination.c_str(), nullptr, 1, sources,
                                            translateOptions.get(), &usageError)));
                    const bool succeeded = output != nullptr && usageError == FALSE;
                    const bool closed = CloseDataset(output);
                    return succeeded && closed;
                });
        }
    }

    struct GdalUtils::Impl
    {
        std::vector<GdalError> errors;
        bool succeeded = false;
        bool cancelled = false;
    };

    GdalUtils::GdalUtils()
        : GdalUtils(std::make_unique<Impl>())
    {
    }

    GdalUtils::GdalUtils(std::unique_ptr<Impl> impl)
        : _impl(std::move(impl))
    {
    }

    GdalUtils::~GdalUtils() = default;

    bool GdalUtils::GdalRasterTranslate(
        const std::filesystem::path& source, const std::filesystem::path& destination,
        const std::vector<std::string>& options)
    {
        return GdalRasterTranslate(source, destination, options, {}, nullptr);
    }

    bool GdalUtils::GdalRasterWarp(
        const std::filesystem::path& source, const std::filesystem::path& destination,
        const std::vector<std::string>& options)
    {
        return GdalRasterWarp(source, destination, options, {}, nullptr);
    }

    bool GdalUtils::GdalVectorTranslate(
        const std::filesystem::path& source, const std::filesystem::path& destination,
        const std::vector<std::string>& options, const bool useSharedConnection)
    {
        return GdalVectorTranslate(source, destination, options, useSharedConnection, {}, nullptr);
    }

    bool GdalUtils::GdalVectorReproject(
        const std::filesystem::path& source, const std::filesystem::path& destination,
        const int sourceEpsgCode, const int destinationEpsgCode, const bool useSharedConnection)
    {
        return GdalVectorReproject(source, destination, sourceEpsgCode, destinationEpsgCode,
                                   useSharedConnection, {}, nullptr);
    }

    bool GdalUtils::GdalBuildOverviews(
        const std::filesystem::path& source, const std::string_view resampling,
        const std::vector<int>& overviews, const std::vector<int>& bands,
        const std::vector<std::pair<std::string, std::string>>& configOptions)
    {
        return GdalBuildOverviews(source, resampling, overviews, bands, configOptions, {}, nullptr);
    }

    bool GdalUtils::ClipVectorWithVector(
        const std::filesystem::path& subject, const std::filesystem::path& overlay,
        const std::filesystem::path& destination, const bool useSharedConnection)
    {
        return ClipVectorWithVector(subject, overlay, destination, useSharedConnection, {}, nullptr);
    }

    int GdalUtils::LastErrorCode() const noexcept
    {
        for (auto error = _impl->errors.rbegin(); error != _impl->errors.rend(); ++error)
        {
            if (error->errorClass == GdalErrorClass::Failure ||
                error->errorClass == GdalErrorClass::Fatal)
            {
                return error->code;
            }
        }
        return 0;
    }

    std::string GdalUtils::ErrorMsg(const int errorCode) const
    {
        for (auto error = _impl->errors.rbegin(); error != _impl->errors.rend(); ++error)
        {
            if (error->code == errorCode)
                return error->message;
        }
        return {};
    }

    std::string GdalUtils::DetailedErrorMsg() const
    {
        return JoinErrors(_impl->errors);
    }

    std::vector<GdalError> GdalUtils::Errors() const
    {
        return _impl->errors;
    }

    GdalOperationResult GdalUtils::Result() const
    {
        GdalOperationResult result;
        result.succeeded = _impl->succeeded;
        result.cancelled = _impl->cancelled;
        result.errors = _impl->errors;
        return result;
    }

    bool GdalUtils::GdalRasterTranslate(
        const std::filesystem::path& sourceFilename,
        const std::filesystem::path& destinationFilename,
        const std::vector<std::string>& options,
        GdalProgressAdapter::Callback progress,
        const GdalCancellationToken* cancellationToken)
    {
        const detail::GdalExecutionLock lock;
        _impl->succeeded = false;
        _impl->cancelled = false;
        if (HasNullByte(options))
            return RecordFailure(_impl->errors, "GDAL utility options must not contain null bytes.");

        return _impl->succeeded = RunUtilityOperation(
            _impl->errors, _impl->cancelled, std::move(progress), cancellationToken,
            [&](GdalProgressAdapter& adapter)
            {
                const std::string destination = detail::ToUtf8(destinationFilename);
                DatasetPtr input = OpenDataset(sourceFilename, GDAL_OF_RASTER | GDAL_OF_READONLY);
                if (input == nullptr)
                    return false;

                ArgumentList arguments = MakeArguments(options);
                TranslateOptionsPtr translateOptions(
                    GDALTranslateOptionsNew(arguments.pointers.data(), nullptr));
                if (translateOptions == nullptr)
                    return false;
                GDALTranslateOptionsSetProgress(
                    translateOptions.get(), adapter.Function(), adapter.UserData());

                int usageError = FALSE;
                DatasetPtr output(static_cast<GDALDataset*>(
                    GDALTranslate(destination.c_str(), input.get(), translateOptions.get(),
                                  &usageError)));
                const bool succeeded = output != nullptr && usageError == FALSE;
                const bool closed = CloseDataset(output);
                return succeeded && closed;
            });
    }

    bool GdalUtils::GdalRasterWarp(
        const std::filesystem::path& sourceFilename,
        const std::filesystem::path& destinationFilename,
        const std::vector<std::string>& options,
        GdalProgressAdapter::Callback progress,
        const GdalCancellationToken* cancellationToken)
    {
        const detail::GdalExecutionLock lock;
        _impl->succeeded = false;
        _impl->cancelled = false;
        if (HasNullByte(options))
            return RecordFailure(_impl->errors, "GDAL utility options must not contain null bytes.");

        return _impl->succeeded = RunUtilityOperation(
            _impl->errors, _impl->cancelled, std::move(progress), cancellationToken,
            [&](GdalProgressAdapter& adapter)
            {
                const std::string destination = detail::ToUtf8(destinationFilename);
                DatasetPtr input = OpenDataset(sourceFilename, GDAL_OF_RASTER | GDAL_OF_READONLY);
                if (input == nullptr)
                    return false;

                ArgumentList arguments = MakeArguments(options);
                WarpOptionsPtr warpOptions(
                    GDALWarpAppOptionsNew(arguments.pointers.data(), nullptr));
                if (warpOptions == nullptr)
                    return false;
                GDALWarpAppOptionsSetProgress(
                    warpOptions.get(), adapter.Function(), adapter.UserData());

                GDALDatasetH sources[] = {input.get()};
                int usageError = FALSE;
                DatasetPtr output(static_cast<GDALDataset*>(
                    GDALWarp(destination.c_str(), nullptr, 1, sources, warpOptions.get(),
                             &usageError)));
                const bool succeeded = output != nullptr && usageError == FALSE;
                const bool closed = CloseDataset(output);
                return succeeded && closed;
            });
    }

    bool GdalUtils::GdalVectorTranslate(
        const std::filesystem::path& sourceFilename,
        const std::filesystem::path& destinationFilename,
        const std::vector<std::string>& options,
        const bool useSharedConnection,
        GdalProgressAdapter::Callback progress,
        const GdalCancellationToken* cancellationToken)
    {
        const detail::GdalExecutionLock lock;
        _impl->succeeded = false;
        _impl->cancelled = false;
        (void)useSharedConnection;
        if (HasNullByte(options))
            return RecordFailure(_impl->errors, "GDAL utility options must not contain null bytes.");

        return _impl->succeeded = RunVectorTranslateOperation(
            _impl->errors, sourceFilename, destinationFilename, options, _impl->cancelled,
            std::move(progress), cancellationToken);
    }

    bool GdalUtils::GdalVectorReproject(
        const std::filesystem::path& sourceFilename,
        const std::filesystem::path& destinationFilename,
        const int sourceEpsgCode,
        const int destinationEpsgCode,
        const bool useSharedConnection,
        GdalProgressAdapter::Callback progress,
        const GdalCancellationToken* cancellationToken)
    {
        const detail::GdalExecutionLock lock;
        _impl->succeeded = false;
        _impl->cancelled = false;
        (void)useSharedConnection;
        const std::vector<std::string> options{
            "-overwrite",
            "-s_srs", "EPSG:" + std::to_string(sourceEpsgCode),
            "-t_srs", "EPSG:" + std::to_string(destinationEpsgCode)};
        return _impl->succeeded = RunVectorTranslateOperation(
            _impl->errors, sourceFilename, destinationFilename, options, _impl->cancelled,
            std::move(progress), cancellationToken);
    }

    bool GdalUtils::GdalBuildOverviews(
        const std::filesystem::path& sourceFilename,
        const std::string_view resamplingMethod,
        const std::vector<int>& overviewList,
        const std::vector<int>& bandList,
        const std::vector<std::pair<std::string, std::string>>& configOptions,
        GdalProgressAdapter::Callback progress,
        const GdalCancellationToken* cancellationToken)
    {
        const detail::GdalExecutionLock lock;
        _impl->succeeded = false;
        _impl->cancelled = false;
        if (resamplingMethod.find('\0') != std::string_view::npos)
            return RecordFailure(_impl->errors, "GDAL resampling method must not contain null bytes.");
        if (overviewList.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            bandList.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            return RecordFailure(_impl->errors, "GDAL overview and band lists are too large.");
        }
        for (const auto& [name, value] : configOptions)
        {
            if (name.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
            {
                return RecordFailure(_impl->errors,
                                     "GDAL configuration values must not contain null bytes.");
            }
        }

        return _impl->succeeded = RunUtilityOperation(
            _impl->errors, _impl->cancelled, std::move(progress), cancellationToken,
            [&](GdalProgressAdapter& adapter)
            {
                ScopedGdalConfigOptions scopedOptions;
                for (const auto& [name, value] : configOptions)
                    scopedOptions.Add(name, value);

                DatasetPtr dataset = OpenDataset(sourceFilename, GDAL_OF_RASTER | GDAL_OF_READONLY);
                if (dataset == nullptr)
                    return false;

                const std::string resampling(resamplingMethod);
                std::vector<int> automaticOverviewList;
                const std::vector<int>* overviewLevels = &overviewList;
                if (overviewList.empty())
                {
                    int overviewWidth = dataset->GetRasterXSize();
                    int overviewHeight = dataset->GetRasterYSize();
                    int overviewFactor = 2;
                    while (overviewWidth > 256 || overviewHeight > 256)
                    {
                        automaticOverviewList.push_back(overviewFactor);
                        overviewWidth = overviewWidth / 2 + overviewWidth % 2;
                        overviewHeight = overviewHeight / 2 + overviewHeight % 2;
                        overviewFactor *= 2;
                    }
                    overviewLevels = &automaticOverviewList;
                    if (overviewLevels->empty())
                        return true;
                }

                const CPLErr result = GDALBuildOverviews(
                    dataset.get(), resampling.c_str(), static_cast<int>(overviewLevels->size()),
                    overviewLevels->data(),
                    static_cast<int>(bandList.size()), bandList.data(),
                    adapter.Function(), adapter.UserData());
                const bool closed = CloseDataset(dataset);
                return result == CE_None && closed;
            });
    }

    bool GdalUtils::ClipVectorWithVector(
        const std::filesystem::path& subjectFilename,
        const std::filesystem::path& overlayFilename,
        const std::filesystem::path& destinationFilename,
        const bool useSharedConnection,
        GdalProgressAdapter::Callback progress,
        const GdalCancellationToken* cancellationToken)
    {
        const detail::GdalExecutionLock lock;
        _impl->succeeded = false;
        _impl->cancelled = false;
        (void)useSharedConnection;
        _impl->errors.clear();
        GdalEnvironment::Initialize();
        return _impl->succeeded = RunClipAlgorithm(
            _impl->errors, subjectFilename, overlayFilename, destinationFilename,
            _impl->cancelled, std::move(progress), cancellationToken);
    }
}
