// SPDX-License-Identifier: MPL-2.0

/**
 * @file ActiveXActivationTests.cpp
 * @brief Tests COM activation, view rendering, and the hosted control lifecycle.
 */
#include <Windows.h>
#include <atlbase.h>
#include <atlhost.h>
#include <objbase.h>
#include <wrl/client.h>

#include <gtest/gtest.h>

#include <filesystem>

#include "MapWinGIS.ActiveX_i.h"
#include "mapwingis/gdal/GdalEnvironment.h"

namespace
{
    using Microsoft::WRL::ComPtr;

    class CMapWinGISTestModule final :
        public ATL::CAtlExeModuleT<CMapWinGISTestModule>
    {
    };

    CMapWinGISTestModule gAtlTestModule;

    class BitmapSurface final
    {
    public:
        BitmapSurface(int width, int height) noexcept
        {
            _screen = GetDC(nullptr);
            if (_screen == nullptr)
            {
                return;
            }

            _memory = CreateCompatibleDC(_screen);
            if (_memory == nullptr)
            {
                return;
            }

            _bitmap = CreateCompatibleBitmap(_screen, width, height);
            if (_bitmap == nullptr)
            {
                return;
            }

            _previous = SelectObject(_memory, _bitmap);
        }

        ~BitmapSurface()
        {
            if (_previous != nullptr && _previous != HGDI_ERROR)
            {
                SelectObject(_memory, _previous);
            }
            if (_bitmap != nullptr)
            {
                DeleteObject(_bitmap);
            }
            if (_memory != nullptr)
            {
                DeleteDC(_memory);
            }
            if (_screen != nullptr)
            {
                ReleaseDC(nullptr, _screen);
            }
        }

        BitmapSurface(const BitmapSurface&) = delete;
        BitmapSurface& operator=(const BitmapSurface&) = delete;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return _memory != nullptr &&
                _bitmap != nullptr &&
                _previous != nullptr &&
                _previous != HGDI_ERROR;
        }

        [[nodiscard]] HDC Get() const noexcept
        {
            return _memory;
        }

