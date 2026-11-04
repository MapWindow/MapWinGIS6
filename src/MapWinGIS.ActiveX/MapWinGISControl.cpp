// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.cpp
 * @brief Implements vector loading, file drop, ActiveX state, and host drawing.
 *
 * Redraws: every map or view change made through the control goes through
 * its MapView; the control calls FireViewChange when the MapView reports
 * that the frame changed. OnDraw draws the MapView's render scene through
 * its viewport, so every layer uses the same transformation.
 */
#include "MapWinGISControl.h"

#include <atlconv.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ComConversion.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/vector/VectorDataset.h"

namespace
{
    using mwg::activex::ComConversion::BstrToUtf8;
    using mwg::activex::ComConversion::CopyStringArray;
    using mwg::activex::ComConversion::CopyToBstr;
    using mwg::activex::ComConversion::CreateStringArray;
    using mwg::activex::ComConversion::Utf8ToBstr;

    void TraceNavigationError(const char* const message) noexcept
    {
        ::OutputDebugStringA("MapWinGIS mouse navigation failed: ");
        ::OutputDebugStringA(message);
        ::OutputDebugStringA("\n");
    }

    [[nodiscard]] HRESULT SetComErrorInfo(
        const std::string& message,
        const HRESULT errorCode)
    {
        const CA2W wideMessage(message.c_str(), CP_UTF8);
        return AtlReportError(
            CLSID_MapWinGISControl,
            wideMessage,
            IID_IMapWinGISControlGdal,
            errorCode);
    }

    [[nodiscard]] HRESULT SetNavigationErrorInfo(
        const std::string& message,
        const HRESULT errorCode)
    {
        const CA2W wideMessage(message.c_str(), CP_UTF8);
        return AtlReportError(
            CLSID_MapWinGISControl,
            wideMessage,
            IID_IMapWinGISControl4,
            errorCode);
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

    [[nodiscard]] bool HasFileDropFormat(IDataObject* data) noexcept
    {
        if (data == nullptr)
        {
            return false;
        }

        FORMATETC format{
            CF_HDROP,
            nullptr,
            DVASPECT_CONTENT,
            -1,
            TYMED_HGLOBAL};
        return SUCCEEDED(data->QueryGetData(&format));
    }

    class StgMediumGuard final
    {
    public:
        explicit StgMediumGuard(STGMEDIUM& medium) noexcept
            : _medium(medium)
        {
        }

        ~StgMediumGuard()
        {
            ::ReleaseStgMedium(&_medium);
        }

        StgMediumGuard(const StgMediumGuard&) = delete;
        StgMediumGuard& operator=(const StgMediumGuard&) = delete;

    private:
        STGMEDIUM& _medium;
    };

    [[nodiscard]] HRESULT GetDroppedPaths(
        IDataObject* data,
        std::vector<std::filesystem::path>& paths)
    {
        if (data == nullptr)
        {
            return E_POINTER;
        }

        FORMATETC format{
            CF_HDROP,
            nullptr,
            DVASPECT_CONTENT,
            -1,
            TYMED_HGLOBAL};
        STGMEDIUM medium{};
        const HRESULT result = data->GetData(&format, &medium);
        if (FAILED(result))
        {
            return result;
        }
        const StgMediumGuard mediumGuard(medium);
        if (medium.tymed != TYMED_HGLOBAL || medium.hGlobal == nullptr)
        {
            return DV_E_TYMED;
        }

        const HDROP drop = static_cast<HDROP>(medium.hGlobal);
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count == 0)
        {
            return DV_E_FORMATETC;
        }

        paths.clear();
        paths.reserve(count);
        for (UINT index = 0; index < count; ++index)
        {
            const UINT length = ::DragQueryFileW(drop, index, nullptr, 0);
            if (length == 0 || length == std::numeric_limits<UINT>::max())
            {
                return DV_E_FORMATETC;
            }

            std::wstring path(length + 1, L'\0');
            const UINT copied = ::DragQueryFileW(
                drop,
                index,
                path.data(),
                static_cast<UINT>(path.size()));
            if (copied != length)
            {
                return DV_E_FORMATETC;
            }
            path.resize(copied);
            paths.emplace_back(std::move(path));
        }
        return S_OK;
    }

