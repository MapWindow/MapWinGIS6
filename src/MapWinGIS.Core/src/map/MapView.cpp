// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapView.cpp
 * @brief Implements the map view and its render-scene invalidation.
 */
#include "mapwingis/map/MapView.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "internal/VectorRenderSceneBuilder.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/vector/VectorDataset.h"

namespace mwg
{
    namespace
    {
        void ValidateTransitionDuration(const std::chrono::milliseconds duration)
        {
            if (duration.count() <= 0)
            {
                throw InvalidArgument("Transition duration must be positive");
            }
        }

        void ValidateStepFactor(const double factor)
        {
            if (!std::isfinite(factor) || factor <= 1.0)
            {
                throw InvalidArgument("Zoom factor must be a finite number above 1");
            }
        }
    }

    class MapView::Impl
    {
    public:
        static constexpr std::size_t kMaxHistoryEntries = 100;

        /// A fit that waits for the viewport to get a size.
        struct PendingFit
        {
            /// The layer to fit, or std::nullopt to fit the whole map.
            std::optional<LayerId> layer;
            double padding = 0.0;
        };

        struct Transition
        {
            NavigationState start;
            NavigationState target;
            std::chrono::milliseconds duration;
            std::chrono::milliseconds elapsed{0};
        };

        struct RasterView
        {
            Extent extent;
            int width = 0;
            int height = 0;
        };

        struct CachedLayer
        {
            std::optional<rendering::LayerRenderScene> layer;
            std::optional<rendering::SkippedLayer> skipped;
        };

        Impl()
        {
            _backHistory.reserve(kMaxHistoryEntries);
            _forwardHistory.reserve(kMaxHistoryEntries);
        }

        [[nodiscard]] std::optional<NavigationState> CaptureView() const noexcept
        {
            const std::optional<MapPoint> center = _viewport.Center();
            const std::optional<double> resolution = _viewport.Resolution();
            if (!center.has_value() || !resolution.has_value())
            {
                return std::nullopt;
            }
            return NavigationState{*center, *resolution};
        }

        static void PushHistory(
            std::vector<NavigationState>& history,
            const NavigationState& state) noexcept
        {
            if (history.size() == kMaxHistoryEntries)
            {
                history.erase(history.begin());
            }
            history.push_back(state);
        }

        bool RecordViewChange(const std::optional<NavigationState>& previous) noexcept
        {
            if (_navigationGestureStart.has_value())
            {
                return false;
            }
            const std::optional<NavigationState> current = CaptureView();
            if (!previous.has_value() || !current.has_value() ||
                (previous->center.x == current->center.x &&
                 previous->center.y == current->center.y &&
                 previous->resolution == current->resolution))
            {
                return false;
            }

            PushHistory(_backHistory, *previous);
            _forwardHistory.clear();
            return true;
        }

        bool EndNavigationGesture() noexcept
        {
            if (!_navigationGestureStart.has_value())
            {
                return false;
            }
            const NavigationState start = *_navigationGestureStart;
            _navigationGestureStart.reset();
            return RecordViewChange(start);
        }

        bool BeginTransition(
            const Viewport& targetViewport,
            const std::chrono::milliseconds duration)
        {
            ValidateTransitionDuration(duration);
            (void)EndNavigationGesture();

            const std::optional<NavigationState> start = CaptureView();
            const std::optional<MapPoint> targetCenter = targetViewport.Center();
            const std::optional<double> targetResolution = targetViewport.Resolution();
            if (!start.has_value() || !targetCenter.has_value() ||
                !targetResolution.has_value())
            {
                _transition.reset();
                return false;
            }

            const NavigationState target{*targetCenter, *targetResolution};
            if (start->center.x == target.center.x &&
                start->center.y == target.center.y &&
                start->resolution == target.resolution)
            {
                _transition.reset();
                return false;
            }

            _pendingFit.reset();
            _transition = Transition{*start, target, duration};
            return true;
        }

