// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapView.h
 * @brief Declares the map view: a map, its viewport, and its render scene.
 *
 * MapView is the single owner of the view state that hosts display: one Map,
 * one Viewport shared by all layers, and the render scene prepared from the
 * map. Hosts such as the ActiveX control change the map and the view only
 * through MapView, so the scene is rebuilt when the map changes and every
 * layer is drawn through the same viewport. MapView holds no windowing, COM,
 * or rendering-backend state.
 */
#pragma once

#include <memory>

#include "mapwingis/core/Api.h"
#include "mapwingis/map/Map.h"
#include "mapwingis/map/Viewport.h"
#include "mapwingis/rendering/MapRenderScene.h"

namespace mwg
{
    class VectorDataset;

    /// A map, the viewport it is shown in, and its render scene.
    ///
    /// Redraws: every mutator returns whether the displayed frame may have
    /// changed. Hosts request a redraw when it returns true and draw Scene()
    /// through GetViewport(). Changes to the layer collection or visibility
    /// mark the scene stale; Scene() rebuilds it on the next call. Edits to
    /// source geometry are not observed; call InvalidateScene after them.
    ///
    /// Initial view: while the viewport has no view, adding a layer fits the
    /// viewport to the map. A fit that cannot happen because the viewport has
    /// no size yet is kept pending and done by the first Resize that gives
    /// the viewport a size, so data loaded before a host window has a size is
    /// fitted once the window appears.
    ///
    /// Navigation: FitToMap, ZoomToLayer, ZoomIn, ZoomOut, ZoomAt, and Pan
    /// change the viewport; they depend on no window, input, or renderer.
    /// A later fit replaces a pending fit, and a navigation operation that
    /// changes the view cancels it, so the latest request wins.
    ///
    /// Not thread-safe; see Map.
    class MWG_CORE_API MapView
    {
    public:
        /// Padding, in pixels, used by FitToMap when none is given.
        static constexpr double kDefaultPadding = 12.0;

        /// Zoom factor used by ZoomIn and ZoomOut when none is given.
        static constexpr double kDefaultZoomFactor = 2.0;

        MapView();
        ~MapView();

        MapView(const MapView&) = delete;
        MapView& operator=(const MapView&) = delete;

        [[nodiscard]] const Map& GetMap() const noexcept;
        [[nodiscard]] const Viewport& GetViewport() const noexcept;

        /// Adds a layer on top of the map; see Map::AddLayer. Fits the view
        /// to the map when the viewport has no view yet.
        /// \throws InvalidArgument as Map::AddLayer does.
        LayerId AddLayer(std::shared_ptr<VectorDataset> dataset, int layerIndex = 0);

        /// Removes a layer; see Map::RemoveLayer. The view is kept.
        bool RemoveLayer(LayerId id) noexcept;

        /// Removes all layers and returns true when there were any. The view
        /// is kept.
        bool ClearLayers() noexcept;

        /// Shows or hides a layer and returns true when its visibility
        /// changed. Returns false when the map has no such layer.
        bool SetLayerVisible(LayerId id, bool visible) noexcept;

        /// Changes the viewport size, keeping the center and resolution so
        /// the map neither moves nor changes scale; see Viewport::SetSize.
        /// Completes a pending fit once both dimensions are positive.
        /// Returns true when the size or view changed.
        /// If reading the pending fit extent throws, the new size stays
        /// applied, the center and resolution stay unchanged, and the
        /// pending fit is discarded. The host must redraw after this error.
        /// \throws InvalidArgument as Viewport::SetSize does.
        /// \throws CoreError when the pending fit extent cannot be read.
        bool Resize(int width, int height);

        /// Fits the viewport to the visible map extent with paddingPixels
        /// free on every side; see Viewport::FitExtent. Returns true when the
        /// view was fitted. When the viewport has no size yet, the fit is
        /// kept pending for Resize and false is returned. A map without a
        /// valid visible extent returns false and leaves the view unchanged.
        /// A map without a valid visible extent also cancels a pending fit.
        /// \throws InvalidArgument when paddingPixels is negative or not finite.
        /// \throws CoreError when a visible layer extent cannot be read.
        bool FitToMap(double paddingPixels = kDefaultPadding);

        /// Fits the viewport to the extent of one layer with paddingPixels
        /// free on every side; see Viewport::FitExtent. The layer is fitted
        /// even when it is hidden, because the caller named it explicitly.
        /// Returns true when the view was fitted. When the viewport has no
        /// size yet, the fit is kept pending for Resize and false is
        /// returned. Returns false and leaves the view unchanged when the map
        /// has no such layer or the layer has no valid extent.
        /// These cases also cancel a pending fit.
        /// \throws InvalidArgument when paddingPixels is negative or not finite.
        /// \throws CoreError when the layer extent cannot be read.
        bool ZoomToLayer(LayerId id, double paddingPixels = kDefaultPadding);

        /// Zooms in around the viewport center: the resolution is divided by
        /// factor; see Viewport::Zoom. Returns true when the view changed;
        /// false without a view or at the resolution limit.
        /// \throws InvalidArgument when factor is not finite or not above 1.
        bool ZoomIn(double factor = kDefaultZoomFactor);

        /// Zooms out around the viewport center: the resolution is
        /// multiplied by factor; see Viewport::ZoomOut. Returns true when the
        /// view changed; false without a view or at the resolution limit.
        /// \throws InvalidArgument when factor is not finite or not above 1.
        bool ZoomOut(double factor = kDefaultZoomFactor);

        /// Zooms around a screen position, keeping the map coordinate under
        /// it in place; see Viewport::ZoomAt. A factor above 1 zooms in and
        /// below 1 zooms out. Returns true when the view changed.
        /// \throws InvalidArgument as Viewport::ZoomAt does.
        bool ZoomAt(const ScreenPoint& anchor, double factor);

        /// Moves the view by a map-coordinate delta; see Viewport::Pan.
        /// Returns true when the view changed.
        /// \throws InvalidArgument as Viewport::Pan does.
        bool Pan(double deltaX, double deltaY);

        /// The render scene of the current map, rebuilt first when the map
        /// changed since the last call; see rendering::CreateMapRenderScene.
        /// The reference stays valid for the lifetime of the MapView; a later
        /// call that rebuilds the scene replaces its contents.
        /// \throws std::bad_alloc when memory runs out.
        [[nodiscard]] const rendering::MapRenderScene& Scene();

        /// Marks the render scene stale, for example after source geometry
        /// was edited. Returns true: the frame must be redrawn.
        bool InvalidateScene() noexcept;

    private:
        /// Clears a pending fit when changed is true and returns changed.
        bool ViewChanged(bool changed) noexcept;

        class Impl;
        std::unique_ptr<Impl> _impl;
    };
}
