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
    return CopyToBstr(_renderer.GetDiagnostic(), backend);
}
