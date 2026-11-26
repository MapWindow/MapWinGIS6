// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.cpp
 * @brief Implements vector loading, map navigation, ActiveX state reporting,
 * and host drawing.
 *
 * Redraws: every map or view change made through the control goes through
 * its MapView; the control calls FireViewChange when the MapView reports
 * that the frame changed. Loaded scenes queue background fill-mesh work and
 * publish completion through a message-only dispatch window. Publication and
 * OpenGL target warm-up wait until a pending layer change has been presented.
 * OnDraw composes
 * the MapView's render scene through its viewport into an offscreen frame
 * buffer and presents the finished
 * frame with a single blit, so every layer uses the same transformation and
 * the visible surface never shows a partially drawn frame.
 *
 * Pan caching: the last fully rendered layer frame is kept in a second
 * offscreen buffer. While the user drags the map (WM_LBUTTONDOWN through
 * WM_LBUTTONUP), OnDraw paints the background and blits the cached frame at
 * the accumulated pixel offset instead of re-rasterizing every layer on
 * each WM_MOUSEMOVE; the drag-end handler invalidates the cache so the new
 * view is rasterized once. The cache is invalidated on layer add/remove/
 * clear, zoom, resize, and backend-preference changes.
 */
#include "MapWinGISControl.h"

#include <atlconv.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windowsx.h>

#include "ComConversion.h"
#include "PanBlit.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/vector/VectorDataset.h"

OBJECT_ENTRY_AUTO(CLSID_MapWinGISControl, CMapWinGISControl)

namespace
{
    constexpr double kMouseWheelZoomFactor = 1.2;
    using VectorPreparationHook = void(*)(void*, bool) noexcept;
    std::atomic<VectorPreparationHook> g_vectorPreparationHook{nullptr};
    std::atomic<void*> g_vectorPreparationContext{nullptr};

    using mwg::activex::ComConversion::BstrToUtf8;
    using mwg::activex::ComConversion::CopyStringArray;
    using mwg::activex::ComConversion::CopyToBstr;
    using mwg::activex::ComConversion::CreateDiagnosticStringArray;
    using mwg::activex::ComConversion::DiagnosticToBstr;
    using mwg::activex::ComConversion::Utf8ToBstr;

    [[nodiscard]] HRESULT SetControlComErrorInfo(
        const char* message,
        REFIID sourceIid,
        const HRESULT errorCode) noexcept
    {
        try
        {
            const CA2W wideMessage(message, CP_UTF8);
            return AtlReportError(CLSID_MapWinGISControl, wideMessage, sourceIid, errorCode);
        }
        catch (...)
        {
            return errorCode;
        }
    }

    [[nodiscard]] HRESULT SetComErrorInfo(
        const char* message,
        const HRESULT errorCode) noexcept
    {
        return SetControlComErrorInfo(message, IID_IMapWinGISControlGdal, errorCode);
    }

    [[nodiscard]] HRESULT CopyLayerId(const mwg::LayerId id, LONGLONG* destination) noexcept
    {
        if (id.value > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
        {
            return E_UNEXPECTED;
        }
        *destination = static_cast<LONGLONG>(id.value);
        return S_OK;
    }

    struct PreparedVectorDataset final
    {
        std::shared_ptr<mwg::VectorDataset> dataset;
        std::shared_ptr<const mwg::rendering::VectorRenderScene> scene;
        LONGLONG featureCount = 0;
    };

    [[nodiscard]] PreparedVectorDataset PrepareVectorDataset(
        const std::filesystem::path& datasetPath)
    {
        std::shared_ptr<mwg::VectorDataset> dataset =
            mwg::VectorDataset::Open(datasetPath);
        if (dataset->LayerCount() == 0)
        {
            throw mwg::CoreError("Vector dataset contains no layers");
        }

        mwg::Map candidate;
        candidate.AddLayer(dataset);
        mwg::rendering::MapRenderScene scene =
            mwg::rendering::CreateMapRenderScene(candidate);
        for (const mwg::rendering::SkippedLayer& skipped : scene.skippedLayers)
        {
            if (skipped.reason == mwg::rendering::LayerSkipReason::Failed)
            {
                throw mwg::CoreError(skipped.message);
            }
        }

        std::shared_ptr<const mwg::rendering::VectorRenderScene> preparedScene;
        if (scene.layers.empty())
        {
            auto emptyScene = std::make_shared<mwg::rendering::VectorRenderScene>();
            emptyScene->sourceFeatureCount = dataset->Layer(0).FeatureCount();
            preparedScene = std::move(emptyScene);
        }
        else
        {
            // The candidate map is discarded, so this is the only owner of the
            // geometry; MapView stores the same snapshot without copying it.
            preparedScene = std::move(scene.layers.front().scene);
        }

        if (preparedScene == nullptr)
        {
            throw mwg::CoreError("Layer render scene is missing");
        }
        if (std::cmp_greater(
                preparedScene->sourceFeatureCount,
                std::numeric_limits<LONGLONG>::max()))
        {
            throw mwg::CoreError("Map feature count is out of range");
        }
        const LONGLONG count =
            static_cast<LONGLONG>(preparedScene->sourceFeatureCount);
        return {std::move(dataset), std::move(preparedScene), count};
    }

    template <typename Operation>
    [[nodiscard]] HRESULT RunViewOperation(
        Operation&& operation,
        bool& changed)
    {
        try
        {
            changed = std::forward<Operation>(operation)();
            return S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return SetControlComErrorInfo("Out of memory while updating the map view.", IID_IMapWinGISControl4, E_OUTOFMEMORY);
        }
        catch (const mwg::InvalidArgument& exception)
        {
            return SetControlComErrorInfo(
                exception.what(),
                IID_IMapWinGISControl4,
                E_INVALIDARG);
        }
        catch (const std::exception& exception)
        {
            return SetControlComErrorInfo(
                exception.what(),
                IID_IMapWinGISControl4,
                E_FAIL);
        }
        catch (...)
        {
            return SetControlComErrorInfo("An unexpected error occurred while updating the map view.", IID_IMapWinGISControl4, E_UNEXPECTED);
        }
    }

    [[nodiscard]] const wchar_t* BackendName(
        const std::optional<mwg::activex::VectorRenderBackendKind> backend) noexcept
    {
        if (!backend.has_value())
        {
            return L"Not rendered";
        }

        return *backend == mwg::activex::VectorRenderBackendKind::OpenGL
            ? L"OpenGL"
            : L"GDI";
    }

    [[nodiscard]] const wchar_t* PreferenceName(
        const mwg::activex::VectorRenderBackendPreference preference) noexcept
    {
        switch (preference)
        {
        case mwg::activex::VectorRenderBackendPreference::Automatic:
            return L"Automatic";
        case mwg::activex::VectorRenderBackendPreference::OpenGL:
            return L"OpenGL";
        case mwg::activex::VectorRenderBackendPreference::Gdi:
            return L"GDI";
        default:
            return L"Automatic";
        }
    }
}

#if defined(MWG_ACTIVEX_TEST_HOOKS)
// Test-only export (MWG_ENABLE_ACTIVEX_TEST_HOOKS / MwgEnableActiveXTestHooks).
// Distributed binaries are built without it, so the hook stays null there.
extern "C" __declspec(dllexport) void __cdecl
MapWinGISControlSetVectorPreparationHookForTesting(
    void(*const hook)(void*, bool) noexcept,
    void* const context) noexcept
{
    g_vectorPreparationContext.store(context, std::memory_order_relaxed);
    g_vectorPreparationHook.store(hook, std::memory_order_release);
}
#endif

struct CMapWinGISControl::VectorLoadState final
{
    std::shared_ptr<mwg::VectorDataset> dataset;
    std::shared_ptr<const mwg::rendering::VectorRenderScene> scene;
    LONGLONG featureCount = 0;
    LONGLONG layerId = 0;
    bool cancelRequestedOnOwner = false;
    bool committed = false;
    bool ownerFinalized = false;
};

CMapWinGISControl::CMapWinGISControl()
    : _gdalEvents(
          m_vec,
          &CMapWinGISControl::DispatchGdalNotifications,
          this)
{
    _gdalEvents.SetCompletionCallback(
        &CMapWinGISControl::CompleteVectorLoad,
        this);
}

STDMETHODIMP CMapWinGISControl::Load(IStream* stream)
{
    if (stream != nullptr)
    {
        LARGE_INTEGER offset{};
        ULARGE_INTEGER position{};
        STATSTG stat{};
        if (SUCCEEDED(stream->Seek(offset, STREAM_SEEK_CUR, &position)) &&
            SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) &&
            stat.cbSize.QuadPart >= position.QuadPart &&
            stat.cbSize.QuadPart - position.QuadPart == sizeof(DWORD))
        {
            // Before the property map had entries, ATL saved only its version.
            static const ATL::ATL_PROPMAP_ENTRY emptyMap[] = {{}};
            return IPersistStreamInit_Load(stream, emptyMap);
        }
    }

