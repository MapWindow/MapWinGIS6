// SPDX-License-Identifier: MPL-2.0

/**
 * @file Map.cpp
 * @brief Implements the map model, its layer collection, and extent aggregation.
 */
#include "mapwingis/map/Map.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "mapwingis/core/Error.h"
#include "mapwingis/vector/VectorDataset.h"

namespace mwg
{
    class MapLayer::Impl
    {
    public:
        Impl(const LayerId id, std::shared_ptr<VectorDataset> dataset, const int sourceLayerIndex)
            : _id(id),
              _dataset(std::move(dataset)),
              _source(&_dataset->Layer(sourceLayerIndex)),
              _sourceLayerIndex(sourceLayerIndex)
        {
        }

        LayerId _id;
        std::shared_ptr<VectorDataset> _dataset;
        VectorLayer* _source;
        int _sourceLayerIndex;
        bool _visible = true;
    };

    MapLayer::MapLayer(std::unique_ptr<Impl> impl)
        : _impl(std::move(impl))
    {
    }

    MapLayer::~MapLayer() = default;

    LayerId MapLayer::Id() const noexcept
    {
        return _impl->_id;
    }

    VectorLayer& MapLayer::Source() const noexcept
    {
        return *_impl->_source;
    }

    VectorDataset& MapLayer::Dataset() const noexcept
    {
        return *_impl->_dataset;
    }

    int MapLayer::SourceLayerIndex() const noexcept
    {
        return _impl->_sourceLayerIndex;
    }

    bool MapLayer::IsVisible() const noexcept
    {
        return _impl->_visible;
    }

    Extent MapLayer::GetExtent() const
    {
        const Extent extent = _impl->_source->GetExtent();
        return extent.IsValid() ? extent : Extent{};
    }

    class Map::Impl
    {
    public:
        [[nodiscard]] std::vector<std::unique_ptr<MapLayer>>::const_iterator Find(
            const LayerId id) const noexcept
        {
            return std::find_if(
                _layers.begin(),
                _layers.end(),
                [id](const std::unique_ptr<MapLayer>& layer)
                {
                    return layer->Id() == id;
                });
        }

        // unique_ptr elements keep MapLayer addresses stable while the
        // vector reorders or reallocates.
        std::vector<std::unique_ptr<MapLayer>> _layers;
        std::uint64_t _lastId = 0;
    };

    Map::Map()
        : _impl(std::make_unique<Impl>())
    {
    }

    Map::~Map() = default;

    LayerId Map::AddLayer(std::shared_ptr<VectorDataset> dataset, const int layerIndex)
    {
        if (dataset == nullptr)
        {
            throw InvalidArgument("Cannot add a layer without a dataset");
        }
        if (layerIndex < 0 || layerIndex >= dataset->LayerCount())
        {
            throw InvalidArgument("Dataset layer index out of range: " + std::to_string(layerIndex));
        }
        if (_impl->_lastId == std::numeric_limits<std::uint64_t>::max())
        {
            throw CoreError("Map layer identity space is exhausted");
        }

        const LayerId id{_impl->_lastId + 1};
        auto layer = std::unique_ptr<MapLayer>(
            new MapLayer(std::make_unique<MapLayer::Impl>(id, std::move(dataset), layerIndex)));
        _impl->_layers.push_back(std::move(layer));
        _impl->_lastId = id.value;
        return id;
    }

    bool Map::RemoveLayer(const LayerId id) noexcept
    {
        const auto layer = _impl->Find(id);
        if (layer == _impl->_layers.end())
        {
            return false;
        }

        _impl->_layers.erase(layer);
        return true;
    }

    void Map::Clear() noexcept
    {
        _impl->_layers.clear();
    }

    int Map::LayerCount() const noexcept
    {
        return static_cast<int>(_impl->_layers.size());
    }

    const MapLayer& Map::Layer(const int position) const
    {
        if (position < 0 || position >= LayerCount())
        {
            throw InvalidArgument("Map layer position out of range: " + std::to_string(position));
        }
        return *_impl->_layers[static_cast<std::size_t>(position)];
    }

    const MapLayer* Map::FindLayer(const LayerId id) const noexcept
    {
        const auto layer = _impl->Find(id);
        return layer != _impl->_layers.end() ? layer->get() : nullptr;
    }

    std::optional<int> Map::LayerPosition(const LayerId id) const noexcept
    {
        const auto layer = _impl->Find(id);
        if (layer == _impl->_layers.end())
        {
            return std::nullopt;
        }
        return static_cast<int>(layer - _impl->_layers.begin());
    }

    bool Map::SetLayerVisible(const LayerId id, const bool visible) noexcept
    {
        const auto layer = _impl->Find(id);
        if (layer == _impl->_layers.end())
        {
            return false;
        }

        (*layer)->_impl->_visible = visible;
        return true;
    }

    Extent Map::GetExtent(const ExtentScope scope) const
    {
        Extent combined;
        for (const std::unique_ptr<MapLayer>& layer : _impl->_layers)
        {
            if (scope == ExtentScope::VisibleLayers && !layer->IsVisible())
            {
                continue;
            }
            combined = combined.Union(layer->GetExtent());
        }
        return combined;
    }
}
