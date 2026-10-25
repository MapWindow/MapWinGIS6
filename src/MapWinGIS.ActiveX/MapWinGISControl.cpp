// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.cpp
 * @brief Implements the core-state query exposed by the ActiveX control.
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
