// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapView.cpp
 * @brief Implements the map view and its render-scene invalidation.
 */
#include "mapwingis/map/MapView.h"

#include <cmath>
#include <optional>
#include <utility>

#include "mapwingis/core/Error.h"
#include "mapwingis/vector/VectorDataset.h"

namespace mwg
{
    namespace
    {
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

        Map _map;
        Viewport _viewport;
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

    bool MapView::RemoveLayer(const LayerId id) noexcept
    {
        if (!_impl->_map.RemoveLayer(id))
        {
            return false;
        }
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
        return ViewChanged(_impl->_viewport.Zoom(1.0 / factor));
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
        if (_impl->_sceneStale)
        {
            _impl->_scene = rendering::CreateMapRenderScene(_impl->_map);
            _impl->_sceneStale = false;
        }
        return _impl->_scene;
    }

    bool MapView::InvalidateScene() noexcept
    {
        _impl->_sceneStale = true;
        return true;
    }
}
