// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.h
 * @brief Declares the COM-creatable MapWinGIS ActiveX control.
 *
 * Provides automation for vector loading, GDAL operations, rendering
 * preferences, and painting the initial polygon layer.
 */
#pragma once

#include "ActiveXModule.h"
#include "GdalDispatchWindow.h"
#include "resource.h"

#include <memory>
#include <optional>
#include <cstdint>

#include <atlctl.h>

#include "mapwingis/rendering/VectorRenderScene.h"
#include "mapwingis/vector/VectorDataset.h"
#include "GdalOperationDispatcher.h"
#include "VectorLayerRenderer.h"

class ATL_NO_VTABLE CMapWinGISControl :
    public ATL::CComObjectRootEx<ATL::CComSingleThreadModel>,
    public ATL::CComCoClass<CMapWinGISControl, &CLSID_MapWinGISControl>,
    public ATL::CComControl<CMapWinGISControl>,
    public ATL::IConnectionPointContainerImpl<CMapWinGISControl>,
    public ATL::IConnectionPointImpl<
        CMapWinGISControl,
        &DIID_IMapWinGISControlEvents,
        ATL::CComDynamicUnkArray>,
    public ATL::IOleControlImpl<CMapWinGISControl>,
    public ATL::IOleObjectImpl<CMapWinGISControl>,
    public ATL::IOleInPlaceActiveObjectImpl<CMapWinGISControl>,
    public ATL::IViewObjectExImpl<CMapWinGISControl>,
    public ATL::IOleInPlaceObjectWindowlessImpl<CMapWinGISControl>,
    public ATL::IPersistStreamInitImpl<CMapWinGISControl>,
    public ATL::IDispatchImpl<
        IMapWinGISControl2,
        &IID_IMapWinGISControl2,
        &LIBID_MapWinGISActiveXLib,
        1,
        0>,
    public ATL::IDispatchImpl<
        IMapWinGISControlGdal,
        &IID_IMapWinGISControlGdal,
        &LIBID_MapWinGISActiveXLib,
        1,
        0>
{
public:
    DECLARE_REGISTRY_RESOURCEID(IDR_MAPWINGISCONTROL)
    DECLARE_NOT_AGGREGATABLE(CMapWinGISControl)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CMapWinGISControl)
        COM_INTERFACE_ENTRY(IMapWinGISControl2)
        COM_INTERFACE_ENTRY(IMapWinGISControl)
        COM_INTERFACE_ENTRY(IMapWinGISControlGdal)
        COM_INTERFACE_ENTRY(IConnectionPointContainer)
        COM_INTERFACE_ENTRY2(IDispatch, IMapWinGISControl2)
        COM_INTERFACE_ENTRY(IViewObjectEx)
        COM_INTERFACE_ENTRY(IViewObject2)
        COM_INTERFACE_ENTRY(IViewObject)
        COM_INTERFACE_ENTRY(IOleInPlaceObjectWindowless)
        COM_INTERFACE_ENTRY(IOleInPlaceObject)
        COM_INTERFACE_ENTRY2(IOleWindow, IOleInPlaceActiveObject)
        COM_INTERFACE_ENTRY(IOleObject)
        COM_INTERFACE_ENTRY(IOleControl)
        COM_INTERFACE_ENTRY(IOleInPlaceActiveObject)
        COM_INTERFACE_ENTRY(IPersistStreamInit)
        COM_INTERFACE_ENTRY2(IPersist, IPersistStreamInit)
    END_COM_MAP()

    BEGIN_CONNECTION_POINT_MAP(CMapWinGISControl)
        CONNECTION_POINT_ENTRY(DIID_IMapWinGISControlEvents)
    END_CONNECTION_POINT_MAP()

    BEGIN_PROP_MAP(CMapWinGISControl)
    END_PROP_MAP()

    BEGIN_MSG_MAP(CMapWinGISControl)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        CHAIN_MSG_MAP(ATL::CComControl<CMapWinGISControl>)
    END_MSG_MAP()

    DECLARE_VIEW_STATUS(VIEWSTATUS_SOLIDBKGND | VIEWSTATUS_OPAQUE)

    STDMETHOD(get_CoreInitialized)(VARIANT_BOOL* initialized) override;
    STDMETHOD(LoadVectorDataset)(BSTR path) override;
    STDMETHOD(get_LayerLoaded)(VARIANT_BOOL* loaded) override;
    STDMETHOD(get_FeatureCount)(LONGLONG* count) override;
    STDMETHOD(get_RenderingBackend)(BSTR* backend) override;
    STDMETHOD(get_RenderingStatus)(BSTR* status) override;
    STDMETHOD(get_RenderingBackendPreference)(BSTR* preference) override;
    STDMETHOD(put_RenderingBackendPreference)(BSTR preference) override;
    STDMETHOD(BeginGdalOperation)(
        BSTR algorithmPath,
        SAFEARRAY* arguments,
        LONGLONG* operationId) override;
    STDMETHOD(CancelGdalOperation)(LONGLONG operationId) override;
    STDMETHOD(EndGdalOperation)(
        LONGLONG operationId,
        VARIANT_BOOL* succeeded,
        VARIANT_BOOL* cancelled,
        LONG* errorCode,
        LONG* callbackErrorCode,
        BSTR* callbackError,
        BSTR* summary,
        BSTR* diagnostics,
        SAFEARRAY** warnings) override;
    STDMETHOD(Close)(DWORD saveOption) override;

    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnGdalDispatch(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled);
    static void DispatchGdalNotifications(void* control) noexcept;
    HRESULT OnDraw(ATL_DRAWINFO& drawInfo);
    ~CMapWinGISControl();

private:
    static bool PostGdalDispatchMessage(void* window) noexcept;
    HRESULT FireGdalProgress(
        const mwg::activex::GdalOperationNotification& notification);
    HRESULT FireGdalOperationCompleted(const std::uint64_t operationId);
    void DestroyGdalDispatchWindow() noexcept;
    void RecordGdalCallbackError(
        std::uint64_t operationId,
        HRESULT result) noexcept;

    std::unique_ptr<mwg::VectorDataset> _dataset;
    std::optional<mwg::rendering::VectorRenderScene> _scene;
    mwg::activex::VectorLayerRenderer _renderer;
    mwg::activex::GdalOperationDispatcher _gdalDispatcher;
    mwg::activex::GdalDispatchWindow _gdalDispatchWindow;
    bool _gdalDispatching = false;
    bool _gdalDispatchWindowDestroyPending = false;
    bool _gdalClosing = false;
    LONGLONG _featureCount = 0;
};

OBJECT_ENTRY_AUTO(CLSID_MapWinGISControl, CMapWinGISControl)
