// SPDX-License-Identifier: MPL-2.0

/**
 * @file ActiveXActivationTests.cpp
 * @brief Tests COM class-factory activation and the initial automation surface.
 */
#include <Windows.h>
#include <objbase.h>
#include <wrl/client.h>

#include <gtest/gtest.h>

#include <filesystem>

#include "MapWinGIS.ActiveX_i.h"
#include "mapwingis/gdal/GdalEnvironment.h"

namespace
{
    using Microsoft::WRL::ComPtr;

    class ComApartment final
    {
    public:
        ComApartment()
            : _result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
        {
        }

        ~ComApartment()
        {
            if (SUCCEEDED(_result))
            {
                CoUninitialize();
            }
        }

        [[nodiscard]] HRESULT Result() const noexcept
        {
            return _result;
        }

    private:
        HRESULT _result;
    };

    class Module final
    {
    public:
        explicit Module(HMODULE handle) noexcept
            : _handle(handle)
        {
        }

        ~Module()
        {
            if (_handle != nullptr)
            {
                FreeLibrary(_handle);
            }
        }

        Module(const Module&) = delete;
        Module& operator=(const Module&) = delete;

        [[nodiscard]] HMODULE Get() const noexcept
        {
            return _handle;
        }

    private:
        HMODULE _handle;
    };
}

TEST(ActiveXActivation, CreatesControlThroughClassFactory)
{
    ComApartment apartment;
    ASSERT_TRUE(SUCCEEDED(apartment.Result()))
        << "CoInitializeEx failed with HRESULT "
        << std::hex << static_cast<unsigned long>(apartment.Result());

    wchar_t executablePath[MAX_PATH]{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    ASSERT_NE(pathLength, 0u);
    ASSERT_LT(pathLength, static_cast<DWORD>(MAX_PATH));

    const std::filesystem::path controlPath =
        std::filesystem::path(executablePath).parent_path() / L"MapWinGIS.ocx";
    Module module(LoadLibraryW(controlPath.c_str()));
    ASSERT_NE(module.Get(), nullptr)
        << "LoadLibrary failed with error " << GetLastError();

    using GetClassObjectFunction = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    const auto getClassObject = reinterpret_cast<GetClassObjectFunction>(
        GetProcAddress(module.Get(), "DllGetClassObject"));
    ASSERT_NE(getClassObject, nullptr);

    CLSID classId{};
    ASSERT_EQ(CLSIDFromString(
                  L"{2F0AB6FC-F7C0-4E42-A002-A61F9C6F294F}",
                  &classId),
              S_OK);

    ComPtr<IClassFactory> classFactory;
    HRESULT result = getClassObject(
        classId,
        IID_IClassFactory,
        reinterpret_cast<void**>(classFactory.GetAddressOf()));
    ASSERT_TRUE(SUCCEEDED(result))
        << "DllGetClassObject failed with HRESULT "
        << std::hex << static_cast<unsigned long>(result);

    ComPtr<IUnknown> control;
    result = classFactory->CreateInstance(
        nullptr,
        IID_IUnknown,
        reinterpret_cast<void**>(control.GetAddressOf()));
    ASSERT_TRUE(SUCCEEDED(result))
        << "IClassFactory::CreateInstance failed with HRESULT "
        << std::hex << static_cast<unsigned long>(result);

    ComPtr<IMapWinGISControl> activeXControl;
    result = control.As(&activeXControl);
    ASSERT_TRUE(SUCCEEDED(result));

    VARIANT_BOOL initialized = VARIANT_FALSE;
    result = activeXControl->get_CoreInitialized(&initialized);
    ASSERT_TRUE(SUCCEEDED(result));
    EXPECT_EQ(initialized == VARIANT_TRUE, mwg::GdalEnvironment::IsInitialized());
}
