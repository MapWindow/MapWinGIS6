// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControl.h
 * @brief Declares the COM-creatable MapWinGIS ActiveX control.
 *
 * Provides automation for vector loading, map navigation, GDAL operations,
 * rendering preferences, and painting polygon layers. The control owns one
 * Core MapView, the only map and view state; it forwards host events to it
 * and draws its render scene through its viewport.
 */
#pragma once

#include "ActiveXModule.h"
#include "resource.h"

#include <cstdint>

#include <atlctl.h>

#include "mapwingis/map/MapView.h"
#include "FrameBuffer.h"
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
    public ATL::IViewObjectExImpl<CMapWinGISControl>,
    public ATL::IOleInPlaceObjectWindowlessImpl<CMapWinGISControl>,
    public ATL::IPersistStreamInitImpl<CMapWinGISControl>,
    public ISupportErrorInfo,
    // Load the default dispatch type information from this OCX, not a
    // potentially older registered copy of the same type library.
    public ATL::IDispatchImpl<
        IMapWinGISControl4,
        &IID_IMapWinGISControl4,
        &LIBID_MapWinGISActiveXLib,
        0xFFFF,
        0xFFFF>,
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
        COM_INTERFACE_ENTRY(IMapWinGISControl4)
        COM_INTERFACE_ENTRY(IMapWinGISControl3)
        COM_INTERFACE_ENTRY(IMapWinGISControl2)
        COM_INTERFACE_ENTRY(IMapWinGISControl)
        COM_INTERFACE_ENTRY(IMapWinGISControlGdal)
        COM_INTERFACE_ENTRY(ISupportErrorInfo)
        COM_INTERFACE_ENTRY(IConnectionPointContainer)
        COM_INTERFACE_ENTRY2(IDispatch, IMapWinGISControl4)
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
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
        MESSAGE_HANDLER(WM_CAPTURECHANGED, OnCaptureChanged)
        MESSAGE_HANDLER(WM_CANCELMODE, OnCancelMode)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
        CHAIN_MSG_MAP(ATL::CComControl<CMapWinGISControl>)
    END_MSG_MAP()

    DECLARE_VIEW_STATUS(VIEWSTATUS_SOLIDBKGND | VIEWSTATUS_OPAQUE)

    STDMETHOD(InterfaceSupportsErrorInfo)(REFIID interfaceId) override;
    STDMETHOD(get_CoreInitialized)(VARIANT_BOOL* initialized) override;
    STDMETHOD(LoadVectorDataset)(BSTR path) override;
    STDMETHOD(ClearLayers)() override;
    STDMETHOD(get_LayerLoaded)(VARIANT_BOOL* loaded) override;
    STDMETHOD(get_FeatureCount)(LONGLONG* count) override;
    STDMETHOD(get_RenderingBackend)(BSTR* backend) override;
    STDMETHOD(get_RenderingStatus)(BSTR* status) override;
    STDMETHOD(get_RenderingBackendPreference)(BSTR* preference) override;
    STDMETHOD(put_RenderingBackendPreference)(BSTR preference) override;
    STDMETHOD(FitToMap)() override;
    STDMETHOD(ZoomIn)(DOUBLE factor) override;
    STDMETHOD(ZoomOut)(DOUBLE factor) override;
    STDMETHOD(Pan)(DOUBLE deltaX, DOUBLE deltaY) override;
    STDMETHOD(ZoomAt)(DOUBLE x, DOUBLE y, DOUBLE factor) override;
    STDMETHOD(ZoomToLayer)(LONGLONG layerId) override;
    STDMETHOD(get_LayerCount)(LONG* count) override;
    STDMETHOD(LoadVectorDatasetEx)(BSTR path, LONGLONG* layerId) override;
    STDMETHOD(GetLayerId)(LONG index, LONGLONG* layerId) override;
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
    STDMETHOD(SetObjectRects)(LPCRECT position, LPCRECT clip) override;

    LRESULT OnLButtonDown(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnMouseMove(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnLButtonUp(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnMouseWheel(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnCancelMode(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnGdalDispatch(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL& handled);
    static void DispatchGdalNotifications(void* control) noexcept;
    HRESULT OnDraw(ATL_DRAWINFO& drawInfo);
    ~CMapWinGISControl();

private:
    HRESULT LoadVectorDatasetImpl(BSTR path, LONGLONG* layerId, REFIID sourceIid);

    /// Resizes the view to a host size, clamped to the supported range, and
    /// returns true when the frame must be redrawn.
    bool ResizeView(LONG width, LONG height) noexcept;
    bool CaptureMouse() noexcept;
    void ReleaseMouseCapture() noexcept;
    void CancelMousePan() noexcept;

    /// Ends an active drag pan through every termination path (button-up,
    /// capture loss, cancel mode, button released): stops panning,
    /// invalidates the layer cache so the final view is rasterized, and
    /// requests a redraw.
    void EndMousePan() noexcept;

    bool GetMousePoint(LPARAM lParam, bool screenCoordinates, POINT& point) const noexcept;

    /// Drops the cached layer frame; the next draw re-renders the layers
    /// into the cache. Called when layers, the view (except a drag pan), the
    /// viewport size, or the backend preference changes.
    void InvalidateLayerCache() noexcept;

    /// Paints the cached layer frame over the already-cleared frame device
    /// context, shifted to the viewport's current view when only the center
    /// moved (a drag pan). Returns true when the cache could be used; false
    /// when the layers must be re-rendered (cache miss, resize, zoom, or a
    /// clamped, sub-pixel pan).
    bool TryBlitCachedLayers(HDC frameDc, const mwg::Viewport& viewport) noexcept;

    /// Draws the layers for the viewport, refreshing the layer cache when
    /// useCache holds. Falls back to drawing straight to frameDc when the
    /// cache cannot be allocated or the cached frame cannot be blitted.
    [[nodiscard]] HRESULT DrawLayersAndUpdateCache(
        HDC frameDc,
        const RECT& frameBounds,
        const mwg::rendering::MapRenderScene& scene,
        const mwg::Viewport& viewport,
        bool useCache,
        bool retainRendererBuffers);

    mwg::MapView _view;
    LONGLONG _featureCount = 0;
    mwg::activex::FrameBuffer _frameBuffer;
    mwg::activex::FrameBuffer _layerCache;
    mwg::Viewport _layerCacheViewport;

    /// COLOR_WINDOW the cached frame was rendered with; a system-color or
    /// high-contrast change makes the cache unusable.
    COLORREF _layerCacheBackground = 0;

    bool _layerCacheValid = false;
    mwg::activex::VectorLayerRenderer _renderer;
    mwg::activex::GdalEventSource _gdalEvents;
    POINT _lastPanPoint{};
    bool _isPanning = false;
};

OBJECT_ENTRY_AUTO(CLSID_MapWinGISControl, CMapWinGISControl)