    private:
        HDC _screen{};
        HDC _memory{};
        HBITMAP _bitmap{};
        HGDIOBJ _previous{};
    };

    class TestHostWindow final
    {
    public:
        TestHostWindow() noexcept
            : _handle(CreateWindowExW(
                  0,
                  L"STATIC",
                  L"MapWinGIS ActiveX host test",
                  WS_OVERLAPPEDWINDOW,
                  CW_USEDEFAULT,
                  CW_USEDEFAULT,
                  240,
                  180,
                  nullptr,
                  nullptr,
                  GetModuleHandleW(nullptr),
                  nullptr))
        {
        }

        ~TestHostWindow()
        {
            if (_handle != nullptr)
            {
                DestroyWindow(_handle);
            }
        }

        TestHostWindow(const TestHostWindow&) = delete;
        TestHostWindow& operator=(const TestHostWindow&) = delete;

        [[nodiscard]] HWND Get() const noexcept
        {
            return _handle;
        }

        [[nodiscard]] bool Destroy() noexcept
        {
            if (_handle == nullptr)
            {
                return true;
            }

            const BOOL result = DestroyWindow(_handle);
            if (result != FALSE)
            {
                _handle = nullptr;
            }
            return result != FALSE;
        }

    private:
        HWND _handle{};
    };

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

    ComPtr<IPersist> persist;
    result = control.As(&persist);
    ASSERT_TRUE(SUCCEEDED(result));

    CLSID persistedClassId{};
    result = persist->GetClassID(&persistedClassId);
    ASSERT_TRUE(SUCCEEDED(result));
    EXPECT_TRUE(IsEqualCLSID(persistedClassId, classId));

    ComPtr<IViewObjectEx> viewObject;
    result = control.As(&viewObject);
    ASSERT_TRUE(SUCCEEDED(result));

    DWORD viewStatus{};
    result = viewObject->GetViewStatus(&viewStatus);
    ASSERT_TRUE(SUCCEEDED(result));
    EXPECT_EQ(
        viewStatus & (VIEWSTATUS_SOLIDBKGND | VIEWSTATUS_OPAQUE),
        static_cast<DWORD>(VIEWSTATUS_SOLIDBKGND | VIEWSTATUS_OPAQUE));

    BitmapSurface surface(80, 60);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_TRUE(PatBlt(surface.Get(), 0, 0, 80, 60, BLACKNESS));

    RECTL drawBounds{0, 0, 24, 18};
    result = viewObject->Draw(
        DVASPECT_CONTENT,
        -1,
        nullptr,
        nullptr,
        nullptr,
        surface.Get(),
        &drawBounds,
        nullptr,
        nullptr,
        0);
    ASSERT_TRUE(SUCCEEDED(result));
    EXPECT_EQ(GetPixel(surface.Get(), 23, 17), GetSysColor(COLOR_WINDOW));
    EXPECT_EQ(GetPixel(surface.Get(), 24, 18), RGB(0, 0, 0));

    drawBounds = {0, 0, 80, 60};
    result = viewObject->Draw(
        DVASPECT_CONTENT,
        -1,
        nullptr,
        nullptr,
        nullptr,
        surface.Get(),
        &drawBounds,
        nullptr,
        nullptr,
        0);
    ASSERT_TRUE(SUCCEEDED(result));
    EXPECT_EQ(GetPixel(surface.Get(), 79, 59), GetSysColor(COLOR_WINDOW));

    ComPtr<IMapWinGISControl> activeXControl;
    result = control.As(&activeXControl);
    ASSERT_TRUE(SUCCEEDED(result));

    VARIANT_BOOL initialized = VARIANT_FALSE;
    result = activeXControl->get_CoreInitialized(&initialized);
    ASSERT_TRUE(SUCCEEDED(result));
    EXPECT_EQ(initialized == VARIANT_TRUE, mwg::GdalEnvironment::IsInitialized());
}

