// SPDX-License-Identifier: MPL-2.0

/**
 * @file ActiveXModule.h
 * @brief Declares the ATL module for the MapWinGIS ActiveX server.
 */
#pragma once

#include <atlbase.h>
#include <atlcom.h>

#include "MapWinGIS.ActiveX_i.h"

class CMapWinGISActiveXModule final : public ATL::CAtlDllModuleT<CMapWinGISActiveXModule>
{
public:
    DECLARE_LIBID(LIBID_MapWinGISActiveXLib)
};

extern CMapWinGISActiveXModule _AtlModule;
