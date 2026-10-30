// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalOperationDispatcher.h
 * @brief Declares the native asynchronous GDAL operation and notification queue.
 *
 * Runs generic GDAL algorithms away from the calling thread and queues owned
 * notifications for the caller to dispatch on its thread.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mapwingis/gdal/GdalProgress.h"

namespace mwg::activex
{
    enum class GdalNotificationKind : int
    {
        Progress,
        Completed,
    };

    struct GdalOperationNotification
    {
        std::uint64_t operationId = 0;
        GdalNotificationKind kind = GdalNotificationKind::Progress;
        double progress = 0.0;
        std::string message;
    };

    struct GdalOperationSnapshot
    {
        bool succeeded = false;
        bool cancelled = false;
        int errorCode = 0;
        int callbackErrorCode = 0;
        std::string summary;
        std::string diagnostics;
        std::string callbackError;
        std::vector<std::string> warnings;
    };

    enum class GdalCancelResult : int
    {
        Requested,
        AlreadyRequested,
        Completed,
        NotFound,
    };

    enum class GdalEndResult : int
    {
        Ended,
        Pending,
        NotFound,
    };

    struct GdalOperationLookup final
    {
        GdalEndResult status = GdalEndResult::NotFound;
        std::optional<GdalOperationSnapshot> result;
    };

    /**
     * @brief Runs generic GDAL algorithms and queues progress for owner-thread delivery.
     *
     * The wake callback is a non-blocking, noexcept notification such as
     * PostMessage. It is never a COM callback. Progress messages are coalesced
     * to one per operation; completion messages are retained. At most sixteen
     * operation results can be outstanding, bounding both active work and the
     * notification queue. Control-facing methods are called on the control's
     * owner thread; cancellation is internally synchronized and may be
     * requested from another thread. Shutdown requests cancellation and
     * detaches workers; workers retain only native state and never wait for
     * owner-thread work.
     */
    class GdalOperationDispatcher final
    {
    public:
        struct State;

        using WakeCallback = bool(*)(void*) noexcept;
        using WorkerFinishedCallback = void(*)(void*) noexcept;
        static constexpr std::size_t kMaxOutstandingOperations = 16;

        /// \throws std::bad_alloc when its shared worker state cannot be created.
        GdalOperationDispatcher();
        ~GdalOperationDispatcher();

        GdalOperationDispatcher(const GdalOperationDispatcher&) = delete;
        GdalOperationDispatcher& operator=(const GdalOperationDispatcher&) = delete;

        /// Sets the wakeup callback on the owner thread; the callback must not block.
        void SetWakeCallback(WakeCallback callback, void* context) noexcept;
        /// Sets the worker-completion callback on the owner thread.
        void SetWorkerFinishedCallback(
            WorkerFinishedCallback callback,
            void* context) noexcept;
        /// Starts work; call from the owner thread.
        /// \throws std::runtime_error when stopping or at capacity.
        /// \throws std::bad_alloc when operation state cannot be allocated.
        /// \throws std::system_error when worker creation or state locking fails.
        [[nodiscard]] std::uint64_t Begin(
            std::string algorithmPath,
            std::vector<std::string> arguments);
        /// Thread-safe cancellation request; it does not wait for the worker.
        [[nodiscard]] GdalCancelResult Cancel(std::uint64_t operationId) noexcept;
        /// Must be called on the owner thread.
        /// \throws std::bad_alloc when copying the completed result fails.
        /// \throws std::system_error when state locking fails.
        [[nodiscard]] GdalOperationLookup GetResult(
            std::uint64_t operationId) const;
        /// Must be called on the owner thread.
        /// \throws std::system_error when state locking fails.
        [[nodiscard]] GdalOperationLookup End(std::uint64_t operationId);
        /// Begins owner-thread delivery; call EndNotificationDelivery afterward.
        [[nodiscard]] bool BeginNotificationDelivery(
            std::uint64_t operationId) noexcept;
        /// Completes owner-thread delivery begun by BeginNotificationDelivery.
        void EndNotificationDelivery(std::uint64_t operationId) noexcept;
        /// Safe to query from any thread.
        [[nodiscard]] bool IsDispatchActive() const noexcept;
        /// Drains notifications on the owner thread.
        /// \throws std::bad_alloc when the notification list cannot be allocated.
        /// \throws std::system_error when state locking fails.
        [[nodiscard]] std::vector<GdalOperationNotification> DrainNotifications();
        /// Records a callback failure on the owner thread.
        /// \throws std::bad_alloc when the message cannot be stored.
        /// \throws std::system_error when state locking fails.
        void RecordCallbackError(
            std::uint64_t operationId,
            int errorCode,
            std::string message);
        /// Stops owner-thread notifications without cancelling active operations.
        /// Call on the owner thread.
        void Suspend() noexcept;
        /// Requests cancellation and detaches workers; safe from any thread.
        void Shutdown() noexcept;

    private:
        std::shared_ptr<State> _state;
    };
}
