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
#include <wrl/implements.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <cwchar>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gdal_priv.h>
#include <gdalalgorithm.h>

#include "MapWinGIS.ActiveX_i.h"
#include "MapWinGISControlTestAccess.h"
#include "DllInstallScope.h"
#include "GdalDispatchWindow.h"
#include "TestData.h"
#include "mapwingis/gdal/GdalAlgorithmRunner.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/map/Viewport.h"
#include "mapwingis/vector/VectorDataset.h"

namespace
{
    using Microsoft::WRL::ComPtr;
    using VectorPreparationHook = void(*)(void*, bool) noexcept;
    using VectorPreparationHookSetter =
        void(__cdecl*)(VectorPreparationHook, void*) noexcept;
    using ResultMarshallingFailureSetter =
        void(__cdecl*)(UINT, HRESULT) noexcept;

    [[nodiscard]] std::string ToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    class TestPropertyBag :
        public ATL::CComObjectRootEx<ATL::CComSingleThreadModel>,
        public IPropertyBag
    {
    public:
        BEGIN_COM_MAP(TestPropertyBag)
            COM_INTERFACE_ENTRY(IPropertyBag)
        END_COM_MAP()

        STDMETHOD(Read)(LPCOLESTR name, VARIANT* value, IErrorLog*) override
        {
            if (name == nullptr || value == nullptr)
            {
                return E_POINTER;
            }
            const auto found = _values.find(name);
            if (found == _values.end())
            {
                return E_INVALIDARG;
            }
            return ::VariantCopy(value, const_cast<VARIANT*>(
                static_cast<const VARIANT*>(&found->second)));
        }

        STDMETHOD(Write)(LPCOLESTR name, VARIANT* value) override
        {
            if (name == nullptr || value == nullptr)
            {
                return E_POINTER;
            }
            try
            {
                _values.insert_or_assign(name, ATL::CComVariant(*value));
                return S_OK;
            }
            catch (const std::bad_alloc&)
            {
                return E_OUTOFMEMORY;
            }
        }

    private:
        std::map<std::wstring, ATL::CComVariant, std::less<>> _values;
    };

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

        [[nodiscard]] std::size_t CompletionCount(const LONGLONG operationId) const
        {
            std::lock_guard lock(_mutex);
            return static_cast<std::size_t>(std::count(
                _completedOperations.begin(),
                _completedOperations.end(),
                operationId));
        }

