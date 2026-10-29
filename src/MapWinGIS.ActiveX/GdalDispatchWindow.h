// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalDispatchWindow.h
 * @brief Declares the message-only window used to dispatch GDAL notifications.
 *
 * The window is created on the control's owning COM thread and accepts only
 * posted wake-up messages from native workers.
 */
#pragma once

#include <memory>

#include <atlwin.h>

namespace mwg::activex
{
    inline constexpr UINT kGdalDispatchMessage = WM_APP + 0x316;
    inline constexpr UINT_PTR kGdalDispatchRetryTimerId = 1;
    inline constexpr UINT kGdalDispatchRetryIntervalMilliseconds = 100;

    class GdalDispatchWindow final :
        public ATL::CWindowImpl<GdalDispatchWindow>,
        public std::enable_shared_from_this<GdalDispatchWindow>
    {
    public:
        using Callback = void(*)(void*) noexcept;

        DECLARE_WND_CLASS_EX(L"MapWinGIS6.GdalDispatchWindow", 0, COLOR_WINDOW)

        [[nodiscard]] HWND Create(Callback callback, void* context)
        {
            _callback = callback;
            _context = context;
            const HWND window =
                ATL::CWindowImpl<GdalDispatchWindow>::Create(HWND_MESSAGE);
            if (window == nullptr)
            {
                return nullptr;
            }
            if (::SetTimer(
                    window,
                    kGdalDispatchRetryTimerId,
                    kGdalDispatchRetryIntervalMilliseconds,
                    nullptr) == 0)
            {
                _callback = nullptr;
                _context = nullptr;
                ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                DestroyWindow();
                return nullptr;
            }
            return window;
        }

        void Destroy() noexcept
        {
            _callback = nullptr;
            _context = nullptr;
            if (m_hWnd == nullptr || _destroyRequested)
            {
                return;
            }

            _destroyRequested = true;
            if (_messageDepth != 0)
            {
                _deferredLifetime = shared_from_this();
            }
            ::KillTimer(m_hWnd, kGdalDispatchRetryTimerId);
            DestroyWindow();
        }

        BEGIN_MSG_MAP(GdalDispatchWindow)
            MESSAGE_HANDLER(kGdalDispatchMessage, OnDispatch)
            MESSAGE_HANDLER(WM_TIMER, OnRetryTimer)
        END_MSG_MAP()

        void OnFinalMessage(HWND) noexcept
        {
            _deferredLifetime.reset();
        }

    private:
        class MessageGuard final
        {
        public:
            explicit MessageGuard(GdalDispatchWindow& window) noexcept
                : _window(window)
            {
                ++_window._messageDepth;
            }

            ~MessageGuard()
            {
                --_window._messageDepth;
            }

        private:
            GdalDispatchWindow& _window;
        };

        LRESULT OnDispatch(UINT, WPARAM, LPARAM, BOOL& handled) noexcept
        {
            const std::shared_ptr<GdalDispatchWindow> lifetime = shared_from_this();
            handled = TRUE;
            MessageGuard messageGuard(*this);
            if (_callback != nullptr)
            {
                _callback(_context);
            }
            return 0;
        }

        LRESULT OnRetryTimer(UINT, WPARAM timerId, LPARAM, BOOL& handled) noexcept
        {
            const std::shared_ptr<GdalDispatchWindow> lifetime = shared_from_this();
            handled = TRUE;
            MessageGuard messageGuard(*this);
            if (timerId == kGdalDispatchRetryTimerId && _callback != nullptr)
            {
                _callback(_context);
            }
            return 0;
        }

        Callback _callback = nullptr;
        void* _context = nullptr;
        std::shared_ptr<GdalDispatchWindow> _deferredLifetime;
        std::uint32_t _messageDepth = 0;
        bool _destroyRequested = false;
    };
}
