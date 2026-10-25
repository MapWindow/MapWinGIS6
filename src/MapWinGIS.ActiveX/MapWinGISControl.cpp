// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.cpp
 * @brief Implements ActiveX core-state reporting and host drawing lifecycle.
 */
#include "MapWinGISControl.h"

#include "mapwingis/gdal/GdalEnvironment.h"

STDMETHODIMP CMapWinGISControl::get_CoreInitialized(VARIANT_BOOL* initialized)
{
    if (initialized == nullptr)
    {
        return E_POINTER;
    }

    *initialized = mwg::GdalEnvironment::IsInitialized() ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

LRESULT CMapWinGISControl::OnSize(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    FireViewChange();
    handled = TRUE;
    return 0;
}

HRESULT CMapWinGISControl::OnDraw(ATL_DRAWINFO& drawInfo)
{
    if (drawInfo.hdcDraw == nullptr || drawInfo.prcBounds == nullptr)
    {
        return E_POINTER;
    }

    const RECT bounds{
        drawInfo.prcBounds->left,
        drawInfo.prcBounds->top,
        drawInfo.prcBounds->right,
        drawInfo.prcBounds->bottom};

    return ::FillRect(
               drawInfo.hdcDraw,
               &bounds,
               ::GetSysColorBrush(COLOR_WINDOW)) != 0
        ? S_OK
        : E_FAIL;
}
