// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.cpp
 * @brief Implements vector loading, ActiveX state reporting, and host drawing.
 *
 * Redraws: every map or view change made through the control goes through
 * its MapView; the control calls FireViewChange when the MapView reports
 * that the frame changed. OnDraw draws the MapView's render scene through
 * its viewport, so every layer uses the same transformation.
 */
#include "MapWinGISControl.h"

#include <atlconv.h>

#include <algorithm>
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

        // The first dataset layer goes on top of the map. A layer that
        // cannot be rendered is rejected so the host gets the error now
        // instead of a layer that never draws.
        const mwg::LayerId id = _view.AddLayer(std::move(dataset));
        try
        {
            for (const mwg::rendering::SkippedLayer& skipped : _view.Scene().skippedLayers)
            {
                if (skipped.layerId == id &&
                    skipped.reason == mwg::rendering::LayerSkipReason::Failed)
                {
                    throw mwg::CoreError(skipped.message);
                }
            }

            // Until the control exposes navigation, every load shows the
            // whole map so that each loaded layer is visible.
            (void)_view.FitToMap();
        }
        catch (...)
        {
            (void)_view.RemoveLayer(id);
            throw;
        }

        FireViewChange();
        return S_OK;
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

    *count = 0;
    try
    {
        const mwg::Map& map = _view.GetMap();
        LONGLONG total = 0;
        for (int position = 0; position < map.LayerCount(); ++position)
        {
            total += static_cast<LONGLONG>(map.Layer(position).Source().FeatureCount());
        }
        *count = total;
        return S_OK;
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
    _gdalEvents.Close();
    return ATL::IOleObjectImpl<CMapWinGISControl>::Close(saveOption);
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

    // Windowless hosts never send WM_SIZE, and a host may draw before it
    // does, so the drawn bounds also define the viewport size. Bounds
    // outside the supported size range are not drawn.
    const LONGLONG width = static_cast<LONGLONG>(bounds.right) - bounds.left;
    const LONGLONG height = static_cast<LONGLONG>(bounds.bottom) - bounds.top;
    if (width <= 0 || height <= 0 ||
        width > mwg::Viewport::kMaxDimension || height > mwg::Viewport::kMaxDimension)
    {
        return S_OK;
    }
    (void)ResizeView(static_cast<LONG>(width), static_cast<LONG>(height));

    try
    {
        const mwg::rendering::MapRenderScene& scene = _view.Scene();
        const mwg::Viewport& viewport = _view.GetViewport();
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
