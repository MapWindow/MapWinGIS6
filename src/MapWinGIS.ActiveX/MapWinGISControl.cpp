// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.cpp
 * @brief Implements vector loading, map navigation, ActiveX state reporting,
 * and host drawing.
 *
 * Redraws: every map or view change made through the control goes through
 * its MapView; the control calls FireViewChange when the MapView reports
 * that the frame changed. OnDraw composes the MapView's render scene through
 * its viewport into an offscreen frame buffer and presents the finished
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
#include <cmath>
#include <exception>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windowsx.h>

#include "ComConversion.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/vector/VectorDataset.h"

namespace
{
    constexpr double kMouseWheelZoomFactor = 1.2;

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

CMapWinGISControl::CMapWinGISControl()
    : _gdalEvents(
          m_vec,
          &CMapWinGISControl::DispatchGdalNotifications,
          this)
{
}

STDMETHODIMP CMapWinGISControl::InterfaceSupportsErrorInfo(REFIID interfaceId)
{
    return IsEqualIID(interfaceId, IID_IMapWinGISControl) ||
        IsEqualIID(interfaceId, IID_IMapWinGISControl2) ||
        IsEqualIID(interfaceId, IID_IMapWinGISControl3) ||
        IsEqualIID(interfaceId, IID_IMapWinGISControl4) ||
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
        return SetControlComErrorInfo("The initialized output pointer is null.", IID_IMapWinGISControlGdal, E_POINTER);
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

        const std::filesystem::path datasetPath(pathText);
        std::shared_ptr<mwg::VectorDataset> dataset = mwg::VectorDataset::Open(datasetPath);
        if (dataset->LayerCount() == 0)
        {
            throw mwg::CoreError("Vector dataset contains no layers");
        }

        // Validate before AddLayer can auto-fit or schedule a pending fit.
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
        mwg::rendering::VectorRenderScene preparedScene;
        if (scene.layers.empty())
        {
            preparedScene.sourceFeatureCount = dataset->Layer(0).FeatureCount();
        }
        else
        {
            preparedScene = std::move(scene.layers.front().scene);
        }
        const LONGLONG count = static_cast<LONGLONG>(preparedScene.sourceFeatureCount);
        if (count < 0 || count > std::numeric_limits<LONGLONG>::max() - _featureCount)
        {
            throw mwg::CoreError("Map feature count is out of range");
        }

        const mwg::LayerId id = _view.AddLayer(std::move(dataset), std::move(preparedScene));
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

        _featureCount += count;
        if (layerId != nullptr)
        {
            *layerId = convertedId;
        }
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
    if (backend == nullptr) return SetControlComErrorInfo("The rendering backend output pointer is null.", IID_IMapWinGISControl4, E_POINTER);
    const HRESULT result = CopyToBstr(BackendName(_renderer.GetActiveBackend()), backend);
    return FAILED(result) ? SetControlComErrorInfo("The rendering backend could not be returned.", IID_IMapWinGISControl4, result) : result;
}

STDMETHODIMP CMapWinGISControl::get_RenderingStatus(BSTR* status)
{
    if (status == nullptr) return SetControlComErrorInfo("The rendering status output pointer is null.", IID_IMapWinGISControl4, E_POINTER);
    const HRESULT result = CopyToBstr(_renderer.GetDiagnostic(), status);
    return FAILED(result) ? SetControlComErrorInfo("The rendering status could not be returned.", IID_IMapWinGISControl4, result) : result;
}

STDMETHODIMP CMapWinGISControl::get_RenderingBackendPreference(BSTR* preference)
{
    if (preference == nullptr) return SetControlComErrorInfo("The rendering backend preference output pointer is null.", IID_IMapWinGISControl4, E_POINTER);
    const HRESULT result = CopyToBstr(PreferenceName(_renderer.GetPreference()), preference);
    return FAILED(result) ? SetControlComErrorInfo("The rendering backend preference could not be returned.", IID_IMapWinGISControl4, result) : result;
}

STDMETHODIMP CMapWinGISControl::put_RenderingBackendPreference(BSTR preference)
{
    if (preference == nullptr)
    {
        return SetControlComErrorInfo("The rendering backend preference is null.", IID_IMapWinGISControl4, E_INVALIDARG);
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
        return SetControlComErrorInfo("The rendering backend preference must be Automatic, OpenGL, or GDI.", IID_IMapWinGISControl4, E_INVALIDARG);
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
    if (FAILED(result)) ::OutputDebugStringW(L"MapWinGIS: unable to reactivate GDAL operations.\\n");
    _gdalEvents.OnSize();
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
    CancelMousePan();
    _gdalEvents.Close();
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

    // Screen position of a fixed map point shifts by the center difference
    // divided by the resolution, with the Y axis pointing down. The center
    // accumulates rounding through repeated pan operations, so the offset can
    // sit a tiny amount off an integer; anything within half a pixel rounds to
    // the nearest pixel, which is imperceptible during the drag and corrected
    // by the final render when the drag ends.
    const double offsetX = (cacheCenter->x - center->x) / *resolution;
    const double offsetY = (center->y - cacheCenter->y) / *resolution;
    const LONG shiftX = static_cast<LONG>(std::lround(offsetX));
    const LONG shiftY = static_cast<LONG>(std::lround(offsetY));
    if (std::abs(offsetX - static_cast<double>(shiftX)) >= 0.5 ||
        std::abs(offsetY - static_cast<double>(shiftY)) >= 0.5)
    {
        // A clamped pan (or extreme accumulated drift) leaves a sub-pixel-or-
        // worse offset; re-render so the frame does not jitter between a
        // shifted blit and a true render.
        return false;
    }

    const LONG width = static_cast<LONG>(viewport.Width());
    const LONG height = static_cast<LONG>(viewport.Height());

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
    const bool useCache)
{
    if (!useCache)
    {
        return _renderer.Draw(frameDc, frameBounds, scene, viewport);
    }

    const int width = viewport.Width();
    const int height = viewport.Height();
    if (!_layerCache.Resize(width, height))
    {
        // Without a cache the layers are drawn straight into the frame; the
        // next draw retries the allocation.
        return _renderer.Draw(frameDc, frameBounds, scene, viewport);
    }

    HDC cacheDc = _layerCache.Get();
    const RECT cacheBounds{0, 0, width, height};
    if (::FillRect(
            cacheDc,
            &cacheBounds,
            ::GetSysColorBrush(COLOR_WINDOW)) == 0)
    {
        return _renderer.Draw(frameDc, frameBounds, scene, viewport);
    }

    const HRESULT result = _renderer.Draw(cacheDc, cacheBounds, scene, viewport);
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
    return _renderer.Draw(frameDc, frameBounds, scene, viewport);
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

    // Compose the frame in an offscreen buffer and present it with a single
    // blit, so the visible surface never shows the background erase or a
    // partially drawn frame. Drawing straight to the target device context
    // is the fallback for sizes the buffer cannot hold.
    const bool buffered = drawable &&
        _frameBuffer.Resize(
            static_cast<int>(width),
            static_cast<int>(height));
    HDC frameDc = buffered ? _frameBuffer.Get() : drawInfo.hdcDraw;
    RECT frameBounds = buffered
        ? RECT{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)}
        : bounds;

    // The cache holds a viewport-sized bitmap that is blitted at the frame
    // origin; it is only valid while the frame is the offscreen buffer. A
    // failed presentation blit switches to the target DC and bounds, and
    // must not use the cache.
    bool useLayerCache = buffered;

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
            viewport.SetSize(static_cast<int>(width), static_cast<int>(height));
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
                    useLayerCache);
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
    _gdalEvents.Shutdown();
}
