// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.cpp
 * @brief Implements vector loading, ActiveX state reporting, and host drawing.
 */
#include "MapWinGISControl.h"

#include <atlconv.h>

#include <exception>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"

namespace
{
    [[nodiscard]] HRESULT CopyToBstr(const std::wstring_view value, BSTR* result) noexcept
    {
        if (result == nullptr)
        {
            return E_POINTER;
        }

        *result = ::SysAllocStringLen(
            value.data(),
            static_cast<UINT>(value.size()));
        return *result != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    [[nodiscard]] HRESULT BstrToUtf8(const BSTR value, std::string& result)
    {
        if (value == nullptr)
        {
            return E_INVALIDARG;
        }

        const UINT length = ::SysStringLen(value);
        if (length > static_cast<UINT>(std::numeric_limits<int>::max()) ||
            std::wstring_view(value, length).find(L'\0') != std::wstring_view::npos)
        {
            return E_INVALIDARG;
        }
        if (length == 0)
        {
            result.clear();
            return S_OK;
        }

        const int required = ::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value,
            static_cast<int>(length),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (required == 0)
        {
            return E_INVALIDARG;
        }

        result.resize(static_cast<std::size_t>(required));
        if (::WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value,
                static_cast<int>(length),
                result.data(),
                required,
                nullptr,
                nullptr) == 0)
        {
            return E_INVALIDARG;
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT Utf8ToBstr(const std::string_view value, BSTR* result)
    {
        if (result == nullptr)
        {
            return E_POINTER;
        }
        *result = nullptr;
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            return E_INVALIDARG;
        }
        if (value.empty())
        {
            *result = ::SysAllocStringLen(nullptr, 0);
            return *result != nullptr ? S_OK : E_OUTOFMEMORY;
        }

        const int required = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0);
        if (required == 0)
        {
            return E_INVALIDARG;
        }

        BSTR converted = ::SysAllocStringLen(nullptr, static_cast<UINT>(required));
        if (converted == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        if (::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                converted,
                required) == 0)
        {
            ::SysFreeString(converted);
            return E_INVALIDARG;
        }

        *result = converted;
        return S_OK;
    }