TEST(ActiveXHosting, ResizesRepaintsAndRecreatesControl)
{
    if (GetEnvironmentVariableW(L"MWG_TEST_ACTIVEX_HOST", nullptr, 0) == 0)
    {
        GTEST_SKIP() << "Run Verify-ActiveXActivation.ps1 to test registered hosting.";
    }

    ComApartment apartment;
    ASSERT_TRUE(SUCCEEDED(apartment.Result()))
        << "CoInitializeEx failed with HRESULT "
        << std::hex << static_cast<unsigned long>(apartment.Result());
    ASSERT_TRUE(AtlAxWinInit());

    for (int iteration = 0; iteration < 2; ++iteration)
    {
        TestHostWindow hostWindow;
        ASSERT_NE(hostWindow.Get(), nullptr)
            << "CreateWindowExW failed with error " << GetLastError();

        ShowWindow(hostWindow.Get(), SW_SHOW);
        UpdateWindow(hostWindow.Get());

        ATL::CComPtr<IUnknown> host;
        ATL::CComPtr<IUnknown> control;
        const HRESULT result = AtlAxCreateControlEx(
            L"MapWinGIS6.Control.6",
            hostWindow.Get(),
            nullptr,
            &host,
            &control,
            IID_NULL,
            nullptr);
        ASSERT_TRUE(SUCCEEDED(result))
            << "AtlAxCreateControlEx failed with HRESULT "
            << std::hex << static_cast<unsigned long>(result);

        ATL::CComPtr<IUnknown> hostedControl;
        const HRESULT queryResult = AtlAxGetControl(hostWindow.Get(), &hostedControl);
        ASSERT_TRUE(SUCCEEDED(queryResult));
        ASSERT_NE(hostedControl.p, nullptr);

        ATL::CComQIPtr<IOleObject> oleObject(hostedControl);
        ASSERT_NE(oleObject.p, nullptr);
        ATL::CComPtr<IOleClientSite> clientSite;
        ASSERT_TRUE(SUCCEEDED(oleObject->GetClientSite(&clientSite)));
        ATL::CComQIPtr<IOleInPlaceSite> inPlaceSite(clientSite);
        ASSERT_NE(inPlaceSite.p, nullptr);

        RECT hostBounds{};
        ASSERT_TRUE(GetClientRect(hostWindow.Get(), &hostBounds));
        const HRESULT activateResult = oleObject->DoVerb(
            OLEIVERB_INPLACEACTIVATE,
            nullptr,
            clientSite,
            0,
            hostWindow.Get(),
            &hostBounds);
        ASSERT_TRUE(SUCCEEDED(activateResult));

        ComPtr<IViewObjectEx> viewObject;
        const HRESULT viewResult = hostedControl->QueryInterface(
            IID_PPV_ARGS(viewObject.GetAddressOf()));
        ASSERT_TRUE(SUCCEEDED(viewResult));

        ATL::CComPtr<IOleInPlaceFrame> inPlaceFrame;
        ATL::CComPtr<IOleInPlaceUIWindow> documentWindow;
        OLEINPLACEFRAMEINFO frameInfo{};
        frameInfo.cb = sizeof(frameInfo);
        RECT initialBounds{};
        RECT initialClip{};
        ASSERT_TRUE(SUCCEEDED(inPlaceSite->GetWindowContext(
            &inPlaceFrame,
            &documentWindow,
            &initialBounds,
            &initialClip,
            &frameInfo)));

        UpdateWindow(hostWindow.Get());
        ASSERT_TRUE(SetWindowPos(
            hostWindow.Get(),
            nullptr,
            0,
            0,
            360,
            260,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW));

        RECT resizedBounds{};
        RECT resizedClip{};
        inPlaceFrame = nullptr;
        documentWindow = nullptr;
        ASSERT_TRUE(SUCCEEDED(inPlaceSite->GetWindowContext(
            &inPlaceFrame,
            &documentWindow,
            &resizedBounds,
            &resizedClip,
            &frameInfo)));
        EXPECT_GT(resizedBounds.right - resizedBounds.left,
                  initialBounds.right - initialBounds.left);
        EXPECT_GT(resizedBounds.bottom - resizedBounds.top,
                  initialBounds.bottom - initialBounds.top);

        RECT invalidatedBounds{};
        EXPECT_TRUE(GetUpdateRect(hostWindow.Get(), &invalidatedBounds, FALSE));

        UpdateWindow(hostWindow.Get());
        EXPECT_FALSE(GetUpdateRect(hostWindow.Get(), nullptr, FALSE));

        const int resizedWidth = resizedBounds.right - resizedBounds.left;
        const int resizedHeight = resizedBounds.bottom - resizedBounds.top;
        BitmapSurface resizedSurface(resizedWidth, resizedHeight);
        ASSERT_TRUE(resizedSurface.IsValid());
        ASSERT_TRUE(PatBlt(
            resizedSurface.Get(),
            0,
            0,
            resizedWidth,
            resizedHeight,
            BLACKNESS));

        const RECTL drawBounds{0, 0, resizedWidth, resizedHeight};
        const HRESULT drawResult = viewObject->Draw(
            DVASPECT_CONTENT,
            -1,
            nullptr,
            nullptr,
            nullptr,
            resizedSurface.Get(),
            &drawBounds,
            nullptr,
            nullptr,
            0);
        ASSERT_TRUE(SUCCEEDED(drawResult));
        EXPECT_EQ(
            GetPixel(resizedSurface.Get(), resizedWidth - 1, resizedHeight - 1),
            GetSysColor(COLOR_WINDOW));

        viewObject = nullptr;
        documentWindow = nullptr;
        inPlaceFrame = nullptr;
        inPlaceSite = nullptr;
        clientSite = nullptr;
        oleObject = nullptr;
        hostedControl = nullptr;
        control = nullptr;
        host = nullptr;
        EXPECT_TRUE(hostWindow.Destroy());
    }
}
