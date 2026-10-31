// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapView.cpp
 * @brief Implements the map view and its render-scene invalidation.
 */
#include "mapwingis/map/MapView.h"

#include <optional>
#include <utility>

#include "mapwingis/vector/VectorDataset.h"

namespace mwg
{
    class MapView::Impl
    {
    public:
        Map _map;
        Viewport _viewport;
        rendering::MapRenderScene _scene;
        bool _sceneStale = false;
        /// Padding of a fit that waits for the viewport to get a size.
        std::optional<double> _pendingFitPadding;
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
        if (_impl->_pendingFitPadding.has_value() && width > 0 && height > 0)
        {
            fitted = FitToMap(*_impl->_pendingFitPadding);
        }
        return sizeChanged || fitted;
    }

    bool MapView::FitToMap(const double paddingPixels)
    {
        const Extent extent = _impl->_map.GetExtent();
        if (_impl->_viewport.FitExtent(extent, paddingPixels))
        {
            _impl->_pendingFitPadding.reset();
            return true;
        }

        // FitExtent fails for an invalid extent or a zero-sized viewport;
        // only the second can succeed later without another request.
        if (extent.IsValid())
        {
            _impl->_pendingFitPadding = paddingPixels;
        }
        else
        {
            _impl->_pendingFitPadding.reset();
        }
        return false;
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