        bool RestoreHistory(
            std::vector<NavigationState>& source,
            std::vector<NavigationState>& destination) noexcept
        {
            (void)EndNavigationGesture();
            if (source.empty())
            {
                return false;
            }

            const std::optional<NavigationState> current = CaptureView();
            if (!current.has_value())
            {
                return false;
            }

            _transition.reset();
            const NavigationState target = source.back();
            source.pop_back();
            PushHistory(destination, *current);
            _viewport.SetView(target.center, target.resolution);
            _pendingFit.reset();
            return true;
        }

        /// Fits extent and returns true, or keeps the fit pending when only
        /// the viewport size prevents it.
        bool Fit(const Extent& extent, const PendingFit& request)
        {
            if (_viewport.FitExtent(extent, request.padding))
            {
                _pendingFit.reset();
                return true;
            }

            // FitExtent fails for an invalid extent or a zero-sized viewport;
            // only the second can succeed later without another request.
            if (extent.IsValid())
            {
                _pendingFit = request;
            }
            else
            {
                _pendingFit.reset();
            }
            return false;
        }

        Map _map;
        Viewport _viewport;
        rendering::MapRenderScene _scene;
        bool _sceneStale = false;
        std::optional<PendingFit> _pendingFit;
        std::vector<NavigationState> _backHistory;
        std::vector<NavigationState> _forwardHistory;
        std::optional<Transition> _transition;
        std::optional<NavigationState> _navigationGestureStart;
        std::optional<RasterView> _rasterView;
        std::unordered_map<std::uint64_t, CachedLayer> _layerSceneCache;
    };

    MapView::MapView()
        : _impl(std::make_unique<Impl>())
    {
    }

    MapView::~MapView() = default;

    const Map& MapView::GetMap() const noexcept
    {
        return _impl->_map;
    }

    const Viewport& MapView::GetViewport() const noexcept
    {
        return _impl->_viewport;
    }

    LayerId MapView::AddLayer(std::shared_ptr<VectorDataset> dataset, const int layerIndex)
    {
        const LayerId id = _impl->_map.AddLayer(std::move(dataset), layerIndex);
        (void)EndNavigationGesture();
        _impl->_transition.reset();
        _impl->_sceneStale = true;
        if (!_impl->_viewport.HasView())
        {
            try
            {
                (void)FitToMap();
            }
            catch (...)
            {
                // Reading the extent failed; leave the map as it was.
                (void)_impl->_map.RemoveLayer(id);
                throw;
            }
        }
        return id;
    }

    LayerId MapView::AddRasterLayer(std::shared_ptr<RasterDataset> dataset)
    {
        const LayerId id = _impl->_map.AddRasterLayer(std::move(dataset));
        (void)EndNavigationGesture();
        _impl->_transition.reset();
        _impl->_sceneStale = true;
        if (!_impl->_viewport.HasView())
        {
            try
            {
                (void)FitToMap();
            }
            catch (...)
            {
                (void)_impl->_map.RemoveLayer(id);
                throw;
            }
        }
        return id;
    }

    bool MapView::RemoveLayer(const LayerId id) noexcept
    {
        if (!_impl->_map.RemoveLayer(id))
        {
            return false;
        }
        (void)EndNavigationGesture();
        _impl->_transition.reset();
        _impl->_sceneStale = true;
        return true;
    }

    bool MapView::ClearLayers() noexcept
    {
        if (_impl->_map.LayerCount() == 0)
        {
            return false;
        }
        (void)EndNavigationGesture();
        _impl->_map.Clear();
        _impl->_transition.reset();
        _impl->_sceneStale = true;
        return true;
    }

    bool MapView::SetLayerVisible(const LayerId id, const bool visible) noexcept
    {
        const MapLayer* layer = _impl->_map.FindLayer(id);
        if (layer == nullptr || layer->IsVisible() == visible)
        {
            return false;
        }
        (void)EndNavigationGesture();
        (void)_impl->_map.SetLayerVisible(id, visible);
        _impl->_transition.reset();
        _impl->_sceneStale = true;
        return true;
    }

    bool MapView::MoveLayer(const LayerId id, const int position)
    {
        if (!_impl->_map.MoveLayer(id, position))
        {
            return false;
        }
        (void)EndNavigationGesture();
        _impl->_transition.reset();
        _impl->_sceneStale = true;
        return true;
    }