        [[nodiscard]] std::size_t ProgressCount(const LONGLONG operationId) const
        {
            std::lock_guard lock(_mutex);
            return static_cast<std::size_t>(std::count(
                _progressOperations.begin(),
                _progressOperations.end(),
                operationId));
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

    struct VectorLoadCompletionObservation final
    {
        IMapWinGISControl5* control = nullptr;
        HRESULT layerCountResult = E_FAIL;
        HRESULT featureCountResult = E_FAIL;
        LONG layerCount = -1;
        LONGLONG featureCount = -1;

        static HRESULT Observe(void* const context, LONGLONG)
        {
            auto& observation =
                *static_cast<VectorLoadCompletionObservation*>(context);
            observation.layerCountResult =
                observation.control->get_LayerCount(&observation.layerCount);
            observation.featureCountResult =
                observation.control->get_FeatureCount(&observation.featureCount);
            return S_OK;
        }
    };

    class ScopedEvent final
    {
    public:
        ScopedEvent() noexcept
            : _handle(CreateEventW(nullptr, TRUE, FALSE, nullptr))
        {
        }

        ~ScopedEvent()
        {
            if (_handle != nullptr)
            {
                CloseHandle(_handle);
            }
        }

        ScopedEvent(const ScopedEvent&) = delete;
        ScopedEvent& operator=(const ScopedEvent&) = delete;

        [[nodiscard]] bool IsValid() const noexcept { return _handle != nullptr; }
        [[nodiscard]] HANDLE Get() const noexcept { return _handle; }

        [[nodiscard]] bool Signal() const noexcept
        {
            return _handle != nullptr && SetEvent(_handle) != FALSE;
        }

        [[nodiscard]] bool Wait(const DWORD timeout) const noexcept
        {
            return _handle != nullptr &&
                WaitForSingleObject(_handle, timeout) == WAIT_OBJECT_0;
        }

    private:
        HANDLE _handle = nullptr;
    };

    class NestedDispatchBarrier final
    {
    public:
        ~NestedDispatchBarrier()
        {
            static_cast<void>(_sinkEntered.Signal());
            static_cast<void>(_wakePosted.Signal());
            static_cast<void>(_allowFinish.Signal());
            if (_started.load(std::memory_order_acquire))
            {
                static_cast<void>(_workerFinished.Wait(10000));
            }
        }

        void MarkStarted() noexcept
        {
            _started.store(true, std::memory_order_release);
        }

        [[nodiscard]] bool WaitForSinkEntered() const noexcept
        {
            return _sinkEntered.Wait(10000);
        }

        [[nodiscard]] bool WaitForWakePosted() const noexcept
        {
            return _wakePosted.Wait(10000);
        }

        [[nodiscard]] bool WaitForCompletionDelivery() const noexcept
        {
            return _allowFinish.Wait(10000);
        }

        [[nodiscard]] bool SignalSinkEntered() const noexcept
        {
            return _sinkEntered.Signal();
        }

        [[nodiscard]] bool SignalWakePosted() const noexcept
        {
            return _wakePosted.Signal();
        }

        [[nodiscard]] bool SignalAllowFinish() const noexcept
        {
            return _allowFinish.Signal();
        }

        [[nodiscard]] bool SignalWorkerFinished() const noexcept
        {
            return _workerFinished.Signal();
        }

        void RecordNestedDispatch() noexcept
        {
            _nestedDispatchCount.fetch_add(1, std::memory_order_relaxed);
        }

        [[nodiscard]] int NestedDispatchCount() const noexcept
        {
            return _nestedDispatchCount.load(std::memory_order_relaxed);
        }

    private:
        ScopedEvent _sinkEntered;
        ScopedEvent _wakePosted;
        ScopedEvent _allowFinish;
        ScopedEvent _workerFinished;
        std::atomic_bool _started{false};
        std::atomic_int _nestedDispatchCount{0};
    };

    std::atomic<NestedDispatchBarrier*> nestedDispatchBarrier{nullptr};

    class HostedNestedDispatchAlgorithm final : public GDALAlgorithm
    {
    public:
        explicit HostedNestedDispatchAlgorithm(
            NestedDispatchBarrier* const barrier)
            : GDALAlgorithm(
                  "mwg-test-activex-nested-progress",
                  "Test nested COM progress dispatch",
                  ""),
              _barrier(barrier)
        {
        }

    protected:
        bool RunImpl(GDALProgressFunc progress, void* userData) override
        {
            if (_barrier == nullptr || progress == nullptr)
            {
                return false;
            }

            struct FinishSignal final
            {
                NestedDispatchBarrier& barrier;
                ~FinishSignal()
                {
                    static_cast<void>(barrier.SignalWorkerFinished());
                }
            } finishSignal{*_barrier};

            if (progress(0.1, "COM nested progress", userData) == 0 ||
                !_barrier->WaitForSinkEntered())
            {
                return false;
            }
            if (progress(0.2, "COM nested progress", userData) == 0)
            {
                return false;
            }
            static_cast<void>(_barrier->SignalWakePosted());
            return _barrier->WaitForCompletionDelivery();
        }

    private:
        NestedDispatchBarrier* _barrier = nullptr;
    };

    class VectorPreparationBarrier final
    {
    public:
        ~VectorPreparationBarrier()
        {
            if (_started.Wait(0))
            {
                static_cast<void>(_release.Signal());
                static_cast<void>(_finished.Wait(10000));
            }
        }

        [[nodiscard]] bool IsValid() const noexcept
        {
            return _started.IsValid() && _release.IsValid() && _finished.IsValid();
        }

        [[nodiscard]] bool WaitForStart(const DWORD timeout) const noexcept
        {
            return _started.Wait(timeout);
        }

        [[nodiscard]] bool Release() const noexcept { return _release.Signal(); }

        [[nodiscard]] bool WaitForFinish(const DWORD timeout) const noexcept
        {
            return _finished.Wait(timeout);
        }

        [[nodiscard]] DWORD ReleaseWaitResult() const noexcept
        {
            return _releaseWaitResult.load(std::memory_order_acquire);
        }

        static void Notify(void* const context, const bool finished) noexcept
        {
            auto& barrier = *static_cast<VectorPreparationBarrier*>(context);
            if (finished)
            {
                static_cast<void>(barrier._finished.Signal());
                return;
            }

            static_cast<void>(barrier._started.Signal());
            barrier._releaseWaitResult.store(
                WaitForSingleObject(barrier._release.Get(), 10000),
                std::memory_order_release);
        }

    private:
        ScopedEvent _started;
        ScopedEvent _release;
        ScopedEvent _finished;
        std::atomic<DWORD> _releaseWaitResult{WAIT_FAILED};
    };

    class ScopedVectorPreparationHook final
    {
    public:
        explicit ScopedVectorPreparationHook(
            const VectorPreparationHookSetter setter) noexcept
            : _setter(setter)
        {
        }

        ~ScopedVectorPreparationHook()
        {
            _setter(nullptr, nullptr);
        }

    private:
        VectorPreparationHookSetter _setter;
    };

    class ScopedResultMarshallingFailure final
    {
    public:
        explicit ScopedResultMarshallingFailure(const HMODULE module) noexcept
            : _setter(reinterpret_cast<ResultMarshallingFailureSetter>(
                  GetProcAddress(
                      module,
                      "MapWinGISControlSetResultMarshallingFailureForTesting")))
        {
            Reset();
        }

        ~ScopedResultMarshallingFailure()
        {
            Reset();
        }

        [[nodiscard]] bool IsAvailable() const noexcept
        {
            return _setter != nullptr;
        }

        void FailOnCall(const UINT call, const HRESULT failure) const noexcept
        {
            if (_setter != nullptr)
            {
                _setter(call, failure);
            }
        }

        void Reset() const noexcept
        {
            FailOnCall(0, S_OK);
        }

    private:
        ResultMarshallingFailureSetter _setter;
    };

    class GdalEventSubscription final
    {
    public:
        explicit GdalEventSubscription(IUnknown* control)
        {
            ATL::CComQIPtr<IConnectionPointContainer> connectionPoints(control);
            if (connectionPoints == nullptr)
            {
                _result = E_NOINTERFACE;
                return;
            }
            _result = connectionPoints->FindConnectionPoint(
                __uuidof(IMapWinGISControlEvents),
                &_connectionPoint);
            if (FAILED(_result))
            {
                return;
            }

            _sink.Attach(new GdalEventSink());
            _result = _connectionPoint->Advise(_sink.Get(), &_cookie);
        }

        ~GdalEventSubscription()
        {
            if (_connectionPoint != nullptr && _cookie != 0)
            {
                _connectionPoint->Unadvise(_cookie);
            }
        }

        GdalEventSubscription(const GdalEventSubscription&) = delete;
        GdalEventSubscription& operator=(const GdalEventSubscription&) = delete;

        [[nodiscard]] HRESULT Result() const noexcept { return _result; }
        [[nodiscard]] GdalEventSink* Sink() const noexcept { return _sink.Get(); }

    private:
        ATL::CComPtr<IConnectionPoint> _connectionPoint;
        ComPtr<GdalEventSink> _sink;
        DWORD _cookie = 0;
        HRESULT _result = E_FAIL;
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
                {"mwg-test-activex", "nested-progress"},
                []
                {
                    return std::make_unique<HostedNestedDispatchAlgorithm>(
                        nestedDispatchBarrier.load(std::memory_order_acquire));
                });
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

    // Simulates a sink that shows UI from a progress event: the nested pump
    // consumes dispatch messages posted while the control is still delivering.
    HRESULT PumpNestedMessages(void* const context, LONGLONG)
    {
        auto& barrier = *static_cast<NestedDispatchBarrier*>(context);
        if (barrier.NestedDispatchCount() != 0)
        {
            return barrier.SignalAllowFinish() ? S_OK : E_FAIL;
        }
        if (!barrier.SignalSinkEntered())
        {
            return E_FAIL;
        }

        const auto dispatchWake = [&barrier]
        {
            MSG message{};
            if (!::PeekMessageW(
                    &message,
                    nullptr,
                    mwg::activex::kGdalDispatchMessage,
                    mwg::activex::kGdalDispatchMessage,
                    PM_REMOVE))
            {
                return false;
            }
            ::DispatchMessageW(&message);
            barrier.RecordNestedDispatch();
            return true;
        };

        if (!barrier.WaitForWakePosted() || !dispatchWake())
        {
            return E_FAIL;
        }
        while (dispatchWake())
        {
        }
        return barrier.NestedDispatchCount() == 1 ? S_OK : E_FAIL;
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

    [[nodiscard]] bool PumpUntilGdalCompletionCount(
        GdalEventSink& sink,
        const LONGLONG operationId,
        const std::size_t expectedCount,
        const std::chrono::steady_clock::time_point deadline)
    {
        while (sink.CompletionCount(operationId) < expectedCount &&
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
        return sink.CompletionCount(operationId) >= expectedCount;
    }

    [[nodiscard]] bool PumpUntilGdalCompletion(
        GdalEventSink& sink,
        const LONGLONG operationId,
        const std::chrono::steady_clock::time_point deadline)
    {
        return PumpUntilGdalCompletionCount(sink, operationId, 1, deadline);
    }

    void PumpGdalMessagesFor(const std::chrono::milliseconds duration)
    {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
            {
                if (message.message == WM_QUIT)
                {
                    PostQuitMessage(static_cast<int>(message.wParam));
                    return;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(1);
        }
    }

    [[nodiscard]] bool WaitForGdalDispatchMessage(
        const std::chrono::steady_clock::time_point deadline)
    {
        MSG message{};
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (PeekMessageW(
                    &message,
                    nullptr,
                    mwg::activex::kGdalDispatchMessage,
                    mwg::activex::kGdalDispatchMessage,
                    PM_NOREMOVE) != FALSE)
            {
                return true;
            }
            Sleep(1);
        }
        return PeekMessageW(
            &message,
            nullptr,
            mwg::activex::kGdalDispatchMessage,
            mwg::activex::kGdalDispatchMessage,
            PM_NOREMOVE) != FALSE;
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

    [[nodiscard]] bool HasDifferentPixel(
        const HDC first,
        const HDC second,
        const int width,
        const int height) noexcept
    {
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                if (GetPixel(first, x, y) != GetPixel(second, x, y))
                {
                    return true;
                }
            }
        }
        return false;
    }

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

        void LoadRectanglesWithFillMeshes(const std::vector<mwg::Extent>& rectangles)
        {
            const auto path = mwg::test::MakeRectanglesDataset(
                _directory.Path(), "layer-" + std::to_string(_datasetCount++) + ".gpkg", rectangles);
            auto dataset = mwg::VectorDataset::Open(path);
            mwg::activex::testing::MapWinGISControlTestAccess::AddLayerWithFillMeshes(
                mwg::activex::testing::MapWinGISControlTestAccess::Control(_control.Get()),
                std::move(dataset));
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
        std::unique_ptr<mwg::test::VectorReadTestDriver> _readDriver;
        ComPtr<IMapWinGISControl3> _control;
        ComPtr<IOleInPlaceObjectWindowless> _window;
        ComPtr<IViewObjectEx> _viewObject;
        int _datasetCount = 0;
    };

}

TEST_F(ActiveXMapTest, InvalidArgumentsSetComErrorInfoForEachAutomationInterface)
{
    const auto expectDescription = [](const wchar_t* expected)
    {
        ComPtr<IErrorInfo> errorInfo;
        EXPECT_EQ(::GetErrorInfo(0, &errorInfo), S_OK);
        if (errorInfo == nullptr) return;
        BSTR description = nullptr;
        EXPECT_EQ(errorInfo->GetDescription(&description), S_OK);
        std::unique_ptr<wchar_t, decltype(&::SysFreeString)> text{description, &::SysFreeString};
        ASSERT_NE(text, nullptr);
        EXPECT_NE(std::wstring(text.get()).find(expected), std::wstring::npos);
    };

    ComPtr<IMapWinGISControlGdal> gdal;
    ASSERT_EQ(_control.As(&gdal), S_OK);
    BSTR emptyAlgorithm = ::SysAllocStringLen(nullptr, 0);
    ASSERT_NE(emptyAlgorithm, nullptr);
    LONGLONG operationId = 0;
    EXPECT_EQ(gdal->BeginGdalOperation(emptyAlgorithm, nullptr, &operationId), E_INVALIDARG);
    ::SysFreeString(emptyAlgorithm);
    expectDescription(L"algorithm path");

    ComPtr<IMapWinGISControl> legacy;
    ASSERT_EQ(_control.As(&legacy), S_OK);
    EXPECT_EQ(legacy->LoadVectorDataset(nullptr), E_INVALIDARG);
    expectDescription(L"dataset path");

    ComPtr<IMapWinGISControl4> current;
    ASSERT_EQ(_control.As(&current), S_OK);
    EXPECT_EQ(current->ZoomToLayer(0), E_INVALIDARG);
    expectDescription(L"Layer ID");

    // Rendering properties are declared on IMapWinGISControl2, so their error
    // information must name that interface for clients that only know it.
    ComPtr<IMapWinGISControl2> rendering;
    ASSERT_EQ(_control.As(&rendering), S_OK);
    EXPECT_EQ(rendering->put_RenderingBackendPreference(ATL::CComBSTR(L"Vulkan")), E_INVALIDARG);
    ComPtr<IErrorInfo> renderingError;
    ASSERT_EQ(::GetErrorInfo(0, &renderingError), S_OK);
    ASSERT_NE(renderingError, nullptr);
    GUID source{};
    ASSERT_EQ(renderingError->GetGUID(&source), S_OK);
    EXPECT_TRUE(IsEqualGUID(source, __uuidof(IMapWinGISControl2)));
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

TEST_F(ActiveXMapTest, EndWaitsForOwnerThreadVectorLoadFinalization)
{
    ComPtr<IMapWinGISControl5> control;
    ASSERT_EQ(_control.As(&control), S_OK);
    GdalEventSubscription subscription(control.Get());
    ASSERT_EQ(subscription.Result(), S_OK);

    const std::vector<mwg::Extent> rectangles{{0, 0, 2, 2}};
    const auto path = mwg::test::MakeRectanglesDataset(
        _directory.Path(),
        "async-end-before-dispatch.gpkg",
        rectangles);
    LONGLONG operationId = 0;
    ASSERT_EQ(control->BeginLoadVectorDataset(
        ATL::CComBSTR(path.c_str()),
        &operationId), S_OK);
    ASSERT_GT(operationId, 0);

    ASSERT_TRUE(WaitForGdalDispatchMessage(
        std::chrono::steady_clock::now() + std::chrono::seconds(10)));

    VARIANT_BOOL succeeded = VARIANT_TRUE;
    VARIANT_BOOL cancelled = VARIANT_TRUE;
    LONGLONG layerId = -1;
    BSTR summary = nullptr;
    BSTR diagnostics = nullptr;
    EXPECT_EQ(control->EndLoadVectorDataset(
        operationId,
        &succeeded,
        &cancelled,
        &layerId,
        &summary,
        &diagnostics), E_PENDING);
    EXPECT_EQ(succeeded, VARIANT_FALSE);
    EXPECT_EQ(cancelled, VARIANT_FALSE);
    EXPECT_EQ(layerId, 0);
    EXPECT_EQ(summary, nullptr);
    EXPECT_EQ(diagnostics, nullptr);

    ASSERT_TRUE(PumpUntilGdalCompletion(
        *subscription.Sink(),
        operationId,
        std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    EXPECT_EQ(control->EndLoadVectorDataset(
        operationId,
        &succeeded,
        &cancelled,
        &layerId,
        &summary,
        &diagnostics), S_OK);
    std::unique_ptr<wchar_t, decltype(&SysFreeString)> summaryText(
        summary,
        &SysFreeString);
    std::unique_ptr<wchar_t, decltype(&SysFreeString)> diagnosticText(
        diagnostics,
        &SysFreeString);
    EXPECT_EQ(succeeded, VARIANT_TRUE);
    EXPECT_EQ(cancelled, VARIANT_FALSE);
    EXPECT_GT(layerId, 0);
}

TEST_F(ActiveXMapTest, EndLoadVectorDatasetCanRetryAfterMarshallingFailure)
{
    ComPtr<IMapWinGISControl5> control;
    ASSERT_EQ(_control.As(&control), S_OK);
    GdalEventSubscription subscription(control.Get());
    ASSERT_EQ(subscription.Result(), S_OK);
    ScopedResultMarshallingFailure failure(_module->Get());
    if (!failure.IsAvailable())
    {
        GTEST_SKIP() << "MapWinGIS.ocx was built without MWG_ENABLE_ACTIVEX_TEST_HOOKS.";
    }

    const auto path = mwg::test::MakeRectanglesDataset(
        _directory.Path(),
        "async-marshalling-retry.gpkg",
        std::vector<mwg::Extent>{{0, 0, 2, 2}});
    LONGLONG operationId = 0;
    ASSERT_EQ(control->BeginLoadVectorDataset(
        ATL::CComBSTR(path.c_str()),
        &operationId), S_OK);
    ASSERT_TRUE(PumpUntilGdalCompletion(
        *subscription.Sink(),
        operationId,
        std::chrono::steady_clock::now() + std::chrono::seconds(10)));

    failure.FailOnCall(2, E_FAIL);
    VARIANT_BOOL succeeded = VARIANT_TRUE;
    VARIANT_BOOL cancelled = VARIANT_TRUE;
    LONGLONG layerId = -1;
    BSTR summary = nullptr;
    BSTR diagnostics = nullptr;
    EXPECT_EQ(control->EndLoadVectorDataset(
        operationId,
        &succeeded,
        &cancelled,
        &layerId,
        &summary,
        &diagnostics), E_FAIL);
    EXPECT_EQ(succeeded, VARIANT_FALSE);
    EXPECT_EQ(cancelled, VARIANT_FALSE);
    EXPECT_EQ(layerId, 0);
    EXPECT_EQ(summary, nullptr);
    EXPECT_EQ(diagnostics, nullptr);

    failure.Reset();
    ASSERT_EQ(control->EndLoadVectorDataset(
        operationId,
        &succeeded,
        &cancelled,
        &layerId,
        &summary,
        &diagnostics), S_OK);
    std::unique_ptr<wchar_t, decltype(&SysFreeString)> summaryText(
        summary,
        &SysFreeString);
    std::unique_ptr<wchar_t, decltype(&SysFreeString)> diagnosticText(
        diagnostics,
        &SysFreeString);
    EXPECT_EQ(succeeded, VARIANT_TRUE);
    EXPECT_EQ(cancelled, VARIANT_FALSE);
    EXPECT_GT(layerId, 0);
    ASSERT_NE(summaryText, nullptr);
    EXPECT_NE(
        std::wstring(summaryText.get()).find(L"loaded"),
        std::wstring::npos);
    LONG layerCount = 0;
    ASSERT_EQ(control->get_LayerCount(&layerCount), S_OK);
    EXPECT_EQ(layerCount, 1);
}

TEST_F(ActiveXMapTest, EndGdalOperationCanRetryAfterMarshallingFailure)
{
    RegisterHostedTestAlgorithms();
    ComPtr<IMapWinGISControlGdal> control;
    ASSERT_EQ(_control.As(&control), S_OK);
    GdalEventSubscription subscription(control.Get());
    ASSERT_EQ(subscription.Result(), S_OK);
    ScopedResultMarshallingFailure failure(_module->Get());
    if (!failure.IsAvailable())
    {
        GTEST_SKIP() << "MapWinGIS.ocx was built without MWG_ENABLE_ACTIVEX_TEST_HOOKS.";
    }

    LONGLONG operationId = 0;
    ASSERT_EQ(StartGdalOperation(
        control.Get(),
        L"mwg-test-activex diagnostics",
        {},
        &operationId), S_OK);
    ASSERT_TRUE(PumpUntilGdalCompletion(
        *subscription.Sink(),
        operationId,
        std::chrono::steady_clock::now() + std::chrono::seconds(10)));

    failure.FailOnCall(2, E_OUTOFMEMORY);
    ComGdalResult failedResult;
    EXPECT_EQ(EndGdalOperation(control.Get(), operationId, failedResult), E_OUTOFMEMORY);

    failure.Reset();
    ComGdalResult result;
    ASSERT_EQ(EndGdalOperation(control.Get(), operationId, result), S_OK);
    EXPECT_EQ(result.succeeded, VARIANT_TRUE);
    EXPECT_EQ(result.cancelled, VARIANT_FALSE);
    EXPECT_EQ(result.warnings.size(), 1u);
    EXPECT_NE(
        result.warnings.front().find(L"COM test warning"),
        std::wstring::npos);
}

TEST_F(ActiveXMapTest, LoadsVectorDatasetAsynchronouslyAndCommitsBeforeCompletionEvent)
{
    ComPtr<IMapWinGISControl5> control;
    ASSERT_EQ(_control.As(&control), S_OK);
    GdalEventSubscription subscription(control.Get());
    ASSERT_EQ(subscription.Result(), S_OK);
    VectorLoadCompletionObservation observation{control.Get()};
    subscription.Sink()->SetCompletionCallback(
        &VectorLoadCompletionObservation::Observe,
        &observation);

    const std::vector<mwg::Extent> rectangles{{0, 0, 2, 2}, {3, 0, 5, 2}};
    const auto path = mwg::test::MakeRectanglesDataset(
        _directory.Path(),
        "async-load.gpkg",
        rectangles);
    LONGLONG operationId = 0;
    ASSERT_EQ(control->BeginLoadVectorDataset(
        ATL::CComBSTR(path.c_str()),
        &operationId), S_OK);
    ASSERT_GT(operationId, 0);

    LONG layerCount = -1;
    ASSERT_EQ(control->get_LayerCount(&layerCount), S_OK);
    EXPECT_EQ(layerCount, 0);
    ASSERT_TRUE(PumpUntilGdalCompletion(
        *subscription.Sink(),
        operationId,
        std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    EXPECT_TRUE(subscription.Sink()->WasDeliveredOn(std::this_thread::get_id()));
    EXPECT_EQ(observation.layerCountResult, S_OK);
    EXPECT_EQ(observation.featureCountResult, S_OK);
    EXPECT_EQ(observation.layerCount, 1);
    EXPECT_EQ(observation.featureCount, 2);

    ComPtr<IOleInPlaceObjectWindowless> window;
    ASSERT_EQ(control.As(&window), S_OK);
    LRESULT windowMessageResult = 0;
    ASSERT_TRUE(SUCCEEDED(window->OnWindowMessage(
        WM_DESTROY,
        0,
        0,
        &windowMessageResult)));
    ASSERT_TRUE(SUCCEEDED(window->OnWindowMessage(
        WM_CREATE,
        0,
        0,
        &windowMessageResult)));
    PumpGdalMessagesFor(std::chrono::milliseconds(100));
    EXPECT_EQ(subscription.Sink()->CompletionCount(operationId), 1u);
    EXPECT_EQ(observation.layerCountResult, S_OK);
    EXPECT_EQ(observation.featureCountResult, S_OK);
    EXPECT_EQ(observation.layerCount, 1);
    EXPECT_EQ(observation.featureCount, 2);

    VARIANT_BOOL succeeded = VARIANT_FALSE;
    VARIANT_BOOL cancelled = VARIANT_TRUE;
    LONGLONG layerId = 0;
    BSTR rawSummary = nullptr;
    BSTR rawDiagnostics = nullptr;
    ASSERT_EQ(control->EndLoadVectorDataset(
        operationId,
        &succeeded,
        &cancelled,
        &layerId,
        &rawSummary,
        &rawDiagnostics), S_OK);
    std::unique_ptr<wchar_t, decltype(&SysFreeString)> summary(
        rawSummary,
        &SysFreeString);
    std::unique_ptr<wchar_t, decltype(&SysFreeString)> diagnostics(
        rawDiagnostics,
        &SysFreeString);
    EXPECT_EQ(succeeded, VARIANT_TRUE);
    EXPECT_EQ(cancelled, VARIANT_FALSE);
    EXPECT_GT(layerId, 0);
    ASSERT_NE(summary, nullptr);
    EXPECT_NE(std::wstring(summary.get()).find(L"loaded"), std::wstring::npos);
    ASSERT_EQ(control->get_LayerCount(&layerCount), S_OK);
    EXPECT_EQ(layerCount, 1);
    LONGLONG featureCount = 0;
    ASSERT_EQ(_control->get_FeatureCount(&featureCount), S_OK);
    EXPECT_EQ(featureCount, 2);
}

TEST_F(ActiveXMapTest, ClosingTheControlFromTheLoadCommitViewChangeIsSafe)
{
    class ClosingViewChangeSink final :
        public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IAdviseSink>
    {
    public:
        explicit ClosingViewChangeSink(IOleObject* object) noexcept : _object(object) {}
        void STDMETHODCALLTYPE OnDataChange(FORMATETC*, STGMEDIUM*) override {}
        void STDMETHODCALLTYPE OnViewChange(DWORD, LONG) override
        {
            if (closeResult == S_FALSE)
            {
                closeResult = _object->Close(OLECLOSE_NOSAVE);
            }
        }
        void STDMETHODCALLTYPE OnRename(IMoniker*) override {}
        void STDMETHODCALLTYPE OnSave() override {}
        void STDMETHODCALLTYPE OnClose() override {}
        HRESULT closeResult = S_FALSE;

    private:
        IOleObject* _object;
    };

    ComPtr<IMapWinGISControl5> control;
    ASSERT_EQ(_control.As(&control), S_OK);
    ComPtr<IOleObject> oleObject;
    ASSERT_EQ(_control.As(&oleObject), S_OK);
    const auto sink = Microsoft::WRL::Make<ClosingViewChangeSink>(oleObject.Get());
    ASSERT_NE(sink, nullptr);
    ASSERT_EQ(_viewObject->SetAdvise(DVASPECT_CONTENT, 0, sink.Get()), S_OK);

    const auto path = mwg::test::MakeRectanglesDataset(
        _directory.Path(),
        "async-close-on-commit.gpkg",
        std::vector<mwg::Extent>{{0, 0, 2, 2}});
    LONGLONG operationId = 0;
    ASSERT_EQ(control->BeginLoadVectorDataset(
        ATL::CComBSTR(path.c_str()),
        &operationId), S_OK);

    // The commit's FireViewChange re-enters Close, which discards every load
    // state; the commit must finish on its own reference to the state.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (sink->closeResult == S_FALSE && std::chrono::steady_clock::now() < deadline)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    }
    ASSERT_EQ(sink->closeResult, S_OK);
    ASSERT_EQ(_viewObject->SetAdvise(DVASPECT_CONTENT, 0, nullptr), S_OK);

    VARIANT_BOOL succeeded = VARIANT_TRUE;
    VARIANT_BOOL cancelled = VARIANT_TRUE;
    LONGLONG layerId = -1;
    BSTR summary = nullptr;
    BSTR diagnostics = nullptr;
    EXPECT_EQ(control->EndLoadVectorDataset(
        operationId,
        &succeeded,
        &cancelled,
        &layerId,
        &summary,
        &diagnostics), E_INVALIDARG);
    EXPECT_EQ(summary, nullptr);
    EXPECT_EQ(diagnostics, nullptr);
    LONG layerCount = 0;
    ASSERT_EQ(control->get_LayerCount(&layerCount), S_OK);
    EXPECT_EQ(layerCount, 1);
}

TEST_F(ActiveXMapTest, CancellingVectorDatasetLoadLeavesTheMapUnchanged)
{
    ComPtr<IMapWinGISControl5> control;
    ASSERT_EQ(_control.As(&control), S_OK);
    GdalEventSubscription subscription(control.Get());
    ASSERT_EQ(subscription.Result(), S_OK);

    const std::vector<mwg::Extent> rectangles{{0, 0, 2, 2}};
    const auto path = mwg::test::MakeRectanglesDataset(
        _directory.Path(),
        "async-cancel.gpkg",
        rectangles);
    LONGLONG operationId = 0;
    ASSERT_EQ(control->BeginLoadVectorDataset(
        ATL::CComBSTR(path.c_str()),
        &operationId), S_OK);
    ASSERT_GT(operationId, 0);
    EXPECT_EQ(control->CancelLoadVectorDataset(operationId), S_OK);
    ASSERT_TRUE(PumpUntilGdalCompletion(
        *subscription.Sink(),
        operationId,
        std::chrono::steady_clock::now() + std::chrono::seconds(10)));

    VARIANT_BOOL succeeded = VARIANT_TRUE;
    VARIANT_BOOL cancelled = VARIANT_FALSE;
    LONGLONG layerId = -1;
    BSTR summary = nullptr;
    BSTR diagnostics = nullptr;
    ASSERT_EQ(control->EndLoadVectorDataset(
        operationId,
        &succeeded,
        &cancelled,
        &layerId,
        &summary,
        &diagnostics), S_OK);
    ::SysFreeString(summary);
    ::SysFreeString(diagnostics);
    EXPECT_EQ(succeeded, VARIANT_FALSE);
    EXPECT_EQ(cancelled, VARIANT_TRUE);
    EXPECT_EQ(layerId, 0);
    LONG layerCount = -1;
    ASSERT_EQ(control->get_LayerCount(&layerCount), S_OK);
    EXPECT_EQ(layerCount, 0);
    LONGLONG featureCount = -1;
    ASSERT_EQ(_control->get_FeatureCount(&featureCount), S_OK);
    EXPECT_EQ(featureCount, 0);
}

TEST_F(ActiveXMapTest, FailedVectorDatasetLoadReportsThePath)
{
    ComPtr<IMapWinGISControl5> control;
    ASSERT_EQ(_control.As(&control), S_OK);
    GdalEventSubscription subscription(control.Get());
    ASSERT_EQ(subscription.Result(), S_OK);
    const auto path = _directory.Path() / "missing.gpkg";
    const mwg::GdalEnvironment::QuietScope quiet;

    LONGLONG operationId = 0;
    ASSERT_EQ(control->BeginLoadVectorDataset(
        ATL::CComBSTR(path.c_str()),
        &operationId), S_OK);
    ASSERT_TRUE(PumpUntilGdalCompletion(
        *subscription.Sink(),
        operationId,
        std::chrono::steady_clock::now() + std::chrono::seconds(10)));

    VARIANT_BOOL succeeded = VARIANT_TRUE;
    VARIANT_BOOL cancelled = VARIANT_TRUE;
    LONGLONG layerId = -1;
    BSTR rawSummary = nullptr;
    BSTR rawDiagnostics = nullptr;
    ASSERT_EQ(control->EndLoadVectorDataset(
        operationId,
        &succeeded,
        &cancelled,
        &layerId,
        &rawSummary,
        &rawDiagnostics), S_OK);
    std::unique_ptr<wchar_t, decltype(&SysFreeString)> summary(
        rawSummary,
        &SysFreeString);
    std::unique_ptr<wchar_t, decltype(&SysFreeString)> diagnostics(
        rawDiagnostics,
        &SysFreeString);
    EXPECT_EQ(succeeded, VARIANT_FALSE);
    EXPECT_EQ(cancelled, VARIANT_FALSE);
    EXPECT_EQ(layerId, 0);
    ASSERT_NE(summary, nullptr);
    EXPECT_NE(std::wstring(summary.get()).find(path.filename().wstring()), std::wstring::npos);
    ASSERT_NE(diagnostics, nullptr);
    EXPECT_NE(std::wstring(diagnostics.get()).find(path.filename().wstring()), std::wstring::npos);
    LONG layerCount = -1;
    ASSERT_EQ(control->get_LayerCount(&layerCount), S_OK);
    EXPECT_EQ(layerCount, 0);
}

TEST_F(ActiveXMapTest, DestroyingControlDuringVectorLoadDoesNotAccessTheControlFromWorker)
{
    VectorPreparationBarrier barrier;
    ASSERT_TRUE(barrier.IsValid());
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    const auto setPreparationHook =
        reinterpret_cast<VectorPreparationHookSetter>(GetProcAddress(
            _module->Get(),
            "MapWinGISControlSetVectorPreparationHookForTesting"));
    if (setPreparationHook == nullptr)
    {
        GTEST_SKIP() << "MapWinGIS.ocx was built without MWG_ENABLE_ACTIVEX_TEST_HOOKS.";
    }
    ScopedVectorPreparationHook hookReset(setPreparationHook);
    setPreparationHook(&VectorPreparationBarrier::Notify, &barrier);

    const auto getClassObject = reinterpret_cast<GetClassObject>(
        GetProcAddress(_module->Get(), "DllGetClassObject"));
    ASSERT_NE(getClassObject, nullptr);
    ComPtr<IClassFactory> factory;
    ASSERT_EQ(getClassObject(
        __uuidof(MapWinGISControl),
        IID_PPV_ARGS(&factory)), S_OK);
    ComPtr<IMapWinGISControl5> control;
    ASSERT_EQ(factory->CreateInstance(
        nullptr,
        IID_PPV_ARGS(&control)), S_OK);

    const std::vector<mwg::Extent> rectangles{{0, 0, 2, 2}};
    const auto path = mwg::test::MakeRectanglesDataset(
        _directory.Path(),
        "async-destroy.gpkg",
        rectangles);
    LONGLONG operationId = 0;
    ASSERT_EQ(control->BeginLoadVectorDataset(
        ATL::CComBSTR(path.c_str()),
        &operationId), S_OK);
    ASSERT_GT(operationId, 0);
    ASSERT_TRUE(barrier.WaitForStart(10000));
    control.Reset();
    ASSERT_TRUE(barrier.Release());
    ASSERT_TRUE(barrier.WaitForFinish(10000));
    EXPECT_EQ(barrier.ReleaseWaitResult(), WAIT_OBJECT_0);
    setPreparationHook(nullptr, nullptr);
}

TEST_F(ActiveXMapTest, ExposesStableLayerIdsAndMapNavigation)
{
    constexpr int width = 120;
    constexpr int height = 100;
    ASSERT_EQ(Resize(width, height), S_OK);

    ComPtr<IMapWinGISControl4> control;
    ASSERT_EQ(_control.As(&control), S_OK);

    LONG layerCount = -1;
    EXPECT_EQ(control->get_LayerCount(nullptr), E_POINTER);
    ASSERT_EQ(control->get_LayerCount(&layerCount), S_OK);
    EXPECT_EQ(layerCount, 0);

    const std::vector<mwg::Extent> firstRectangle{{0, 0, 10, 10}};
    const auto firstPath = mwg::test::MakeRectanglesDataset(
        _directory.Path(),
        "navigation-first.gpkg",
        firstRectangle);
    LONGLONG firstLayerId = -1;
    EXPECT_EQ(control->LoadVectorDatasetEx(ATL::CComBSTR(firstPath.c_str()), nullptr), E_POINTER);
    EXPECT_EQ(control->GetLayerId(0, nullptr), E_POINTER);
    ASSERT_EQ(
        control->LoadVectorDatasetEx(
            ATL::CComBSTR(firstPath.c_str()),
            &firstLayerId),
        S_OK);
    EXPECT_GT(firstLayerId, 0);

    LONGLONG indexedLayerId = 0;
    ASSERT_EQ(control->GetLayerId(0, &indexedLayerId), S_OK);
    EXPECT_EQ(indexedLayerId, firstLayerId);
    ASSERT_EQ(control->get_LayerCount(&layerCount), S_OK);
    EXPECT_EQ(layerCount, 1);

    ASSERT_EQ(LoadRectangles({{20, 0, 30, 10}}), S_OK);
    LONGLONG secondLayerId = 0;
    ASSERT_EQ(control->GetLayerId(1, &secondLayerId), S_OK);
    EXPECT_GT(secondLayerId, firstLayerId);
    ASSERT_EQ(control->get_LayerCount(&layerCount), S_OK);
    EXPECT_EQ(layerCount, 2);

    LONGLONG failedLayerId = -1;
    EXPECT_EQ(control->LoadVectorDatasetEx(nullptr, &failedLayerId), E_INVALIDARG);
    EXPECT_EQ(failedLayerId, 0);
    ASSERT_EQ(control->get_LayerCount(&layerCount), S_OK);
    EXPECT_EQ(layerCount, 2);
    ASSERT_EQ(LoadLine(), E_FAIL);
    failedLayerId = -1;
    EXPECT_EQ(control->LoadVectorDatasetEx(
        ATL::CComBSTR((_directory.Path() / "line.geojson").c_str()), &failedLayerId), E_FAIL);
    EXPECT_EQ(failedLayerId, 0);
    ASSERT_EQ(control->get_LayerCount(&layerCount), S_OK);
    EXPECT_EQ(layerCount, 2);

    EXPECT_EQ(control->GetLayerId(-1, &indexedLayerId), E_INVALIDARG);
    EXPECT_EQ(control->GetLayerId(2, &indexedLayerId), E_INVALIDARG);
    EXPECT_EQ(control->ZoomToLayer(0), E_INVALIDARG);
    EXPECT_EQ(control->ZoomToLayer(secondLayerId + 100), E_INVALIDARG);
    EXPECT_EQ(control->ZoomIn(1.0), E_INVALIDARG);
    EXPECT_EQ(control->ZoomOut(1.0), E_INVALIDARG);
    EXPECT_EQ(control->ZoomIn(std::numeric_limits<double>::quiet_NaN()), E_INVALIDARG);
    EXPECT_EQ(
        control->Pan(std::numeric_limits<double>::infinity(), 0.0),
        E_INVALIDARG);
    EXPECT_EQ(control->ZoomAt(0.0, 0.0, 0.0), E_INVALIDARG);

    BitmapSurface fitted(width, height);
    BitmapSurface zoomed(width, height);
    ASSERT_TRUE(fitted.IsValid());
    ASSERT_TRUE(zoomed.IsValid());
    mwg::Viewport expected(width, height);
    ASSERT_TRUE(expected.FitExtent({0, 0, 30, 10}, 12.0));
    const auto expectFeatureAt = [&](const mwg::MapPoint& point)
    {
        const auto screen = expected.MapToScreen(point);
        ASSERT_TRUE(screen.has_value());
        const auto pixel = mwg::ToPixel(*screen);
        ASSERT_TRUE(pixel.has_value());
        ASSERT_GE(pixel->x, 0);
        ASSERT_LT(pixel->x, width);
        ASSERT_GE(pixel->y, 0);
        ASSERT_LT(pixel->y, height);
        EXPECT_EQ(GetPixel(zoomed.Get(), pixel->x, pixel->y), RGB(61, 122, 169));
    };

    ASSERT_EQ(control->FitToMap(), S_OK);
    ASSERT_EQ(Draw(fitted.Get(), width, height), S_OK);
    ASSERT_EQ(control->ZoomIn(2.0), S_OK);
    ASSERT_TRUE(expected.Zoom(2.0));
    ASSERT_EQ(Draw(zoomed.Get(), width, height), S_OK);
    EXPECT_TRUE(HasDifferentPixel(fitted.Get(), zoomed.Get(), width, height));
    expectFeatureAt({8, 5});

    ASSERT_EQ(control->ZoomOut(2.0), S_OK);
    ASSERT_TRUE(expected.ZoomOut(2.0));
    ASSERT_EQ(Draw(zoomed.Get(), width, height), S_OK);
    expectFeatureAt({2, 5});

    ASSERT_EQ(control->Pan(2.0, -1.0), S_OK);
    ASSERT_TRUE(expected.Pan(2.0, -1.0));
    ASSERT_EQ(Draw(zoomed.Get(), width, height), S_OK);
    expectFeatureAt({2, 5});

    ASSERT_EQ(control->ZoomAt(15.0, 47.0, 1.5), S_OK);
    ASSERT_TRUE(expected.ZoomAt({15.0, 47.0}, 1.5));
    ASSERT_EQ(Draw(zoomed.Get(), width, height), S_OK);
    expectFeatureAt({8.5, 5});

    ASSERT_EQ(control->ZoomToLayer(firstLayerId), S_OK);

    ASSERT_EQ(control->ClearLayers(), S_OK);
    EXPECT_EQ(control->ZoomToLayer(firstLayerId), E_INVALIDARG);
    ASSERT_EQ(control->get_LayerCount(&layerCount), S_OK);
    EXPECT_EQ(layerCount, 0);
    LONGLONG reloadedLayerId = 0;
    ASSERT_EQ(
        control->LoadVectorDatasetEx(
            ATL::CComBSTR(firstPath.c_str()),
            &reloadedLayerId),
        S_OK);
    EXPECT_GT(reloadedLayerId, secondLayerId);
    EXPECT_EQ(control->ZoomToLayer(firstLayerId), E_INVALIDARG);
}

TEST_F(ActiveXMapTest, ReportsAutomationErrorInformation)
{
    ComPtr<IMapWinGISControl4> control;
    ComPtr<ISupportErrorInfo> support;
    ASSERT_EQ(_control.As(&control), S_OK);
    ASSERT_EQ(_control.As(&support), S_OK);
    for (const IID* iid : {&__uuidof(IMapWinGISControl), &__uuidof(IMapWinGISControl2),
                           &__uuidof(IMapWinGISControl3), &__uuidof(IMapWinGISControl4),
                           &__uuidof(IMapWinGISControlGdal),
                           &__uuidof(IMapWinGISControlProjection)})
    {
        EXPECT_EQ(support->InterfaceSupportsErrorInfo(*iid), S_OK);
    }
    EXPECT_EQ(support->InterfaceSupportsErrorInfo(IID_IUnknown), S_FALSE);

    ASSERT_EQ(control->ZoomIn(1.0), E_INVALIDARG);
    ComPtr<IErrorInfo> error;
    ASSERT_EQ(GetErrorInfo(0, &error), S_OK);
    GUID source{};
    ASSERT_EQ(error->GetGUID(&source), S_OK);
    EXPECT_TRUE(IsEqualIID(source, __uuidof(IMapWinGISControl4)));
    ATL::CComBSTR description;
    ASSERT_EQ(error->GetDescription(&description), S_OK);
    EXPECT_STREQ(description.m_str, L"Zoom factor must be a finite number above 1");

    ASSERT_EQ(control->ZoomToLayer(123), E_INVALIDARG);
    error.Reset();
    ASSERT_EQ(GetErrorInfo(0, &error), S_OK);
    ASSERT_EQ(error->GetGUID(&source), S_OK);
    EXPECT_TRUE(IsEqualIID(source, __uuidof(IMapWinGISControl4)));
    description.Empty();
    ASSERT_EQ(error->GetDescription(&description), S_OK);
    EXPECT_STREQ(description.m_str, L"Layer ID was not found");

    ASSERT_EQ(LoadLine(), E_FAIL);
    error.Reset();
    ASSERT_EQ(GetErrorInfo(0, &error), S_OK);
    ASSERT_EQ(error->GetGUID(&source), S_OK);
    EXPECT_TRUE(IsEqualIID(source, __uuidof(IMapWinGISControl)));
}

TEST_F(ActiveXMapTest, ExposesProjectionAutomationAndReprojectsDisplay)
{
    ComPtr<IMapWinGISControlProjection> projection;
    ComPtr<IMapWinGISControl4> control;
    ASSERT_EQ(_control.As(&projection), S_OK);
    ASSERT_EQ(_control.As(&control), S_OK);

    LONG epsg = 0;
    ATL::CComBSTR wkt(L"unchanged");
    ASSERT_EQ(projection->get_MapEpsg(&epsg), S_OK);
    EXPECT_EQ(epsg, -1);
    ASSERT_EQ(projection->get_MapProjection(&wkt), S_OK);
    EXPECT_EQ(wkt.Length(), 0U);

    const auto path = mwg::test::MakeSampleShapefile(
        _directory.Path(), "projection");
    VARIANT_BOOL matchesMapProjection = VARIANT_TRUE;
    ASSERT_EQ(projection->ProbeVectorProjection(
        ATL::CComBSTR(path.c_str()),
        &epsg,
        &wkt,
        &matchesMapProjection), S_OK);
    EXPECT_EQ(epsg, 28992);
    EXPECT_GT(wkt.Length(), 0U);
    EXPECT_EQ(matchesMapProjection, VARIANT_FALSE);

    LONGLONG layerId = 0;
    ASSERT_EQ(control->LoadVectorDatasetEx(
        ATL::CComBSTR(path.c_str()), &layerId), S_OK);
    ASSERT_EQ(projection->get_MapEpsg(&epsg), S_OK);
    EXPECT_EQ(epsg, 28992);
    ASSERT_EQ(projection->LayerEpsg(layerId, &epsg), S_OK);
    EXPECT_EQ(epsg, 28992);
    ASSERT_EQ(projection->LayerProjection(layerId, &wkt), S_OK);
    EXPECT_GT(wkt.Length(), 0U);
    ASSERT_EQ(projection->LayerDisplayProjection(layerId, &wkt), S_OK);
    EXPECT_EQ(wkt.Length(), 0U);
    ASSERT_EQ(projection->LayerDisplayEpsg(layerId, &epsg), S_OK);
    EXPECT_EQ(epsg, -1);
    ASSERT_EQ(projection->ProbeVectorProjection(
        ATL::CComBSTR(path.c_str()),
        &epsg,
        &wkt,
        &matchesMapProjection), S_OK);
    EXPECT_EQ(matchesMapProjection, VARIANT_TRUE);

    const std::string wgs84 =
        mwg::Projection::FromEpsg(4326).ExportToWkt();
    ASSERT_EQ(
        projection->put_MapProjection(ATL::CComBSTR(wgs84.c_str())),
        S_OK);
    ASSERT_EQ(projection->get_MapEpsg(&epsg), S_OK);
    EXPECT_EQ(epsg, 4326);
    ASSERT_EQ(projection->ReprojectLayer(layerId), S_OK);
    ASSERT_EQ(projection->LayerEpsg(layerId, &epsg), S_OK);
    EXPECT_EQ(epsg, 28992);
    ASSERT_EQ(projection->LayerDisplayEpsg(layerId, &epsg), S_OK);
    EXPECT_EQ(epsg, 4326);
    ASSERT_EQ(projection->LayerDisplayProjection(layerId, &wkt), S_OK);
    EXPECT_GT(wkt.Length(), 0U);
    EXPECT_TRUE(mwg::activex::testing::
        MapWinGISControlTestAccess::LayerHasDisplayProjection(
            mwg::activex::testing::MapWinGISControlTestAccess::Control(
                _control.Get()),
            mwg::LayerId{static_cast<std::uint64_t>(layerId)}));

    ASSERT_EQ(projection->ClearLayerReprojection(layerId), S_OK);
    ASSERT_EQ(projection->LayerDisplayEpsg(layerId, &epsg), S_OK);
    EXPECT_EQ(epsg, -1);
    ASSERT_EQ(projection->LayerDisplayProjection(layerId, &wkt), S_OK);
    EXPECT_EQ(wkt.Length(), 0U);
    EXPECT_FALSE(mwg::activex::testing::
        MapWinGISControlTestAccess::LayerHasDisplayProjection(
            mwg::activex::testing::MapWinGISControlTestAccess::Control(
                _control.Get()),
            mwg::LayerId{static_cast<std::uint64_t>(layerId)}));

    ASSERT_EQ(projection->put_MapProjection(ATL::CComBSTR(L"")), S_OK);
    ASSERT_EQ(projection->get_MapEpsg(&epsg), S_OK);
    EXPECT_EQ(epsg, -1);
    EXPECT_EQ(projection->LayerEpsg(layerId + 100, &epsg), E_INVALIDARG);
    EXPECT_EQ(projection->ReprojectLayer(layerId), E_INVALIDARG);
}

TEST_F(ActiveXMapTest, AClearedMapTakesTheNextLayerProjectionAfterAProjectionReset)
{
    ComPtr<IMapWinGISControlProjection> projection;
    ComPtr<IMapWinGISControl4> control;
    ASSERT_EQ(_control.As(&projection), S_OK);
    ASSERT_EQ(_control.As(&control), S_OK);

    const auto path = mwg::test::MakeSampleShapefile(
        _directory.Path(), "cleared");
    LONGLONG layerId = 0;
    ASSERT_EQ(control->LoadVectorDatasetEx(
        ATL::CComBSTR(path.c_str()), &layerId), S_OK);
    const std::string wgs84 =
        mwg::Projection::FromEpsg(4326).ExportToWkt();
    ASSERT_EQ(
        projection->put_MapProjection(ATL::CComBSTR(wgs84.c_str())),
        S_OK);

    // Clearing layers keeps the map projection; the host resets it.
    ASSERT_EQ(control->ClearLayers(), S_OK);
    LONG epsg = 0;
    ASSERT_EQ(projection->get_MapEpsg(&epsg), S_OK);
    EXPECT_EQ(epsg, 4326);
    ASSERT_EQ(projection->put_MapProjection(ATL::CComBSTR(L"")), S_OK);
    ATL::CComBSTR wkt(L"unchanged");
    ASSERT_EQ(projection->get_MapProjection(&wkt), S_OK);
    EXPECT_EQ(wkt.Length(), 0U);

    ASSERT_EQ(control->LoadVectorDatasetEx(
        ATL::CComBSTR(path.c_str()), &layerId), S_OK);
    ASSERT_EQ(projection->get_MapEpsg(&epsg), S_OK);
    EXPECT_EQ(epsg, 28992);
    ASSERT_EQ(projection->LayerDisplayEpsg(layerId, &epsg), S_OK);
    EXPECT_EQ(epsg, -1);
    VARIANT_BOOL matchesMapProjection = VARIANT_FALSE;
    ASSERT_EQ(projection->ProbeVectorProjection(
        ATL::CComBSTR(path.c_str()),
        &epsg,
        &wkt,
        &matchesMapProjection), S_OK);
    EXPECT_EQ(matchesMapProjection, VARIANT_TRUE);
}

TEST_F(ActiveXMapTest, AnEmptyFrameAfterClearingKeepsThePreferenceButReportsNoBackend)
{
    ASSERT_EQ(Resize(120, 100), S_OK);
    ASSERT_EQ(
        _control->put_RenderingBackendPreference(ATL::CComBSTR(L"GDI")),
        S_OK);
    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    ATL::CComBSTR backend;
    ASSERT_EQ(_control->get_RenderingBackend(&backend), S_OK);
    EXPECT_STREQ(backend, L"GDI");

    ASSERT_EQ(_control->ClearLayers(), S_OK);
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    backend.Empty();
    ASSERT_EQ(_control->get_RenderingBackend(&backend), S_OK);
    EXPECT_STREQ(backend, L"Not rendered");
    ATL::CComBSTR status;
    ASSERT_EQ(_control->get_RenderingStatus(&status), S_OK);
    EXPECT_STREQ(status, L"There are no layers to render.");
    ATL::CComBSTR preference;
    ASSERT_EQ(_control->get_RenderingBackendPreference(&preference), S_OK);
    EXPECT_STREQ(preference, L"GDI");

    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    backend.Empty();
    ASSERT_EQ(_control->get_RenderingBackend(&backend), S_OK);
    EXPECT_STREQ(backend, L"GDI");
}

TEST_F(ActiveXMapTest, AnEmptyFrameBeforeAnyRenderKeepsTheInitialStatus)
{
    ASSERT_EQ(Resize(120, 100), S_OK);
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);

    ATL::CComBSTR backend;
    ASSERT_EQ(_control->get_RenderingBackend(&backend), S_OK);
    EXPECT_STREQ(backend, L"Not rendered");
    ATL::CComBSTR status;
    ASSERT_EQ(_control->get_RenderingStatus(&status), S_OK);
    EXPECT_STREQ(status, L"Rendering has not been attempted.");
}

TEST_F(ActiveXMapTest, AnEmptyFrameWithALayerWithoutGeometryReportsWhyNothingWasDrawn)
{
    ASSERT_EQ(Resize(120, 100), S_OK);
    ASSERT_EQ(
        _control->put_RenderingBackendPreference(ATL::CComBSTR(L"GDI")),
        S_OK);
    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);

    ASSERT_EQ(_control->ClearLayers(), S_OK);
    ASSERT_EQ(LoadRectangles({}), S_OK);
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    ATL::CComBSTR backend;
    ASSERT_EQ(_control->get_RenderingBackend(&backend), S_OK);
    EXPECT_STREQ(backend, L"Not rendered");
    ATL::CComBSTR status;
    ASSERT_EQ(_control->get_RenderingStatus(&status), S_OK);
    EXPECT_STREQ(
        status,
        L"No layer was rendered; every layer is hidden or has no geometry to draw.");
}

TEST_F(ActiveXMapTest, AnEmptyPrintDrawKeepsTheInteractiveRenderingStatus)
{
    ASSERT_EQ(Resize(120, 100), S_OK);
    ASSERT_EQ(
        _control->put_RenderingBackendPreference(ATL::CComBSTR(L"GDI")),
        S_OK);
    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    ATL::CComBSTR status;
    ASSERT_EQ(_control->get_RenderingStatus(&status), S_OK);
    const std::wstring interactiveStatus(status, status.Length());

    // A draw at another size, such as a print, must not report its empty
    // frame as the on-screen status.
    ASSERT_EQ(_control->ClearLayers(), S_OK);
    BitmapSurface printSurface(240, 200);
    ASSERT_TRUE(printSurface.IsValid());
    ASSERT_EQ(Draw(printSurface.Get(), 240, 200), S_OK);
    ATL::CComBSTR backend;
    ASSERT_EQ(_control->get_RenderingBackend(&backend), S_OK);
    EXPECT_STREQ(backend, L"GDI");
    status.Empty();
    ASSERT_EQ(_control->get_RenderingStatus(&status), S_OK);
    EXPECT_STREQ(status, interactiveStatus.c_str());
}

TEST_F(ActiveXMapTest, NavigationSucceedsWhenThereIsNoViewToChange)
{
    ComPtr<IMapWinGISControl4> control;
    ASSERT_EQ(_control.As(&control), S_OK);
    EXPECT_EQ(control->FitToMap(), S_OK);
    EXPECT_EQ(control->ZoomIn(2), S_OK);
    EXPECT_EQ(control->ZoomOut(2), S_OK);
    EXPECT_EQ(control->Pan(1, 1), S_OK);
    EXPECT_EQ(control->ZoomAt(0, 0, 2), S_OK);
}

TEST_F(ActiveXMapTest, PreservesDispatchIdsAndInterfaceIdentity)
{
    ComPtr<IMapWinGISControl> first;
    ComPtr<IMapWinGISControl2> second;
    ComPtr<IMapWinGISControl3> third;
    ComPtr<IMapWinGISControl4> fourth;
    ComPtr<IMapWinGISControl5> fifth;
    ComPtr<IMapWinGISControlProjection> projection;
    ASSERT_EQ(_control.As(&first), S_OK);
    ASSERT_EQ(_control.As(&second), S_OK);
    ASSERT_EQ(_control.As(&third), S_OK);
    ASSERT_EQ(_control.As(&fourth), S_OK);
    ASSERT_EQ(_control.As(&fifth), S_OK);
    ASSERT_EQ(_control.As(&projection), S_OK);
    ComPtr<IUnknown> identity;
    ASSERT_EQ(fourth.As(&identity), S_OK);
    ComPtr<IUnknown> other;
    ASSERT_EQ(first.As(&other), S_OK);
    EXPECT_EQ(identity.Get(), other.Get());
    other.Reset();
    ASSERT_EQ(second.As(&other), S_OK);
    EXPECT_EQ(identity.Get(), other.Get());
    other.Reset();
    ASSERT_EQ(third.As(&other), S_OK);
    EXPECT_EQ(identity.Get(), other.Get());
    other.Reset();
    ASSERT_EQ(fifth.As(&other), S_OK);
    EXPECT_EQ(identity.Get(), other.Get());

    const IID firstIid{0x1B6D4167, 0xB2D7, 0x4AE4, {0x8F, 0x88, 0x92, 0xC5, 0x75, 0xBF, 0x4A, 0xF7}};
    const IID secondIid{0x2C386A29, 0xC378, 0x45C1, {0xB8, 0xD5, 0x56, 0x24, 0x50, 0x0C, 0xCF, 0xF2}};
    const IID thirdIid{0x994F4526, 0x284B, 0x463F, {0x8E, 0xD5, 0x15, 0x51, 0xB6, 0x01, 0x98, 0x1B}};
    EXPECT_TRUE(IsEqualIID(__uuidof(IMapWinGISControl), firstIid));
    EXPECT_TRUE(IsEqualIID(__uuidof(IMapWinGISControl2), secondIid));
    EXPECT_TRUE(IsEqualIID(__uuidof(IMapWinGISControl3), thirdIid));
    const IID fifthIid{0x4F8CE31A, 0x60D1, 0x4B9D, {0xA4, 0xAF, 0x2C, 0x80, 0xC5, 0xF6, 0x3A, 0x91}};
    EXPECT_TRUE(IsEqualIID(__uuidof(IMapWinGISControl5), fifthIid));
    const IID projectionIid{0x26717861, 0xE47D, 0x4326,
                            {0x94, 0x14, 0xFC, 0xF2, 0xE9, 0x04, 0xB7, 0x0D}};
    EXPECT_TRUE(IsEqualIID(
        __uuidof(IMapWinGISControlProjection), projectionIid));

    ComPtr<IDispatch> dispatch;
    ASSERT_EQ(_control.As(&dispatch), S_OK);
    struct Member { const wchar_t* name; DISPID id; };
    const Member members[]{
        {L"LoadVectorDataset", 2}, {L"FitToMap", 9}, {L"ZoomIn", 10},
        {L"ZoomOut", 11}, {L"Pan", 12}, {L"ZoomAt", 13},
        {L"ZoomToLayer", 14}, {L"LayerCount", 15},
        {L"LoadVectorDatasetEx", 16}, {L"GetLayerId", 17},
        {L"BeginLoadVectorDataset", 18}, {L"CancelLoadVectorDataset", 19},
        {L"EndLoadVectorDataset", 20}};
    for (const auto& member : members)
    {
        std::wstring name(member.name);
        LPOLESTR namePointer = name.data();
        DISPID id = DISPID_UNKNOWN;
        ASSERT_EQ(dispatch->GetIDsOfNames(IID_NULL, &namePointer, 1, LOCALE_USER_DEFAULT, &id), S_OK);
        EXPECT_EQ(id, member.id) << name.c_str();
    }

    const std::vector<mwg::Extent> rectangles{{0, 0, 10, 10}};
    const auto path = mwg::test::MakeRectanglesDataset(
        _directory.Path(), "dispatch.gpkg", rectangles);
    ATL::CComVariant argument(path.c_str());
    DISPPARAMS parameters{&argument, nullptr, 1, 0};
    ASSERT_EQ(dispatch->Invoke(2, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
        &parameters, nullptr, nullptr, nullptr), S_OK);
    DISPPARAMS noArguments{};
    ATL::CComVariant result;
    ASSERT_EQ(dispatch->Invoke(15, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
        &noArguments, &result, nullptr, nullptr), S_OK);
    EXPECT_EQ(result.vt, VT_I4);
    EXPECT_EQ(result.lVal, 1);
}


TEST_F(ActiveXMapTest, ExposesClassInfoAndGdalTypeInformation)
{
    ComPtr<IProvideClassInfo2> classInfo;
    ASSERT_EQ(_control.As(&classInfo), S_OK);

    GUID sourceDispId{};
    ASSERT_EQ(
        classInfo->GetGUID(GUIDKIND_DEFAULT_SOURCE_DISP_IID, &sourceDispId),
        S_OK);
    EXPECT_TRUE(IsEqualIID(sourceDispId, __uuidof(IMapWinGISControlEvents)));

    ComPtr<ITypeInfo> coclassInfo;
    ASSERT_EQ(classInfo->GetClassInfo(&coclassInfo), S_OK);
    TYPEATTR* coclassAttributes = nullptr;
    ASSERT_EQ(coclassInfo->GetTypeAttr(&coclassAttributes), S_OK);
    EXPECT_TRUE(IsEqualGUID(coclassAttributes->guid, __uuidof(MapWinGISControl)));
    coclassInfo->ReleaseTypeAttr(coclassAttributes);

    ComPtr<IMapWinGISControlGdal> gdal;
    ASSERT_EQ(_control.As(&gdal), S_OK);
    UINT typeInfoCount = 0;
    ASSERT_EQ(gdal->GetTypeInfoCount(&typeInfoCount), S_OK);
    EXPECT_EQ(typeInfoCount, 1U);
    ComPtr<ITypeInfo> gdalTypeInfo;
    ASSERT_EQ(gdal->GetTypeInfo(0, LOCALE_USER_DEFAULT, &gdalTypeInfo), S_OK);
    TYPEATTR* gdalAttributes = nullptr;
    ASSERT_EQ(gdalTypeInfo->GetTypeAttr(&gdalAttributes), S_OK);
    EXPECT_TRUE(IsEqualIID(gdalAttributes->guid, __uuidof(IMapWinGISControlGdal)));
    gdalTypeInfo->ReleaseTypeAttr(gdalAttributes);
}

TEST_F(ActiveXMapTest, PersistsRenderingPreferenceThroughStream)
{
    ComPtr<IPersistStreamInit> persistence;
    ASSERT_EQ(_control.As(&persistence), S_OK);
    ComPtr<IMapWinGISControl2> control;
    ASSERT_EQ(_control.As(&control), S_OK);

    ATL::CComBSTR openGl(L"OpenGL");
    ASSERT_EQ(control->put_RenderingBackendPreference(openGl), S_OK);
    EXPECT_EQ(persistence->IsDirty(), S_OK);

    ComPtr<IStream> stream;
    ASSERT_EQ(CreateStreamOnHGlobal(nullptr, TRUE, &stream), S_OK);
    ASSERT_EQ(persistence->Save(stream.Get(), TRUE), S_OK);
    EXPECT_EQ(persistence->IsDirty(), S_FALSE);
    ASSERT_EQ(control->put_RenderingBackendPreference(openGl), S_OK);
    EXPECT_EQ(persistence->IsDirty(), S_FALSE);

    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    const auto getClassObject = reinterpret_cast<GetClassObject>(
        GetProcAddress(_module->Get(), "DllGetClassObject"));
    ASSERT_NE(getClassObject, nullptr);
    ComPtr<IClassFactory> factory;
    ASSERT_EQ(getClassObject(
        __uuidof(MapWinGISControl), IID_PPV_ARGS(&factory)), S_OK);
    ComPtr<IMapWinGISControl2> restoredControl;
    ASSERT_EQ(factory->CreateInstance(
        nullptr, IID_PPV_ARGS(&restoredControl)), S_OK);
    ComPtr<IPersistStreamInit> restoredPersistence;
    ASSERT_EQ(restoredControl.As(&restoredPersistence), S_OK);

    LARGE_INTEGER beginning{};
    ASSERT_EQ(stream->Seek(beginning, STREAM_SEEK_SET, nullptr), S_OK);
    ASSERT_EQ(restoredPersistence->Load(stream.Get()), S_OK);
    EXPECT_EQ(restoredPersistence->IsDirty(), S_FALSE);

    ATL::CComBSTR actual;
    ASSERT_EQ(restoredControl->get_RenderingBackendPreference(&actual), S_OK);
    EXPECT_STREQ(actual, L"OpenGL");
}

TEST_F(ActiveXMapTest, LoadsLegacyEmptyPropertyStream)
{
    ComPtr<IPersistStreamInit> persistence;
    ASSERT_EQ(_control.As(&persistence), S_OK);
    ComPtr<IStream> stream;
    ASSERT_EQ(CreateStreamOnHGlobal(nullptr, TRUE, &stream), S_OK);
    const DWORD version = _ATL_VER;
    ASSERT_EQ(stream->Write(&version, sizeof(version), nullptr), S_OK);
    LARGE_INTEGER beginning{};
    ASSERT_EQ(stream->Seek(beginning, STREAM_SEEK_SET, nullptr), S_OK);
    ASSERT_EQ(persistence->Load(stream.Get()), S_OK);
    EXPECT_EQ(persistence->IsDirty(), S_FALSE);
}

TEST_F(ActiveXMapTest, RejectsNewerLegacyStreamVersion)
{
    ComPtr<IPersistStreamInit> persistence;
    ASSERT_EQ(_control.As(&persistence), S_OK);
    ComPtr<IStream> stream;
    ASSERT_EQ(CreateStreamOnHGlobal(nullptr, TRUE, &stream), S_OK);
    const DWORD version = _ATL_VER + 1;
    ASSERT_EQ(stream->Write(&version, sizeof(version), nullptr), S_OK);
    LARGE_INTEGER beginning{};
    ASSERT_EQ(stream->Seek(beginning, STREAM_SEEK_SET, nullptr), S_OK);
    EXPECT_TRUE(FAILED(persistence->Load(stream.Get())));
}


TEST_F(ActiveXMapTest, PersistsRenderingPreferenceThroughPropertyBag)
{
    ComPtr<IPersistPropertyBag> persistence;
    ASSERT_EQ(_control.As(&persistence), S_OK);
    ComPtr<IMapWinGISControl2> control;
    ASSERT_EQ(_control.As(&control), S_OK);

    ATL::CComBSTR openGl(L"OpenGL");
    ASSERT_EQ(control->put_RenderingBackendPreference(openGl), S_OK);

    ATL::CComObject<TestPropertyBag>* rawBag = nullptr;
    ASSERT_EQ(ATL::CComObject<TestPropertyBag>::CreateInstance(&rawBag), S_OK);
    ASSERT_NE(rawBag, nullptr);
    rawBag->AddRef();
    ComPtr<IPropertyBag> bag;
    bag.Attach(rawBag);
    ASSERT_EQ(persistence->Save(bag.Get(), TRUE, FALSE), S_OK);

    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    const auto getClassObject = reinterpret_cast<GetClassObject>(
        GetProcAddress(_module->Get(), "DllGetClassObject"));
    ASSERT_NE(getClassObject, nullptr);
    ComPtr<IClassFactory> factory;
    ASSERT_EQ(getClassObject(__uuidof(MapWinGISControl), IID_PPV_ARGS(&factory)), S_OK);
    ComPtr<IMapWinGISControl2> restoredControl;
    ASSERT_EQ(factory->CreateInstance(nullptr, IID_PPV_ARGS(&restoredControl)), S_OK);
    ComPtr<IPersistPropertyBag> restoredPersistence;
    ASSERT_EQ(restoredControl.As(&restoredPersistence), S_OK);
    ASSERT_EQ(restoredPersistence->Load(bag.Get(), nullptr), S_OK);

    ATL::CComBSTR actual;
    ASSERT_EQ(restoredControl->get_RenderingBackendPreference(&actual), S_OK);
    EXPECT_STREQ(actual, L"OpenGL");
}

TEST(ActiveXModule, ParsesDllInstallRegistrationScope)
{
    EXPECT_EQ(
        ParseDllInstallRegistrationScope(nullptr),
        DllInstallRegistrationScope::Machine);
    EXPECT_EQ(
        ParseDllInstallRegistrationScope(L""),
        DllInstallRegistrationScope::Machine);
    EXPECT_EQ(
        ParseDllInstallRegistrationScope(L"user"),
        DllInstallRegistrationScope::User);
    EXPECT_EQ(
        ParseDllInstallRegistrationScope(L"USER"),
        DllInstallRegistrationScope::User);
    EXPECT_EQ(
        ParseDllInstallRegistrationScope(L"machine"),
        DllInstallRegistrationScope::Invalid);
    EXPECT_EQ(
        ParseDllInstallRegistrationScope(L"user "),
        DllInstallRegistrationScope::Invalid);
}

TEST_F(ActiveXMapTest, ExtendedLoadRefitsAnExistingNavigationView)
{
    ASSERT_EQ(Resize(120, 100), S_OK);
    const std::vector<mwg::Extent> rectangles{{0, 0, 10, 10}};
    const auto path = mwg::test::MakeRectanglesDataset(
        _directory.Path(), "refit.gpkg", rectangles);
    ComPtr<IMapWinGISControl4> control;
    ASSERT_EQ(_control.As(&control), S_OK);
    LONGLONG id = 0;
    ASSERT_EQ(control->LoadVectorDatasetEx(ATL::CComBSTR(path.c_str()), &id), S_OK);
    BitmapSurface initial(120, 100);
    BitmapSurface zoomed(120, 100);
    BitmapSurface refitted(120, 100);
    ASSERT_TRUE(initial.IsValid());
    ASSERT_TRUE(zoomed.IsValid());
    ASSERT_TRUE(refitted.IsValid());
    ASSERT_EQ(Draw(initial.Get(), 120, 100), S_OK);
    ASSERT_EQ(control->ZoomIn(2), S_OK);
    ASSERT_EQ(Draw(zoomed.Get(), 120, 100), S_OK);
    ASSERT_TRUE(HasDifferentPixel(initial.Get(), zoomed.Get(), 120, 100));
    ASSERT_EQ(control->LoadVectorDatasetEx(ATL::CComBSTR(path.c_str()), &id), S_OK);
    ASSERT_EQ(Draw(refitted.Get(), 120, 100), S_OK);
    EXPECT_FALSE(HasDifferentPixel(initial.Get(), refitted.Get(), 120, 100));
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

TEST_F(ActiveXMapTest, LoadsLayersWhenTheDriverCannotReportAFeatureCount)
{
    _readDriver = std::make_unique<mwg::test::VectorReadTestDriver>();
    _readDriver->unknownCount = true;
    ASSERT_EQ(Resize(120, 100), S_OK);
    ASSERT_EQ(_control->LoadVectorDataset(
        ATL::CComBSTR(mwg::test::VectorReadTestDriver::kPath)), S_OK);

    LONGLONG count = -1;
    ASSERT_EQ(_control->get_FeatureCount(&count), S_OK);
    EXPECT_EQ(count, 3);
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), RGB(61, 122, 169));
}

TEST_F(ActiveXMapTest, ValidatedLoadsDoNotRereadGeometryOnTheFirstDraw)
{
    _readDriver = std::make_unique<mwg::test::VectorReadTestDriver>();
    ASSERT_EQ(Resize(120, 100), S_OK);
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    for (int load = 1; load <= 2; ++load)
    {
        ASSERT_EQ(_control->LoadVectorDataset(
            ATL::CComBSTR(mwg::test::VectorReadTestDriver::kPath)), S_OK);
        ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
        EXPECT_EQ(_readDriver->readCalls, 4 * load);
        EXPECT_EQ(GetPixel(surface.Get(), 60, 50), RGB(61, 122, 169));
    }

    _readDriver->failOnSecondRead = true;
    EXPECT_EQ(_control->LoadVectorDataset(
        ATL::CComBSTR(mwg::test::VectorReadTestDriver::kPath)), E_FAIL);
    LONGLONG count = -1;
    ASSERT_EQ(_control->get_FeatureCount(&count), S_OK);
    EXPECT_EQ(count, 6);
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    EXPECT_EQ(_readDriver->readCalls, 10);
    EXPECT_EQ(GetPixel(surface.Get(), 60, 50), RGB(61, 122, 169));
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

TEST_F(ActiveXMapTest, AlternateSizeDrawsScaleTheLiveVisibleExtent)
{
    ASSERT_EQ(Resize(120, 100), S_OK);
    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    mwg::Viewport live(120, 100);
    ASSERT_TRUE(live.FitExtent({0, 0, 10, 5}, 12.0));
    mwg::Viewport printed(240, 200);
    ASSERT_TRUE(printed.FitExtent(live.VisibleExtent()));

    BitmapSurface surface(240, 200);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 240, 200), S_OK);

    // At the screen resolution the rectangle would cover only the middle of
    // a double-size target, leaving its right part unpainted.
    for (const mwg::MapPoint point : {mwg::MapPoint{0.5, 2.5}, mwg::MapPoint{9.5, 2.5}})
    {
        const auto pixel = mwg::ToPixel(*printed.MapToScreen(point));
        ASSERT_TRUE(pixel.has_value());
        EXPECT_EQ(GetPixel(surface.Get(), pixel->x, pixel->y), RGB(61, 122, 169))
            << point.x << ", " << point.y;
    }
    const auto outside = mwg::ToPixel(*printed.MapToScreen({-0.5, 2.5}));
    ASSERT_TRUE(outside.has_value());
    EXPECT_EQ(GetPixel(surface.Get(), outside->x, outside->y), GetSysColor(COLOR_WINDOW));

    // The live view keeps its size and scale.
    using Access = mwg::activex::testing::MapWinGISControlTestAccess;
    EXPECT_DOUBLE_EQ(Access::Resolution(Access::Control(_control.Get())), *live.Resolution());
}

TEST_F(ActiveXMapTest, ScaledDrawsKeepScreenBuffersAtTheClientSize)
{
    using Access = mwg::activex::testing::MapWinGISControlTestAccess;
    ASSERT_EQ(Resize(120, 100), S_OK);
    LoadRectanglesWithFillMeshes({{0, 0, 10, 5}});
    ASSERT_EQ(_control->put_RenderingBackendPreference(ATL::CComBSTR(L"OpenGL")), S_OK);
    BitmapSurface surface(480, 401);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    ATL::CComBSTR backend;
    ASSERT_EQ(_control->get_RenderingBackend(&backend), S_OK);
    if (backend == nullptr || std::wcscmp(backend, L"OpenGL") != 0)
    {
        GTEST_SKIP() << "The control selected GDI for the OpenGL preference.";
    }
    const auto& control = Access::Control(_control.Get());
    const auto frameDc = Access::Frame(control).Get();
    const auto cacheDc = Access::LayerCache(control).Get();
    const auto capacity = Access::StagingCapacity(control);
    ASSERT_GT(capacity, 0U);
    ASSERT_TRUE(Access::CacheValid(control));
    ASSERT_EQ(Draw(surface.Get(), 480, 401), S_OK);
    EXPECT_EQ(Access::Frame(control).Width(), 120);
    EXPECT_EQ(Access::Frame(control).Height(), 100);
    EXPECT_EQ(Access::Frame(control).Get(), frameDc);
    EXPECT_EQ(Access::LayerCache(control).Width(), 120);
    EXPECT_EQ(Access::LayerCache(control).Height(), 100);
    EXPECT_EQ(Access::LayerCache(control).Get(), cacheDc);
    EXPECT_EQ(Access::StagingCapacity(control), capacity);
    EXPECT_TRUE(Access::CacheValid(control));
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    EXPECT_EQ(Access::Frame(control).Get(), frameDc);
    EXPECT_EQ(Access::StagingCapacity(control), capacity);
}

TEST_F(ActiveXMapTest, ClearLayersReleasesLazyFillMeshes)
{
    using Access = mwg::activex::testing::MapWinGISControlTestAccess;
    ASSERT_EQ(Resize(120, 100), S_OK);
    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    ASSERT_EQ(_control->put_RenderingBackendPreference(ATL::CComBSTR(L"OpenGL")), S_OK);

    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    ATL::CComBSTR backend;
    ASSERT_EQ(_control->get_RenderingBackend(&backend), S_OK);
    if (backend == nullptr || std::wcscmp(backend, L"OpenGL") != 0)
    {
        GTEST_SKIP() << "The control selected GDI for the OpenGL preference.";
    }

    const auto& control = Access::Control(_control.Get());
    EXPECT_GT(Access::FillMeshCacheSize(control), 0U);
    ASSERT_EQ(_control->ClearLayers(), S_OK);
    EXPECT_EQ(Access::FillMeshCacheSize(control), 0U);
}

TEST_F(ActiveXMapTest, ReactivatingTheControlRestartsFillMeshPreparation)
{
    using Access = mwg::activex::testing::MapWinGISControlTestAccess;
    ASSERT_EQ(Resize(120, 100), S_OK);
    ASSERT_EQ(
        _control->put_RenderingBackendPreference(
            ATL::CComBSTR(L"Automatic")),
        S_OK);
    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    CMapWinGISControl& control = Access::Control(_control.Get());
    ASSERT_TRUE(Access::FillMeshPreparationDispatchCreated(control));

    LRESULT result = 0;
    ASSERT_TRUE(SUCCEEDED(
        _window->OnWindowMessage(WM_DESTROY, 0, 0, &result)));
    EXPECT_FALSE(Access::FillMeshPreparationDispatchCreated(control));

    ASSERT_TRUE(SUCCEEDED(
        _window->OnWindowMessage(WM_CREATE, 0, 0, &result)));
    EXPECT_TRUE(Access::FillMeshPreparationDispatchCreated(control));
    EXPECT_EQ(Access::FillMeshCacheSize(control), 1U);
}

TEST_F(ActiveXMapTest, FillMeshCompletionBeforeTheFirstPaintWaitsForThatFrame)
{
    using Access = mwg::activex::testing::MapWinGISControlTestAccess;
    ASSERT_EQ(Resize(120, 100), S_OK);
    ASSERT_EQ(
        _control->put_RenderingBackendPreference(
            ATL::CComBSTR(L"Automatic")),
        S_OK);
    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    CMapWinGISControl& control = Access::Control(_control.Get());
    ASSERT_TRUE(Access::WaitForCompletedFillMeshPreparation(control));

    // The posted completion outranks the pending WM_PAINT; it must not
    // publish meshes or warm OpenGL before the first frame is presented.
    Access::DeliverFillMeshCompletion(control);
    EXPECT_TRUE(Access::FillMeshPublicationDeferred(control));
    EXPECT_TRUE(Access::HasCompletedFillMeshPreparation(control));

    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    ATL::CComBSTR backend;
    ASSERT_EQ(_control->get_RenderingBackend(&backend), S_OK);
    EXPECT_STREQ(backend, L"GDI");
    EXPECT_FALSE(Access::FillMeshPublicationDeferred(control));

    Access::DeliverFillMeshCompletion(control);
    EXPECT_FALSE(Access::FillMeshPublicationDeferred(control));
    EXPECT_FALSE(Access::HasCompletedFillMeshPreparation(control));
}

TEST_F(ActiveXMapTest, AnUndrawableFrameKeepsFillMeshPublicationDeferred)
{
    using Access = mwg::activex::testing::MapWinGISControlTestAccess;
    ASSERT_EQ(Resize(120, 100), S_OK);
    ASSERT_EQ(
        _control->put_RenderingBackendPreference(
            ATL::CComBSTR(L"Automatic")),
        S_OK);
    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    CMapWinGISControl& control = Access::Control(_control.Get());
    ASSERT_TRUE(Access::WaitForCompletedFillMeshPreparation(control));
    Access::DeliverFillMeshCompletion(control);
    ASSERT_TRUE(Access::FillMeshPublicationDeferred(control));

    ASSERT_EQ(Resize(0, 0), S_OK);
    BitmapSurface surface(1, 1);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 0, 0), S_OK);
    EXPECT_TRUE(Access::FillMeshPublicationDeferred(control));
    EXPECT_TRUE(Access::HasCompletedFillMeshPreparation(control));
}

TEST_F(ActiveXMapTest, DragDrawBlitsWholePixelShiftsAndRendersFractionalOrHugeShifts)
{
    using Access = mwg::activex::testing::MapWinGISControlTestAccess;
    ASSERT_EQ(Resize(120, 100), S_OK);
    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    auto& control = Access::Control(_control.Get());
    const auto initial = Access::CachedCenter(control);
    ASSERT_TRUE(Access::PanDuringDrag(control, 4.0));
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    EXPECT_DOUBLE_EQ(Access::CachedCenter(control).x, initial.x);
    ASSERT_TRUE(Access::PanDuringDrag(control, 0.25));
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    EXPECT_NE(Access::CachedCenter(control).x, initial.x);
    const auto fractional = Access::CachedCenter(control);
    ASSERT_TRUE(Access::PanDuringDrag(control, 120.0));
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    EXPECT_NE(Access::CachedCenter(control).x, fractional.x);
    ASSERT_TRUE(Access::PanDuringDrag(control, 1e12));
    // The blit rejects this shift before integer conversion. GDI then
    // reports its existing coordinate-range failure instead of caching it.
    EXPECT_EQ(Draw(surface.Get(), 120, 100), E_INVALIDARG);
    EXPECT_FALSE(Access::CacheValid(control));
}

TEST_F(ActiveXMapTest, FailedResizeInvalidatesTheCacheAndNotifiesTheHost)
{
    using Access = mwg::activex::testing::MapWinGISControlTestAccess;
    class ViewChangeSink final :
        public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IAdviseSink>
    {
    public:
        void STDMETHODCALLTYPE OnDataChange(FORMATETC*, STGMEDIUM*) override {}
        void STDMETHODCALLTYPE OnViewChange(DWORD, LONG) override { ++changes; }
        void STDMETHODCALLTYPE OnRename(IMoniker*) override {}
        void STDMETHODCALLTYPE OnSave() override {}
        void STDMETHODCALLTYPE OnClose() override {}
        int changes = 0;
    };

    ASSERT_EQ(Resize(120, 100), S_OK);
    ASSERT_EQ(LoadRectangles({{0, 0, 10, 5}}), S_OK);
    BitmapSurface surface(120, 100);
    ASSERT_TRUE(surface.IsValid());
    ASSERT_EQ(Draw(surface.Get(), 120, 100), S_OK);
    auto& control = Access::Control(_control.Get());
    ASSERT_TRUE(Access::CacheValid(control));
    _readDriver = std::make_unique<mwg::test::VectorReadTestDriver>();
    Access::StagePendingFit(control, mwg::VectorDataset::Open(mwg::test::VectorReadTestDriver::kPath));
    _readDriver->failExtent = true;
    const auto sink = Microsoft::WRL::Make<ViewChangeSink>();
    ASSERT_NE(sink, nullptr);
    ASSERT_EQ(_viewObject->SetAdvise(DVASPECT_CONTENT, 0, sink.Get()), S_OK);
    const mwg::GdalEnvironment::QuietScope quiet;
    ASSERT_EQ(Resize(200, 120), S_OK);
    EXPECT_GT(_readDriver->extentCalls, 0);
    EXPECT_FALSE(Access::CacheValid(control));
    EXPECT_EQ(sink->changes, 1);
    ASSERT_EQ(_viewObject->SetAdvise(DVASPECT_CONTENT, 0, nullptr), S_OK);
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

    bool openGlUnavailable = false;
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

            NestedDispatchBarrier nestedBarrier;
            struct NestedDispatchCleanup final
            {
                GdalEventSink& eventSink;
                ~NestedDispatchCleanup()
                {
                    eventSink.SetProgressCallback(nullptr, nullptr);
                    nestedDispatchBarrier.store(
                        nullptr,
                        std::memory_order_release);
                }
            } nestedDispatchCleanup{*eventSink.Get()};
            static_cast<void>(nestedDispatchCleanup);
            nestedDispatchBarrier.store(
                &nestedBarrier,
                std::memory_order_release);
            eventSink->SetProgressCallback(
                &PumpNestedMessages,
                &nestedBarrier);
            LONGLONG nestedPumpOperationId = 0;
            ASSERT_TRUE(SUCCEEDED(StartGdalOperation(
                gdalControl,
                L"mwg-test-activex nested-progress",
                {},
                &nestedPumpOperationId)));
            nestedBarrier.MarkStarted();
            const HWND dispatchWindow = ::FindWindowExW(
                HWND_MESSAGE,
                nullptr,
                L"MapWinGIS6.GdalDispatchWindow",
                nullptr);
            ASSERT_NE(dispatchWindow, nullptr);
            ASSERT_TRUE(::KillTimer(
                dispatchWindow,
                mwg::activex::kGdalDispatchRetryTimerId));
            ComGdalResult nestedPumpResult;
            ASSERT_TRUE(SUCCEEDED(finishOperation(
                nestedPumpOperationId,
                nestedPumpResult)));
            EXPECT_EQ(nestedPumpResult.succeeded, VARIANT_TRUE);
            EXPECT_EQ(nestedPumpResult.callbackErrorCode, 0);
            EXPECT_EQ(
                eventSink->ProgressCount(nestedPumpOperationId),
                2u);
            EXPECT_EQ(nestedBarrier.NestedDispatchCount(), 1);

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

        if (iteration == 0)
        {
            ATL::CComQIPtr<IMapWinGISControl3> rendererControl(hostedControl);
            ASSERT_NE(rendererControl.p, nullptr);
            mwg::test::TempDirectory renderDirectory;
            const std::vector<mwg::Extent> renderRectangles{{0, 0, 10, 5}};
            const std::filesystem::path renderDataset =
                mwg::test::MakeRectanglesDataset(
                    renderDirectory.Path(),
                    "registered-render.gpkg",
                    renderRectangles);
            const std::wstring renderDatasetText = renderDataset.wstring();
            ATL::CComBSTR renderDatasetBstr(renderDatasetText.c_str());
            ASSERT_EQ(rendererControl->LoadVectorDataset(renderDatasetBstr), S_OK);

            const RECT rendererRect{0, 0, resizedWidth, resizedHeight};
            const RECTL rendererBounds{0, 0, resizedWidth, resizedHeight};
            ATL::CComBSTR gdiPreference(L"GDI");
            ASSERT_EQ(
                rendererControl->put_RenderingBackendPreference(gdiPreference),
                S_OK);
            ASSERT_NE(FillRect(
                resizedSurface.Get(),
                &rendererRect,
                GetSysColorBrush(COLOR_WINDOW)), 0);
            ASSERT_TRUE(SUCCEEDED(viewObject->Draw(
                DVASPECT_CONTENT,
                -1,
                nullptr,
                nullptr,
                nullptr,
                resizedSurface.Get(),
                &rendererBounds,
                nullptr,
                nullptr,
                0)));
            ATL::CComBSTR gdiBackend;
            ASSERT_TRUE(SUCCEEDED(rendererControl->get_RenderingBackend(&gdiBackend)));
            EXPECT_STREQ(gdiBackend, L"GDI");
            EXPECT_EQ(
                GetPixel(resizedSurface.Get(), resizedWidth / 2, resizedHeight / 2),
                RGB(61, 122, 169));

            ATL::CComQIPtr<IOleInPlaceObjectWindowless> inputControl(hostedControl);
            ASSERT_NE(inputControl.p, nullptr);
            ATL::CComQIPtr<IMapWinGISControl4> navigationControl(hostedControl);
            ASSERT_NE(navigationControl.p, nullptr);
            BitmapSurface initialNavigationSurface(resizedWidth, resizedHeight);
            BitmapSurface wheelNavigationSurface(resizedWidth, resizedHeight);
            BitmapSurface panNavigationSurface(resizedWidth, resizedHeight);
            BitmapSurface expectedNavigationSurface(resizedWidth, resizedHeight);
            ASSERT_TRUE(initialNavigationSurface.IsValid());
            ASSERT_TRUE(wheelNavigationSurface.IsValid());
            ASSERT_TRUE(panNavigationSurface.IsValid());
            ASSERT_TRUE(expectedNavigationSurface.IsValid());
            ASSERT_EQ(navigationControl->FitToMap(), S_OK);
            ASSERT_TRUE(SUCCEEDED(viewObject->Draw(
                DVASPECT_CONTENT,
                -1,
                nullptr,
                nullptr,
                nullptr,
                initialNavigationSurface.Get(),
                &rendererBounds,
                nullptr,
                nullptr,
                0)));

            const int anchorX = resizedWidth / 3;
            const int anchorY = resizedHeight / 3;
            POINT wheelPoint{
                resizedBounds.left + anchorX,
                resizedBounds.top + anchorY};
            ASSERT_TRUE(ClientToScreen(hostWindow.Get(), &wheelPoint));
            LRESULT inputResult = 0;
            ASSERT_EQ(
                inputControl->OnWindowMessage(
                    WM_MOUSEWHEEL,
                    MAKEWPARAM(0, WHEEL_DELTA),
                    MAKELPARAM(wheelPoint.x, wheelPoint.y),
                    &inputResult),
                S_OK);
            ASSERT_TRUE(SUCCEEDED(viewObject->Draw(
                DVASPECT_CONTENT,
                -1,
                nullptr,
                nullptr,
                nullptr,
                wheelNavigationSurface.Get(),
                &rendererBounds,
                nullptr,
                nullptr,
                0)));
            EXPECT_TRUE(HasDifferentPixel(
                initialNavigationSurface.Get(),
                wheelNavigationSurface.Get(),
                resizedWidth,
                resizedHeight));

            HWND controlWindow = nullptr;
            const HRESULT getWindowResult = inputControl->GetWindow(&controlWindow);
            POINT dragStart{anchorX, anchorY};
            if (getWindowResult == E_FAIL)
            {
                dragStart.x += resizedBounds.left;
                dragStart.y += resizedBounds.top;
            }
            else
            {
                ASSERT_TRUE(SUCCEEDED(getWindowResult));
                ASSERT_NE(controlWindow, nullptr);
            }

            constexpr int dragDeltaX = 24;
            constexpr int dragDeltaY = 12;
            ASSERT_EQ(
                inputControl->OnWindowMessage(
                    WM_LBUTTONDOWN,
                    MK_LBUTTON,
                    MAKELPARAM(dragStart.x, dragStart.y),
                    &inputResult),
                S_OK);
            ASSERT_EQ(
                inputControl->OnWindowMessage(
                    WM_MOUSEMOVE,
                    MK_LBUTTON,
                    MAKELPARAM(
                        dragStart.x + dragDeltaX,
                        dragStart.y + dragDeltaY),
                    &inputResult),
                S_OK);
            ASSERT_EQ(
                inputControl->OnWindowMessage(
                    WM_LBUTTONUP,
                    0,
                    MAKELPARAM(
                        dragStart.x + dragDeltaX,
                        dragStart.y + dragDeltaY),
                    &inputResult),
                S_OK);
            ASSERT_TRUE(SUCCEEDED(viewObject->Draw(
                DVASPECT_CONTENT,
                -1,
                nullptr,
                nullptr,
                nullptr,
                panNavigationSurface.Get(),
                &rendererBounds,
                nullptr,
                nullptr,
                0)));
            EXPECT_TRUE(HasDifferentPixel(
                wheelNavigationSurface.Get(),
                panNavigationSurface.Get(),
                resizedWidth,
                resizedHeight));

            mwg::Viewport expectedNavigation(resizedWidth, resizedHeight);
            ASSERT_TRUE(expectedNavigation.FitExtent({0, 0, 10, 5}, 12.0));
            ASSERT_TRUE(expectedNavigation.ZoomAt(
                {static_cast<double>(anchorX), static_cast<double>(anchorY)},
                1.2));
            const std::optional<double> resolutionAfterWheel =
                expectedNavigation.Resolution();
            ASSERT_TRUE(resolutionAfterWheel.has_value());
            ASSERT_TRUE(expectedNavigation.Pan(
                -dragDeltaX * *resolutionAfterWheel,
                dragDeltaY * *resolutionAfterWheel));
            ASSERT_EQ(navigationControl->FitToMap(), S_OK);
            ASSERT_EQ(
                navigationControl->ZoomAt(anchorX, anchorY, 1.2),
                S_OK);
            ASSERT_EQ(
                navigationControl->Pan(
                    -dragDeltaX * *resolutionAfterWheel,
                    dragDeltaY * *resolutionAfterWheel),
                S_OK);
            ASSERT_TRUE(SUCCEEDED(viewObject->Draw(
                DVASPECT_CONTENT,
                -1,
                nullptr,
                nullptr,
                nullptr,
                expectedNavigationSurface.Get(),
                &rendererBounds,
                nullptr,
                nullptr,
                0)));
            EXPECT_FALSE(HasDifferentPixel(
                panNavigationSurface.Get(),
                expectedNavigationSurface.Get(),
                resizedWidth,
                resizedHeight));
            const auto expectedFeaturePixel =
                mwg::ToPixel(*expectedNavigation.MapToScreen({1, 2.5}));
            ASSERT_TRUE(expectedFeaturePixel.has_value());
            EXPECT_EQ(
                GetPixel(
                    panNavigationSurface.Get(),
                    expectedFeaturePixel->x,
                    expectedFeaturePixel->y),
                RGB(61, 122, 169));
            ASSERT_EQ(navigationControl->FitToMap(), S_OK);

            ATL::CComBSTR openGlPreference(L"OpenGL");
            ASSERT_EQ(
                rendererControl->put_RenderingBackendPreference(openGlPreference),
                S_OK);
            ASSERT_NE(FillRect(
                resizedSurface.Get(),
                &rendererRect,
                GetSysColorBrush(COLOR_WINDOW)), 0);
            ASSERT_TRUE(SUCCEEDED(viewObject->Draw(
                DVASPECT_CONTENT,
                -1,
                nullptr,
                nullptr,
                nullptr,
                resizedSurface.Get(),
                &rendererBounds,
                nullptr,
                nullptr,
                0)));
            ATL::CComBSTR openGlBackend;
            ASSERT_TRUE(SUCCEEDED(
                rendererControl->get_RenderingBackend(&openGlBackend)));
            if (openGlBackend == nullptr ||
                std::wcscmp(openGlBackend, L"OpenGL") != 0)
            {
                openGlUnavailable = true;
            }
            else
            {
                EXPECT_EQ(
                    GetPixel(
                        resizedSurface.Get(),
                        resizedWidth / 2,
                        resizedHeight / 2),
                    RGB(61, 122, 169));
            }
        }

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

    if (openGlUnavailable)
    {
        GTEST_SKIP() << "The registered control selected GDI for the OpenGL preference.";
    }
}
