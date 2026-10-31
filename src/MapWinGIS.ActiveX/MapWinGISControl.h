// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.h
 * @brief Declares the COM-creatable MapWinGIS ActiveX control.
 *
 * Provides automation for vector, raster, and XYZ tile loading, map
 * navigation, layer ordering, file drop, GDAL operations, and rendering
 * preferences. The control owns one Core MapView, the only map and view
 * state; it forwards host events to it and draws its render scene through its
 * viewport.
 */
#pragma once

#include "ActiveXModule.h"
#include "resource.h"

#include <array>
#include <chrono>
#include <cstdint>

#include <atlctl.h>

#include "mapwingis/map/MapView.h"
#include "GdalEventSource.h"
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
    public IDropTarget,
    public ATL::IViewObjectExImpl<CMapWinGISControl>,
    public ATL::IOleInPlaceObjectWindowlessImpl<CMapWinGISControl>,
    public ATL::IPersistStreamInitImpl<CMapWinGISControl>,
    public ATL::IDispatchImpl<
        IMapWinGISControl7,
        &IID_IMapWinGISControl7,
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
    CMapWinGISControl();

    DECLARE_REGISTRY_RESOURCEID(IDR_MAPWINGISCONTROL)
    DECLARE_NOT_AGGREGATABLE(CMapWinGISControl)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CMapWinGISControl)
        COM_INTERFACE_ENTRY(IMapWinGISControl7)
        COM_INTERFACE_ENTRY(IMapWinGISControl6)
        COM_INTERFACE_ENTRY(IMapWinGISControl5)
        COM_INTERFACE_ENTRY(IMapWinGISControl4)
        COM_INTERFACE_ENTRY(IMapWinGISControl3)
        COM_INTERFACE_ENTRY(IMapWinGISControl2)
        COM_INTERFACE_ENTRY(IMapWinGISControl)
        COM_INTERFACE_ENTRY(IMapWinGISControlGdal)
        COM_INTERFACE_ENTRY(IDropTarget)
        COM_INTERFACE_ENTRY(IConnectionPointContainer)
        COM_INTERFACE_ENTRY2(IDispatch, IMapWinGISControl7)
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
        MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
        MESSAGE_HANDLER(WM_CAPTURECHANGED, OnCaptureChanged)
        MESSAGE_HANDLER(WM_CANCELMODE, OnCancelMode)
        MESSAGE_HANDLER(WM_POINTERDOWN, OnPointerDown)
        MESSAGE_HANDLER(WM_POINTERUPDATE, OnPointerUpdate)
        MESSAGE_HANDLER(WM_POINTERUP, OnPointerUp)
        MESSAGE_HANDLER(WM_POINTERCAPTURECHANGED, OnPointerCaptureChanged)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        CHAIN_MSG_MAP(ATL::CComControl<CMapWinGISControl>)
    END_MSG_MAP()

    DECLARE_VIEW_STATUS(VIEWSTATUS_SOLIDBKGND | VIEWSTATUS_OPAQUE)

    STDMETHOD(get_LayerCount)(LONG* count) override;
    STDMETHOD(GetLayerId)(LONG index, LONGLONG* layerId) override;
    STDMETHOD(ZoomToMap)(VARIANT_BOOL* changed) override;
    STDMETHOD(ZoomToLayer)(LONGLONG layerId, VARIANT_BOOL* changed) override;
    STDMETHOD(ZoomIn)(VARIANT_BOOL* changed) override;
    STDMETHOD(ZoomOut)(VARIANT_BOOL* changed) override;
    STDMETHOD(RemoveLayer)(LONGLONG layerId, VARIANT_BOOL* changed) override;
    STDMETHOD(SetLayerVisible)(
        LONGLONG layerId,
        VARIANT_BOOL visible,
        VARIANT_BOOL* changed) override;
    STDMETHOD(get_CoreInitialized)(VARIANT_BOOL* initialized) override;
    STDMETHOD(LoadVectorDataset)(BSTR path) override;
    STDMETHOD(LoadRasterDataset)(BSTR path) override;
    STDMETHOD(Pan)(double deltaX, double deltaY, VARIANT_BOOL* changed) override;
    STDMETHOD(ZoomAt)(
        double screenX,
        double screenY,
        double factor,
        VARIANT_BOOL* changed) override;
    STDMETHOD(ZoomInBy)(double factor, VARIANT_BOOL* changed) override;
    STDMETHOD(ZoomOutBy)(double factor, VARIANT_BOOL* changed) override;
    STDMETHOD(get_CanGoBack)(VARIANT_BOOL* canGoBack) override;
    STDMETHOD(get_CanGoForward)(VARIANT_BOOL* canGoForward) override;
    STDMETHOD(GoBack)(VARIANT_BOOL* changed) override;
    STDMETHOD(GoForward)(VARIANT_BOOL* changed) override;
    STDMETHOD(AnimateZoomToMap)(LONG durationMilliseconds) override;
    STDMETHOD(AnimateZoomToLayer)(LONGLONG layerId, LONG durationMilliseconds) override;
    STDMETHOD(get_IsAnimating)(VARIANT_BOOL* isAnimating) override;
    STDMETHOD(StopAnimation)() override;
    STDMETHOD(MoveLayer)(
        LONGLONG layerId,
        LONG newPosition,
        VARIANT_BOOL* changed) override;
    STDMETHOD(LoadXyzTileLayer)(
        BSTR urlTemplate,
        LONG minimumZoom,
        LONG maximumZoom) override;
    STDMETHOD(ClearLayers)() override;
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
    STDMETHOD(GetDropTarget)(IDropTarget** dropTarget) override;
    STDMETHOD(DragEnter)(
        IDataObject* data,
        DWORD keyState,
        POINTL point,
        DWORD* effect) override;
    STDMETHOD(DragOver)(DWORD keyState, POINTL point, DWORD* effect) override;
    STDMETHOD(DragLeave)() override;
    STDMETHOD(Drop)(
        IDataObject* data,
        DWORD keyState,
        POINTL point,
        DWORD* effect) override;
    STDMETHOD(Close)(DWORD saveOption) override;
    STDMETHOD(SetObjectRects)(LPCRECT position, LPCRECT clip) override;

    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnKeyDown(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnMouseWheel(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnLButtonDown(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnMouseMove(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnLButtonUp(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnCancelMode(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnPointerDown(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnPointerUpdate(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnPointerUp(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnPointerCaptureChanged(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnGdalDispatch(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled);
    static void DispatchGdalNotifications(void* control) noexcept;
    HRESULT OnDraw(ATL_DRAWINFO& drawInfo);
    ~CMapWinGISControl();

private:
    /// Resizes the view to a host size, clamped to the supported range, and
    /// returns true when the frame must be redrawn.
    bool ResizeView(LONG width, LONG height) noexcept;
    /// Converts a host pointer position to viewport coordinates.
    bool GetPointerPosition(
        LPARAM position,
        bool screenCoordinates,
        mwg::ScreenPoint& point) const noexcept;
    /// Acquires or releases capture through the active window or windowless site.
    HRESULT SetPointerCapture(bool capture) noexcept;
    /// Ends a drag and optionally releases capture still owned by this control.
    void EndPan(bool releaseCapture) noexcept;
    void EndTouchNavigation() noexcept;
    bool UpdateTouchPointer(UINT32 pointerId, LPARAM position) noexcept;
    void ApplyTouchGesture() noexcept;

    struct TouchPointer
    {
        UINT32 id = 0;
        mwg::ScreenPoint position{};
        bool active = false;
    };

    mwg::MapView _view;
    LONGLONG _featureCount = 0;
    mwg::activex::VectorLayerRenderer _renderer;
    mwg::activex::GdalEventSource _gdalEvents;
    mwg::ScreenPoint _lastPanPosition{};
    std::array<TouchPointer, 2> _touchPointers{};
    mwg::ScreenPoint _lastTouchCenter{};
    double _lastTouchDistance = 0.0;
    std::chrono::steady_clock::time_point _lastAnimationTick{};
    bool _isPanning = false;
    bool _isTouchNavigating = false;
    bool _dropAcceptsFiles = false;
};

OBJECT_ENTRY_AUTO(CLSID_MapWinGISControl, CMapWinGISControl)
