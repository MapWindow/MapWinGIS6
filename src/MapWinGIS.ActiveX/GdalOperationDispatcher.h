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
#include <string>
#include <string_view>
#include <vector>

#include "mapwingis/gdal/GdalRuntime.h"

namespace mwg::activex
{
    enum class GdalNotificationKind
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

    enum class GdalCancelResult
    {
        Requested,
        AlreadyRequested,
        Completed,
        NotFound,
    };

    enum class GdalEndResult
    {
        Ended,
        Pending,
        NotFound,
    };

    /**
     * @brief Runs generic GDAL algorithms and queues progress for owner-thread delivery.
     *
     * The wake callback is a non-blocking, noexcept notification such as
     * PostMessage. It is never a COM callback. Progress messages are coalesced
     * to one per operation; completion messages are retained. At most sixteen
     * operation results can be outstanding, bounding both active work and the
     * notification queue. The owner calls DrainNotifications() and End() on
     * its dispatch thread. Shutdown requests cancellation and detaches workers;
     * workers retain only native state and never wait for owner-thread work.
     */
    class GdalOperationDispatcher final
    {
    public:
        struct State;

        using WakeCallback = bool(*)(void*) noexcept;
        using WorkerFinishedCallback = void(*)(void*) noexcept;
        static constexpr std::size_t kMaxOutstandingOperations = 16;

        GdalOperationDispatcher();
        ~GdalOperationDispatcher();

        GdalOperationDispatcher(const GdalOperationDispatcher&) = delete;
        GdalOperationDispatcher& operator=(const GdalOperationDispatcher&) = delete;

        void SetWakeCallback(WakeCallback callback, void* context) noexcept;
        void SetWorkerFinishedCallback(
            WorkerFinishedCallback callback,
            void* context) noexcept;
        [[nodiscard]] std::uint64_t Begin(
            std::string algorithmPath,
            std::vector<std::string> arguments);
        [[nodiscard]] GdalCancelResult Cancel(std::uint64_t operationId) noexcept;
        [[nodiscard]] GdalEndResult GetResult(
            std::uint64_t operationId,
            GdalOperationSnapshot& result) const;
        [[nodiscard]] GdalEndResult End(
            std::uint64_t operationId,
            GdalOperationSnapshot& result);
        [[nodiscard]] bool BeginNotificationDelivery(
            std::uint64_t operationId) noexcept;
        void EndNotificationDelivery(std::uint64_t operationId) noexcept;
        [[nodiscard]] bool IsDispatchActive() const noexcept;
        [[nodiscard]] std::vector<GdalOperationNotification> DrainNotifications();
        void RecordCallbackError(
            std::uint64_t operationId,
            int errorCode,
            std::string message);
        /// Stops owner-thread notifications without cancelling active operations.
        void Suspend() noexcept;
        void Shutdown() noexcept;

    private:
        std::shared_ptr<State> _state;
    };
}
