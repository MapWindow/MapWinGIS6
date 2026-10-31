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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gdal_priv.h>
#include <gdalalgorithm.h>

#include "MapWinGIS.ActiveX_i.h"
#include "TestData.h"
#include "mapwingis/gdal/GdalAlgorithmRunner.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/map/Viewport.h"

namespace
{
    using Microsoft::WRL::ComPtr;

    [[nodiscard]] std::string ToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    class GdalEventSink final : public IDispatch
    {
    public:
        using ProgressCallback = HRESULT(*)(void*, LONGLONG);
        using CompletionCallback = HRESULT(*)(void*, LONGLONG);

        HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID interfaceId,
            void** result) override
        {
            if (result == nullptr)
            {
                return E_POINTER;
            }
            *result = nullptr;
            if (IsEqualIID(interfaceId, IID_IUnknown) ||
                IsEqualIID(interfaceId, IID_IDispatch) ||
                IsEqualIID(interfaceId, __uuidof(IMapWinGISControlEvents)))
            {
                *result = static_cast<IDispatch*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return ++_references;
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG remaining = --_references;
            if (remaining == 0)
            {
                delete this;
            }
            return remaining;
        }

        HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override
        {
            if (count == nullptr)
            {
                return E_POINTER;
            }
            *count = 0;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetTypeInfo(
            UINT,
            LCID,
            ITypeInfo**) override
        {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE GetIDsOfNames(
            REFIID,
            LPOLESTR*,
            UINT,
            LCID,
            DISPID*) override
        {
            return DISP_E_UNKNOWNNAME;
        }

        HRESULT STDMETHODCALLTYPE Invoke(
            DISPID memberId,
            REFIID,
            LCID,
            WORD,
            DISPPARAMS* parameters,
            VARIANT*,
            EXCEPINFO*,
            UINT*) override
        {
            if (parameters == nullptr || parameters->rgvarg == nullptr)
            {
                return E_INVALIDARG;
            }

            if ((memberId == 1 && parameters->cArgs != 3) ||
                (memberId == 2 && parameters->cArgs != 1))
            {
                return E_INVALIDARG;
            }

            const LONGLONG operationId = memberId == 1
                ? parameters->rgvarg[2].llVal
                : parameters->rgvarg[0].llVal;
            ProgressCallback progressCallback = nullptr;
            void* progressContext = nullptr;
            CompletionCallback completionCallback = nullptr;
            void* completionContext = nullptr;
            {
                std::lock_guard lock(_mutex);
                _ownerThread = std::this_thread::get_id();
                try
                {
                    if (memberId == 1)
                    {
                        _progressOperations.push_back(operationId);
                    }
                    else if (memberId == 2)
                    {
                        _completedOperations.push_back(operationId);
                    }
                    else
                    {
                        return DISP_E_MEMBERNOTFOUND;
                    }
                }
                catch (const std::bad_alloc&)
                {
                    return E_OUTOFMEMORY;
                }
                progressCallback = _progressCallback;
                progressContext = _progressContext;
                completionCallback = _completionCallback;
                completionContext = _completionContext;
            }
            if (memberId == 1 && progressCallback != nullptr)
            {
                return progressCallback(progressContext, operationId);
            }
            if (memberId == 2 && completionCallback != nullptr)
            {
                return completionCallback(completionContext, operationId);
            }
            return S_OK;
        }

        void SetProgressCallback(
            const ProgressCallback callback,
            void* const context) noexcept
        {
            std::lock_guard lock(_mutex);
            _progressCallback = callback;
            _progressContext = context;
        }

        void SetCompletionCallback(
            const CompletionCallback callback,
            void* const context) noexcept
        {
            std::lock_guard lock(_mutex);
            _completionCallback = callback;
            _completionContext = context;
        }

        [[nodiscard]] bool HasCompleted(const LONGLONG operationId) const
        {
            std::lock_guard lock(_mutex);
            return std::find(
                _completedOperations.begin(),
                _completedOperations.end(),
                operationId) != _completedOperations.end();
        }

        [[nodiscard]] bool HasProgress(const LONGLONG operationId) const
        {
            std::lock_guard lock(_mutex);
            return std::find(
                _progressOperations.begin(),
                _progressOperations.end(),
                operationId) != _progressOperations.end();
        }

        [[nodiscard]] bool WasDeliveredOn(
            const std::thread::id threadId) const
        {
            std::lock_guard lock(_mutex);
            return _ownerThread == threadId;
        }

    private:
        std::atomic<ULONG> _references{1};
        mutable std::mutex _mutex;
        std::thread::id _ownerThread;
        std::vector<LONGLONG> _progressOperations;
        std::vector<LONGLONG> _completedOperations;
        ProgressCallback _progressCallback = nullptr;
        void* _progressContext = nullptr;
        CompletionCallback _completionCallback = nullptr;
        void* _completionContext = nullptr;
    };

    class HostedProgressAlgorithm final : public GDALAlgorithm
    {
    public:
        HostedProgressAlgorithm()
            : GDALAlgorithm(
                  "mwg-test-activex-progress",
                  "Test hosted COM progress",
                  "")
        {
        }

    protected:
        bool RunImpl(GDALProgressFunc progress, void* userData) override
        {
            for (int step = 0; step <= 100; ++step)
            {
                if (progress != nullptr &&
                    progress(
                        static_cast<double>(step) / 100.0,
                        "COM progress",
                        userData) == 0)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return true;
        }
    };

    class HostedDiagnosticsAlgorithm final : public GDALAlgorithm
    {
    public:
        HostedDiagnosticsAlgorithm()
            : GDALAlgorithm(
                  "mwg-test-activex-diagnostics",
                  "Test hosted COM diagnostics",
                  "")
        {
            AddArg("fail", 0, "Fail after a warning", &_fail);
        }

    protected:
        bool RunImpl(GDALProgressFunc, void*) override
        {
            ReportError(CE_Warning, CPLE_AppDefined, "%s", "COM test warning");
            if (_fail)
            {
                ReportError(CE_Failure, CPLE_AppDefined, "%s", "COM test failure");
                return false;
            }
            return true;
        }

    private:
        bool _fail = false;
    };

    void RegisterHostedTestAlgorithms()
    {
        static std::once_flag registrationFlag;
        std::call_once(registrationFlag, []
        {
            auto& registry = GDALGlobalAlgorithmRegistry::GetSingleton();
            registry.DeclareAlgorithm(
                {"mwg-test-activex", "progress"},
                [] { return std::make_unique<HostedProgressAlgorithm>(); });
            registry.DeclareAlgorithm(
                {"mwg-test-activex", "diagnostics"},
                [] { return std::make_unique<HostedDiagnosticsAlgorithm>(); });
        });
    }

    struct ComCancellation final
    {
        IMapWinGISControlGdal* control = nullptr;
        LONGLONG operationId = 0;
        HRESULT first = E_FAIL;
        HRESULT repeated = E_FAIL;

        static HRESULT Cancel(void* context, const LONGLONG operationId)
        {
            auto& cancellation = *static_cast<ComCancellation*>(context);
            if (operationId != cancellation.operationId ||
                cancellation.first != E_FAIL)
            {
                return S_OK;
            }
            cancellation.first =
                cancellation.control->CancelGdalOperation(operationId);
            cancellation.repeated =
                cancellation.control->CancelGdalOperation(operationId);
            return S_OK;
        }
    };

    HRESULT FailProgress(void*, LONGLONG)
    {
        return E_FAIL;
    }

    HRESULT FailCompletion(void*, LONGLONG)
    {
        return E_FAIL;
    }

    struct ComEndAttempt final
    {
        IMapWinGISControlGdal* control = nullptr;
        HRESULT result = E_FAIL;

        static HRESULT TryEnd(void* context, const LONGLONG operationId)
        {
            auto& attempt = *static_cast<ComEndAttempt*>(context);
            VARIANT_BOOL succeeded = VARIANT_FALSE;
            VARIANT_BOOL cancelled = VARIANT_FALSE;
            LONG errorCode = 0;
            LONG callbackErrorCode = 0;
            BSTR callbackError = nullptr;
            BSTR summary = nullptr;
            BSTR diagnostics = nullptr;
            SAFEARRAY* warnings = nullptr;
            attempt.result = attempt.control->EndGdalOperation(
                operationId,
                &succeeded,
                &cancelled,
                &errorCode,
                &callbackErrorCode,
                &callbackError,
                &summary,
                &diagnostics,
                &warnings);
            SysFreeString(callbackError);
            SysFreeString(summary);
            SysFreeString(diagnostics);
            if (warnings != nullptr)
            {
                SafeArrayDestroy(warnings);
            }
            return S_OK;
        }
    };

    struct ComGdalResult final
    {
        VARIANT_BOOL succeeded = VARIANT_FALSE;
        VARIANT_BOOL cancelled = VARIANT_FALSE;
        LONG errorCode = 0;
        LONG callbackErrorCode = 0;
        std::wstring callbackError;
        std::wstring summary;
        std::wstring diagnostics;
        std::vector<std::wstring> warnings;
    };

    [[nodiscard]] HRESULT StartGdalOperation(
        IMapWinGISControlGdal* control,
        const std::wstring_view algorithmPath,
        const std::vector<std::wstring>& arguments,
        LONGLONG* operationId)
    {
        using BstrPtr = std::unique_ptr<wchar_t, decltype(&SysFreeString)>;
        BstrPtr algorithm(SysAllocStringLen(
            algorithmPath.data(),
            static_cast<UINT>(algorithmPath.size())), &SysFreeString);
        if (algorithm == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        using SafeArrayPtr =
            std::unique_ptr<SAFEARRAY, decltype(&SafeArrayDestroy)>;
        SafeArrayPtr array(
            SafeArrayCreateVector(
                VT_BSTR,
                0,
                static_cast<ULONG>(arguments.size())),
            &SafeArrayDestroy);
        if (array == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        for (std::size_t offset = 0; offset < arguments.size(); ++offset)
        {
            BstrPtr argument(
                SysAllocStringLen(
                    arguments[offset].data(),
                    static_cast<UINT>(arguments[offset].size())),
                &SysFreeString);
            if (argument == nullptr)
            {
                return E_OUTOFMEMORY;
            }
            LONG index = static_cast<LONG>(offset);
            const HRESULT result = SafeArrayPutElement(
                array.get(),
                &index,
                argument.get());
            if (FAILED(result))
            {
                return result;
            }
        }
        return control->BeginGdalOperation(
            algorithm.get(),
            array.get(),
            operationId);
    }

    [[nodiscard]] HRESULT EndGdalOperation(
        IMapWinGISControlGdal* control,
        const LONGLONG operationId,
        ComGdalResult& result)
    {
        using BstrPtr = std::unique_ptr<wchar_t, decltype(&SysFreeString)>;
        using SafeArrayPtr =
            std::unique_ptr<SAFEARRAY, decltype(&SafeArrayDestroy)>;
        BSTR rawSummary = nullptr;
        BSTR rawDiagnostics = nullptr;
        BSTR rawCallbackError = nullptr;
        SAFEARRAY* rawWarnings = nullptr;
        const HRESULT status = control->EndGdalOperation(
            operationId,
            &result.succeeded,
            &result.cancelled,
            &result.errorCode,
            &result.callbackErrorCode,
            &rawCallbackError,
            &rawSummary,
            &rawDiagnostics,
            &rawWarnings);
        BstrPtr summary(rawSummary, &SysFreeString);
        BstrPtr diagnostics(rawDiagnostics, &SysFreeString);
        BstrPtr callbackError(rawCallbackError, &SysFreeString);
        SafeArrayPtr warnings(rawWarnings, &SafeArrayDestroy);
        if (FAILED(status))
        {
            return status;
        }

        if (summary != nullptr)
        {
            result.summary.assign(summary.get(), SysStringLen(summary.get()));
        }
        if (callbackError != nullptr)
        {
            result.callbackError.assign(
                callbackError.get(),
                SysStringLen(callbackError.get()));
        }
        if (diagnostics != nullptr)
        {
            result.diagnostics.assign(
                diagnostics.get(),
                SysStringLen(diagnostics.get()));
        }
        if (warnings != nullptr)
        {
            LONG lowerBound = 0;
            LONG upperBound = -1;
            HRESULT arrayResult = SafeArrayGetLBound(
                warnings.get(),
                1,
                &lowerBound);
            if (FAILED(arrayResult))
            {
                return arrayResult;
            }
            arrayResult = SafeArrayGetUBound(warnings.get(), 1, &upperBound);
            if (FAILED(arrayResult))
            {
                return arrayResult;
            }
            for (LONG index = lowerBound; index <= upperBound; ++index)
            {
                BstrPtr warning(nullptr, &SysFreeString);
                BSTR rawWarning = nullptr;
                arrayResult = SafeArrayGetElement(
                    warnings.get(),
                    &index,
                    &rawWarning);
                warning.reset(rawWarning);
                if (FAILED(arrayResult))
                {
                    return arrayResult;
                }
                result.warnings.emplace_back(
                    warning.get(),
                    SysStringLen(warning.get()));
            }
        }
        return S_OK;
    }

    [[nodiscard]] bool PumpUntilGdalCompletion(
        GdalEventSink& sink,
        const LONGLONG operationId,
        const std::chrono::steady_clock::time_point deadline)
    {
        while (!sink.HasCompleted(operationId) &&
               std::chrono::steady_clock::now() < deadline)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
            {
                if (message.message == WM_QUIT)
                {
                    PostQuitMessage(static_cast<int>(message.wParam));
                    return false;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(1);
        }
        return sink.HasCompleted(operationId);
    }

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

    struct ReleaseControlFromEvent final
    {
        TestHostWindow* window = nullptr;
        ATL::CComPtrBase<IMapWinGISControlGdal>* control = nullptr;

        static HRESULT Release(void* context, LONGLONG)
        {
            auto& release = *static_cast<ReleaseControlFromEvent*>(context);
            (void)release.window->Destroy();
            release.control->Release();
            return S_OK;
        }
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

    class ActiveXMapTest : public testing::Test
    {
    protected:
        void SetUp() override
        {
            ASSERT_TRUE(SUCCEEDED(_apartment.Result()));
            wchar_t executablePath[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
            ASSERT_GT(length, 0U);
            ASSERT_LT(length, static_cast<DWORD>(MAX_PATH));
            _module = std::make_unique<Module>(LoadLibraryW(
                (std::filesystem::path(executablePath).parent_path() / L"MapWinGIS.ocx").c_str()));
            ASSERT_NE(_module->Get(), nullptr);
            using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
            const auto getClassObject = reinterpret_cast<GetClassObject>(
                GetProcAddress(_module->Get(), "DllGetClassObject"));
            ASSERT_NE(getClassObject, nullptr);
            ComPtr<IClassFactory> factory;
            ASSERT_EQ(getClassObject(__uuidof(MapWinGISControl), IID_PPV_ARGS(&factory)), S_OK);
            ASSERT_EQ(factory->CreateInstance(nullptr, IID_PPV_ARGS(&_control)), S_OK);
            ASSERT_EQ(_control.As(&_window), S_OK);
            ASSERT_EQ(_control.As(&_viewObject), S_OK);
            ASSERT_EQ(_control->put_RenderingBackendPreference(ATL::CComBSTR(L"GDI")), S_OK);
        }

        HRESULT LoadRectangles(const std::vector<mwg::Extent>& rectangles)
        {
            const auto path = mwg::test::MakeRectanglesDataset(
                _directory.Path(), "layer-" + std::to_string(_datasetCount++) + ".gpkg", rectangles);
            return _control->LoadVectorDataset(ATL::CComBSTR(path.c_str()));
        }

        HRESULT LoadLine()
        {
            const auto path = _directory.Path() / "line.geojson";
            std::ofstream output(path);
            output << R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"LineString","coordinates":[[1000,1000],[1010,1010]]}}]})";
            output.close();
            if (!output)
            {
                return E_FAIL;
            }
            return _control->LoadVectorDataset(ATL::CComBSTR(path.c_str()));
        }

        HRESULT Resize(const int width, const int height)
        {
            LRESULT result = 0;
            return _window->OnWindowMessage(WM_SIZE, 0, MAKELPARAM(width, height), &result);
        }

        HRESULT Draw(const HDC target, const int width, const int height)
        {
            const RECTL bounds{0, 0, width, height};
            return _viewObject->Draw(
                DVASPECT_CONTENT, -1, nullptr, nullptr, nullptr, target,
                &bounds, nullptr, nullptr, 0);
        }

        ComApartment _apartment;
        std::unique_ptr<Module> _module;
        mwg::test::TempDirectory _directory;
        ComPtr<IMapWinGISControl3> _control;
        ComPtr<IOleInPlaceObjectWindowless> _window;
        ComPtr<IViewObjectEx> _viewObject;
        int _datasetCount = 0;
    };

}

TEST_F(ActiveXMapTest, LoadsStackAndClearReleasesTheDatasets)
{
    ASSERT_EQ(LoadRectangles({{0, 0, 2, 2}, {3, 0, 5, 2}}), S_OK);
    ASSERT_EQ(LoadRectangles({{10, 0, 12, 2}}), S_OK);
    LONGLONG count = -1;
    for (int read = 0; read < 3; ++read)
    {
        ASSERT_EQ(_control->get_FeatureCount(&count), S_OK);
        EXPECT_EQ(count, 3);
    }
    EXPECT_EQ(_control->get_FeatureCount(nullptr), E_POINTER);
    ASSERT_EQ(_control->ClearLayers(), S_OK);
    ASSERT_EQ(_control->ClearLayers(), S_OK);
    VARIANT_BOOL loaded = VARIANT_TRUE;
    ASSERT_EQ(_control->get_LayerLoaded(&loaded), S_OK);
    EXPECT_EQ(loaded, VARIANT_FALSE);
    ASSERT_EQ(_control->get_FeatureCount(&count), S_OK);
    EXPECT_EQ(count, 0);
    // Windows refuses an exclusive file open while GDAL still holds it.
    const auto path = _directory.Path() / "layer-0.gpkg";
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(file, INVALID_HANDLE_VALUE);
    CloseHandle(file);
    ASSERT_EQ(LoadRectangles({{0, 0, 2, 2}}), S_OK);
    ASSERT_EQ(_control->get_FeatureCount(&count), S_OK);
    EXPECT_EQ(count, 1);
}

TEST_F(ActiveXMapTest, RejectedLoadsLeaveTheMapAndRenderingUnchanged)
{
    EXPECT_EQ(LoadLine(), E_FAIL);
    VARIANT_BOOL loaded = VARIANT_TRUE;
    ASSERT_EQ(_control->get_LayerLoaded(&loaded), S_OK);
    EXPECT_EQ(loaded, VARIANT_FALSE);
    ASSERT_EQ(Resize(120, 100), S_OK);
    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    BitmapSurface before(120, 100);
    BitmapSurface after(120, 100);
    ASSERT_TRUE(before.IsValid());
    ASSERT_TRUE(after.IsValid());
    ASSERT_EQ(Draw(before.Get(), 120, 100), S_OK);
    EXPECT_EQ(LoadLine(), E_FAIL);
    LONGLONG count = -1;
    ASSERT_EQ(_control->get_FeatureCount(&count), S_OK);
    EXPECT_EQ(count, 1);
    ASSERT_EQ(Draw(after.Get(), 120, 100), S_OK);
    for (int y = 0; y < 100; ++y)
    {
        for (int x = 0; x < 120; ++x)
        {
            ASSERT_EQ(GetPixel(before.Get(), x, y), GetPixel(after.Get(), x, y)) << x << ", " << y;
        }
    }
}

TEST_F(ActiveXMapTest, AlternateDrawBoundsDoNotChangeTheLiveSizeUsedForFitting)
{
    ASSERT_EQ(Resize(120, 100), S_OK);
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 24, 18), S_OK);
    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    mwg::Viewport expected(120, 100);
    ASSERT_TRUE(expected.FitExtent({0, 0, 10, 5}, 12.0));
    const auto sample = mwg::ToPixel(*expected.MapToScreen({1, 2.5}));
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(GetPixel(surface.Get(), sample->x, sample->y), RGB(61, 122, 169));
    ASSERT_EQ(Resize(200, 120), S_OK);
    BitmapSurface resized(200, 120);
    ASSERT_TRUE(resized.IsValid());
    ASSERT_EQ(Draw(resized.Get(), 200, 120), S_OK);
    expected.SetSize(200, 120);
    const auto moved = mwg::ToPixel(*expected.MapToScreen({1, 2.5}));
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(GetPixel(resized.Get(), moved->x, moved->y), RGB(61, 122, 169));
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

    ComPtr<IMapWinGISControl> originalInterface;
    result = control.As(&originalInterface);
    ASSERT_TRUE(SUCCEEDED(result));

    ComPtr<IMapWinGISControl2> activeXControl;
    result = control.As(&activeXControl);
    ASSERT_TRUE(SUCCEEDED(result));

    VARIANT_BOOL initialized = VARIANT_FALSE;
    result = originalInterface->get_CoreInitialized(&initialized);
    ASSERT_TRUE(SUCCEEDED(result));
    EXPECT_EQ(initialized == VARIANT_TRUE, mwg::GdalEnvironment::IsInitialized());

    BSTR backendPreference{};
    result = activeXControl->get_RenderingBackendPreference(&backendPreference);
    ASSERT_TRUE(SUCCEEDED(result));
    EXPECT_STREQ(backendPreference, L"Automatic");
    SysFreeString(backendPreference);

    backendPreference = SysAllocString(L"SoftwareOnly");
    ASSERT_NE(backendPreference, nullptr);
    EXPECT_EQ(
        activeXControl->put_RenderingBackendPreference(backendPreference),
        E_INVALIDARG);
    SysFreeString(backendPreference);

    backendPreference = SysAllocString(L"gdi");
    ASSERT_NE(backendPreference, nullptr);
    result = activeXControl->put_RenderingBackendPreference(backendPreference);
    SysFreeString(backendPreference);
    ASSERT_TRUE(SUCCEEDED(result));
    result = activeXControl->get_RenderingBackendPreference(&backendPreference);
    ASSERT_TRUE(SUCCEEDED(result));
    EXPECT_STREQ(backendPreference, L"GDI");
    SysFreeString(backendPreference);

    BSTR renderingBackend{};
    result = activeXControl->get_RenderingBackend(&renderingBackend);
    ASSERT_TRUE(SUCCEEDED(result));
    EXPECT_STREQ(renderingBackend, L"Not rendered");
    SysFreeString(renderingBackend);

    BSTR renderingStatus{};
    result = activeXControl->get_RenderingStatus(&renderingStatus);
    ASSERT_TRUE(SUCCEEDED(result));
    EXPECT_STREQ(renderingStatus, L"Rendering has not been attempted.");
    SysFreeString(renderingStatus);
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

        if (iteration == 0)
        {
            RegisterHostedTestAlgorithms();
            ATL::CComQIPtr<IMapWinGISControlGdal> gdalControl(hostedControl);
            ASSERT_NE(gdalControl.p, nullptr);
            ATL::CComQIPtr<IConnectionPointContainer> connectionPoints(hostedControl);
            ASSERT_NE(connectionPoints.p, nullptr);
            ATL::CComPtr<IConnectionPoint> connectionPoint;
            ASSERT_TRUE(SUCCEEDED(connectionPoints->FindConnectionPoint(
                __uuidof(IMapWinGISControlEvents),
                &connectionPoint)));

            ComPtr<GdalEventSink> eventSink;
            eventSink.Attach(new GdalEventSink());
            DWORD adviseCookie = 0;
            const HRESULT adviseResult = connectionPoint->Advise(
                eventSink.Get(),
                &adviseCookie);
            ASSERT_TRUE(SUCCEEDED(adviseResult));

            struct ConnectionPointSubscription final
            {
                ATL::CComPtr<IConnectionPoint> connectionPoint;
                DWORD cookie = 0;

                ~ConnectionPointSubscription()
                {
                    if (connectionPoint != nullptr && cookie != 0)
                    {
                        connectionPoint->Unadvise(cookie);
                    }
                }
            } subscription{connectionPoint, adviseCookie};

            mwg::test::TempDirectory operationDirectory;
            const std::filesystem::path input =
                mwg::test::MakeRaster(operationDirectory.Path(), "gdal-source.tif");
            const std::filesystem::path output =
                operationDirectory.Path() / "gdal-converted.png";
            std::vector<std::wstring> argumentTexts{
                L"--input",
                input.wstring(),
                L"--output",
                output.wstring(),
                L"--output-format",
                L"PNG"};
            using SafeArrayPtr =
                std::unique_ptr<SAFEARRAY, decltype(&SafeArrayDestroy)>;
            SafeArrayPtr arguments(
                SafeArrayCreateVector(
                    VT_BSTR,
                    0,
                    static_cast<ULONG>(argumentTexts.size())),
                &SafeArrayDestroy);
            ASSERT_NE(arguments, nullptr);
            for (std::size_t index = 0; index < argumentTexts.size(); ++index)
            {
                using BstrPtr = std::unique_ptr<wchar_t, decltype(&SysFreeString)>;
                BstrPtr argument(
                    SysAllocStringLen(
                        argumentTexts[index].data(),
                        static_cast<UINT>(argumentTexts[index].size())),
                    &SysFreeString);
                ASSERT_NE(argument, nullptr);
                LONG arrayIndex = static_cast<LONG>(index);
                const HRESULT setResult = SafeArrayPutElement(
                    arguments.get(),
                    &arrayIndex,
                    argument.get());
                ASSERT_TRUE(SUCCEEDED(setResult));
            }

            using BstrPtr = std::unique_ptr<wchar_t, decltype(&SysFreeString)>;
            BstrPtr algorithmPath(SysAllocString(L"raster convert"), &SysFreeString);
            ASSERT_NE(algorithmPath, nullptr);
            LONGLONG operationId = 0;
            const HRESULT beginResult = gdalControl->BeginGdalOperation(
                algorithmPath.get(),
                arguments.get(),
                &operationId);
            ASSERT_TRUE(SUCCEEDED(beginResult))
                << "BeginGdalOperation failed with HRESULT "
                << std::hex << static_cast<unsigned long>(beginResult);
            ASSERT_GT(operationId, 0);

            ASSERT_TRUE(PumpUntilGdalCompletion(
                *eventSink.Get(),
                operationId,
                std::chrono::steady_clock::now() + std::chrono::seconds(10)));
            EXPECT_TRUE(eventSink->WasDeliveredOn(std::this_thread::get_id()));

            VARIANT_BOOL succeeded = VARIANT_FALSE;
            VARIANT_BOOL cancelled = VARIANT_FALSE;
            LONG errorCode = 0;
            LONG callbackErrorCode = 0;
            BSTR rawCallbackError = nullptr;
            BSTR rawSummary = nullptr;
            BSTR rawDiagnostics = nullptr;
            SAFEARRAY* rawWarnings = nullptr;
            const HRESULT endResult = gdalControl->EndGdalOperation(
                operationId,
                &succeeded,
                &cancelled,
                &errorCode,
                &callbackErrorCode,
                &rawCallbackError,
                &rawSummary,
                &rawDiagnostics,
                &rawWarnings);
            BstrPtr summary(rawSummary, &SysFreeString);
            BstrPtr diagnostics(rawDiagnostics, &SysFreeString);
            BstrPtr callbackError(rawCallbackError, &SysFreeString);
            SafeArrayPtr warnings(rawWarnings, &SafeArrayDestroy);
            ASSERT_TRUE(SUCCEEDED(endResult));
            EXPECT_EQ(succeeded, VARIANT_TRUE) << diagnostics.get();
            EXPECT_EQ(cancelled, VARIANT_FALSE);
            EXPECT_EQ(callbackErrorCode, 0);

            using DatasetPtr =
                std::unique_ptr<GDALDataset, decltype(&GDALClose)>;
            DatasetPtr converted(
                static_cast<GDALDataset*>(GDALOpen(
                    ToUtf8(output).c_str(),
                    GA_ReadOnly)),
                &GDALClose);
            ASSERT_NE(converted, nullptr);
            std::uint8_t pixel = 0;
            ASSERT_EQ(
                converted->GetRasterBand(1)->RasterIO(
                    GF_Read,
                    0,
                    0,
                    1,
                    1,
                    &pixel,
                    1,
                    1,
                    GDT_Byte,
                    0,
                    0),
                CE_None);
            EXPECT_EQ(pixel, 42);

            const auto finishOperation = [&](const LONGLONG id,
                                             ComGdalResult& operationResult)
            {
                if (!PumpUntilGdalCompletion(
                        *eventSink.Get(),
                        id,
                        std::chrono::steady_clock::now() +
                            std::chrono::seconds(10)))
                {
                    return E_ABORT;
                }
                return EndGdalOperation(
                    gdalControl,
                    id,
                    operationResult);
            };

            ComCancellation cancellation{
                gdalControl,
                0,
                E_FAIL,
                E_FAIL};
            eventSink->SetProgressCallback(
                &ComCancellation::Cancel,
                &cancellation);
            LONGLONG cancelledOperationId = 0;
            ASSERT_TRUE(SUCCEEDED(StartGdalOperation(
                gdalControl,
                L"mwg-test-activex progress",
                {},
                &cancelledOperationId)));
            cancellation.operationId = cancelledOperationId;
            ComGdalResult cancelledResult;
            ASSERT_TRUE(SUCCEEDED(finishOperation(
                cancelledOperationId,
                cancelledResult)));
            EXPECT_EQ(cancellation.first, S_OK);
            EXPECT_EQ(cancellation.repeated, S_FALSE);
            EXPECT_EQ(cancelledResult.succeeded, VARIANT_FALSE);
            EXPECT_EQ(cancelledResult.cancelled, VARIANT_TRUE);

            eventSink->SetProgressCallback(&FailProgress, nullptr);
            LONGLONG callbackFailureOperationId = 0;
            ASSERT_TRUE(SUCCEEDED(StartGdalOperation(
                gdalControl,
                L"mwg-test-activex progress",
                {},
                &callbackFailureOperationId)));
            ComGdalResult callbackFailureResult;
            ASSERT_TRUE(SUCCEEDED(finishOperation(
                callbackFailureOperationId,
                callbackFailureResult)));
            EXPECT_EQ(callbackFailureResult.succeeded, VARIANT_TRUE);
            EXPECT_EQ(
                callbackFailureResult.callbackErrorCode,
                static_cast<LONG>(E_FAIL));
            EXPECT_FALSE(callbackFailureResult.callbackError.empty());

            eventSink->SetProgressCallback(nullptr, nullptr);
            ComPtr<GdalEventSink> endSink;
            endSink.Attach(new GdalEventSink());
            DWORD endSinkCookie = 0;
            ASSERT_TRUE(SUCCEEDED(connectionPoint->Advise(
                endSink.Get(),
                &endSinkCookie)));
            ComEndAttempt endAttempt{gdalControl};
            eventSink->SetCompletionCallback(&FailCompletion, nullptr);
            endSink->SetCompletionCallback(&ComEndAttempt::TryEnd, &endAttempt);
            LONGLONG callbackEndOperationId = 0;
            ASSERT_TRUE(SUCCEEDED(StartGdalOperation(
                gdalControl,
                L"mwg-test-activex diagnostics",
                {},
                &callbackEndOperationId)));
            ComGdalResult callbackEndResult;
            ASSERT_TRUE(SUCCEEDED(finishOperation(
                callbackEndOperationId,
                callbackEndResult)));
            EXPECT_EQ(endAttempt.result, E_PENDING);
            EXPECT_EQ(
                callbackEndResult.callbackErrorCode,
                static_cast<LONG>(E_FAIL));
            EXPECT_FALSE(callbackEndResult.callbackError.empty());
            eventSink->SetCompletionCallback(nullptr, nullptr);
            endSink->SetCompletionCallback(nullptr, nullptr);
            EXPECT_TRUE(SUCCEEDED(connectionPoint->Unadvise(endSinkCookie)));

            LONGLONG warningOperationId = 0;
            ASSERT_TRUE(SUCCEEDED(StartGdalOperation(
                gdalControl,
                L"mwg-test-activex diagnostics",
                {},
                &warningOperationId)));
            ComGdalResult warningResult;
            ASSERT_TRUE(SUCCEEDED(finishOperation(
                warningOperationId,
                warningResult)));
            EXPECT_EQ(warningResult.succeeded, VARIANT_TRUE);
            ASSERT_EQ(warningResult.warnings.size(), 1u);
            EXPECT_NE(
                warningResult.warnings.front().find(L"COM test warning"),
                std::wstring::npos);

            LONGLONG failureOperationId = 0;
            ASSERT_TRUE(SUCCEEDED(StartGdalOperation(
                gdalControl,
                L"mwg-test-activex diagnostics",
                {L"--fail"},
                &failureOperationId)));
            ComGdalResult failureResult;
            ASSERT_TRUE(SUCCEEDED(finishOperation(
                failureOperationId,
                failureResult)));
            EXPECT_EQ(failureResult.succeeded, VARIANT_FALSE);
            EXPECT_FALSE(failureResult.diagnostics.empty());

            LONGLONG unknownOperationId = 0;
            ASSERT_TRUE(SUCCEEDED(StartGdalOperation(
                gdalControl,
                L"mwg-unknown algorithm",
                {},
                &unknownOperationId)));
            ComGdalResult unknownResult;
            ASSERT_TRUE(SUCCEEDED(finishOperation(
                unknownOperationId,
                unknownResult)));
            EXPECT_EQ(unknownResult.succeeded, VARIANT_FALSE);
            EXPECT_NE(
                unknownResult.diagnostics.find(L"Unknown GDAL algorithm"),
                std::wstring::npos);

            EXPECT_TRUE(SUCCEEDED(connectionPoint->Unadvise(adviseCookie)));
        }

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

        if (iteration == 1)
        {
            ATL::CComQIPtr<IMapWinGISControlGdal> gdalControl(hostedControl);
            ASSERT_NE(gdalControl.p, nullptr);
            ATL::CComQIPtr<IConnectionPointContainer> connectionPoints(hostedControl);
            ASSERT_NE(connectionPoints.p, nullptr);
            ATL::CComPtr<IConnectionPoint> connectionPoint;
            ASSERT_TRUE(SUCCEEDED(connectionPoints->FindConnectionPoint(
                __uuidof(IMapWinGISControlEvents),
                &connectionPoint)));

            ComPtr<GdalEventSink> releaseEventSink;
            releaseEventSink.Attach(new GdalEventSink());
            ReleaseControlFromEvent releaseControl{
                &hostWindow,
                std::addressof(gdalControl)};
            releaseEventSink->SetProgressCallback(
                &ReleaseControlFromEvent::Release,
                &releaseControl);
            DWORD releaseCookie = 0;
            ASSERT_TRUE(SUCCEEDED(connectionPoint->Advise(
                releaseEventSink.Get(),
                &releaseCookie)));
            connectionPoint = nullptr;
            connectionPoints = nullptr;

            LONGLONG operationId = 0;
            ASSERT_TRUE(SUCCEEDED(StartGdalOperation(
                gdalControl,
                L"mwg-test-activex progress",
                {},
                &operationId)));

            viewObject = nullptr;
            documentWindow = nullptr;
            inPlaceFrame = nullptr;
            inPlaceSite = nullptr;
            clientSite = nullptr;
            oleObject = nullptr;
            hostedControl = nullptr;
            control = nullptr;
            host = nullptr;

            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!releaseEventSink->HasProgress(operationId) &&
                   std::chrono::steady_clock::now() < deadline)
            {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
                {
                    if (message.message == WM_QUIT)
                    {
                        PostQuitMessage(static_cast<int>(message.wParam));
                        FAIL() << "Received WM_QUIT while waiting for the release event";
                    }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                Sleep(1);
            }
            ASSERT_TRUE(releaseEventSink->HasProgress(operationId));
            EXPECT_EQ(gdalControl.p, nullptr);
            EXPECT_EQ(hostWindow.Get(), nullptr);
        }
        else
        {
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
}
