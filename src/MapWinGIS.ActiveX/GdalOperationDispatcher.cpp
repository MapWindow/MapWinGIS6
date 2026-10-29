// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalOperationDispatcher.cpp
 * @brief Implements asynchronous GDAL execution and owner-thread notifications.
 */
#include "GdalOperationDispatcher.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include "mapwingis/gdal/GdalAlgorithmRunner.h"

namespace mwg::activex
{
    struct GdalOperationDispatcher::State
    {
        struct Operation
        {
            std::uint64_t id = 0;
            GdalCancellationToken cancellation;
            bool completed = false;
            bool completionPending = false;
            int callbackErrorCode = 0;
            std::string callbackError;
            std::optional<GdalOperationNotification> progress;
            GdalOperationSnapshot result;
        };

        std::mutex mutex;
        std::unordered_map<std::uint64_t, std::shared_ptr<Operation>> operations;
        std::uint64_t nextId = 1;
        WakeCallback wakeCallback = nullptr;
        void* wakeContext = nullptr;
        WorkerFinishedCallback workerFinishedCallback = nullptr;
        void* workerFinishedContext = nullptr;
        bool wakePending = false;
        bool stopping = false;
    };

    namespace
    {
        [[nodiscard]] bool HasPendingNotifications(
            const GdalOperationDispatcher::State& state) noexcept
        {
            return std::any_of(
                state.operations.begin(),
                state.operations.end(),
                [](const auto& entry)
                {
                    return entry.second->progress.has_value() ||
                        entry.second->completionPending;
                });
        }

        void WakeOwner(GdalOperationDispatcher::State& state) noexcept
        {
            if (state.stopping || state.wakePending ||
                state.wakeCallback == nullptr ||
                !HasPendingNotifications(state))
            {
                return;
            }

            state.wakePending = state.wakeCallback(state.wakeContext);
        }

        void QueueProgress(
            const std::shared_ptr<GdalOperationDispatcher::State>& state,
            const std::uint64_t operationId,
            const double progress,
            const std::string_view message)
        {
            std::lock_guard lock(state->mutex);
            const auto operation = state->operations.find(operationId);
            if (state->stopping || operation == state->operations.end() ||
                operation->second->completed)
            {
                return;
            }

            operation->second->progress = GdalOperationNotification{
                operationId,
                GdalNotificationKind::Progress,
                progress,
                std::string(message)};
            WakeOwner(*state);
        }

        [[nodiscard]] GdalOperationSnapshot MakeSnapshot(
            const mwg::GdalOperationResult& result)
        {
            GdalOperationSnapshot snapshot;
            snapshot.succeeded = result.Succeeded();
            snapshot.cancelled = result.WasCancelled();
            snapshot.errorCode = result.LastErrorCode();
            snapshot.summary = result.ErrorMsg();
            snapshot.diagnostics = result.DetailedErrorMsg();
            for (const mwg::GdalError& error : result.AllErrors())
            {
                if (error.errorClass == mwg::GdalErrorClass::Warning)
                {
                    snapshot.warnings.push_back(error.message);
                }
            }

            if (snapshot.cancelled && snapshot.summary.empty())
            {
                snapshot.summary = "GDAL operation was cancelled.";
            }
            else if (!snapshot.succeeded && snapshot.summary.empty())
            {
                snapshot.summary = "GDAL operation failed without a diagnostic.";
            }
            return snapshot;
        }

        void Execute(
            const std::shared_ptr<GdalOperationDispatcher::State>& state,
            const std::shared_ptr<GdalOperationDispatcher::State::Operation>& operation,
            std::string algorithmPath,
            std::vector<std::string> arguments,
            const GdalOperationDispatcher::WorkerFinishedCallback workerFinishedCallback,
            void* const workerFinishedContext) noexcept
        {
            struct WorkerFinishedGuard final
            {
                GdalOperationDispatcher::WorkerFinishedCallback callback;
                void* context;

                ~WorkerFinishedGuard()
                {
                    if (callback != nullptr)
                    {
                        callback(context);
                    }
                }
            } workerFinished{workerFinishedCallback, workerFinishedContext};

            GdalOperationSnapshot result;
            try
            {
                if (operation->cancellation.IsCancellationRequested())
                {
                    result.cancelled = true;
                    result.summary = "GDAL operation was cancelled before execution.";
                }
                else
                {
                    mwg::GdalAlgorithmRunner runner(algorithmPath);
                    if (runner.ParseCommandLineArgs(arguments) &&
                        runner.ValidateArguments())
                    {
                        static_cast<void>(runner.Run(
                            [state, operationId = operation->id](
                                const double progress,
                                const std::string_view message)
                            {
                                QueueProgress(state, operationId, progress, message);
                                return true;
                            },
                            &operation->cancellation));
                    }
                    result = MakeSnapshot(runner.Result());
                }
            }
            catch (const std::exception& exception)
            {
                result.succeeded = false;
                result.summary = exception.what();
                result.diagnostics = exception.what();
            }
            catch (...)
            {
                result.succeeded = false;
                result.summary = "GDAL operation failed with an unknown native exception.";
                result.diagnostics = result.summary;
            }

            std::lock_guard lock(state->mutex);
            const auto current = state->operations.find(operation->id);
            if (current == state->operations.end())
            {
                return;
            }

            operation->result = std::move(result);
            operation->completed = true;
            if (!state->stopping)
            {
                operation->completionPending = true;
                WakeOwner(*state);
            }
        }
    }

    GdalOperationDispatcher::GdalOperationDispatcher()
        : _state(std::make_shared<State>())
    {
    }