    return ATL::IPersistStreamInitImpl<CMapWinGISControl>::Load(stream);
}

STDMETHODIMP CMapWinGISControl::InterfaceSupportsErrorInfo(REFIID interfaceId)
{
    return IsEqualIID(interfaceId, IID_IMapWinGISControl) ||
        IsEqualIID(interfaceId, IID_IMapWinGISControl2) ||
        IsEqualIID(interfaceId, IID_IMapWinGISControl3) ||
        IsEqualIID(interfaceId, IID_IMapWinGISControl4) ||
        IsEqualIID(interfaceId, IID_IMapWinGISControl5) ||
        IsEqualIID(interfaceId, IID_IMapWinGISControlGdal) ? S_OK : S_FALSE;
}

STDMETHODIMP CMapWinGISControl::BeginGdalOperation(
    BSTR algorithmPath,
    SAFEARRAY* arguments,
    LONGLONG* operationId)
{
    if (operationId == nullptr)
    {
        return SetComErrorInfo("The operation ID output pointer is null.", E_POINTER);
    }
    *operationId = 0;
    const HRESULT dispatchWindowResult = _gdalEvents.EnsureDispatchWindow();
    if (FAILED(dispatchWindowResult))
    {
        return SetComErrorInfo("The GDAL notification window could not be created.", dispatchWindowResult);
    }

    std::string algorithm;
    std::vector<std::string> commandLineArguments;
    HRESULT result = S_OK;
    try
    {
        result = BstrToUtf8(algorithmPath, algorithm);
        if (FAILED(result) || algorithm.empty())
        {
            return SetComErrorInfo("The GDAL algorithm path is null or empty.", E_INVALIDARG);
        }

        result = CopyStringArray(arguments, commandLineArguments);
        if (FAILED(result))
        {
            return SetComErrorInfo("The GDAL argument array is invalid.", result);
        }
    }
    catch (const std::bad_alloc&)
    {
        return SetComErrorInfo("Out of memory while preparing the GDAL operation.", E_OUTOFMEMORY);
    }
    catch (const std::exception& exception)
    {
        return SetComErrorInfo(exception.what(), E_INVALIDARG);
    }
    catch (...)
    {
        return SetComErrorInfo("An unexpected error occurred while preparing the GDAL operation.", E_UNEXPECTED);
    }

    try
    {
        *operationId = static_cast<LONGLONG>(
            _gdalEvents.Begin(
                std::move(algorithm),
                std::move(commandLineArguments)));
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return SetComErrorInfo("Out of memory while starting the GDAL operation.", E_OUTOFMEMORY);
    }
    catch (const std::exception& exception)
    {
        return SetComErrorInfo(exception.what(), E_FAIL);
    }
    catch (...)
    {
        return SetComErrorInfo("An unexpected error occurred while starting the GDAL operation.", E_UNEXPECTED);
    }
}

STDMETHODIMP CMapWinGISControl::CancelGdalOperation(const LONGLONG operationId)
{
    if (operationId <= 0)
    {
        return SetComErrorInfo("The GDAL operation ID must be positive.", E_INVALIDARG);
    }
    if (_vectorLoadOperations.contains(
            static_cast<std::uint64_t>(operationId)))
    {
        return SetComErrorInfo(
            "The operation ID belongs to an asynchronous vector load.",
            E_INVALIDARG);
    }

    switch (_gdalEvents.Cancel(static_cast<std::uint64_t>(operationId)))
    {
    case mwg::activex::GdalCancelResult::Requested:
        return S_OK;
    case mwg::activex::GdalCancelResult::AlreadyRequested:
    case mwg::activex::GdalCancelResult::Completed:
        return S_FALSE;
    case mwg::activex::GdalCancelResult::NotFound:
    default:
        return SetComErrorInfo("The GDAL operation ID was not found.", E_INVALIDARG);
    }
}