    bool MapView::Resize(const int width, const int height)
    {
        Viewport& viewport = _impl->_viewport;
        const bool sizeChanged = viewport.Width() != width || viewport.Height() != height;
        viewport.SetSize(width, height);

        bool fitted = false;
        if (_impl->_pendingFit.has_value() && width > 0 && height > 0)
        {
            // Cleared first, so a layer removed in the meantime does not
            // leave the fit pending forever.
            const Impl::PendingFit pending = *_impl->_pendingFit;
            _impl->_pendingFit.reset();
            fitted = pending.layer.has_value()
                ? ZoomToLayer(*pending.layer, pending.padding)
                : FitToMap(pending.padding);
        }
        return sizeChanged || fitted;
    }

    bool MapView::FitToMap(const double paddingPixels)
    {
        (void)EndNavigationGesture();
        _impl->_transition.reset();
        const std::optional<NavigationState> previous = _impl->CaptureView();
        const bool fitted = _impl->Fit(
            _impl->_map.GetExtent(),
            {std::nullopt, paddingPixels});
        _impl->RecordViewChange(previous);
        return fitted;
    }

    bool MapView::ZoomToLayer(const LayerId id, const double paddingPixels)
    {
        if (!std::isfinite(paddingPixels) || paddingPixels < 0.0)
        {
            throw InvalidArgument("Viewport padding must be a finite non-negative number");
        }
        const MapLayer* layer = _impl->_map.FindLayer(id);
        if (layer == nullptr)
        {
            (void)EndNavigationGesture();
            _impl->_pendingFit.reset();
            _impl->_transition.reset();
            return false;
        }
        (void)EndNavigationGesture();
        _impl->_transition.reset();
        const std::optional<NavigationState> previous = _impl->CaptureView();
        const bool fitted = _impl->Fit(layer->GetExtent(), {id, paddingPixels});
        _impl->RecordViewChange(previous);
        return fitted;
    }

    bool MapView::ZoomIn(const double factor)
    {
        ValidateStepFactor(factor);
        const std::optional<NavigationState> previous = _impl->CaptureView();
        return ViewChanged(_impl->_viewport.Zoom(factor), previous);
    }

    bool MapView::ZoomOut(const double factor)
    {
        ValidateStepFactor(factor);
        const std::optional<NavigationState> previous = _impl->CaptureView();
        return ViewChanged(_impl->_viewport.ZoomOut(factor), previous);
    }

    bool MapView::ZoomAt(const ScreenPoint& anchor, const double factor)
    {
        const std::optional<NavigationState> previous = _impl->CaptureView();
        return ViewChanged(_impl->_viewport.ZoomAt(anchor, factor), previous);
    }

    bool MapView::Pan(const double deltaX, const double deltaY)
    {
        const std::optional<NavigationState> previous = _impl->CaptureView();
        return ViewChanged(_impl->_viewport.Pan(deltaX, deltaY), previous);
    }

    bool MapView::BeginNavigationGesture() noexcept
    {
        if (_impl->_navigationGestureStart.has_value())
        {
            return false;
        }
        const std::optional<NavigationState> current = _impl->CaptureView();
        if (!current.has_value())
        {
            return false;
        }
        _impl->_navigationGestureStart = *current;
        return true;
    }

    bool MapView::EndNavigationGesture() noexcept
    {
        return _impl->EndNavigationGesture();
    }

    bool MapView::CanGoBack() const noexcept
    {
        return !_impl->_backHistory.empty();
    }

    bool MapView::CanGoForward() const noexcept
    {
        return !_impl->_forwardHistory.empty();
    }

    bool MapView::GoBack() noexcept
    {
        return _impl->RestoreHistory(_impl->_backHistory, _impl->_forwardHistory);
    }

    bool MapView::GoForward() noexcept
    {
        return _impl->RestoreHistory(_impl->_forwardHistory, _impl->_backHistory);
    }

    bool MapView::BeginTransitionToMap(const std::chrono::milliseconds duration)
    {
        ValidateTransitionDuration(duration);
        Viewport target = _impl->_viewport;
        if (!target.FitExtent(_impl->_map.GetExtent(), kDefaultPadding))
        {
            _impl->_transition.reset();
            return false;
        }
        return _impl->BeginTransition(target, duration);
    }