    void LogDropFailure(
        const std::filesystem::path& path,
        const HRESULT result) noexcept
    {
        wchar_t message[64]{};
        (void)::swprintf_s(
            message,
            64,
            L"MapWinGIS could not load dropped file (HRESULT 0x%08lX): ",
            static_cast<unsigned long>(result));
        ::OutputDebugStringW(message);
        ::OutputDebugStringW(path.c_str());
        ::OutputDebugStringW(L"\n");
    }
}

CMapWinGISControl::CMapWinGISControl()
    : _gdalEvents(
          m_vec,
          &CMapWinGISControl::DispatchGdalNotifications,
          this)
{
}

STDMETHODIMP CMapWinGISControl::BeginGdalOperation(
    BSTR algorithmPath,
    SAFEARRAY* arguments,
    LONGLONG* operationId)
{
    if (operationId == nullptr)
    {
        return E_POINTER;
    }
    *operationId = 0;
    const HRESULT dispatchWindowResult = _gdalEvents.EnsureDispatchWindow();
    if (FAILED(dispatchWindowResult))
    {
        return dispatchWindowResult;
    }

    std::string algorithm;
    std::vector<std::string> commandLineArguments;
    HRESULT result = S_OK;
    try
    {
        result = BstrToUtf8(algorithmPath, algorithm);
        if (FAILED(result) || algorithm.empty())
        {
            return E_INVALIDARG;
        }

        result = CopyStringArray(arguments, commandLineArguments);
        if (FAILED(result))
        {
            return result;
        }
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetComErrorInfo(exception.what(), E_INVALIDARG);
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
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetComErrorInfo(exception.what(), E_FAIL);
    }
}

STDMETHODIMP CMapWinGISControl::CancelGdalOperation(const LONGLONG operationId)
{
    if (operationId <= 0)
    {
        return E_INVALIDARG;
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
        return E_INVALIDARG;
    }
}