    [[nodiscard]] HRESULT CopyStringArray(
        SAFEARRAY* source,
        std::vector<std::string>& values)
    {
        if (source == nullptr)
        {
            return S_OK;
        }
        if (::SafeArrayGetDim(source) != 1)
        {
            return E_INVALIDARG;
        }

        VARTYPE elementType = VT_EMPTY;
        if (FAILED(::SafeArrayGetVartype(source, &elementType)) ||
            elementType != VT_BSTR)
        {
            return E_INVALIDARG;
        }

        LONG lowerBound = 0;
        LONG upperBound = -1;
        HRESULT result = ::SafeArrayGetLBound(source, 1, &lowerBound);
        if (FAILED(result))
        {
            return result;
        }
        result = ::SafeArrayGetUBound(source, 1, &upperBound);
        if (FAILED(result))
        {
            return result;
        }
        if (upperBound < lowerBound)
        {
            return S_OK;
        }

        const auto count =
            static_cast<unsigned long long>(
                static_cast<long long>(upperBound) -
                static_cast<long long>(lowerBound) + 1);
        if (count > static_cast<unsigned long long>(values.max_size()))
        {
            return E_INVALIDARG;
        }
        values.reserve(static_cast<std::size_t>(count));

        for (LONG index = lowerBound;; ++index)
        {
            BSTR argument = nullptr;
            result = ::SafeArrayGetElement(source, &index, &argument);
            if (FAILED(result))
            {
                return result;
            }

            std::string converted;
            result = BstrToUtf8(argument, converted);
            ::SysFreeString(argument);
            if (FAILED(result))
            {
                return result;
            }
            values.push_back(std::move(converted));
            if (index == upperBound)
            {
                break;
            }
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT CreateWarningArray(
        const std::vector<std::string>& warnings,
        SAFEARRAY** result)
    {
        if (result == nullptr)
        {
            return E_POINTER;
        }
        *result = nullptr;
        if (warnings.empty())
        {
            return S_OK;
        }
        if (warnings.size() >
            static_cast<std::size_t>(std::numeric_limits<LONG>::max()))
        {
            return E_INVALIDARG;
        }

        SAFEARRAY* array = ::SafeArrayCreateVector(
            VT_BSTR,
            0,
            static_cast<ULONG>(warnings.size()));
        if (array == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        for (std::size_t offset = 0; offset < warnings.size(); ++offset)
        {
            BSTR warning = nullptr;
            HRESULT status = Utf8ToBstr(warnings[offset], &warning);
            if (FAILED(status))
            {
                ::SafeArrayDestroy(array);
                return status;
            }

            LONG index = static_cast<LONG>(offset);
            status = ::SafeArrayPutElement(array, &index, warning);
            ::SysFreeString(warning);
            if (FAILED(status))
            {
                ::SafeArrayDestroy(array);
                return status;
            }
        }

        *result = array;
        return S_OK;
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

    [[nodiscard]] std::string FormatHresult(const HRESULT errorCode)
    {
        return "COM event callback failed with HRESULT " +
            std::to_string(static_cast<unsigned long>(errorCode)) + ".";
    }

    void UnlockActiveXModule(void*) noexcept
    {
        _AtlModule.Unlock();
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
    if (!_gdalDispatchWindow.IsWindow())
    {
        if (_gdalDispatchWindow.Create(
                &CMapWinGISControl::DispatchGdalNotifications,
                this) == nullptr)
        {
            return HRESULT_FROM_WIN32(::GetLastError());
        }
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

    _gdalDispatcher.SetWakeCallback(
        &CMapWinGISControl::PostGdalDispatchMessage,
        _gdalDispatchWindow.m_hWnd);
    _gdalDispatcher.SetWorkerFinishedCallback(
        &UnlockActiveXModule,
        nullptr);
    _AtlModule.Lock();
    try
    {
        *operationId = static_cast<LONGLONG>(
            _gdalDispatcher.Begin(
                std::move(algorithm),
                std::move(commandLineArguments)));
        return S_OK;
    }
    catch (const std::exception& exception)
    {
        _AtlModule.Unlock();
        return SetComErrorInfo(exception.what(), E_FAIL);
    }
}

STDMETHODIMP CMapWinGISControl::CancelGdalOperation(const LONGLONG operationId)
{
    if (operationId <= 0)
    {
        return E_INVALIDARG;
    }

    switch (_gdalDispatcher.Cancel(static_cast<std::uint64_t>(operationId)))
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

    mwg::activex::GdalOperationSnapshot operationResult;
    mwg::activex::GdalEndResult endResult = mwg::activex::GdalEndResult::NotFound;
    try
    {
        endResult = _gdalDispatcher.GetResult(
            static_cast<std::uint64_t>(operationId),
            operationResult);
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    if (endResult == mwg::activex::GdalEndResult::Pending)
    {
        return E_PENDING;
    }
    if (endResult == mwg::activex::GdalEndResult::NotFound)
    {
        return E_INVALIDARG;
    }

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
    result = CreateWarningArray(operationResult.warnings, &warningArray);
    if (FAILED(result))
    {
        ::SysFreeString(callbackErrorText);
        ::SysFreeString(summaryText);
        ::SysFreeString(diagnosticText);
        return result;
    }

    mwg::activex::GdalOperationSnapshot endedResult;
    const auto cleanupResult = _gdalDispatcher.End(
        static_cast<std::uint64_t>(operationId),
        endedResult);
    if (cleanupResult != mwg::activex::GdalEndResult::Ended)
    {
        ::SysFreeString(callbackErrorText);
        ::SysFreeString(summaryText);
        ::SysFreeString(diagnosticText);
        if (warningArray != nullptr)
        {
            ::SafeArrayDestroy(warningArray);
        }
        return cleanupResult == mwg::activex::GdalEndResult::Pending
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
        std::unique_ptr<mwg::VectorDataset> dataset = mwg::VectorDataset::Open(datasetPath);
        if (dataset->LayerCount() == 0)
        {
            throw mwg::CoreError("Vector dataset contains no layers");
        }

        mwg::VectorLayer& layer = dataset->Layer(0);
        mwg::rendering::VectorRenderScene scene = mwg::rendering::CreateVectorRenderScene(layer);
        const LONGLONG featureCount = static_cast<LONGLONG>(layer.FeatureCount());

        _dataset = std::move(dataset);
        _scene = std::move(scene);
        _featureCount = featureCount;
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

    *loaded = _scene.has_value() ? VARIANT_TRUE : VARIANT_FALSE;
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

LRESULT CMapWinGISControl::OnSize(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    FireViewChange();
    handled = TRUE;
    return 0;
}

bool CMapWinGISControl::PostGdalDispatchMessage(void* const window) noexcept
{
    return ::PostMessageW(
        static_cast<HWND>(window),
        mwg::activex::kGdalDispatchMessage,
        0,
        0) != FALSE;
}

LRESULT CMapWinGISControl::OnGdalDispatch(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    handled = TRUE;
    for (const mwg::activex::GdalOperationNotification& notification :
         _gdalDispatcher.DrainNotifications())
    {
        const HRESULT result =
            notification.kind == mwg::activex::GdalNotificationKind::Progress
            ? FireGdalProgress(notification)
            : FireGdalOperationCompleted(notification.operationId);
        if (FAILED(result))
        {
            _gdalDispatcher.RecordCallbackError(
                notification.operationId,
                result,
                FormatHresult(result));
        }
    }
    return 0;
}

LRESULT CMapWinGISControl::OnDestroy(
    UINT,
    WPARAM,
    LPARAM,
    BOOL& handled)
{
    _gdalDispatcher.Shutdown();
    _gdalDispatchWindow.Destroy();
    handled = FALSE;
    return 0;
}

void CMapWinGISControl::DispatchGdalNotifications(void* const control) noexcept
{
    auto* const instance = static_cast<CMapWinGISControl*>(control);
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

HRESULT CMapWinGISControl::FireGdalProgress(
    const mwg::activex::GdalOperationNotification& notification)
{
    BSTR message = nullptr;
    HRESULT result = Utf8ToBstr(notification.message, &message);
    if (FAILED(result))
    {
        return result;
    }

    std::vector<ATL::CComPtr<IUnknown>> sinks;
    try
    {
        sinks.reserve(static_cast<std::size_t>(m_vec.GetSize()));
        for (int index = 0; index < m_vec.GetSize(); ++index)
        {
            IUnknown* sink = m_vec.GetAt(index);
            if (sink != nullptr)
            {
                sinks.emplace_back(sink);
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        ::SysFreeString(message);
        return E_OUTOFMEMORY;
    }

    HRESULT firstError = S_OK;
    for (const ATL::CComPtr<IUnknown>& unknown : sinks)
    {
        ATL::CComQIPtr<IDispatch> sink(unknown);
        if (sink == nullptr)
        {
            if (SUCCEEDED(firstError))
            {
                firstError = E_NOINTERFACE;
            }
            continue;
        }

        VARIANTARG arguments[3]{};
        arguments[0].vt = VT_BSTR;
        arguments[0].bstrVal = message;
        arguments[1].vt = VT_R8;
        arguments[1].dblVal = notification.progress;
        arguments[2].vt = VT_I8;
        arguments[2].llVal = static_cast<LONGLONG>(notification.operationId);
        DISPPARAMS parameters{arguments, nullptr, 3, 0};
        result = sink->Invoke(
            1,
            IID_NULL,
            LOCALE_USER_DEFAULT,
            DISPATCH_METHOD,
            &parameters,
            nullptr,
            nullptr,
            nullptr);
        if (FAILED(result) && SUCCEEDED(firstError))
        {
            firstError = result;
        }
        arguments[0].vt = VT_EMPTY;
        arguments[0].bstrVal = nullptr;
    }
    ::SysFreeString(message);
    return firstError;
}

HRESULT CMapWinGISControl::FireGdalOperationCompleted(
    const std::uint64_t operationId)
{
    std::vector<ATL::CComPtr<IUnknown>> sinks;
    try
    {
        sinks.reserve(static_cast<std::size_t>(m_vec.GetSize()));
        for (int index = 0; index < m_vec.GetSize(); ++index)
        {
            IUnknown* sink = m_vec.GetAt(index);
            if (sink != nullptr)
            {
                sinks.emplace_back(sink);
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }

    HRESULT firstError = S_OK;
    for (const ATL::CComPtr<IUnknown>& unknown : sinks)
    {
        ATL::CComQIPtr<IDispatch> sink(unknown);
        if (sink == nullptr)
        {
            if (SUCCEEDED(firstError))
            {
                firstError = E_NOINTERFACE;
            }
            continue;
        }

        VARIANTARG argument{};
        argument.vt = VT_I8;
        argument.llVal = static_cast<LONGLONG>(operationId);
        DISPPARAMS parameters{&argument, nullptr, 1, 0};
        const HRESULT result = sink->Invoke(
            2,
            IID_NULL,
            LOCALE_USER_DEFAULT,
            DISPATCH_METHOD,
            &parameters,
            nullptr,
            nullptr,
            nullptr);
        if (FAILED(result) && SUCCEEDED(firstError))
        {
            firstError = result;
        }
    }
    return firstError;
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

    return _scene.has_value()
        ? _renderer.Draw(drawInfo.hdcDraw, bounds, *_scene)
        : S_OK;
}

CMapWinGISControl::~CMapWinGISControl()
{
    _gdalDispatcher.Shutdown();
    _gdalDispatchWindow.Destroy();
}