    bool MapView::BeginTransitionToLayer(
        const LayerId id,
        const std::chrono::milliseconds duration)
    {
        ValidateTransitionDuration(duration);
        const MapLayer* layer = _impl->_map.FindLayer(id);
        if (layer == nullptr)
        {
            _impl->_transition.reset();
            return false;
        }

        Viewport target = _impl->_viewport;
        if (!target.FitExtent(layer->GetExtent(), kDefaultPadding))
        {
            _impl->_transition.reset();
            return false;
        }
        return _impl->BeginTransition(target, duration);
    }

    bool MapView::AdvanceTransition(const std::chrono::milliseconds elapsed)
    {
        if (elapsed.count() < 0)
        {
            throw InvalidArgument("Transition elapsed time cannot be negative");
        }
        if (!_impl->_transition.has_value() || elapsed.count() == 0)
        {
            return false;
        }

        Impl::Transition& transition = *_impl->_transition;
        const std::chrono::milliseconds remaining =
            transition.duration - transition.elapsed;
        transition.elapsed += std::min(elapsed, remaining);
        const double progress = std::clamp(
            static_cast<double>(transition.elapsed.count()) /
                static_cast<double>(transition.duration.count()),
            0.0,
            1.0);
        const double eased = progress * progress * (3.0 - 2.0 * progress);
        const auto interpolate = [eased](const double start, const double target)
        {
            return start + (target - start) * eased;
        };
        const double resolution = std::exp(
            std::log(transition.start.resolution) +
            (std::log(transition.target.resolution) -
             std::log(transition.start.resolution)) * eased);
        const NavigationState start = transition.start;
        _impl->_viewport.SetView(
            {
                interpolate(start.center.x, transition.target.center.x),
                interpolate(start.center.y, transition.target.center.y)},
            resolution);
        const bool changed =
            _impl->_viewport.Center()->x != start.center.x ||
            _impl->_viewport.Center()->y != start.center.y ||
            *_impl->_viewport.Resolution() != start.resolution;

        if (transition.elapsed == transition.duration)
        {
            _impl->RecordViewChange(start);
            _impl->_transition.reset();
        }
        return changed;
    }

    bool MapView::CancelTransition() noexcept
    {
        if (!_impl->_transition.has_value())
        {
            return false;
        }
        (void)_impl->RecordViewChange(_impl->_transition->start);
        _impl->_transition.reset();
        return true;
    }

    bool MapView::IsTransitioning() const noexcept
    {
        return _impl->_transition.has_value();
    }

    bool MapView::ViewChanged(
        const bool changed,
        const std::optional<NavigationState>& previousState) noexcept
    {
        if (!changed)
        {
            return false;
        }

        _impl->_pendingFit.reset();
        _impl->_transition.reset();
        _impl->RecordViewChange(previousState);
        return true;
    }