    GdalOperationDispatcher::~GdalOperationDispatcher()
    {
        Shutdown();
    }

    void GdalOperationDispatcher::SetWakeCallback(
        const WakeCallback callback,
        void* const context) noexcept
    {
        std::lock_guard lock(_state->mutex);
        _state->wakeCallback = callback;
        _state->wakeContext = context;
        _state->wakePending = false;
        WakeOwner(*_state);
    }

    void GdalOperationDispatcher::SetWorkerFinishedCallback(
        const WorkerFinishedCallback callback,
        void* const context) noexcept
    {
        std::lock_guard lock(_state->mutex);
        _state->workerFinishedCallback = callback;
        _state->workerFinishedContext = context;
    }

    std::uint64_t GdalOperationDispatcher::Begin(
        std::string algorithmPath,
        std::vector<std::string> arguments)
    {
        std::shared_ptr<State::Operation> operation;
        std::uint64_t operationId = 0;
        WorkerFinishedCallback workerFinishedCallback = nullptr;
        void* workerFinishedContext = nullptr;
        {
            std::lock_guard lock(_state->mutex);
            if (_state->stopping)
            {
                throw std::runtime_error("The GDAL operation dispatcher is shutting down.");
            }
            if (_state->operations.size() >= kMaxOutstandingOperations)
            {
                throw std::runtime_error("The maximum number of outstanding GDAL operations was reached.");
            }

            operation = std::make_shared<State::Operation>();
            do
            {
                operationId = _state->nextId++;
            } while (operationId == 0 || _state->operations.contains(operationId));
            operation->id = operationId;
            _state->operations.emplace(operationId, operation);
            workerFinishedCallback = _state->workerFinishedCallback;
            workerFinishedContext = _state->workerFinishedContext;
        }

        try
        {
            std::thread worker(
                Execute,
                _state,
                operation,
                std::move(algorithmPath),
                std::move(arguments),
                workerFinishedCallback,
                workerFinishedContext);
            worker.detach();
        }
        catch (...)
        {
            std::lock_guard lock(_state->mutex);
            _state->operations.erase(operationId);
            throw;
        }

        return operationId;
    }

    GdalCancelResult GdalOperationDispatcher::Cancel(
        const std::uint64_t operationId) noexcept
    {
        std::lock_guard lock(_state->mutex);
        const auto operation = _state->operations.find(operationId);
        if (operation == _state->operations.end())
        {
            return GdalCancelResult::NotFound;
        }
        if (operation->second->completed)
        {
            return GdalCancelResult::Completed;
        }
        if (operation->second->cancellation.IsCancellationRequested())
        {
            return GdalCancelResult::AlreadyRequested;
        }

        operation->second->cancellation.Cancel();
        return GdalCancelResult::Requested;
    }

    GdalEndResult GdalOperationDispatcher::End(
        const std::uint64_t operationId,
        GdalOperationSnapshot& result)
    {
        std::lock_guard lock(_state->mutex);
        const auto operation = _state->operations.find(operationId);
        if (operation == _state->operations.end())
        {
            return GdalEndResult::NotFound;
        }
        if (!operation->second->completed)
        {
            return GdalEndResult::Pending;
        }

        result = std::move(operation->second->result);
        result.callbackErrorCode = operation->second->callbackErrorCode;
        result.callbackError = std::move(operation->second->callbackError);
        _state->operations.erase(operation);
        return GdalEndResult::Ended;
    }

    GdalEndResult GdalOperationDispatcher::GetResult(
        const std::uint64_t operationId,
        GdalOperationSnapshot& result) const
    {
        std::lock_guard lock(_state->mutex);
        const auto operation = _state->operations.find(operationId);
        if (operation == _state->operations.end())
        {
            return GdalEndResult::NotFound;
        }
        if (!operation->second->completed)
        {
            return GdalEndResult::Pending;
        }

        result = operation->second->result;
        result.callbackErrorCode = operation->second->callbackErrorCode;
        result.callbackError = operation->second->callbackError;
        return GdalEndResult::Ended;
    }

    std::vector<GdalOperationNotification>
    GdalOperationDispatcher::DrainNotifications()
    {
        std::vector<GdalOperationNotification> notifications;
        std::lock_guard lock(_state->mutex);
        _state->wakePending = false;
        for (auto& [operationId, operation] : _state->operations)
        {
            if (operation->progress.has_value())
            {
                notifications.push_back(std::move(*operation->progress));
                operation->progress.reset();
            }
            if (operation->completionPending)
            {
                notifications.push_back(GdalOperationNotification{
                    operationId,
                    GdalNotificationKind::Completed,
                    1.0,
                    {}});
                operation->completionPending = false;
            }
        }
        return notifications;
    }

    void GdalOperationDispatcher::RecordCallbackError(
        const std::uint64_t operationId,
        const int errorCode,
        std::string message)
    {
        std::lock_guard lock(_state->mutex);
        const auto operation = _state->operations.find(operationId);
        if (operation == _state->operations.end())
        {
            return;
        }

        operation->second->callbackErrorCode = errorCode;
        operation->second->callbackError = std::move(message);
    }

    void GdalOperationDispatcher::Shutdown() noexcept
    {
        std::lock_guard lock(_state->mutex);
        if (_state->stopping)
        {
            return;
        }

        _state->stopping = true;
        _state->wakeCallback = nullptr;
        _state->wakeContext = nullptr;
        _state->wakePending = false;
        for (auto& [operationId, operation] : _state->operations)
        {
            static_cast<void>(operationId);
            operation->cancellation.Cancel();
            operation->progress.reset();
            operation->completionPending = false;
        }
    }
}
