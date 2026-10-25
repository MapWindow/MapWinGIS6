// SPDX-License-Identifier: MPL-2.0

/**
 * @file ActiveXModule.cpp
 * @brief Implements the ATL in-process COM server entry points.
 */
#include "ActiveXModule.h"

#include <cwchar>

CMapWinGISActiveXModule _AtlModule;

extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
    (void)hInstance;
    return _AtlModule.DllMain(dwReason, lpReserved);
}

STDAPI DllCanUnloadNow()
{
    return _AtlModule.DllCanUnloadNow();
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    return _AtlModule.DllGetClassObject(rclsid, riid, ppv);
}

STDAPI DllRegisterServer()
{
    return _AtlModule.DllRegisterServer();
}

STDAPI DllUnregisterServer()
{
    return _AtlModule.DllUnregisterServer();
}

STDAPI DllInstall(BOOL bInstall, LPCWSTR pszCmdLine)
{
    if (pszCmdLine != nullptr && _wcsicmp(pszCmdLine, L"user") == 0)
    {
        ATL::AtlSetPerUserRegistration(true);
        return bInstall ? DllRegisterServer() : DllUnregisterServer();
    }

    if (pszCmdLine != nullptr)
    {
        return E_INVALIDARG;
    }

    return bInstall ? DllRegisterServer() : DllUnregisterServer();
}