STDMETHODIMP CMapWinGISControl::EndGdalOperation(
    const LONGLONG operationId,
    VARIANT_BOOL* succeeded,
    VARIANT_BOOL* cancelled,
    LONG* errorCode,
    LONG* callbackErrorCode,
    BSTR* callbackError,
    BSTR* summary,
    BSTR* diagnostics,
    SAFEARRAY** warnings)
{
    if (succeeded == nullptr || cancelled == nullptr || errorCode == nullptr ||
        callbackErrorCode == nullptr || callbackError == nullptr ||
        summary == nullptr ||
        diagnostics == nullptr || warnings == nullptr)
    {
        return E_POINTER;
    }
    *succeeded = VARIANT_FALSE;
    *cancelled = VARIANT_FALSE;
    *errorCode = 0;
    *callbackErrorCode = 0;
    *callbackError = nullptr;
    *summary = nullptr;
    *diagnostics = nullptr;
    *warnings = nullptr;
    if (operationId <= 0)
    {
        return E_INVALIDARG;
    }

    mwg::activex::GdalOperationLookup resultLookup;
    try
    {
        resultLookup = _gdalEvents.GetResult(static_cast<std::uint64_t>(operationId));
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    if (resultLookup.status == mwg::activex::GdalEndResult::Pending)
    {
        return E_PENDING;
    }
    if (resultLookup.status == mwg::activex::GdalEndResult::NotFound ||
        !resultLookup.result.has_value())
    {
        return E_INVALIDARG;
    }
    const mwg::activex::GdalOperationSnapshot& operationResult =
        *resultLookup.result;

    BSTR summaryText = nullptr;
    BSTR callbackErrorText = nullptr;
    HRESULT result = Utf8ToBstr(
        operationResult.callbackError,
        &callbackErrorText);
    if (FAILED(result))
    {
        return result;
    }

    result = Utf8ToBstr(operationResult.summary, &summaryText);
    if (FAILED(result))
    {
        ::SysFreeString(callbackErrorText);
        return result;
    }

    BSTR diagnosticText = nullptr;
    result = Utf8ToBstr(operationResult.diagnostics, &diagnosticText);
    if (FAILED(result))
    {
        ::SysFreeString(callbackErrorText);
        ::SysFreeString(summaryText);
        return result;
    }

    SAFEARRAY* warningArray = nullptr;
    result = CreateStringArray(operationResult.warnings, &warningArray);
    if (FAILED(result))
    {
        ::SysFreeString(callbackErrorText);
        ::SysFreeString(summaryText);
        ::SysFreeString(diagnosticText);
        return result;
    }

    const mwg::activex::GdalOperationLookup cleanupResult =
        _gdalEvents.End(static_cast<std::uint64_t>(operationId));
    if (cleanupResult.status != mwg::activex::GdalEndResult::Ended)
    {
        ::SysFreeString(callbackErrorText);
        ::SysFreeString(summaryText);
        ::SysFreeString(diagnosticText);
        if (warningArray != nullptr)
        {
            ::SafeArrayDestroy(warningArray);
        }
        return cleanupResult.status == mwg::activex::GdalEndResult::Pending
            ? E_PENDING
            : E_INVALIDARG;
    }

    *succeeded = operationResult.succeeded ? VARIANT_TRUE : VARIANT_FALSE;
    *cancelled = operationResult.cancelled ? VARIANT_TRUE : VARIANT_FALSE;
    *errorCode = operationResult.errorCode;
    *callbackErrorCode = operationResult.callbackErrorCode;
    *callbackError = callbackErrorText;
    *summary = summaryText;
    *diagnostics = diagnosticText;
    *warnings = warningArray;
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::get_CoreInitialized(VARIANT_BOOL* initialized)
{
    if (initialized == nullptr)
    {
        return E_POINTER;
    }

    *initialized = mwg::GdalEnvironment::IsInitialized() ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::get_LayerCount(LONG* count)
{
    if (count == nullptr)
    {
        return E_POINTER;
    }

    *count = 0;
    try
    {
        *count = _view.GetMap().LayerCount();
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetNavigationErrorInfo(exception.what(), E_FAIL);
    }
}

STDMETHODIMP CMapWinGISControl::GetLayerId(
    const LONG index,
    LONGLONG* layerId)
{
    if (layerId == nullptr)
    {
        return E_POINTER;
    }
    *layerId = 0;

    try
    {
        const mwg::Map& map = _view.GetMap();
        if (index < 0 || index >= map.LayerCount())
        {
            return E_INVALIDARG;
        }

        const std::uint64_t value = map.Layer(index).Id().value;
        if (value > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
        {
            return SetNavigationErrorInfo("Layer identity is out of range", E_FAIL);
        }
        *layerId = static_cast<LONGLONG>(value);
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetNavigationErrorInfo(exception.what(), E_FAIL);
    }
}

STDMETHODIMP CMapWinGISControl::ZoomToMap(VARIANT_BOOL* changed)
{
    if (changed == nullptr)
    {
        return E_POINTER;
    }
    *changed = VARIANT_FALSE;

    try
    {
        if (_view.FitToMap())
        {
            *changed = VARIANT_TRUE;
            FireViewChange();
        }
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetNavigationErrorInfo(exception.what(), E_FAIL);
    }
}

STDMETHODIMP CMapWinGISControl::ZoomToLayer(
    const LONGLONG layerId,
    VARIANT_BOOL* changed)
{
    if (changed == nullptr)
    {
        return E_POINTER;
    }
    *changed = VARIANT_FALSE;
    if (layerId <= 0)
    {
        return E_INVALIDARG;
    }

    try
    {
        if (_view.ZoomToLayer(
                mwg::LayerId{static_cast<std::uint64_t>(layerId)}))
        {
            *changed = VARIANT_TRUE;
            FireViewChange();
        }
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetNavigationErrorInfo(exception.what(), E_FAIL);
    }
}

STDMETHODIMP CMapWinGISControl::ZoomIn(VARIANT_BOOL* changed)
{
    if (changed == nullptr)
    {
        return E_POINTER;
    }
    *changed = VARIANT_FALSE;

    try
    {
        if (_view.ZoomIn())
        {
            *changed = VARIANT_TRUE;
            FireViewChange();
        }
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetNavigationErrorInfo(exception.what(), E_FAIL);
    }
}

STDMETHODIMP CMapWinGISControl::ZoomOut(VARIANT_BOOL* changed)
{
    if (changed == nullptr)
    {
        return E_POINTER;
    }
    *changed = VARIANT_FALSE;

    try
    {
        if (_view.ZoomOut())
        {
            *changed = VARIANT_TRUE;
            FireViewChange();
        }
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetNavigationErrorInfo(exception.what(), E_FAIL);
    }
}

STDMETHODIMP CMapWinGISControl::RemoveLayer(
    const LONGLONG layerId,
    VARIANT_BOOL* changed)
{
    if (changed == nullptr)
    {
        return E_POINTER;
    }
    *changed = VARIANT_FALSE;
    if (layerId <= 0)
    {
        return E_INVALIDARG;
    }

    try
    {
        const mwg::LayerId id{static_cast<std::uint64_t>(layerId)};
        const mwg::MapLayer* layer = _view.GetMap().FindLayer(id);
        if (layer == nullptr)
        {
            return S_OK;
        }

        const auto contribution = std::find_if(
            _layerFeatureCounts.begin(),
            _layerFeatureCounts.end(),
            [id](const auto& entry) { return entry.first == id; });
        if (contribution == _layerFeatureCounts.end())
        {
            return SetNavigationErrorInfo("Layer feature count is missing", E_FAIL);
        }
        if (_view.RemoveLayer(id))
        {
            _featureCount -= contribution->second;
            _layerFeatureCounts.erase(contribution);
            *changed = VARIANT_TRUE;
            FireViewChange();
        }
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetNavigationErrorInfo(exception.what(), E_FAIL);
    }
}

STDMETHODIMP CMapWinGISControl::SetLayerVisible(
    const LONGLONG layerId,
    const VARIANT_BOOL visible,
    VARIANT_BOOL* changed)
{
    if (changed == nullptr)
    {
        return E_POINTER;
    }
    *changed = VARIANT_FALSE;
    if (layerId <= 0)
    {
        return E_INVALIDARG;
    }

    try
    {
        if (_view.SetLayerVisible(
                mwg::LayerId{static_cast<std::uint64_t>(layerId)},
                visible != VARIANT_FALSE))
        {
            *changed = VARIANT_TRUE;
            FireViewChange();
        }
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetNavigationErrorInfo(exception.what(), E_FAIL);
    }
}

STDMETHODIMP CMapWinGISControl::LoadVectorDataset(BSTR path)
{
    if (path == nullptr)
    {
        return E_INVALIDARG;
    }

    const UINT pathLength = ::SysStringLen(path);
    if (pathLength == 0)
    {
        return E_INVALIDARG;
    }

    try
    {
        const std::wstring pathText(path, static_cast<std::size_t>(pathLength));
        if (pathText.find(L'\0') != std::wstring::npos)
        {
            return E_INVALIDARG;
        }

        const std::filesystem::path datasetPath(pathText);
        std::shared_ptr<mwg::VectorDataset> dataset = mwg::VectorDataset::Open(datasetPath);
        if (dataset->LayerCount() == 0)
        {
            throw mwg::CoreError("Vector dataset contains no layers");
        }

        // Validate before AddLayer can auto-fit or schedule a pending fit.
        mwg::Map candidate;
        candidate.AddLayer(dataset);
        const mwg::rendering::MapRenderScene scene =
            mwg::rendering::CreateMapRenderScene(candidate);
        for (const mwg::rendering::SkippedLayer& skipped : scene.skippedLayers)
        {
            if (skipped.reason == mwg::rendering::LayerSkipReason::Failed)
            {
                throw mwg::CoreError(skipped.message);
            }
        }
        const LONGLONG count = scene.layers.empty()
            ? static_cast<LONGLONG>(dataset->Layer(0).FeatureCount())
            : static_cast<LONGLONG>(scene.layers.front().scene.sourceFeatureCount);
        if (count < 0 || count > std::numeric_limits<LONGLONG>::max() - _featureCount)
        {
            throw mwg::CoreError("Map feature count is out of range");
        }

        // Allocate bookkeeping before AddLayer changes the map or viewport.
        _layerFeatureCounts.reserve(_layerFeatureCounts.size() + 1);
        const mwg::LayerId id = _view.AddLayer(std::move(dataset));
        _layerFeatureCounts.emplace_back(id, count);

        _featureCount += count;
        FireViewChange();
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        const CA2W message(exception.what(), CP_UTF8);
        return AtlReportError(
            CLSID_MapWinGISControl,
            message,
            IID_IMapWinGISControl,
            E_FAIL);
    }
}

STDMETHODIMP CMapWinGISControl::ClearLayers()
{
    if (_view.ClearLayers())
    {
        _layerFeatureCounts.clear();
        _featureCount = 0;
        FireViewChange();
    }
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::get_LayerLoaded(VARIANT_BOOL* loaded)
{
    if (loaded == nullptr)
    {
        return E_POINTER;
    }

    *loaded = _view.GetMap().LayerCount() > 0 ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::get_FeatureCount(LONGLONG* count)
{
    if (count == nullptr)
    {
        return E_POINTER;
    }

    *count = _featureCount;
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::get_RenderingBackend(BSTR* backend)
{
    return CopyToBstr(BackendName(_renderer.GetActiveBackend()), backend);
}

STDMETHODIMP CMapWinGISControl::get_RenderingStatus(BSTR* status)
{
    return CopyToBstr(_renderer.GetDiagnostic(), status);
}

STDMETHODIMP CMapWinGISControl::get_RenderingBackendPreference(BSTR* preference)
{
    return CopyToBstr(PreferenceName(_renderer.GetPreference()), preference);
}

STDMETHODIMP CMapWinGISControl::put_RenderingBackendPreference(BSTR preference)
{
    if (preference == nullptr)
    {
        return E_INVALIDARG;
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
        return E_INVALIDARG;
    }

    FireViewChange();
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::GetDropTarget(IDropTarget** dropTarget)
{
    if (dropTarget == nullptr)
    {
        return E_POINTER;
    }

    *dropTarget = static_cast<IDropTarget*>(this);
    AddRef();
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::DragEnter(
    IDataObject* data,
    DWORD,
    POINTL,
    DWORD* effect)
{
    if (effect == nullptr)
    {
        return E_POINTER;
    }

    const DWORD allowedEffects = *effect;
    *effect = DROPEFFECT_NONE;
    _dropAcceptsFiles =
        !_dropQueueClosed &&
        (allowedEffects & DROPEFFECT_COPY) != 0 && HasFileDropFormat(data);
    if (_dropAcceptsFiles)
    {
        *effect = DROPEFFECT_COPY;
    }
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::DragOver(DWORD, POINTL, DWORD* effect)
{
    if (effect == nullptr)
    {
        return E_POINTER;
    }

    if (_dropAcceptsFiles && (*effect & DROPEFFECT_COPY) != 0)
    {
        *effect = DROPEFFECT_COPY;
    }
    else
    {
        *effect = DROPEFFECT_NONE;
    }
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::DragLeave()
{
    _dropAcceptsFiles = false;
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::Drop(
    IDataObject* data,
    DWORD,
    POINTL,
    DWORD* effect)
{
    if (effect == nullptr)
    {
        return E_POINTER;
    }

    const DWORD allowedEffects = *effect;
    *effect = DROPEFFECT_NONE;
    _dropAcceptsFiles = false;
    if ((allowedEffects & DROPEFFECT_COPY) == 0)
    {
        return S_OK;
    }
    if (_dropQueueClosed)
    {
        return E_UNEXPECTED;
    }

    std::vector<std::filesystem::path> paths;
    try
    {
        const HRESULT extractResult = GetDroppedPaths(data, paths);
        if (FAILED(extractResult))
        {
            ::OutputDebugStringA("MapWinGIS rejected dropped data without usable file paths.\n");
            return extractResult;
        }
        if (paths.size() > static_cast<std::size_t>(std::numeric_limits<LONG>::max()))
        {
            return E_INVALIDARG;
        }
        if (_dropDispatchWindow == nullptr)
        {
            _dropDispatchWindow = std::make_shared<mwg::activex::GdalDispatchWindow>();
            if (_dropDispatchWindow->Create(
                    &CMapWinGISControl::DispatchDroppedFiles, this) == nullptr)
            {
                const DWORD error = ::GetLastError();
                _dropDispatchWindow.reset();
                return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
            }
        }
        _pendingDrops.push_back(std::move(paths));
        if (!::PostMessageW(
                _dropDispatchWindow->m_hWnd,
                mwg::activex::kGdalDispatchMessage, 0, 0))
        {
            const DWORD error = ::GetLastError();
            _pendingDrops.pop_back();
            return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
        }
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetNavigationErrorInfo(exception.what(), E_FAIL);
    }
}

HRESULT CMapWinGISControl::LoadDroppedFile(
    const std::filesystem::path& path) noexcept
{
    try
    {
        std::error_code fileError;
        if (!std::filesystem::exists(path, fileError))
        {
            return fileError
                ? HRESULT_FROM_WIN32(static_cast<DWORD>(fileError.value()))
                : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
        const std::wstring& pathText = path.native();
        if (pathText.size() > std::numeric_limits<UINT>::max())
        {
            return E_INVALIDARG;
        }
        ATL::CComBSTR pathBstr;
        pathBstr.Attach(::SysAllocStringLen(
            pathText.data(), static_cast<UINT>(pathText.size())));
        if (pathBstr.m_str == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        return LoadVectorDataset(pathBstr);
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const ATL::CAtlException& exception)
    {
        return exception.m_hr;
    }
    catch (const std::exception& exception)
    {
        ::OutputDebugStringA(exception.what());
        return E_FAIL;
    }
}

void CMapWinGISControl::LoadDroppedFiles(
    const std::vector<std::filesystem::path>& paths) noexcept
{
    LONG loadedCount = 0;
    LONG failedCount = 0;
    HRESULT firstFailure = S_OK;
    for (const std::filesystem::path& path : paths)
    {
        if (_dropQueueClosed)
        {
            return;
        }
        const HRESULT loadResult = LoadDroppedFile(path);
        if (FAILED(loadResult))
        {
            ++failedCount;
            LogDropFailure(path, loadResult);
            if (SUCCEEDED(firstFailure))
            {
                firstFailure = loadResult;
            }
        }
        else
        {
            ++loadedCount;
        }
        ATL::CComVariant arguments[2];
        arguments[0] = loadResult;
        arguments[1].vt = VT_BSTR;
        arguments[1].bstrVal = ::SysAllocString(path.c_str());
        if (arguments[1].bstrVal == nullptr)
        {
            ::OutputDebugStringA("MapWinGIS could not allocate the dropped path event.\n");
            if (SUCCEEDED(firstFailure))
            {
                firstFailure = E_OUTOFMEMORY;
            }
        }
        else
        {
            FireDropEvent(3, arguments, 2);
        }
    }

    if (_dropQueueClosed)
    {
        return;
    }
    if (loadedCount != 0)
    {
        try
        {
            if (_view.FitToMap())
            {
                FireViewChange();
            }
        }
        catch (const std::bad_alloc&)
        {
            if (SUCCEEDED(firstFailure))
            {
                firstFailure = E_OUTOFMEMORY;
            }
        }
        catch (const std::exception& exception)
        {
            ::OutputDebugStringA(exception.what());
            if (SUCCEEDED(firstFailure))
            {
                firstFailure = E_FAIL;
            }
        }
    }
    ATL::CComVariant arguments[]{
        ATL::CComVariant(firstFailure),
        ATL::CComVariant(failedCount),
        ATL::CComVariant(loadedCount)};
    FireDropEvent(4, arguments, 3);
}

void CMapWinGISControl::FireDropEvent(
    const DISPID eventId,
    ATL::CComVariant* arguments,
    const UINT count) noexcept
{
    try
    {
        std::vector<ATL::CComPtr<IUnknown>> sinks;
        for (int index = 0; index < m_vec.GetSize(); ++index)
        {
            if (IUnknown* sink = m_vec.GetAt(index))
            {
                sinks.emplace_back(sink);
            }
        }
        for (const auto& unknown : sinks)
        {
            if (_dropQueueClosed)
            {
                break;
            }
            ATL::CComQIPtr<IDispatch> sink(unknown);
            if (sink == nullptr)
            {
                ::OutputDebugStringA("MapWinGIS drop event sink does not support IDispatch.\n");
                continue;
            }
            ATL::CComVariant copy[3];
            for (UINT index = 0; index < count; ++index)
            {
                const HRESULT result = copy[index].Copy(&arguments[index]);
                if (FAILED(result))
                {
                    AtlThrow(result);
                }
            }
            DISPPARAMS parameters{copy, nullptr, count, 0};
            if (FAILED(sink->Invoke(eventId, IID_NULL, LOCALE_USER_DEFAULT,
                    DISPATCH_METHOD, &parameters, nullptr, nullptr, nullptr)))
            {
                ::OutputDebugStringA("MapWinGIS drop event callback failed.\n");
            }
        }
    }
    catch (const ATL::CAtlException&)
    {
        ::OutputDebugStringA("MapWinGIS could not allocate drop event arguments.\n");
    }
    catch (const std::exception& exception)
    {
        ::OutputDebugStringA(exception.what());
    }
}

void CMapWinGISControl::DispatchDroppedFiles(void* const control) noexcept
{
    auto* const instance = static_cast<CMapWinGISControl*>(control);
    ATL::CComPtr<CMapWinGISControl> lifetime(instance);
    if (instance->_dispatchingDrop || instance->_dropQueueClosed)
    {
        return;
    }
    instance->_dispatchingDrop = true;
    while (!instance->_pendingDrops.empty() && !instance->_dropQueueClosed)
    {
        auto paths = std::move(instance->_pendingDrops.front());
        instance->_pendingDrops.pop_front();
        instance->LoadDroppedFiles(paths);
    }
    instance->_dispatchingDrop = false;
}

void CMapWinGISControl::CloseDropQueue() noexcept
{
    _dropQueueClosed = true;
    _dropAcceptsFiles = false;
    _pendingDrops.clear();
    if (_dropDispatchWindow != nullptr)
    {
        _dropDispatchWindow->Destroy();
        _dropDispatchWindow.reset();
    }
}

LRESULT CMapWinGISControl::OnCreate(UINT, WPARAM, LPARAM, BOOL& handled)
{
    const HRESULT result = ::RegisterDragDrop(m_hWndCD, this);
    if (FAILED(result))
    {
        (void)AtlReportError(CLSID_MapWinGISControl,
            L"Could not register the window drop target", IID_IMapWinGISControl4, result);
        handled = TRUE;
        return -1;
    }
    _dropTargetWindow = m_hWndCD;
    handled = FALSE;
    return 0;
}

void CMapWinGISControl::RevokeWindowDropTarget() noexcept
{
    if (_dropTargetWindow != nullptr)
    {
        const HWND window = std::exchange(_dropTargetWindow, nullptr);
        if (FAILED(::RevokeDragDrop(window)))
        {
            ::OutputDebugStringA("MapWinGIS could not revoke the window drop target.\n");
        }
    }
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
        return false;
    }
}

bool CMapWinGISControl::GetPointerPosition(
    const LPARAM position,
    const bool screenCoordinates,
    mwg::ScreenPoint& point) const noexcept
{
    LONG x = GET_X_LPARAM(position);
    LONG y = GET_Y_LPARAM(position);
    if (screenCoordinates)
    {
        HWND window = m_hWndCD;
        if (m_bWndLess)
        {
            if (m_spInPlaceSite == nullptr ||
                FAILED(m_spInPlaceSite->GetWindow(&window)))
            {
                return false;
            }
        }
        if (window == nullptr)
        {
            return false;
        }

        POINT positionInClient{x, y};
        if (!::ScreenToClient(window, &positionInClient))
        {
            return false;
        }
        x = positionInClient.x;
        y = positionInClient.y;
    }

    if (m_bWndLess)
    {
        x -= m_rcPos.left;
        y -= m_rcPos.top;
    }
    point = {static_cast<double>(x), static_cast<double>(y)};
    return true;
}

HRESULT CMapWinGISControl::SetPointerCapture(const bool capture) noexcept
{
    if (m_bWndLess)
    {
        if (m_spInPlaceSite == nullptr)
        {
            return E_UNEXPECTED;
        }
        return m_spInPlaceSite->SetCapture(capture ? TRUE : FALSE);
    }

    if (m_hWndCD == nullptr)
    {
        return E_UNEXPECTED;
    }
    if (capture)
    {
        ::SetCapture(m_hWndCD);
        return ::GetCapture() == m_hWndCD ? S_OK : E_FAIL;
    }
    if (::GetCapture() == m_hWndCD && !::ReleaseCapture())
    {
        return HRESULT_FROM_WIN32(::GetLastError());
    }
    return S_OK;
}

void CMapWinGISControl::EndPan(const bool releaseCapture) noexcept
{
    if (!_isPanning)
    {
        return;
    }
    _isPanning = false;
    if (releaseCapture && FAILED(SetPointerCapture(false)))
    {
        ::OutputDebugStringA("MapWinGIS could not release pointer capture after pan.\n");
    }
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
        FireViewChange();
    }
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnMouseWheel(
    UINT,
    const WPARAM wParam,
    const LPARAM lParam,
    BOOL& handled)
{
    if (_view.GetMap().LayerCount() == 0 || !_view.GetViewport().IsValid())
    {
        handled = FALSE;
        return 0;
    }

    const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    mwg::ScreenPoint anchor;
    if (delta == 0 || !GetPointerPosition(lParam, true, anchor))
    {
        handled = FALSE;
        return 0;
    }
    const mwg::Viewport& viewport = _view.GetViewport();
    if (anchor.x < 0.0 || anchor.y < 0.0 ||
        anchor.x >= viewport.Width() || anchor.y >= viewport.Height())
    {
        anchor = {viewport.Width() / 2.0, viewport.Height() / 2.0};
    }

    const double factor = std::pow(
        mwg::MapView::kDefaultZoomFactor,
        static_cast<double>(delta) / WHEEL_DELTA);
    try
    {
        if (_view.ZoomAt(anchor, factor))
        {
            FireViewChange();
        }
    }
    catch (const std::exception& exception)
    {
        TraceNavigationError(exception.what());
        handled = FALSE;
        return 0;
    }
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnLButtonDown(
    UINT,
    WPARAM,
    const LPARAM lParam,
    BOOL& handled)
{
    mwg::ScreenPoint position;
    if (_view.GetMap().LayerCount() == 0 ||
        !_view.GetViewport().IsValid() ||
        !GetPointerPosition(lParam, false, position))
    {
        handled = FALSE;
        return 0;
    }

    const HRESULT captureResult = SetPointerCapture(true);
    if (FAILED(captureResult))
    {
        ::OutputDebugStringA("MapWinGIS could not capture the pointer for pan.\n");
        handled = FALSE;
        return 0;
    }

    _isPanning = true;
    _lastPanPosition = position;
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
        EndPan(true);
        handled = FALSE;
        return 0;
    }

    mwg::ScreenPoint position;
    if (!GetPointerPosition(lParam, false, position))
    {
        handled = TRUE;
        return 0;
    }

    const std::optional<double> resolution = _view.GetViewport().Resolution();
    if (resolution.has_value())
    {
        try
        {
            const double deltaX = (_lastPanPosition.x - position.x) * *resolution;
            const double deltaY = (position.y - _lastPanPosition.y) * *resolution;
            if (_view.Pan(deltaX, deltaY))
            {
                FireViewChange();
            }
        }
        catch (const std::exception& exception)
        {
            TraceNavigationError(exception.what());
            EndPan(true);
            handled = FALSE;
            return 0;
        }
    }

    _lastPanPosition = position;
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnLButtonUp(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    if (_isPanning)
    {
        EndPan(true);
        handled = TRUE;
    }
    else
    {
        handled = FALSE;
    }
    return 0;
}

LRESULT CMapWinGISControl::OnCaptureChanged(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    EndPan(false);
    handled = FALSE;
    return 0;
}

LRESULT CMapWinGISControl::OnCancelMode(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    EndPan(true);
    handled = FALSE;
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
    EndPan(true);
    RevokeWindowDropTarget();
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
}

STDMETHODIMP CMapWinGISControl::Close(const DWORD saveOption)
{
    EndPan(true);
    CloseDropQueue();
    RevokeWindowDropTarget();
    _gdalEvents.Close();
    return ATL::IOleObjectImpl<CMapWinGISControl>::Close(saveOption);
}

STDMETHODIMP CMapWinGISControl::InPlaceDeactivate()
{
    EndPan(true);
    return ATL::IOleInPlaceObjectWindowlessImpl<CMapWinGISControl>::InPlaceDeactivate();
}

STDMETHODIMP CMapWinGISControl::SetObjectRects(LPCRECT position, LPCRECT clip)
{
    if (position == nullptr || clip == nullptr)
    {
        return E_POINTER;
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
            FireViewChange();
        }
    }
    return result;
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

    if (::FillRect(
            drawInfo.hdcDraw,
            &bounds,
            ::GetSysColorBrush(COLOR_WINDOW)) == 0)
    {
        return E_FAIL;
    }

    const LONGLONG width = static_cast<LONGLONG>(bounds.right) - bounds.left;
    const LONGLONG height = static_cast<LONGLONG>(bounds.bottom) - bounds.top;
    if (width <= 0 || height <= 0 ||
        width > mwg::Viewport::kMaxDimension || height > mwg::Viewport::kMaxDimension)
    {
        return S_OK;
    }
    try
    {
        const mwg::rendering::MapRenderScene& scene = _view.Scene();
        // Printing and scaled draws use a snapshot, never the live viewport.
        mwg::Viewport viewport = _view.GetViewport();
        viewport.SetSize(static_cast<int>(width), static_cast<int>(height));
        if (!viewport.HasView())
        {
            (void)viewport.FitExtent(_view.GetMap().GetExtent(), mwg::MapView::kDefaultPadding);
        }
        if (scene.layers.empty() || !viewport.IsValid())
        {
            return S_OK;
        }
        return _renderer.Draw(drawInfo.hdcDraw, bounds, scene, viewport);
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
}

CMapWinGISControl::~CMapWinGISControl()
{
    CloseDropQueue();
    _gdalEvents.Shutdown();
}
