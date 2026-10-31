// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.cpp
 * @brief Implements dataset loading, navigation, file drop, and host drawing.
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
#include "mapwingis/raster/RasterDataset.h"
#include "mapwingis/vector/VectorDataset.h"

namespace
{
    using mwg::activex::ComConversion::BstrToUtf8;
    using mwg::activex::ComConversion::CopyStringArray;
    using mwg::activex::ComConversion::CopyToBstr;
    using mwg::activex::ComConversion::CreateStringArray;
    using mwg::activex::ComConversion::Utf8ToBstr;

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

    *count = _view.GetMap().LayerCount();
    return S_OK;
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

    const mwg::Map& map = _view.GetMap();
    if (index < 0 || index >= map.LayerCount())
    {
        return E_INVALIDARG;
    }

    const std::uint64_t value = map.Layer(index).Id().value;
    if (value > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
    {
        return E_FAIL;
    }
    *layerId = static_cast<LONGLONG>(value);
    return S_OK;
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

        const LONGLONG featureCount = layer->IsRaster()
            ? 0
            : static_cast<LONGLONG>(
                layer->Dataset().Layer(layer->SourceLayerIndex()).FeatureCount());
        if (featureCount < 0 || featureCount > _featureCount)
        {
            return SetNavigationErrorInfo("Layer feature count is out of range", E_FAIL);
        }
        if (_view.RemoveLayer(id))
        {
            _featureCount -= featureCount;
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

    if (_view.SetLayerVisible(
            mwg::LayerId{static_cast<std::uint64_t>(layerId)},
            visible != VARIANT_FALSE))
    {
        *changed = VARIANT_TRUE;
        FireViewChange();
    }
    return S_OK;
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

        (void)_view.AddLayer(std::move(dataset));

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

STDMETHODIMP CMapWinGISControl::LoadRasterDataset(BSTR path)
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

        auto dataset = mwg::RasterDataset::Open(std::filesystem::path(pathText));
        mwg::Map candidate;
        candidate.AddRasterLayer(dataset);
        const mwg::rendering::MapRenderScene scene =
            mwg::rendering::CreateMapRenderScene(candidate);
        if (scene.layers.empty() || !scene.layers.front().raster.has_value())
        {
            throw mwg::CoreError("Raster dataset has no renderable pixels");
        }

        (void)_view.AddRasterLayer(std::move(dataset));
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
            IID_IMapWinGISControl5,
            E_FAIL);
    }
}

STDMETHODIMP CMapWinGISControl::Pan(
    const double deltaX,
    const double deltaY,
    VARIANT_BOOL* changed)
{
    if (changed == nullptr)
    {
        return E_POINTER;
    }
    *changed = VARIANT_FALSE;
    try
    {
        if (_view.Pan(deltaX, deltaY))
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
        return SetNavigationErrorInfo(exception.what(), E_INVALIDARG);
    }
}

STDMETHODIMP CMapWinGISControl::ZoomAt(
    const double screenX,
    const double screenY,
    const double factor,
    VARIANT_BOOL* changed)
{
    if (changed == nullptr)
    {
        return E_POINTER;
    }
    *changed = VARIANT_FALSE;
    try
    {
        if (_view.ZoomAt({screenX, screenY}, factor))
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
        return SetNavigationErrorInfo(exception.what(), E_INVALIDARG);
    }
}

STDMETHODIMP CMapWinGISControl::ZoomInBy(
    const double factor,
    VARIANT_BOOL* changed)
{
    if (changed == nullptr)
    {
        return E_POINTER;
    }
    *changed = VARIANT_FALSE;
    try
    {
        if (_view.ZoomIn(factor))
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
        return SetNavigationErrorInfo(exception.what(), E_INVALIDARG);
    }
}

STDMETHODIMP CMapWinGISControl::ZoomOutBy(
    const double factor,
    VARIANT_BOOL* changed)
{
    if (changed == nullptr)
    {
        return E_POINTER;
    }
    *changed = VARIANT_FALSE;
    try
    {
        if (_view.ZoomOut(factor))
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
        return SetNavigationErrorInfo(exception.what(), E_INVALIDARG);
    }
}

