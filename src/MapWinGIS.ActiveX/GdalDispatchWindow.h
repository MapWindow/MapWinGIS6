// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalDispatchWindow.h
 * @brief Declares the message-only window used to dispatch GDAL notifications.
 *
 * The window is created on the control's owning COM thread and accepts only
 * posted wake-up messages from native workers.
 */
#pragma once

#include <atlwin.h>

namespace mwg::activex
{
    inline constexpr UINT kGdalDispatchMessage = WM_APP + 0x316;

    class GdalDispatchWindow final : public ATL::CWindowImpl<GdalDispatchWindow>
    {
    public:
        using Callback = void(*)(void*) noexcept;

        DECLARE_WND_CLASS_EX(L"MapWinGIS6.GdalDispatchWindow", 0, COLOR_WINDOW)

        [[nodiscard]] HWND Create(Callback callback, void* context)
        {
            _callback = callback;
            _context = context;
            return ATL::CWindowImpl<GdalDispatchWindow>::Create(HWND_MESSAGE);
        }

        void Destroy() noexcept
        {
            _callback = nullptr;
            _context = nullptr;
            if (m_hWnd != nullptr)
            {
                DestroyWindow();
            }
        }

        BEGIN_MSG_MAP(GdalDispatchWindow)
            MESSAGE_HANDLER(kGdalDispatchMessage, OnDispatch)
        END_MSG_MAP()

    private:
        LRESULT OnDispatch(UINT, WPARAM, LPARAM, BOOL& handled) noexcept
        {
            handled = TRUE;
            if (_callback != nullptr)
            {
                _callback(_context);
            }
            return 0;
        }

        Callback _callback = nullptr;
        void* _context = nullptr;
    };
}
