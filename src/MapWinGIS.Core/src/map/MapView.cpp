// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapView.cpp
 * @brief Implements the map view and its render-scene invalidation.
 */
#include "mapwingis/map/MapView.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include "mapwingis/core/Error.h"
#include "mapwingis/diagnostics/PerformanceDiagnostics.h"
#include "mapwingis/vector/VectorDataset.h"
#include "internal/VectorLayerAccess.h"
#include "internal/VectorRenderSceneBuilder.h"

namespace mwg
{
    namespace
    {
        constexpr double kMinimumViewportSceneMarginPixels = 64.0;
        constexpr double kViewportSceneMarginFraction = 0.5;

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
        /// A fit that waits for the viewport to get a size.
        struct PendingFit
        {
            /// The layer to fit, or std::nullopt to fit the whole map.
            std::optional<LayerId> layer;
            double padding = 0.0;
        };

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

        using LayerScene = rendering::detail::LayerScene;

        struct CachedLayerScene
        {
            LayerScene scene;
            Extent processingExtent;
            /// True for complete scenes and stable failures. False with an
            /// empty extent means that the scene is awaiting a valid viewport.
            bool complete = true;
        };

        [[nodiscard]] Extent ViewportProcessingExtent() const
        {
            const Extent visible = _viewport.VisibleExtent();
            const std::optional<double> resolution = _viewport.Resolution();
            if (!visible.IsValid() || !resolution.has_value())
            {
                return {};
            }
            const double horizontalMarginPixels =
                std::max(kMinimumViewportSceneMarginPixels,
                         static_cast<double>(_viewport.Width()) *
                             kViewportSceneMarginFraction);
            const double verticalMarginPixels =
                std::max(kMinimumViewportSceneMarginPixels,
                         static_cast<double>(_viewport.Height()) *
                             kViewportSceneMarginFraction);
            const double horizontalMargin = horizontalMarginPixels * *resolution;
            const double verticalMargin = verticalMarginPixels * *resolution;
            return {
                visible.xMin - horizontalMargin,
                visible.yMin - verticalMargin,
                visible.xMax + horizontalMargin,
                visible.yMax + verticalMargin};
        }

        [[nodiscard]] bool CacheCoversViewport(const CachedLayerScene& cached) const
        {
            const Extent visible = _viewport.VisibleExtent();
            if (!visible.IsValid() || cached.complete)
            {
                return true;
            }
            if (cached.processingExtent.IsEmpty())
            {
                return false;
            }
            const Extent& processing = cached.processingExtent;
            return processing.xMin <= visible.xMin &&
                   processing.yMin <= visible.yMin &&
                   processing.xMax >= visible.xMax &&
                   processing.yMax >= visible.yMax;
        }

        Map _map;
        Viewport _viewport;
        std::map<LayerId, CachedLayerScene> _layerScenes;
        rendering::MapRenderScene _scene;
        bool _sceneStale = false;
        std::optional<PendingFit> _pendingFit;
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

    LayerId MapView::AddLayer(std::shared_ptr<VectorDataset> dataset,
                             rendering::VectorRenderScene scene, const int layerIndex)
    {
        return AddLayer(
            std::move(dataset),
            std::make_shared<const rendering::VectorRenderScene>(std::move(scene)),
            layerIndex);
    }

    LayerId MapView::AddLayer(std::shared_ptr<VectorDataset> dataset,
                             std::shared_ptr<const rendering::VectorRenderScene> scene,
                             const int layerIndex)
    {
        if (scene == nullptr)
        {
            throw InvalidArgument("Layer scene must not be null");
        }

        (void)Scene();
        const bool empty = scene->polygons.empty();
        if (empty)
        {
            _impl->_scene.skippedLayers.reserve(_impl->_scene.skippedLayers.size() + 1);
        }
        else
        {
            _impl->_scene.layers.reserve(_impl->_scene.layers.size() + 1);
        }

        // Stage the cache node before AddLayer can change the map or pending
        // fit. Updating a node handle's key and inserting it later cannot allocate.
        std::map<LayerId, Impl::CachedLayerScene> staged;
        if (empty)
        {
            staged.emplace(LayerId{}, Impl::CachedLayerScene{
                rendering::SkippedLayer{
                    LayerId{}, rendering::LayerSkipReason::NoGeometry, {}},
                {}, true});
        }
        else
        {
            staged.emplace(LayerId{}, Impl::CachedLayerScene{
                rendering::LayerRenderScene{LayerId{}, scene},
                {}, true});
        }
        auto cachedScene = staged.extract(staged.begin());

        const LayerId id = AddLayer(std::move(dataset), layerIndex);
        cachedScene.key() = id;
        if (empty)
        {
            std::get<rendering::SkippedLayer>(cachedScene.mapped().scene).layerId = id;
        }
        else
        {
            std::get<rendering::LayerRenderScene>(cachedScene.mapped().scene).layerId = id;
        }
        _impl->_layerScenes.insert(std::move(cachedScene));
        if (empty)
        {
            _impl->_scene.skippedLayers.push_back(
                {id, rendering::LayerSkipReason::NoGeometry, {}});
        }
        else
        {
            _impl->_scene.layers.push_back({id, std::move(scene)});
        }
        _impl->_sceneStale = false;
        return id;
    }

    bool MapView::RemoveLayer(const LayerId id) noexcept
    {
        if (!_impl->_map.RemoveLayer(id))
        {
            return false;
        }
        _impl->_layerScenes.erase(id);
        _impl->_sceneStale = true;
        return true;
    }

