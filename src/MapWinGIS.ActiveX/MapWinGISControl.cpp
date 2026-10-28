// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.cpp
 * @brief Implements vector loading, ActiveX state reporting, and host drawing.
 */
#include "MapWinGISControl.h"

#include <atlconv.h>

#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

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
