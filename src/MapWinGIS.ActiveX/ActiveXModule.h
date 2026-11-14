// SPDX-License-Identifier: MPL-2.0

/**
 * @file ActiveXModule.h
 * @brief Declares the ATL module for the MapWinGIS ActiveX server.
 */
#pragma once

#include <atlbase.h>
#include <atlcom.h>

/// DllInstall treats a missing or empty command line as machine-wide.
/// Only the exact token `user` (case-insensitive) selects per-user registration.
enum class DllInstallRegistrationScope
{
    Machine,
    User,
    Invalid
};

[[nodiscard]] inline DllInstallRegistrationScope ParseDllInstallRegistrationScope(
    const wchar_t* commandLine) noexcept
{
    if (commandLine == nullptr || commandLine[0] == L'\0')
    {
        return DllInstallRegistrationScope::Machine;
    }
    return ::CompareStringOrdinal(commandLine, -1, L"user", -1, TRUE) == CSTR_EQUAL
        ? DllInstallRegistrationScope::User
        : DllInstallRegistrationScope::Invalid;
}

#include "MapWinGIS.ActiveX_i.h"

class CMapWinGISActiveXModule final : public ATL::CAtlDllModuleT<CMapWinGISActiveXModule>
{
public:
    DECLARE_LIBID(LIBID_MapWinGISActiveXLib)
};

extern CMapWinGISActiveXModule _AtlModule;