STDMETHODIMP CMapWinGISControl::get_CanGoBack(VARIANT_BOOL* canGoBack)
{
    if (canGoBack == nullptr)
    {
        return E_POINTER;
    }
    *canGoBack = _view.CanGoBack() ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::get_CanGoForward(VARIANT_BOOL* canGoForward)
{
    if (canGoForward == nullptr)
    {
        return E_POINTER;
    }
    *canGoForward = _view.CanGoForward() ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::GoBack(VARIANT_BOOL* changed)
{
    if (changed == nullptr)
    {
        return E_POINTER;
    }
    *changed = VARIANT_FALSE;
    if (_view.GoBack())
    {
        (void)_gdalEvents.SetAnimationTimer(false);
        *changed = VARIANT_TRUE;
        FireViewChange();
    }
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::GoForward(VARIANT_BOOL* changed)
{
    if (changed == nullptr)
    {
        return E_POINTER;
    }
    *changed = VARIANT_FALSE;
    if (_view.GoForward())
    {
        (void)_gdalEvents.SetAnimationTimer(false);
        *changed = VARIANT_TRUE;
        FireViewChange();
    }
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::AnimateZoomToMap(
    const LONG durationMilliseconds)
{
    if (durationMilliseconds <= 0)
    {
        return E_INVALIDARG;
    }
    try
    {
        if (!_view.BeginTransitionToMap(
                std::chrono::milliseconds(durationMilliseconds)))
        {
            return S_FALSE;
        }
        const HRESULT dispatchResult = _gdalEvents.EnsureDispatchWindow();
        if (FAILED(dispatchResult))
        {
            (void)_view.CancelTransition();
            return dispatchResult;
        }
        _lastAnimationTick = std::chrono::steady_clock::now();
        const HRESULT timerResult = _gdalEvents.SetAnimationTimer(true);
        if (FAILED(timerResult))
        {
            (void)_view.CancelTransition();
            return timerResult;
        }
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetNavigationErrorInfo(exception.what(), E_INVALIDARG);
    }
}

STDMETHODIMP CMapWinGISControl::AnimateZoomToLayer(
    const LONGLONG layerId,
    const LONG durationMilliseconds)
{
    if (layerId <= 0 || durationMilliseconds <= 0)
    {
        return E_INVALIDARG;
    }
    try
    {
        if (!_view.BeginTransitionToLayer(
                mwg::LayerId{static_cast<std::uint64_t>(layerId)},
                std::chrono::milliseconds(durationMilliseconds)))
        {
            return S_FALSE;
        }
        const HRESULT dispatchResult = _gdalEvents.EnsureDispatchWindow();
        if (FAILED(dispatchResult))
        {
            (void)_view.CancelTransition();
            return dispatchResult;
        }
        _lastAnimationTick = std::chrono::steady_clock::now();
        const HRESULT timerResult = _gdalEvents.SetAnimationTimer(true);
        if (FAILED(timerResult))
        {
            (void)_view.CancelTransition();
            return timerResult;
        }
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetNavigationErrorInfo(exception.what(), E_INVALIDARG);
    }
}

STDMETHODIMP CMapWinGISControl::get_IsAnimating(VARIANT_BOOL* isAnimating)
{
    if (isAnimating == nullptr)
    {
        return E_POINTER;
    }
    *isAnimating = _view.IsTransitioning() ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CMapWinGISControl::StopAnimation()
{
    const bool stopped = _view.CancelTransition();
    (void)_gdalEvents.SetAnimationTimer(false);
    if (stopped)
    {
        FireViewChange();
        return S_OK;
    }
    return S_FALSE;
}

STDMETHODIMP CMapWinGISControl::ClearLayers()
{
    if (_view.ClearLayers())
    {
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

    std::vector<std::filesystem::path> paths;
    try
    {
        const HRESULT extractResult = GetDroppedPaths(data, paths);
        if (FAILED(extractResult))
        {
            ::OutputDebugStringA("MapWinGIS rejected dropped data without usable file paths.\n");
            return extractResult;
        }
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

    bool loadedAny = false;
    HRESULT firstFailure = S_OK;
    for (const std::filesystem::path& path : paths)
    {
        std::error_code fileError;
        if (!std::filesystem::exists(path, fileError))
        {
            const HRESULT pathResult = fileError
                ? HRESULT_FROM_WIN32(static_cast<DWORD>(fileError.value()))
                : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            LogDropFailure(path, pathResult);
            if (SUCCEEDED(firstFailure))
            {
                firstFailure = pathResult;
            }
            continue;
        }

        HRESULT loadResult = E_FAIL;
        try
        {
            const std::wstring pathText = path.native();
            if (pathText.size() > std::numeric_limits<UINT>::max())
            {
                loadResult = E_INVALIDARG;
            }
            else
            {
                BSTR pathBstr = ::SysAllocStringLen(
                    pathText.data(),
                    static_cast<UINT>(pathText.size()));
                if (pathBstr == nullptr)
                {
                    loadResult = E_OUTOFMEMORY;
                }
                else
                {
                    loadResult = LoadVectorDataset(pathBstr);
                    if (FAILED(loadResult))
                    {
                        loadResult = LoadRasterDataset(pathBstr);
                    }
                    ::SysFreeString(pathBstr);
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            loadResult = E_OUTOFMEMORY;
        }
        catch (const std::exception& exception)
        {
            ::OutputDebugStringA(exception.what());
            if (SUCCEEDED(firstFailure))
            {
                firstFailure = E_FAIL;
            }
        }

        if (FAILED(loadResult))
        {
            LogDropFailure(path, loadResult);
            if (SUCCEEDED(firstFailure))
            {
                firstFailure = loadResult;
            }
            if (loadResult == E_OUTOFMEMORY)
            {
                break;
            }
            continue;
        }
        loadedAny = true;
    }

    if (loadedAny)
    {
        try
        {
            if (_view.FitToMap())
            {
                FireViewChange();
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
        *effect = DROPEFFECT_COPY;
        return FAILED(firstFailure) ? S_FALSE : S_OK;
    }

    return FAILED(firstFailure) ? firstFailure : E_FAIL;
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
    (void)_view.EndNavigationGesture();
    if (releaseCapture && FAILED(SetPointerCapture(false)))
    {
        ::OutputDebugStringA("MapWinGIS could not release pointer capture after pan.\n");
    }
}

void CMapWinGISControl::EndTouchNavigation() noexcept
{
    if (!_isTouchNavigating)
    {
        return;
    }
    for (TouchPointer& pointer : _touchPointers)
    {
        pointer.active = false;
    }
    _lastTouchDistance = 0.0;
    _isTouchNavigating = false;
    (void)_view.EndNavigationGesture();
}

bool CMapWinGISControl::UpdateTouchPointer(
    const UINT32 pointerId,
    const LPARAM position) noexcept
{
    for (TouchPointer& pointer : _touchPointers)
    {
        if (pointer.active && pointer.id == pointerId)
        {
            return GetPointerPosition(position, true, pointer.position);
        }
    }
    return false;
}

void CMapWinGISControl::ApplyTouchGesture() noexcept
{
    const TouchPointer* first = nullptr;
    const TouchPointer* second = nullptr;
    for (const TouchPointer& pointer : _touchPointers)
    {
        if (!pointer.active)
        {
            continue;
        }
        if (first == nullptr)
        {
            first = &pointer;
        }
        else
        {
            second = &pointer;
        }
    }

    try
    {
        const std::optional<double> resolution = _view.GetViewport().Resolution();
        if (resolution.has_value() && first != nullptr && second == nullptr)
        {
            const double deltaX = (_lastPanPosition.x - first->position.x) * *resolution;
            const double deltaY = (first->position.y - _lastPanPosition.y) * *resolution;
            if (_view.Pan(deltaX, deltaY))
            {
                FireViewChange();
            }
            _lastPanPosition = first->position;
        }
        else if (resolution.has_value() && first != nullptr && second != nullptr)
        {
            const mwg::ScreenPoint center{
                (first->position.x + second->position.x) / 2.0,
                (first->position.y + second->position.y) / 2.0};
            const double distance = std::hypot(
                first->position.x - second->position.x,
                first->position.y - second->position.y);
            if (_lastTouchDistance > 0.0 && distance > 0.0)
            {
                const double factor = distance / _lastTouchDistance;
                bool changed = false;
                if (std::isfinite(factor) && factor > 0.0 && factor != 1.0)
                {
                    changed = _view.ZoomAt(center, factor);
                }
                else if (center.x != _lastTouchCenter.x ||
                         center.y != _lastTouchCenter.y)
                {
                    changed = _view.Pan(
                        (_lastTouchCenter.x - center.x) * *resolution,
                        (center.y - _lastTouchCenter.y) * *resolution);
                }
                if (changed)
                {
                    FireViewChange();
                }
            }
            _lastTouchCenter = center;
            _lastTouchDistance = distance;
        }
    }
    catch (const std::exception& exception)
    {
        ::OutputDebugStringA(exception.what());
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

LRESULT CMapWinGISControl::OnKeyDown(
    UINT,
    const WPARAM key,
    LPARAM,
    BOOL& handled)
{
    const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool changed = false;
    try
    {
        if (alt && key == VK_LEFT)
        {
            changed = _view.GoBack();
        }
        else if (alt && key == VK_RIGHT)
        {
            changed = _view.GoForward();
        }
        else if (control && (key == 'Z' || key == 'Y'))
        {
            changed = key == 'Z' ? _view.GoBack() : _view.GoForward();
        }
        else if (_view.GetViewport().IsValid())
        {
            const double step = *_view.GetViewport().Resolution() * 32.0;
            switch (key)
            {
            case VK_LEFT:
                changed = _view.Pan(-step, 0.0);
                break;
            case VK_RIGHT:
                changed = _view.Pan(step, 0.0);
                break;
            case VK_UP:
                changed = _view.Pan(0.0, step);
                break;
            case VK_DOWN:
                changed = _view.Pan(0.0, -step);
                break;
            case VK_ADD:
            case VK_OEM_PLUS:
                changed = _view.ZoomIn();
                break;
            case VK_SUBTRACT:
            case VK_OEM_MINUS:
                changed = _view.ZoomOut();
                break;
            default:
                handled = FALSE;
                return 0;
            }
        }
        else
        {
            handled = FALSE;
            return 0;
        }
        if (changed)
        {
            if (alt || control)
            {
                (void)_gdalEvents.SetAnimationTimer(false);
            }
            FireViewChange();
        }
        handled = TRUE;
    }
    catch (const std::exception& exception)
    {
        ::OutputDebugStringA(exception.what());
        handled = TRUE;
    }
    return 0;
}

LRESULT CMapWinGISControl::OnMouseWheel(
    UINT,
    const WPARAM wParam,
    const LPARAM lParam,
    BOOL& handled)
{
    if (_isTouchNavigating)
    {
        handled = TRUE;
        return 0;
    }
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
        ::OutputDebugStringA(exception.what());
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
    if (_isTouchNavigating)
    {
        handled = TRUE;
        return 0;
    }
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
    (void)_view.BeginNavigationGesture();
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnMouseMove(
    UINT,
    WPARAM,
    const LPARAM lParam,
    BOOL& handled)
{
    if (_isTouchNavigating)
    {
        handled = TRUE;
        return 0;
    }
    if (!_isPanning)
    {
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
            ::OutputDebugStringA(exception.what());
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
    if (_isTouchNavigating)
    {
        handled = TRUE;
        return 0;
    }
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
    EndTouchNavigation();
    handled = FALSE;
    return 0;
}

LRESULT CMapWinGISControl::OnPointerDown(
    UINT,
    const WPARAM wParam,
    const LPARAM lParam,
    BOOL& handled)
{
    const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
    POINTER_INPUT_TYPE pointerType{};
    mwg::ScreenPoint position;
    if (!::GetPointerType(pointerId, &pointerType) ||
        pointerType != PT_TOUCH ||
        _view.GetMap().LayerCount() == 0 ||
        !_view.GetViewport().IsValid() ||
        !GetPointerPosition(lParam, true, position))
    {
        handled = FALSE;
        return 0;
    }

    TouchPointer* available = nullptr;
    for (TouchPointer& pointer : _touchPointers)
    {
        if (pointer.active && pointer.id == pointerId)
        {
            handled = TRUE;
            return 0;
        }
        if (!pointer.active && available == nullptr)
        {
            available = &pointer;
        }
    }
    if (available == nullptr)
    {
        handled = FALSE;
        return 0;
    }

    *available = {pointerId, position, true};
    if (!_isTouchNavigating)
    {
        (void)_view.BeginNavigationGesture();
    }
    _isTouchNavigating = true;
    const std::size_t activeCount = static_cast<std::size_t>(
        _touchPointers[0].active) + static_cast<std::size_t>(_touchPointers[1].active);
    if (activeCount == 1)
    {
        _lastPanPosition = position;
    }
    else
    {
        _lastTouchCenter = {
            (_touchPointers[0].position.x + _touchPointers[1].position.x) / 2.0,
            (_touchPointers[0].position.y + _touchPointers[1].position.y) / 2.0};
        _lastTouchDistance = std::hypot(
            _touchPointers[0].position.x - _touchPointers[1].position.x,
            _touchPointers[0].position.y - _touchPointers[1].position.y);
    }
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnPointerUpdate(
    UINT,
    const WPARAM wParam,
    const LPARAM lParam,
    BOOL& handled)
{
    if (!UpdateTouchPointer(GET_POINTERID_WPARAM(wParam), lParam))
    {
        handled = FALSE;
        return 0;
    }
    ApplyTouchGesture();
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnPointerUp(
    UINT,
    const WPARAM wParam,
    const LPARAM lParam,
    BOOL& handled)
{
    const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
    if (!UpdateTouchPointer(pointerId, lParam))
    {
        handled = FALSE;
        return 0;
    }
    ApplyTouchGesture();
    for (TouchPointer& pointer : _touchPointers)
    {
        if (pointer.active && pointer.id == pointerId)
        {
            pointer.active = false;
            break;
        }
    }
    if (!_touchPointers[0].active && !_touchPointers[1].active)
    {
        EndTouchNavigation();
    }
    else
    {
        const TouchPointer& remaining =
            _touchPointers[0].active ? _touchPointers[0] : _touchPointers[1];
        _lastPanPosition = remaining.position;
        _lastTouchDistance = 0.0;
    }
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnPointerCaptureChanged(
    UINT,
    const WPARAM wParam,
    LPARAM,
    BOOL& handled)
{
    const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
    for (TouchPointer& pointer : _touchPointers)
    {
        if (pointer.active && pointer.id == pointerId)
        {
            EndTouchNavigation();
            handled = TRUE;
            return 0;
        }
    }
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
    EndTouchNavigation();
    (void)_view.CancelTransition();
    (void)_gdalEvents.SetAnimationTimer(false);
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
        if (instance->_view.IsTransitioning())
        {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - instance->_lastAnimationTick);
            if (elapsed.count() > 0)
            {
                instance->_lastAnimationTick = now;
                if (instance->_view.AdvanceTransition(elapsed))
                {
                    instance->FireViewChange();
                }
            }
            if (!instance->_view.IsTransitioning())
            {
                (void)instance->_gdalEvents.SetAnimationTimer(false);
            }
        }
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
    (void)_view.CancelTransition();
    (void)_gdalEvents.SetAnimationTimer(false);
    _gdalEvents.Close();
    return ATL::IOleObjectImpl<CMapWinGISControl>::Close(saveOption);
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
    _gdalEvents.Shutdown();
}
