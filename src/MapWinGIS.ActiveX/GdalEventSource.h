// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalEventSource.h
 * @brief Declares owner-thread dispatch and COM event delivery for GDAL work.
 *
 * Owns the operation dispatcher and its message window, and invokes
 * connection-point sinks for progress and completion notifications.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ActiveXModule.h"
#include "GdalDispatchWindow.h"
#include "GdalOperationDispatcher.h"

#include <atlctl.h>

namespace mwg::activex
{
    class GdalEventSource final
    {
    public:
        GdalEventSource(
            ATL::CComDynamicUnkArray& sinks,
            GdalDispatchWindow::Callback callback,
            void* context);
        ~GdalEventSource();

        GdalEventSource(const GdalEventSource&) = delete;
        GdalEventSource& operator=(const GdalEventSource&) = delete;

        [[nodiscard]] HRESULT EnsureDispatchWindow() noexcept;
        [[nodiscard]] std::uint64_t Begin(
            std::string algorithmPath,
            std::vector<std::string> arguments);
        [[nodiscard]] GdalCancelResult Cancel(std::uint64_t operationId) noexcept;
        [[nodiscard]] GdalOperationLookup GetResult(
            std::uint64_t operationId) const;
        [[nodiscard]] GdalOperationLookup End(std::uint64_t operationId);
        void RecordCallbackError(
            std::uint64_t operationId,
            HRESULT result) noexcept;
        void OnSize() noexcept;
        void OnDestroy() noexcept;
        [[nodiscard]] LRESULT OnDispatch(BOOL& handled);
        void Close() noexcept;
        void Shutdown() noexcept;

    private:
        static bool PostDispatchMessage(void* window) noexcept;
        static void UnlockActiveXModule(void*) noexcept;
        void DestroyDispatchWindow() noexcept;
        [[nodiscard]] HRESULT FireProgress(
            const GdalOperationNotification& notification);
        [[nodiscard]] HRESULT FireCompleted(std::uint64_t operationId);
        [[nodiscard]] HRESULT FireEvent(
            DISPID eventId,
            DISPPARAMS& parameters,
            std::uint64_t operationId);

        GdalOperationDispatcher _dispatcher;
        ATL::CComDynamicUnkArray& _sinks;
        GdalDispatchWindow::Callback _dispatchCallback = nullptr;
        void* _dispatchContext = nullptr;
        std::shared_ptr<GdalDispatchWindow> _dispatchWindow;
        bool _dispatching = false;
        bool _dispatchWindowDestroyPending = false;
        bool _closing = false;
    };
}
