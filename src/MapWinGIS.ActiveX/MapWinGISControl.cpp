// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.cpp
 * @brief Implements vector loading, map navigation, ActiveX state reporting,
 * and host drawing.
 *
 * Redraws: every map or view change made through the control goes through
 * its MapView; the control calls FireViewChange when the MapView reports
 * that the frame changed. OnDraw draws the MapView's render scene through
 * its viewport, so every layer uses the same transformation.
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
    using mwg::activex::ComConversion::CreateStringArray;
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
            return E_OUTOFMEMORY;
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
            return E_FAIL;
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
    catch (...)
    {
        return E_FAIL;
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
    catch (...)
    {
        return E_FAIL;
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
        return E_POINTER;
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
            return E_UNEXPECTED;
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
        FireViewChange();
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& exception)
    {
        return SetControlComErrorInfo(exception.what(), sourceIid, E_FAIL);
    }
    catch (...)
    {
        return E_FAIL;
    }
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

STDMETHODIMP CMapWinGISControl::FitToMap()
{
    bool changed = false;
    const HRESULT result = RunViewOperation(
        [this]() { return _view.FitToMap(); },
        changed);
    if (SUCCEEDED(result) && changed)
    {
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
        FireViewChange();
    }
    return result;
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

    return CopyLayerId(map.Layer(index).Id(), layerId);
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
            SUCCEEDED(m_spInPlaceSite->SetCapture(TRUE));
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
        ReleaseMouseCapture();
    }
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
        CancelMousePan();
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

    CancelMousePan();
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
    _isPanning = false;
    handled = TRUE;
    return 0;
}

LRESULT CMapWinGISControl::OnCancelMode(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    CancelMousePan();
    handled = TRUE;
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
    CancelMousePan();
    _gdalEvents.Shutdown();
}
