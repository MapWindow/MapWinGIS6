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
    /// through GetViewport(). Automatically filtered scenes cover only the
    /// live viewport plus a processing margin; drawing a different viewport
    /// requires a complete prepared scene. Changes to the layer collection or
    /// visibility mark the scene stale; Scene() rebuilds it on the next call.
    /// For a valid viewport, automatically built layer scenes use a padded
    /// spatial filter. Small viewport changes reuse the cached scene; a change
    /// beyond that margin refreshes only the affected layer scenes. Adding a
    /// prepared scene retains the existing complete geometry instead. Edits to
    /// source geometry are not observed. Call InvalidateLayerScene for each
    /// affected map layer, or InvalidateScene for all layers.
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

        /// Adds a layer with an already prepared snapshot of its geometry.
        /// The scene must match the specified source layer without intervening
        /// edits. An empty scene adds a layer with no drawable geometry.
        /// Transfers scene ownership and retains the current layers' scenes,
        /// rebuilding stale scenes first. The cache and the assembled scene
        /// share one immutable copy of the new geometry. The next Scene()
        /// does not reread data.
        /// Fitting and argument errors are the same as the other AddLayer overload.
        /// \throws std::bad_alloc when memory runs out.
        LayerId AddLayer(std::shared_ptr<VectorDataset> dataset,
                         rendering::VectorRenderScene scene, int layerIndex = 0);

        /// Adds a layer with an already prepared, shared snapshot of its
        /// geometry, as returned in LayerRenderScene::scene. Behaves like the
        /// overload above but stores the given snapshot without copying it.
        /// \throws InvalidArgument when scene is null, before the map changes.
        LayerId AddLayer(std::shared_ptr<VectorDataset> dataset,
                         std::shared_ptr<const rendering::VectorRenderScene> scene,
                         int layerIndex = 0);

        /// Removes a layer; see Map::RemoveLayer. The view is kept.
        bool RemoveLayer(LayerId id) noexcept;

        /// Rolls back a layer add after a post-add operation fails. Removes the
        /// layer and restores the projection that existed before the add.
        /// Returns false when the layer no longer exists.
        [[nodiscard]] bool RollbackLayerAdd(
            LayerId id, const std::optional<mwg::Projection>& previousProjection);

        /// Removes all layers and returns true when there were any. The view
        /// is kept.
        bool ClearLayers() noexcept;

        /// Shows or hides a layer and returns true when its visibility
        /// changed. Returns false when the map has no such layer.
        bool SetLayerVisible(LayerId id, bool visible) noexcept;

        /// Changes one layer's display-only projection and invalidates only
        /// that layer's cached scene when its displayed geometry changes.
        /// See Map::SetLayerDisplayProjection for errors and source rules.
        bool SetLayerDisplayProjection(LayerId id, mwg::Projection projection);

        /// Restores one layer's source projection and invalidates its scene
        /// when the displayed geometry changes.
        bool ClearLayerDisplayProjection(LayerId id) noexcept;

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

        /// The render scene of the current map, reassembled from cached
        /// per-layer scenes when the map or a layer scene changed. The
        /// reference stays valid for the lifetime of the MapView; a later
        /// call that reassembles the scene replaces its contents. Geometry
        /// for a visible uncached layer is read once. A valid viewport limits
        /// automatically built scenes to features intersecting a padded
        /// visible extent when the driver supports a fast spatial filter.
        /// Drivers without that capability retain a complete scene. Visibility
        /// changes and viewport moves within the processing extent reuse
        /// the cache. Reassembly shares cached geometry with the map snapshot
        /// instead of copying it, so a held LayerRenderScene::scene stays
        /// valid after the layer is invalidated or removed.
        /// Failed layers retain their diagnostics until explicitly invalidated.
        /// \throws std::bad_alloc when memory runs out.
        [[nodiscard]] const rendering::MapRenderScene& Scene();

        /// Marks every cached layer scene stale, for example after edits that
        /// may affect multiple source layers. Visible scenes are rebuilt by the
        /// next Scene() call; hidden scenes wait until shown.
        /// Returns true: the frame must be redrawn.
        bool InvalidateScene() noexcept;

        /// Marks one layer's cached scene stale after its source geometry was
        /// edited. Other layer scenes remain cached. Returns false when id
        /// does not identify a current layer; otherwise returns true and the
        /// layer is rebuilt when next visible in Scene(). Also retries a
        /// previously failed layer. Duplicate map layers of the same source
        /// have independent caches; invalidate each affected map layer.
        bool InvalidateLayerScene(LayerId id) noexcept;

    private:
        /// Clears a pending fit when changed is true and returns changed.
        bool ViewChanged(bool changed) noexcept;

        class Impl;
        std::unique_ptr<Impl> _impl;
    };
}