    bool MapView::ClearLayers() noexcept
    {
        if (_impl->_map.LayerCount() == 0)
        {
            return false;
        }
        _impl->_map.Clear();
        _impl->_layerScenes.clear();
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
        (void)_impl->_map.SetLayerVisible(id, visible);
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
        return _impl->Fit(_impl->_map.GetExtent(), {std::nullopt, paddingPixels});
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
            _impl->_pendingFit.reset();
            return false;
        }
        return _impl->Fit(layer->GetExtent(), {id, paddingPixels});
    }

    bool MapView::ZoomIn(const double factor)
    {
        ValidateStepFactor(factor);
        return ViewChanged(_impl->_viewport.Zoom(factor));
    }

    bool MapView::ZoomOut(const double factor)
    {
        ValidateStepFactor(factor);
        return ViewChanged(_impl->_viewport.ZoomOut(factor));
    }

    bool MapView::ZoomAt(const ScreenPoint& anchor, const double factor)
    {
        return ViewChanged(_impl->_viewport.ZoomAt(anchor, factor));
    }

    bool MapView::Pan(const double deltaX, const double deltaY)
    {
        return ViewChanged(_impl->_viewport.Pan(deltaX, deltaY));
    }

    bool MapView::ViewChanged(const bool changed) noexcept
    {
        if (changed)
        {
            _impl->_pendingFit.reset();
        }
        return changed;
    }

    const rendering::MapRenderScene& MapView::Scene()
    {
        const Extent processingExtent = _impl->ViewportProcessingExtent();
        bool refreshScene = _impl->_sceneStale;
        if (!refreshScene)
        {
            for (int position = 0; position < _impl->_map.LayerCount(); ++position)
            {
                const MapLayer& layer = _impl->_map.Layer(position);
                if (!layer.IsVisible())
                {
                    continue;
                }
                const auto cached = _impl->_layerScenes.find(layer.Id());
                if (cached == _impl->_layerScenes.end() ||
                    !_impl->CacheCoversViewport(cached->second))
                {
                    refreshScene = true;
                    break;
                }
            }
        }

        if (refreshScene)
        {
            _impl->_sceneStale = true;
            rendering::MapRenderScene scene;
            scene.layers.reserve(static_cast<std::size_t>(_impl->_map.LayerCount()));
            scene.skippedLayers.reserve(static_cast<std::size_t>(_impl->_map.LayerCount()));

            for (int position = 0; position < _impl->_map.LayerCount(); ++position)
            {
                const MapLayer& layer = _impl->_map.Layer(position);
                if (!layer.IsVisible())
                {
                    scene.skippedLayers.push_back(
                        {layer.Id(), rendering::LayerSkipReason::Hidden, {}});
                    continue;
                }

                auto cached = _impl->_layerScenes.find(layer.Id());
                if (cached == _impl->_layerScenes.end() ||
                    !_impl->CacheCoversViewport(cached->second))
                {
                    diagnostics::PerformanceDiagnostics::IncrementCounter(
                        diagnostics::PerformanceCounter::SceneCacheMisses);
                    rendering::VectorRenderSceneOptions options;
                    const bool canFilter =
                        processingExtent.IsValid() &&
                        detail::VectorLayerAccess::HasFastSpatialFilter(layer.Source());
                    if (canFilter)
                    {
                        options.spatialFilter = processingExtent;
                    }
                    Impl::LayerScene layerScene =
                        rendering::detail::BuildLayerRenderScene(layer, options);
                    bool complete = processingExtent.IsValid() && !canFilter;
                    if (canFilter)
                    {
                        const auto* skipped =
                            std::get_if<rendering::SkippedLayer>(&layerScene);
                        if (skipped != nullptr &&
                            skipped->reason == rendering::LayerSkipReason::Failed)
                        {
                            layerScene =
                                rendering::detail::BuildLayerRenderScene(layer, {});
                            complete = true;
                        }
                    }
                    else if (const auto* skipped =
                                 std::get_if<rendering::SkippedLayer>(&layerScene);
                             skipped != nullptr &&
                             skipped->reason == rendering::LayerSkipReason::Failed)
                    {
                        complete = true;
                    }
                    cached = _impl->_layerScenes.insert_or_assign(
                        layer.Id(),
                        Impl::CachedLayerScene{
                            std::move(layerScene),
                            canFilter && !complete ? processingExtent : Extent{},
                            complete}).first;
                }
                else
                {
                    diagnostics::PerformanceDiagnostics::IncrementCounter(
                        diagnostics::PerformanceCounter::SceneCacheHits);
                }

                if (const auto* drawable =
                        std::get_if<rendering::LayerRenderScene>(&cached->second.scene))
                {
                    scene.layers.push_back(*drawable);
                }
                else
                {
                    scene.skippedLayers.push_back(
                        std::get<rendering::SkippedLayer>(cached->second.scene));
                }
            }

            _impl->_scene = std::move(scene);
            _impl->_sceneStale = false;
        }
        else if (diagnostics::PerformanceDiagnostics::IsEnabled())
        {
            std::int64_t hits = static_cast<std::int64_t>(_impl->_scene.layers.size());
            for (const rendering::SkippedLayer& layer : _impl->_scene.skippedLayers)
            {
                if (layer.reason != rendering::LayerSkipReason::Hidden)
                {
                    ++hits;
                }
            }
            diagnostics::PerformanceDiagnostics::IncrementCounter(
                diagnostics::PerformanceCounter::SceneCacheHits, hits);
        }
        return _impl->_scene;
    }

    bool MapView::InvalidateScene() noexcept
    {
        _impl->_layerScenes.clear();
        _impl->_sceneStale = true;
        return true;
    }

    bool MapView::InvalidateLayerScene(const LayerId id) noexcept
    {
        if (_impl->_map.FindLayer(id) == nullptr)
        {
            return false;
        }
        _impl->_layerScenes.erase(id);
        _impl->_sceneStale = true;
        return true;
    }
}