STDMETHODIMP CMapWinGISControl::EndGdalOperation(
    const LONGLONG operationId, VARIANT_BOOL* succeeded, VARIANT_BOOL* cancelled,
    LONG* errorCode, LONG* callbackErrorCode, BSTR* callbackError,
    BSTR* summary, BSTR* diagnostics, SAFEARRAY** warnings)
{
    if (succeeded == nullptr || cancelled == nullptr || errorCode == nullptr ||
        callbackErrorCode == nullptr || callbackError == nullptr ||
        summary == nullptr || diagnostics == nullptr || warnings == nullptr)
        return SetComErrorInfo("One or more GDAL result output pointers are null.", E_POINTER);
    *succeeded = VARIANT_FALSE;
    *cancelled = VARIANT_FALSE;
    *errorCode = 0;
    *callbackErrorCode = 0;
    *callbackError = nullptr;
    *summary = nullptr;
    *diagnostics = nullptr;
    *warnings = nullptr;
    if (operationId <= 0)
        return SetComErrorInfo("The GDAL operation ID must be positive.", E_INVALIDARG);
    if (_vectorLoadOperations.contains(
            static_cast<std::uint64_t>(operationId)))
    {
        return SetComErrorInfo(
            "The operation ID belongs to an asynchronous vector load.",
            E_INVALIDARG);
    }

    mwg::activex::GdalOperationLookup ended;
    try
    {
        // Remove the operation slot before any COM allocations or string conversions.
        ended = _gdalEvents.End(static_cast<std::uint64_t>(operationId));
    }
    catch (const std::bad_alloc&)
    {
        return SetComErrorInfo("Out of memory while retrieving the GDAL operation result.", E_OUTOFMEMORY);
    }
    catch (const std::exception& exception)
    {
        return SetComErrorInfo(exception.what(), E_FAIL);
    }
    catch (...)
    {
        return SetComErrorInfo("An unexpected error occurred while retrieving the GDAL operation result.", E_UNEXPECTED);
    }

    if (ended.status == mwg::activex::GdalEndResult::Pending)
        return SetComErrorInfo("The GDAL operation has not finished or is still delivering events.", E_PENDING);
    if (ended.status != mwg::activex::GdalEndResult::Ended || !ended.result.has_value())
        return SetComErrorInfo("The GDAL operation ID was not found.", E_INVALIDARG);

    const auto& value = *ended.result;
    BSTR callbackText = nullptr;
    BSTR summaryText = nullptr;
    BSTR diagnosticText = nullptr;
    SAFEARRAY* warningArray = nullptr;
    HRESULT result = DiagnosticToBstr(value.callbackError, &callbackText);
    if (SUCCEEDED(result)) result = DiagnosticToBstr(value.summary, &summaryText);
    if (SUCCEEDED(result)) result = DiagnosticToBstr(value.diagnostics, &diagnosticText);
    if (SUCCEEDED(result)) result = CreateDiagnosticStringArray(value.warnings, &warningArray);
    if (FAILED(result))
    {
        ::SysFreeString(callbackText);
        ::SysFreeString(summaryText);
        ::SysFreeString(diagnosticText);
        if (warningArray != nullptr) ::SafeArrayDestroy(warningArray);
        return SetComErrorInfo("The completed GDAL result could not be marshalled to COM.", result);
    }
    *succeeded = value.succeeded ? VARIANT_TRUE : VARIANT_FALSE;
    *cancelled = value.cancelled ? VARIANT_TRUE : VARIANT_FALSE;
    *errorCode = value.errorCode;
    *callbackErrorCode = value.callbackErrorCode;
    *callbackError = callbackText;
    *summary = summaryText;
    *diagnostics = diagnosticText;
    *warnings = warningArray;
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::get_CoreInitialized(VARIANT_BOOL* initialized)
{
    if (initialized == nullptr)
    {
        return SetControlComErrorInfo("The initialized output pointer is null.", IID_IMapWinGISControl, E_POINTER);
    }

    *initialized = mwg::GdalEnvironment::IsInitialized() ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::LoadVectorDataset(BSTR path)
{
    return LoadVectorDatasetImpl(path, nullptr, IID_IMapWinGISControl);
}

STDMETHODIMP CMapWinGISControl::LoadVectorDatasetEx(
    BSTR path,
    LONGLONG* layerId)
{
    if (layerId == nullptr)
    {
        return SetControlComErrorInfo("The layer ID output pointer is null.", IID_IMapWinGISControl4, E_POINTER);
    }
    *layerId = 0;
    return LoadVectorDatasetImpl(path, layerId, IID_IMapWinGISControl4);
}

STDMETHODIMP CMapWinGISControl::BeginLoadVectorDataset(
    BSTR path,
    LONGLONG* operationId)
{
    if (operationId == nullptr)
    {
        return SetControlComErrorInfo(
            "The operation ID output pointer is null.",
            IID_IMapWinGISControl5,
            E_POINTER);
    }
    *operationId = 0;
    if (path == nullptr)
    {
        return SetControlComErrorInfo(
            "The dataset path is null.",
            IID_IMapWinGISControl5,
            E_INVALIDARG);
    }

    const UINT pathLength = ::SysStringLen(path);
    if (pathLength == 0)
    {
        return SetControlComErrorInfo(
            "The dataset path is empty.",
            IID_IMapWinGISControl5,
            E_INVALIDARG);
    }

    try
    {
        const std::wstring pathText(path, static_cast<std::size_t>(pathLength));
        if (pathText.find(L'\0') != std::wstring::npos)
        {
            return SetControlComErrorInfo(
                "The dataset path contains an embedded null character.",
                IID_IMapWinGISControl5,
                E_INVALIDARG);
        }

        const HRESULT windowResult = _gdalEvents.EnsureDispatchWindow();
        if (FAILED(windowResult))
        {
            return SetControlComErrorInfo(
                "The GDAL notification window could not be created.",
                IID_IMapWinGISControl5,
                windowResult);
        }

        auto state = std::make_shared<VectorLoadState>();
        _vectorLoadOperations.reserve(_vectorLoadOperations.size() + 1);
        const auto [placeholder, inserted] =
            _vectorLoadOperations.emplace(0, state);
        if (!inserted)
        {
            return SetControlComErrorInfo(
                "A vector dataset load is already being prepared.",
                IID_IMapWinGISControl5,
                E_UNEXPECTED);
        }

        std::uint64_t id = 0;
        try
        {
            const std::filesystem::path datasetPath(pathText);
            id = _gdalEvents.BeginWork(
                [state, datasetPath](
                    mwg::GdalCancellationToken& cancellation,
                    const mwg::activex::GdalOperationDispatcher::ProgressCallback&)
                {
                    mwg::activex::GdalOperationSnapshot result;
                    if (cancellation.IsCancellationRequested())
                    {
                        result.cancelled = true;
                        result.summary = "Vector dataset loading was cancelled.";
                        return result;
                    }

                    try
                    {
                        const auto preparationHook =
                            g_vectorPreparationHook.load(std::memory_order_acquire);
                        void* const preparationContext =
                            g_vectorPreparationContext.load(std::memory_order_relaxed);
                        struct PreparationFinishedNotification final
                        {
                            VectorPreparationHook hook;
                            void* context;

                            ~PreparationFinishedNotification()
                            {
                                if (hook != nullptr)
                                {
                                    hook(context, true);
                                }
                            }
                        } preparationFinished{preparationHook, preparationContext};
                        if (preparationHook != nullptr)
                        {
                            preparationHook(preparationContext, false);
                        }

                        PreparedVectorDataset prepared =
                            PrepareVectorDataset(datasetPath);
                        if (cancellation.IsCancellationRequested())
                        {
                            result.cancelled = true;
                            result.summary =
                                "Vector dataset loading was cancelled.";
                            return result;
                        }

                        state->dataset = std::move(prepared.dataset);
                        state->scene = std::move(prepared.scene);
                        state->featureCount = prepared.featureCount;
                        result.succeeded = true;
                        result.summary = "Vector dataset loaded.";
                        return result;
                    }
                    catch (const std::bad_alloc&)
                    {
                        throw;
                    }
                    catch (const std::exception& exception)
                    {
                        result.summary = exception.what();
                        result.diagnostics = result.summary;
                        return result;
                    }
                    catch (...)
                    {
                        result.summary =
                            "An unexpected error occurred while loading the vector dataset.";
                        result.diagnostics = result.summary;
                        return result;
                    }
                });
        }
        catch (...)
        {
            _vectorLoadOperations.erase(placeholder);
            throw;
        }

        auto operation = _vectorLoadOperations.extract(placeholder);
        operation.key() = id;
        _vectorLoadOperations.insert(std::move(operation));
        *operationId = static_cast<LONGLONG>(id);
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return SetControlComErrorInfo(
            "Out of memory while starting the vector dataset load.",
            IID_IMapWinGISControl5,
            E_OUTOFMEMORY);
    }
    catch (const std::exception& exception)
    {
        return SetControlComErrorInfo(
            exception.what(),
            IID_IMapWinGISControl5,
            E_FAIL);
    }
    catch (...)
    {
        return SetControlComErrorInfo(
            "An unexpected error occurred while starting the vector dataset load.",
            IID_IMapWinGISControl5,
            E_UNEXPECTED);
    }
}

STDMETHODIMP CMapWinGISControl::CancelLoadVectorDataset(
    const LONGLONG operationId)
{
    if (operationId <= 0)
    {
        return SetControlComErrorInfo(
            "The vector load operation ID must be positive.",
            IID_IMapWinGISControl5,
            E_INVALIDARG);
    }

    const auto found = _vectorLoadOperations.find(
        static_cast<std::uint64_t>(operationId));
    if (found == _vectorLoadOperations.end())
    {
        return SetControlComErrorInfo(
            "The vector load operation ID was not found.",
            IID_IMapWinGISControl5,
            E_INVALIDARG);
    }
    switch (_gdalEvents.Cancel(static_cast<std::uint64_t>(operationId)))
    {
    case mwg::activex::GdalCancelResult::Requested:
        return S_OK;
    case mwg::activex::GdalCancelResult::Completed:
        if (!found->second->committed)
        {
            try
            {
                const mwg::activex::GdalOperationLookup completed =
                    _gdalEvents.GetResult(
                        static_cast<std::uint64_t>(operationId));
                if (completed.status == mwg::activex::GdalEndResult::Ended &&
                    completed.result.has_value() &&
                    completed.result->succeeded &&
                    !completed.result->cancelled)
                {
                    found->second->cancelRequestedOnOwner = true;
                    return S_OK;
                }
            }
            catch (const std::bad_alloc&)
            {
                return SetControlComErrorInfo(
                    "Out of memory while checking the vector load result.",
                    IID_IMapWinGISControl5,
                    E_OUTOFMEMORY);
            }
            catch (...)
            {
                return SetControlComErrorInfo(
                    "The vector load result could not be checked.",
                    IID_IMapWinGISControl5,
                    E_FAIL);
            }
        }
        return S_FALSE;
    case mwg::activex::GdalCancelResult::AlreadyRequested:
        return S_FALSE;
    case mwg::activex::GdalCancelResult::NotFound:
    default:
        return SetControlComErrorInfo(
            "The vector load operation ID was not found.",
            IID_IMapWinGISControl5,
            E_INVALIDARG);
    }
}

STDMETHODIMP CMapWinGISControl::EndLoadVectorDataset(
    const LONGLONG operationId,
    VARIANT_BOOL* succeeded,
    VARIANT_BOOL* cancelled,
    LONGLONG* layerId,
    BSTR* summary,
    BSTR* diagnostics)
{
    if (succeeded == nullptr || cancelled == nullptr || layerId == nullptr ||
        summary == nullptr || diagnostics == nullptr)
    {
        return SetControlComErrorInfo(
            "One or more vector load result output pointers are null.",
            IID_IMapWinGISControl5,
            E_POINTER);
    }
    *succeeded = VARIANT_FALSE;
    *cancelled = VARIANT_FALSE;
    *layerId = 0;
    *summary = nullptr;
    *diagnostics = nullptr;
    if (operationId <= 0)
    {
        return SetControlComErrorInfo(
            "The vector load operation ID must be positive.",
            IID_IMapWinGISControl5,
            E_INVALIDARG);
    }

    const auto found = _vectorLoadOperations.find(
        static_cast<std::uint64_t>(operationId));
    if (found == _vectorLoadOperations.end())
    {
        return SetControlComErrorInfo(
            "The vector load operation ID was not found.",
            IID_IMapWinGISControl5,
            E_INVALIDARG);
    }
    if (!found->second->ownerFinalized)
    {
        return SetControlComErrorInfo(
            "The vector load has not finished or is still delivering events.",
            IID_IMapWinGISControl5,
            E_PENDING);
    }

    mwg::activex::GdalOperationLookup ended;
    try
    {
        ended = _gdalEvents.End(static_cast<std::uint64_t>(operationId));
    }
    catch (const std::bad_alloc&)
    {
        return SetControlComErrorInfo(
            "Out of memory while retrieving the vector load result.",
            IID_IMapWinGISControl5,
            E_OUTOFMEMORY);
    }
    catch (const std::exception& exception)
    {
        return SetControlComErrorInfo(
            exception.what(),
            IID_IMapWinGISControl5,
            E_FAIL);
    }
    catch (...)
    {
        return SetControlComErrorInfo(
            "An unexpected error occurred while retrieving the vector load result.",
            IID_IMapWinGISControl5,
            E_UNEXPECTED);
    }

    if (ended.status == mwg::activex::GdalEndResult::Pending)
    {
        return SetControlComErrorInfo(
            "The vector load has not finished or is still delivering events.",
            IID_IMapWinGISControl5,
            E_PENDING);
    }
    if (ended.status != mwg::activex::GdalEndResult::Ended ||
        !ended.result.has_value())
    {
        _vectorLoadOperations.erase(found);
        return SetControlComErrorInfo(
            "The vector load operation ID was not found.",
            IID_IMapWinGISControl5,
            E_INVALIDARG);
    }

    const auto state = found->second;
    _vectorLoadOperations.erase(found);
    BSTR summaryText = nullptr;
    BSTR diagnosticText = nullptr;
    HRESULT result = DiagnosticToBstr(ended.result->summary, &summaryText);
    if (SUCCEEDED(result))
    {
        result = DiagnosticToBstr(ended.result->diagnostics, &diagnosticText);
    }
    if (FAILED(result))
    {
        ::SysFreeString(summaryText);
        ::SysFreeString(diagnosticText);
        return SetControlComErrorInfo(
            "The completed vector load result could not be marshalled to COM.",
            IID_IMapWinGISControl5,
            result);
    }

    *succeeded = ended.result->succeeded ? VARIANT_TRUE : VARIANT_FALSE;
    *cancelled = ended.result->cancelled ? VARIANT_TRUE : VARIANT_FALSE;
    *layerId = state->committed ? state->layerId : 0;
    *summary = summaryText;
    *diagnostics = diagnosticText;
    return S_OK;
}

void CMapWinGISControl::CompleteVectorLoad(
    void* const context,
    const std::uint64_t operationId) noexcept
{
    static_cast<CMapWinGISControl*>(context)->CompleteVectorLoad(operationId);
}

void CMapWinGISControl::CompleteVectorLoad(
    const std::uint64_t operationId) noexcept
{
    const auto found = _vectorLoadOperations.find(operationId);
    if (found == _vectorLoadOperations.end())
    {
        return;
    }
    // Own the state: FireViewChange and other host callbacks can re-enter the
    // control (for example Close or EndLoadVectorDataset) and erase this entry,
    // so neither the iterator nor a reference into the map may be used later.
    const std::shared_ptr<VectorLoadState> state = found->second;
    if (state->committed)
    {
        return;
    }

    struct OwnerFinalizationGuard final
    {
        bool& finalized;

        ~OwnerFinalizationGuard()
        {
            finalized = true;
        }
    } ownerFinalization{state->ownerFinalized};

    mwg::activex::GdalOperationSnapshot result;
    try
    {
        const mwg::activex::GdalOperationLookup completed =
            _gdalEvents.GetResultForDelivery(operationId);
        if (completed.status != mwg::activex::GdalEndResult::Ended ||
            !completed.result.has_value())
        {
            return;
        }
        result = *completed.result;
        if (state->cancelRequestedOnOwner)
        {
            result.succeeded = false;
            result.cancelled = true;
            result.summary = "Vector dataset loading was cancelled.";
            result.diagnostics.clear();
            state->dataset.reset();
            state->scene = {};
            state->featureCount = 0;
            _gdalEvents.UpdateCompletedResult(operationId, std::move(result));
            return;
        }
        if (result.cancelled)
        {
            result.succeeded = false;
            result.summary = "Vector dataset loading was cancelled.";
            result.diagnostics.clear();
            state->dataset.reset();
            state->scene = {};
            state->featureCount = 0;
            _gdalEvents.UpdateCompletedResult(operationId, std::move(result));
            return;
        }
        if (!result.succeeded)
        {
            state->dataset.reset();
            state->scene = {};
            state->featureCount = 0;
            return;
        }

        if (state->featureCount >
            std::numeric_limits<LONGLONG>::max() - _featureCount)
        {
            throw mwg::CoreError("Map feature count is out of range");
        }
        const mwg::LayerId id = _view.AddLayer(
            std::move(state->dataset),
            std::move(state->scene));
        LONGLONG convertedId = 0;
        if (FAILED(CopyLayerId(id, &convertedId)))
        {
            (void)_view.RemoveLayer(id);
            throw mwg::CoreError("The layer ID could not be represented as a COM integer.");
        }
        try
        {
            (void)_view.FitToMap();
        }
        catch (...)
        {
            (void)_view.RemoveLayer(id);
            throw;
        }

        _featureCount += state->featureCount;
        state->layerId = convertedId;
        state->committed = true;
        state->featureCount = 0;
        PrepareFillMeshesInBackground();
        InvalidateLayerCache();
        FireViewChange();
    }
    catch (const std::bad_alloc&)
    {
        result.succeeded = false;
        result.cancelled = false;
        result.summary.clear();
        result.diagnostics.clear();
        try
        {
            result.summary = "Out of memory while committing the vector dataset.";
            result.diagnostics = result.summary;
        }
        catch (...)
        {
        }
        state->dataset.reset();
        state->scene = {};
        state->featureCount = 0;
        try
        {
            _gdalEvents.UpdateCompletedResult(operationId, std::move(result));
        }
        catch (...)
        {
            ::OutputDebugStringW(
                L"MapWinGIS: unable to retain the vector load commit failure.\n");
        }
    }
    catch (const std::exception& exception)
    {
        result.succeeded = false;
        result.cancelled = false;
        try
        {
            result.summary = exception.what();
            result.diagnostics = result.summary;
        }
        catch (...)
        {
            result.summary.clear();
            result.diagnostics.clear();
        }
        state->dataset.reset();
        state->scene = {};
        state->featureCount = 0;
        try
        {
            _gdalEvents.UpdateCompletedResult(operationId, std::move(result));
        }
        catch (...)
        {
            ::OutputDebugStringW(
                L"MapWinGIS: unable to retain the vector load commit failure.\n");
        }
    }
    catch (...)
    {
        result.succeeded = false;
        result.cancelled = false;
        try
        {
            result.summary =
                "An unexpected error occurred while committing the vector dataset.";
            result.diagnostics = result.summary;
        }
        catch (...)
        {
            result.summary.clear();
            result.diagnostics.clear();
        }
        state->dataset.reset();
        state->scene = {};
        state->featureCount = 0;
        try
        {
            _gdalEvents.UpdateCompletedResult(operationId, std::move(result));
        }
        catch (...)
        {
            ::OutputDebugStringW(
                L"MapWinGIS: unable to retain the vector load commit failure.\n");
        }
    }
}

HRESULT CMapWinGISControl::LoadVectorDatasetImpl(
    BSTR path,
    LONGLONG* layerId,
    REFIID sourceIid)
{
    if (path == nullptr)
    {
        return SetControlComErrorInfo("The dataset path is null.", sourceIid, E_INVALIDARG);
    }

    const UINT pathLength = ::SysStringLen(path);
    if (pathLength == 0)
    {
        return SetControlComErrorInfo("The dataset path is empty.", sourceIid, E_INVALIDARG);
    }

    try
    {
        const std::wstring pathText(path, static_cast<std::size_t>(pathLength));
        if (pathText.find(L'\0') != std::wstring::npos)
        {
            return SetControlComErrorInfo("The dataset path contains an embedded null character.", sourceIid, E_INVALIDARG);
        }

        // Validate before AddLayer can auto-fit or schedule a pending fit.
        PreparedVectorDataset prepared =
            PrepareVectorDataset(std::filesystem::path(pathText));
        if (prepared.featureCount >
            std::numeric_limits<LONGLONG>::max() - _featureCount)
        {
            throw mwg::CoreError("Map feature count is out of range");
        }

        const mwg::LayerId id = _view.AddLayer(
            std::move(prepared.dataset),
            std::move(prepared.scene));
        LONGLONG convertedId = 0;
        if (layerId != nullptr && FAILED(CopyLayerId(id, &convertedId)))
        {
            (void)_view.RemoveLayer(id);
            return SetControlComErrorInfo("The layer ID could not be represented as a COM integer.", sourceIid, E_UNEXPECTED);
        }
        try
        {
            // Fit the combined extent so the newly added layer is visible.
            (void)_view.FitToMap();
        }
        catch (...)
        {
            (void)_view.RemoveLayer(id);
            throw;
        }

        _featureCount += prepared.featureCount;
        if (layerId != nullptr)
        {
            *layerId = convertedId;
        }
        PrepareFillMeshesInBackground();
        InvalidateLayerCache();
        FireViewChange();
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return SetControlComErrorInfo("Out of memory while loading the vector dataset.", sourceIid, E_OUTOFMEMORY);
    }
    catch (const std::exception& exception)
    {
        return SetControlComErrorInfo(exception.what(), sourceIid, E_FAIL);
    }
    catch (...)
    {
        return SetControlComErrorInfo("An unexpected error occurred while loading the vector dataset.", sourceIid, E_UNEXPECTED);
    }
}

STDMETHODIMP CMapWinGISControl::ClearLayers()
{
    if (_view.ClearLayers())
    {
        _featureCount = 0;
        InvalidateLayerCache();
        FireViewChange();
    }
    _renderer.InvalidateSceneCache();
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::get_LayerLoaded(VARIANT_BOOL* loaded)
{
    if (loaded == nullptr)
    {
        return SetControlComErrorInfo("The loaded output pointer is null.", IID_IMapWinGISControl, E_POINTER);
    }

    *loaded = _view.GetMap().LayerCount() > 0 ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::get_FeatureCount(LONGLONG* count)
{
    if (count == nullptr)
    {
        return SetControlComErrorInfo("The feature count output pointer is null.", IID_IMapWinGISControl, E_POINTER);
    }

    *count = _featureCount;
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::get_RenderingBackend(BSTR* backend)
{
    if (backend == nullptr) return SetControlComErrorInfo("The rendering backend output pointer is null.", IID_IMapWinGISControl2, E_POINTER);
    const HRESULT result = CopyToBstr(BackendName(_renderer.GetActiveBackend()), backend);
    return FAILED(result) ? SetControlComErrorInfo("The rendering backend could not be returned.", IID_IMapWinGISControl2, result) : result;
}

STDMETHODIMP CMapWinGISControl::get_RenderingStatus(BSTR* status)
{
    if (status == nullptr) return SetControlComErrorInfo("The rendering status output pointer is null.", IID_IMapWinGISControl2, E_POINTER);
    const HRESULT result = CopyToBstr(_renderer.GetDiagnostic(), status);
    return FAILED(result) ? SetControlComErrorInfo("The rendering status could not be returned.", IID_IMapWinGISControl2, result) : result;
}

STDMETHODIMP CMapWinGISControl::get_RenderingBackendPreference(BSTR* preference)
{
    if (preference == nullptr) return SetControlComErrorInfo("The rendering backend preference output pointer is null.", IID_IMapWinGISControl2, E_POINTER);
    const HRESULT result = CopyToBstr(PreferenceName(_renderer.GetPreference()), preference);
    return FAILED(result) ? SetControlComErrorInfo("The rendering backend preference could not be returned.", IID_IMapWinGISControl2, result) : result;
}

STDMETHODIMP CMapWinGISControl::put_RenderingBackendPreference(BSTR preference)
{
    if (preference == nullptr)
    {
        return SetControlComErrorInfo("The rendering backend preference is null.", IID_IMapWinGISControl2, E_INVALIDARG);
    }

    const UINT length = ::SysStringLen(preference);
    auto equals = [preference, length](const wchar_t* value, const int valueLength)
    {
        return length == static_cast<UINT>(valueLength) &&
            ::CompareStringOrdinal(
                preference,
                static_cast<int>(length),
                value,
                valueLength,
                TRUE) == CSTR_EQUAL;
    };

    const auto previousPreference = _renderer.GetPreference();
    if (equals(L"Automatic", 9))
    {
        _renderer.SetPreference(mwg::activex::VectorRenderBackendPreference::Automatic);
    }
    else if (equals(L"OpenGL", 6))
    {
        _renderer.SetPreference(mwg::activex::VectorRenderBackendPreference::OpenGL);
    }
    else if (equals(L"GDI", 3))
    {
        _renderer.SetPreference(mwg::activex::VectorRenderBackendPreference::Gdi);
    }
    else
    {
        return SetControlComErrorInfo("The rendering backend preference must be Automatic, OpenGL, or GDI.", IID_IMapWinGISControl2, E_INVALIDARG);
    }

    if (_renderer.GetPreference() != previousPreference)
    {
        SetDirty(TRUE);
    }
    if (_renderer.GetPreference() !=
        mwg::activex::VectorRenderBackendPreference::Gdi)
    {
        PrepareFillMeshesInBackground();
    }
    InvalidateLayerCache();
    FireViewChange();
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::FitToMap()
{
    bool changed = false;
    const HRESULT result = RunViewOperation(
        [this]() { return _view.FitToMap(); },
        changed);
    if (SUCCEEDED(result) && changed)
    {
        InvalidateLayerCache();
        FireViewChange();
    }
    return result;
}

STDMETHODIMP CMapWinGISControl::ZoomIn(const DOUBLE factor)
{
    bool changed = false;
    const HRESULT result = RunViewOperation(
        [this, factor]() { return _view.ZoomIn(factor); },
        changed);
    if (SUCCEEDED(result) && changed)
    {
        InvalidateLayerCache();
        FireViewChange();
    }
    return result;
}

STDMETHODIMP CMapWinGISControl::ZoomOut(const DOUBLE factor)
{
    bool changed = false;
    const HRESULT result = RunViewOperation(
        [this, factor]() { return _view.ZoomOut(factor); },
        changed);
    if (SUCCEEDED(result) && changed)
    {
        InvalidateLayerCache();
        FireViewChange();
    }
    return result;
}

STDMETHODIMP CMapWinGISControl::Pan(
    const DOUBLE deltaX,
    const DOUBLE deltaY)
{
    bool changed = false;
    const HRESULT result = RunViewOperation(
        [this, deltaX, deltaY]() { return _view.Pan(deltaX, deltaY); },
        changed);
    if (SUCCEEDED(result) && changed)
    {
        InvalidateLayerCache();
        FireViewChange();
    }
    return result;
}

STDMETHODIMP CMapWinGISControl::ZoomAt(
    const DOUBLE x,
    const DOUBLE y,
    const DOUBLE factor)
{
    bool changed = false;
    const HRESULT result = RunViewOperation(
        [this, x, y, factor]()
        {
            return _view.ZoomAt({x, y}, factor);
        },
        changed);
    if (SUCCEEDED(result) && changed)
    {
        InvalidateLayerCache();
        FireViewChange();
    }
    return result;
}

STDMETHODIMP CMapWinGISControl::ZoomToLayer(const LONGLONG layerId)
{
    if (layerId <= 0)
    {
        return SetControlComErrorInfo("Layer ID must be positive", IID_IMapWinGISControl4, E_INVALIDARG);
    }
    const mwg::LayerId id{static_cast<std::uint64_t>(layerId)};
    if (_view.GetMap().FindLayer(id) == nullptr)
    {
        return SetControlComErrorInfo("Layer ID was not found", IID_IMapWinGISControl4, E_INVALIDARG);
    }

    bool changed = false;
    const HRESULT result = RunViewOperation(
        [this, id]() { return _view.ZoomToLayer(id); },
        changed);
    if (SUCCEEDED(result) && changed)
    {
        InvalidateLayerCache();
        FireViewChange();
    }
    return result;
}

STDMETHODIMP CMapWinGISControl::get_LayerCount(LONG* count)
{
    if (count == nullptr)
    {
        return SetControlComErrorInfo("The layer count output pointer is null.", IID_IMapWinGISControl4, E_POINTER);
    }
    *count = _view.GetMap().LayerCount();
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::GetLayerId(
    const LONG index,
    LONGLONG* layerId)
{
    if (layerId == nullptr)
    {
        return SetControlComErrorInfo("The layer ID output pointer is null.", IID_IMapWinGISControl4, E_POINTER);
    }
    *layerId = 0;
    const mwg::Map& map = _view.GetMap();
    if (index < 0 || index >= map.LayerCount())
    {
        return SetControlComErrorInfo("The layer index is out of range.", IID_IMapWinGISControl4, E_INVALIDARG);
    }

    const HRESULT result = CopyLayerId(map.Layer(index).Id(), layerId);
    return FAILED(result)
        ? SetControlComErrorInfo("The layer ID could not be represented as a COM integer.", IID_IMapWinGISControl4, result)
        : S_OK;
}

bool CMapWinGISControl::ResizeView(const LONG width, const LONG height) noexcept
{
    try
    {
        return _view.Resize(
            static_cast<int>(std::clamp<LONG>(width, 0, mwg::Viewport::kMaxDimension)),
            static_cast<int>(std::clamp<LONG>(height, 0, mwg::Viewport::kMaxDimension)));
    }
    catch (const std::exception& exception)
    {
        ::OutputDebugStringA(exception.what());
        InvalidateLayerCache();
        FireViewChange();
        return false;
    }
    catch (...)
    {
        ::OutputDebugStringW(L"MapWinGIS: resizing the map view failed.\n");
        InvalidateLayerCache();
        FireViewChange();
        return false;
    }
}

bool CMapWinGISControl::CaptureMouse() noexcept
{
    if (m_bWndLess)
    {
        return m_spInPlaceSite != nullptr &&
            m_spInPlaceSite->SetCapture(TRUE) == S_OK;
    }

    if (m_hWnd == nullptr)
    {
        return false;
    }

    ::SetCapture(m_hWnd);
    return ::GetCapture() == m_hWnd;
}

void CMapWinGISControl::ReleaseMouseCapture() noexcept
{
    if (m_bWndLess)
    {
        if (m_spInPlaceSite != nullptr)
        {
            m_spInPlaceSite->SetCapture(FALSE);
        }
    }
    else if (m_hWnd != nullptr && ::GetCapture() == m_hWnd)
    {
        ::ReleaseCapture();
    }
}

void CMapWinGISControl::CancelMousePan() noexcept
{
    if (_isPanning)
    {
        _isPanning = false;
        // Drop the cache even when the control is closing or being destroyed:
        // a reactivated control must never present a frame from a drag that
        // ended without a final render.
        InvalidateLayerCache();
        ReleaseMouseCapture();
    }
}

void CMapWinGISControl::EndMousePan() noexcept
{
    if (!_isPanning)
    {
        return;
    }

    CancelMousePan();
    // The drag ended: rasterize the layers once at the final view so the
    // cached frame matches the new center exactly (a clamped pan can leave
    // the blitted frame up to half a pixel off). Every drag termination
    // path (button-up, capture loss, cancel mode, button released) goes
    // through here, so a terminated drag never leaves the shifted cached
    // frame on screen.
    InvalidateLayerCache();
    FireViewChange();
}

bool CMapWinGISControl::GetMousePoint(
    const LPARAM lParam,
    const bool screenCoordinates,
    POINT& point) const noexcept
{
    point = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

    HWND coordinateWindow = m_hWnd;
    if (screenCoordinates)
    {
        if (m_bWndLess)
        {
            if (m_spInPlaceSite == nullptr ||
                FAILED(m_spInPlaceSite->GetWindow(&coordinateWindow)))
            {
                return false;
            }
        }

        if (coordinateWindow == nullptr ||
            !::ScreenToClient(coordinateWindow, &point))
        {
            return false;
        }
    }

    if (m_bWndLess)
    {
        point.x -= m_rcPos.left;
        point.y -= m_rcPos.top;
    }
    return true;
}

LRESULT CMapWinGISControl::OnLButtonDown(
    UINT,
    WPARAM,
    const LPARAM lParam,
    BOOL& handled)
{
    POINT point{};
    if (!GetMousePoint(lParam, false, point) || !CaptureMouse())
    {
        handled = FALSE;
        return 0;
    }

    _lastPanPoint = point;
    _isPanning = true;
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnMouseMove(
    UINT,
    const WPARAM wParam,
    const LPARAM lParam,
    BOOL& handled)
{
    if (!_isPanning)
    {
        handled = FALSE;
        return 0;
    }

    if ((wParam & MK_LBUTTON) == 0)
    {
        EndMousePan();
        handled = TRUE;
        return 0;
    }

    POINT point{};
    if (!GetMousePoint(lParam, false, point))
    {
        handled = TRUE;
        return 0;
    }

    const LONG pixelDeltaX = point.x - _lastPanPoint.x;
    const LONG pixelDeltaY = point.y - _lastPanPoint.y;
    _lastPanPoint = point;
    if (pixelDeltaX == 0 && pixelDeltaY == 0)
    {
        handled = TRUE;
        return 0;
    }

    const std::optional<double> resolution = _view.GetViewport().Resolution();
    if (!resolution.has_value())
    {
        handled = TRUE;
        return 0;
    }

    // The drag pan changes only the view center, so OnDraw can serve the
    // frame from the layer cache; the cache is dropped on button-up.
    bool changed = false;
    const HRESULT result = RunViewOperation(
        [this, pixelDeltaX, pixelDeltaY, resolution]()
        {
            return _view.Pan(
                -static_cast<double>(pixelDeltaX) * *resolution,
                static_cast<double>(pixelDeltaY) * *resolution);
        },
        changed);
    if (SUCCEEDED(result) && changed)
    {
        FireViewChange();
    }

    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnLButtonUp(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    if (!_isPanning)
    {
        handled = FALSE;
        return 0;
    }

    EndMousePan();
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnMouseWheel(
    UINT,
    const WPARAM wParam,
    const LPARAM lParam,
    BOOL& handled)
{
    const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
    POINT point{};
    const mwg::Viewport& viewport = _view.GetViewport();
    if (wheelDelta == 0 ||
        !viewport.IsValid() ||
        !GetMousePoint(lParam, true, point) ||
        point.x < 0 || point.x >= viewport.Width() ||
        point.y < 0 || point.y >= viewport.Height())
    {
        handled = FALSE;
        return 0;
    }

    const double factor = std::pow(
        kMouseWheelZoomFactor,
        static_cast<double>(wheelDelta) / WHEEL_DELTA);
    bool changed = false;
    const HRESULT result = RunViewOperation(
        [this, point, factor]()
        {
            return _view.ZoomAt(
                {static_cast<double>(point.x), static_cast<double>(point.y)},
                factor);
        },
        changed);
    if (SUCCEEDED(result) && changed)
    {
        InvalidateLayerCache();
        FireViewChange();
    }

    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnCaptureChanged(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    EndMousePan();
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnCancelMode(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    EndMousePan();
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnCreate(UINT, WPARAM, LPARAM, BOOL& handled)
{
    const HRESULT result = _gdalEvents.Reactivate();
    if (FAILED(result))
    {
        ::OutputDebugStringW(L"MapWinGIS: unable to reactivate GDAL operations.\n");
    }
    _gdalEvents.OnSize();
    InvalidateLayerCache();
    PrepareFillMeshesInBackground();
    handled = FALSE;
    return 0;
}

LRESULT CMapWinGISControl::OnSize(
    UINT,
    WPARAM,
    const LPARAM lParam,
    BOOL& handled)
{
    _gdalEvents.OnSize();
    if (ResizeView(LOWORD(lParam), HIWORD(lParam)))
    {
        InvalidateLayerCache();
        FireViewChange();
    }
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnGdalDispatch(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    return _gdalEvents.OnDispatch(handled);
}

LRESULT CMapWinGISControl::OnDestroy(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    CancelMousePan();
    _renderer.InvalidateSceneCache();
    _renderer.ConfigureFillMeshPreparation(nullptr, nullptr);
    if (_fillMeshPreparationDispatch != nullptr)
    {
        _fillMeshPreparationDispatch->Destroy();
        _fillMeshPreparationDispatch.reset();
    }
    _gdalEvents.OnDestroy();
    handled = FALSE;
    return 0;
}

void CMapWinGISControl::DispatchGdalNotifications(void* const control) noexcept
{
    auto* const instance = static_cast<CMapWinGISControl*>(control);
    ATL::CComPtr<CMapWinGISControl> lifetime(instance);
    BOOL handled = FALSE;
    try
    {
        instance->OnGdalDispatch(
            mwg::activex::kGdalDispatchMessage,
            0,
            0,
            handled);
    }
    catch (const std::exception& exception)
    {
        ::OutputDebugStringA(exception.what());
    }
    catch (...)
    {
        // This callback is noexcept; a non-standard exception must not
        // terminate the host process.
        ::OutputDebugStringW(L"MapWinGIS: unexpected exception while dispatching GDAL notifications.\n");
    }
}

bool CMapWinGISControl::WakeFillMeshPreparation(
    void* const context) noexcept
{
    return ::PostMessageW(
        static_cast<HWND>(context),
        mwg::activex::kGdalDispatchMessage,
        0,
        0) != FALSE;
}

void CMapWinGISControl::DispatchFillMeshPreparation(
    void* const control) noexcept
{
    auto* const instance = static_cast<CMapWinGISControl*>(control);
    ATL::CComPtr<CMapWinGISControl> lifetime(instance);
    try
    {
        if (instance->_fillMeshPreparationDispatch != nullptr)
        {
            instance->_fillMeshPreparationDispatch->StopRetryTimer();
        }
        if (instance->_layerFramePending)
        {
            // Posted completions outrank WM_PAINT, so they can arrive before
            // the GDI frame of a new or changed scene is presented. Warming
            // the OpenGL target here would delay that frame; OnDraw re-posts
            // the wake once the frame has been presented.
            instance->_fillMeshPublicationDeferred = true;
            instance->FireViewChange();
            return;
        }
        if (instance->_renderer.PublishPreparedFillMeshes())
        {
            const mwg::Viewport& viewport = instance->_view.GetViewport();
            instance->_renderer.WarmUpOpenGl(
                instance->_view.Scene(), viewport.Width(), viewport.Height());
            instance->InvalidateLayerCache();
            instance->FireViewChange();
        }
        if (instance->_fillMeshPreparationDispatch != nullptr)
        {
            if (instance->_renderer.HasCompletedFillMeshPreparation())
            {
                if (!instance->_fillMeshPreparationDispatch->StartRetryTimer())
                {
                    ::OutputDebugStringW(
                        L"MapWinGIS: unable to arm the fill-mesh retry timer.\n");
                }
            }
            else
            {
                instance->_fillMeshPreparationDispatch->StopRetryTimer();
            }
        }
    }
    catch (const std::exception& exception)
    {
        ::OutputDebugStringA(exception.what());
    }
    catch (...)
    {
        ::OutputDebugStringW(
            L"MapWinGIS: unexpected exception while publishing fill meshes.\n");
    }
}

bool CMapWinGISControl::EnsureFillMeshPreparationDispatch() noexcept
{
    if (_fillMeshPreparationDispatch != nullptr)
    {
        return true;
    }
    try
    {
        auto dispatch = std::make_shared<mwg::activex::GdalDispatchWindow>();
        const HWND window = dispatch->Create(
            &CMapWinGISControl::DispatchFillMeshPreparation,
            this);
        if (window == nullptr)
        {
            ::OutputDebugStringW(
                L"MapWinGIS: unable to create the fill-mesh dispatch window.\n");
            return false;
        }
        _fillMeshPreparationDispatch = std::move(dispatch);
        _renderer.ConfigureFillMeshPreparation(
            &CMapWinGISControl::WakeFillMeshPreparation,
            static_cast<void*>(window));
        return true;
    }
    catch (const std::exception& exception)
    {
        ::OutputDebugStringA(exception.what());
        return false;
    }
    catch (...)
    {
        ::OutputDebugStringW(
            L"MapWinGIS: unable to initialize background fill-mesh preparation.\n");
        return false;
    }
}

void CMapWinGISControl::PrepareFillMeshesInBackground() noexcept
{
    if (_renderer.GetPreference() ==
            mwg::activex::VectorRenderBackendPreference::Gdi ||
        !EnsureFillMeshPreparationDispatch())
    {
        return;
    }
    try
    {
        _renderer.PrepareFillMeshesInBackground(_view.Scene());
    }
    catch (const std::exception& exception)
    {
        ::OutputDebugStringA(exception.what());
    }
    catch (...)
    {
        ::OutputDebugStringW(
            L"MapWinGIS: unable to queue background fill-mesh preparation.\n");
    }
}

STDMETHODIMP CMapWinGISControl::Close(const DWORD saveOption)
{
    CancelMousePan();
    _renderer.InvalidateSceneCache();
    _renderer.ConfigureFillMeshPreparation(nullptr, nullptr);
    if (_fillMeshPreparationDispatch != nullptr)
    {
        _fillMeshPreparationDispatch->Destroy();
        _fillMeshPreparationDispatch.reset();
    }
    _gdalEvents.Close();
    _vectorLoadOperations.clear();
    const HRESULT result = ATL::IOleObjectImpl<CMapWinGISControl>::Close(saveOption);
    return FAILED(result)
        ? SetControlComErrorInfo("The ActiveX control could not be closed.", IID_IMapWinGISControl4, result)
        : result;
}

STDMETHODIMP CMapWinGISControl::SetClientSite(IOleClientSite* const clientSite)
{
    const HRESULT result = ATL::IOleObjectImpl<CMapWinGISControl>::SetClientSite(clientSite);
    if (SUCCEEDED(result) && clientSite != nullptr)
    {
        const HRESULT reset = _gdalEvents.Reactivate();
        if (FAILED(reset))
            return SetControlComErrorInfo("The GDAL operation source could not be reactivated.", IID_IMapWinGISControlGdal, reset);
        InvalidateLayerCache();
        PrepareFillMeshesInBackground();
    }
    return FAILED(result)
        ? SetControlComErrorInfo("The ActiveX client site could not be changed.", IID_IMapWinGISControl4, result)
        : result;
}

STDMETHODIMP CMapWinGISControl::SetObjectRects(LPCRECT position, LPCRECT clip)
{
    if (position == nullptr || clip == nullptr)
    {
        return SetControlComErrorInfo("The object position or clip rectangle is null.", IID_IMapWinGISControl4, E_POINTER);
    }
    const HRESULT result =
        ATL::IOleInPlaceObjectWindowlessImpl<CMapWinGISControl>::SetObjectRects(position, clip);
    if (SUCCEEDED(result) && m_bWndLess)
    {
        const LONGLONG width = static_cast<LONGLONG>(position->right) - position->left;
        const LONGLONG height = static_cast<LONGLONG>(position->bottom) - position->top;
        if (ResizeView(
                static_cast<LONG>(std::clamp<LONGLONG>(width, 0, mwg::Viewport::kMaxDimension)),
                static_cast<LONG>(std::clamp<LONGLONG>(height, 0, mwg::Viewport::kMaxDimension))))
        {
            InvalidateLayerCache();
            FireViewChange();
        }
    }
    return FAILED(result)
        ? SetControlComErrorInfo("The ActiveX object rectangles could not be set.", IID_IMapWinGISControl4, result)
        : result;
}

void CMapWinGISControl::InvalidateLayerCache() noexcept
{
    _layerCacheValid = false;
    _layerFramePending = true;
}

void CMapWinGISControl::OnLayerFramePresented() noexcept
{
    _layerFramePending = false;
    if (!_fillMeshPublicationDeferred || _fillMeshPreparationDispatch == nullptr)
    {
        return;
    }
    _fillMeshPublicationDeferred = false;
    if (!_fillMeshPreparationDispatch->PostWake() &&
        !_fillMeshPreparationDispatch->StartRetryTimer())
    {
        ::OutputDebugStringW(
            L"MapWinGIS: unable to resume deferred fill-mesh publication.\n");
    }
}

bool CMapWinGISControl::TryBlitCachedLayers(
    HDC frameDc,
    const mwg::Viewport& viewport) noexcept
{
    if (!_layerCacheValid ||
        !_layerCache.IsValid() ||
        _layerCache.Width() != viewport.Width() ||
        _layerCache.Height() != viewport.Height())
    {
        return false;
    }

    const std::optional<double> resolution = viewport.Resolution();
    const std::optional<double> cacheResolution = _layerCacheViewport.Resolution();
    const std::optional<mwg::MapPoint> center = viewport.Center();
    const std::optional<mwg::MapPoint> cacheCenter = _layerCacheViewport.Center();
    if (!resolution.has_value() ||
        !cacheResolution.has_value() ||
        !center.has_value() ||
        !cacheCenter.has_value() ||
        *resolution != *cacheResolution)
    {
        // A resolution change (or a missing view) needs a re-render.
        return false;
    }

    // The cached background must match the color the frame is cleared with;
    // a system-color or high-contrast change would otherwise leave the old
    // background showing through an otherwise valid cache.
    if (::GetSysColor(COLOR_WINDOW) != _layerCacheBackground)
    {
        return false;
    }

    // GDI BitBlt addresses whole pixels only. The accumulated map-center
    // arithmetic can introduce tiny floating-point noise, but a genuine
    // fractional-pixel shift must use a fresh render rather than rounding.
    const double offsetX = (cacheCenter->x - center->x) / *resolution;
    const double offsetY = (center->y - cacheCenter->y) / *resolution;
    const LONG width = static_cast<LONG>(viewport.Width());
    const LONG height = static_cast<LONG>(viewport.Height());
    LONG shiftX = 0;
    LONG shiftY = 0;
    if (!mwg::activex::TryGetWholePixelPanShift(offsetX, width, shiftX) ||
        !mwg::activex::TryGetWholePixelPanShift(offsetY, height, shiftY))
    {
        return false;
    }

    // Destination (x, y) shows cache pixel (x - shiftX, y - shiftY); blit the
    // rectangle where that source position is inside the cached frame. The
    // rest of the destination keeps the background already painted.
    const LONG destLeft = std::max<LONG>(0, shiftX);
    const LONG destTop = std::max<LONG>(0, shiftY);
    const LONG destRight = std::min<LONG>(width, width + shiftX);
    const LONG destBottom = std::min<LONG>(height, height + shiftY);
    if (destRight <= destLeft || destBottom <= destTop)
    {
        // The cached content is entirely outside the frame.
        return true;
    }

    return ::BitBlt(
               frameDc,
               destLeft,
               destTop,
               destRight - destLeft,
               destBottom - destTop,
               _layerCache.Get(),
               destLeft - shiftX,
               destTop - shiftY,
               SRCCOPY) != 0;
}

HRESULT CMapWinGISControl::DrawLayersAndUpdateCache(
    HDC frameDc,
    const RECT& frameBounds,
    const mwg::rendering::MapRenderScene& scene,
    const mwg::Viewport& viewport,
    const bool useCache,
    const bool retainRendererBuffers)
{
    if (!useCache)
    {
        return _renderer.Draw(frameDc, frameBounds, scene, viewport, retainRendererBuffers);
    }

    const int width = viewport.Width();
    const int height = viewport.Height();
    if (!_layerCache.Resize(width, height))
    {
        // Without a cache the layers are drawn straight into the frame; the
        // next draw retries the allocation.
        return _renderer.Draw(frameDc, frameBounds, scene, viewport, retainRendererBuffers);
    }

    HDC cacheDc = _layerCache.Get();
    const RECT cacheBounds{0, 0, width, height};
    if (::FillRect(
            cacheDc,
            &cacheBounds,
            ::GetSysColorBrush(COLOR_WINDOW)) == 0)
    {
        return _renderer.Draw(frameDc, frameBounds, scene, viewport, retainRendererBuffers);
    }

    const HRESULT result = _renderer.Draw(cacheDc, cacheBounds, scene, viewport, retainRendererBuffers);
    if (FAILED(result))
    {
        _layerCacheValid = false;
        return result;
    }

    _layerCacheViewport = viewport;
    _layerCacheBackground = ::GetSysColor(COLOR_WINDOW);
    _layerCacheValid = true;

    if (::BitBlt(
            frameDc,
            frameBounds.left,
            frameBounds.top,
            width,
            height,
            cacheDc,
            0,
            0,
            SRCCOPY) != 0)
    {
        return result;
    }

    // A blit between different devices (printer or metafile targets) can
    // fail; present the layers by drawing straight to the target.
    _layerCacheValid = false;
    return _renderer.Draw(frameDc, frameBounds, scene, viewport, retainRendererBuffers);
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

    const LONGLONG width = static_cast<LONGLONG>(bounds.right) - bounds.left;
    const LONGLONG height = static_cast<LONGLONG>(bounds.bottom) - bounds.top;
    const bool drawable = width > 0 && height > 0 &&
        width <= mwg::Viewport::kMaxDimension && height <= mwg::Viewport::kMaxDimension;
    const mwg::Viewport& clientViewport = _view.GetViewport();
    const bool alternateSize =
        width != clientViewport.Width() || height != clientViewport.Height();

    // Only a client-sized draw presents the pending layer frame; prints and
    // scaled draws leave deferred mesh publication waiting.
    struct LayerFramePresentation final
    {
        CMapWinGISControl* control;

        ~LayerFramePresentation()
        {
            if (control != nullptr)
            {
                control->OnLayerFramePresented();
            }
        }
    } layerFramePresentation{alternateSize ? nullptr : this};

    // Printing, metafiles, and scaled draws must not resize or retain memory
    // in the interactive screen buffers. Their frame and OpenGL staging
    // storage are temporary and released when this draw returns.
    mwg::activex::FrameBuffer temporaryFrameBuffer;
    mwg::activex::FrameBuffer& frameBuffer =
        alternateSize ? temporaryFrameBuffer : _frameBuffer;

    // Compose the frame in an offscreen buffer and present it with a single
    // blit, so the visible surface never shows the background erase or a
    // partially drawn frame. Drawing straight to the target device context
    // is the fallback for sizes the buffer cannot hold.
    const bool buffered = drawable &&
        frameBuffer.Resize(
            static_cast<int>(width),
            static_cast<int>(height));
    HDC frameDc = buffered ? frameBuffer.Get() : drawInfo.hdcDraw;
    RECT frameBounds = buffered
        ? RECT{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)}
        : bounds;

    // The cache holds a viewport-sized bitmap that is blitted at the frame
    // origin; it is only valid while the frame is the offscreen buffer. A
    // failed presentation blit switches to the target DC and bounds, and
    // must not use the cache.
    bool useLayerCache = buffered && !alternateSize;

    const auto renderFrame = [&]() -> HRESULT
    {
        if (::FillRect(
                frameDc,
                &frameBounds,
                ::GetSysColorBrush(COLOR_WINDOW)) == 0)
        {
            return E_FAIL;
        }

        if (!drawable)
        {
            return S_OK;
        }

        try
        {
            const mwg::rendering::MapRenderScene& scene = _view.Scene();
            // Printing and scaled draws use a snapshot, never the live viewport.
            mwg::Viewport viewport = _view.GetViewport();
            const mwg::Extent liveExtent = viewport.VisibleExtent();
            viewport.SetSize(static_cast<int>(width), static_cast<int>(height));
            if (alternateSize && liveExtent.IsValid())
            {
                // Scale what the screen shows to the target: a print or a
                // larger metafile must not reveal more of the map at screen
                // resolution.
                (void)viewport.FitExtent(liveExtent);
            }
            if (!viewport.HasView())
            {
                (void)viewport.FitExtent(_view.GetMap().GetExtent(), mwg::MapView::kDefaultPadding);
            }
            if (!scene.layers.empty() && viewport.IsValid())
            {
                if (useLayerCache && TryBlitCachedLayers(frameDc, viewport))
                {
                    return S_OK;
                }
                return DrawLayersAndUpdateCache(
                    frameDc,
                    frameBounds,
                    scene,
                    viewport,
                    useLayerCache,
                    !alternateSize);
            }
            return S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
        catch (const std::exception& exception)
        {
            ::OutputDebugStringA(exception.what());
            return E_FAIL;
        }
        catch (...)
        {
            ::OutputDebugStringW(L"MapWinGIS: unexpected exception while drawing the map.\n");
            return E_UNEXPECTED;
        }
    };

    if (buffered)
    {
        const HRESULT result = renderFrame();
        if (FAILED(result))
        {
            return result;
        }
        if (::BitBlt(
                drawInfo.hdcDraw,
                bounds.left,
                bounds.top,
                static_cast<int>(width),
                static_cast<int>(height),
                frameDc,
                0,
                0,
                SRCCOPY) != 0)
        {
            return result;
        }

        // A blit between different devices (printer or metafile targets)
        // can fail; present the frame by drawing straight to the target.
        frameDc = drawInfo.hdcDraw;
        frameBounds = bounds;
        useLayerCache = false;
    }

    return renderFrame();
}

LRESULT CMapWinGISControl::OnEraseBkgnd(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    // OnDraw paints an opaque frame over the whole bounds, so erasing the
    // background first would only flash the host's color through.
    handled = TRUE;
    return 1;
}

CMapWinGISControl::~CMapWinGISControl()
{
    CancelMousePan();
    _renderer.InvalidateSceneCache();
    _renderer.ConfigureFillMeshPreparation(nullptr, nullptr);
    if (_fillMeshPreparationDispatch != nullptr)
    {
        _fillMeshPreparationDispatch->Destroy();
    }
    _gdalEvents.Shutdown();
}