    const rendering::MapRenderScene& MapView::Scene()
    {
        std::optional<Impl::RasterView> rasterView;
        const Viewport& viewport = _impl->_viewport;
        if (viewport.IsValid())
        {
            rasterView = Impl::RasterView{
                viewport.VisibleExtent(), viewport.Width(), viewport.Height()};
        }

        if (_impl->_sceneStale)
        {
            for (rendering::LayerRenderScene& layer : _impl->_scene.layers)
            {
                Impl::CachedLayer& cached = _impl->_layerSceneCache[layer.layerId.value];
                cached.layer = std::move(layer);
                cached.skipped.reset();
            }
            for (rendering::SkippedLayer& skipped : _impl->_scene.skippedLayers)
            {
                if (skipped.reason != rendering::LayerSkipReason::Hidden)
                {
                    Impl::CachedLayer& cached =
                        _impl->_layerSceneCache[skipped.layerId.value];
                    cached.layer.reset();
                    cached.skipped = std::move(skipped);
                }
            }

            for (auto cached = _impl->_layerSceneCache.begin();
                 cached != _impl->_layerSceneCache.end();)
            {
                if (_impl->_map.FindLayer(LayerId{cached->first}) == nullptr)
                {
                    cached = _impl->_layerSceneCache.erase(cached);
                }
                else
                {
                    ++cached;
                }
            }

            rendering::MapRenderScene updated;
            for (int position = 0; position < _impl->_map.LayerCount(); ++position)
            {
                const MapLayer& layer = _impl->_map.Layer(position);
                if (!layer.IsVisible())
                {
                    updated.skippedLayers.push_back(
                        {layer.Id(), rendering::LayerSkipReason::Hidden, {}});
                    continue;
                }

                auto cached = _impl->_layerSceneCache.find(layer.Id().value);
                if (cached != _impl->_layerSceneCache.end() &&
                    cached->second.layer.has_value())
                {
                    Impl::CachedLayer& entry = cached->second;
                    if (layer.IsRaster())
                    {
                        try
                        {
                            if (rasterView.has_value())
                            {
                                entry.layer->raster = layer.RasterSource()->ReadWindow(
                                    rasterView->extent,
                                    _impl->_map.CoordinateSystemWkt(),
                                    rasterView->width,
                                    rasterView->height);
                            }
                            else
                            {
                                entry.layer->raster =
                                    layer.RasterSource()->ReadImage(
                                        _impl->_map.CoordinateSystemWkt());
                                entry.layer->raster->extent = layer.GetExtent();
                            }
                        }
                        catch (const std::bad_alloc&)
                        {
                            throw;
                        }
                        catch (const std::exception& error)
                        {
                            updated.skippedLayers.push_back(
                                {layer.Id(), rendering::LayerSkipReason::Failed, error.what()});
                            continue;
                        }
                    }
                    updated.layers.push_back(std::move(*entry.layer));
                    _impl->_layerSceneCache.erase(cached);
                    continue;
                }
                if (cached != _impl->_layerSceneCache.end() &&
                    cached->second.skipped.has_value())
                {
                    updated.skippedLayers.push_back(std::move(*cached->second.skipped));
                    _impl->_layerSceneCache.erase(cached);
                    continue;
                }

                rendering::detail::LayerRenderSceneResult built =
                    rendering::detail::BuildLayerRenderScene(
                        _impl->_map,
                        layer,
                        rasterView.has_value() ? &rasterView->extent : nullptr,
                        rasterView.has_value() ? rasterView->width : 0,
                        rasterView.has_value() ? rasterView->height : 0);
                if (built.layer.has_value())
                {
                    updated.layers.push_back(std::move(*built.layer));
                }
                if (built.skipped.has_value())
                {
                    updated.skippedLayers.push_back(std::move(*built.skipped));
                }
            }
            _impl->_scene = std::move(updated);
            _impl->_sceneStale = false;
            _impl->_rasterView = rasterView;
        }
        else if (rasterView.has_value() &&
                 (!_impl->_rasterView.has_value() ||
                  _impl->_rasterView->width != rasterView->width ||
                  _impl->_rasterView->height != rasterView->height ||
                  _impl->_rasterView->extent.xMin != rasterView->extent.xMin ||
                  _impl->_rasterView->extent.yMin != rasterView->extent.yMin ||
                  _impl->_rasterView->extent.xMax != rasterView->extent.xMax ||
                  _impl->_rasterView->extent.yMax != rasterView->extent.yMax))
        {
            std::vector<std::pair<rendering::LayerRenderScene*, RasterImage>> replacements;
            for (rendering::LayerRenderScene& renderLayer : _impl->_scene.layers)
            {
                const MapLayer* layer = _impl->_map.FindLayer(renderLayer.layerId);
                if (layer != nullptr && layer->IsRaster() && layer->IsVisible())
                {
                    replacements.emplace_back(
                        &renderLayer,
                        layer->RasterSource()->ReadWindow(
                            rasterView->extent,
                            _impl->_map.CoordinateSystemWkt(),
                            rasterView->width,
                            rasterView->height));
                }
            }
            for (auto& [layer, image] : replacements)
            {
                layer->raster = std::move(image);
            }
            _impl->_rasterView = rasterView;
        }
        return _impl->_scene;
    }

    bool MapView::InvalidateScene() noexcept
    {
        _impl->_sceneStale = true;
        _impl->_rasterView.reset();
        _impl->_layerSceneCache.clear();
        _impl->_scene = {};
        return true;
    }
}
