// SPDX-License-Identifier: MPL-2.0

/**
 * @file Map.cpp
 * @brief Implements the map model, its layer collection, and extent aggregation.
 */
#include "mapwingis/map/Map.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "internal/CoordinateTransformation.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/raster/RasterDataset.h"
#include "mapwingis/vector/VectorDataset.h"

namespace mwg
{
    class MapLayer::Impl
    {
    public:
        Impl(
            const LayerId id,
            std::shared_ptr<VectorDataset> dataset,
            const int sourceLayerIndex,
            std::string mapProjectionWkt)
            : _id(id),
              _dataset(std::move(dataset)),
              _source(&_dataset->Layer(sourceLayerIndex)),
              _sourceLayerIndex(sourceLayerIndex),
              _mapProjectionWkt(std::move(mapProjectionWkt))
        {
        }

        Impl(
            const LayerId id,
            std::shared_ptr<RasterDataset> raster,
            std::string mapProjectionWkt)
            : _id(id),
              _raster(std::move(raster)),
              _mapProjectionWkt(std::move(mapProjectionWkt))
        {
        }

        LayerId _id;
        std::shared_ptr<VectorDataset> _dataset;
        std::shared_ptr<RasterDataset> _raster;
        VectorLayer* _source = nullptr;
        int _sourceLayerIndex = 0;
        std::string _mapProjectionWkt;
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

    VectorLayer& MapLayer::Source() const
    {
        if (_impl->_source == nullptr)
        {
            throw CoreError("Raster map layers do not have a vector source");
        }
        return *_impl->_source;
    }

    VectorDataset& MapLayer::Dataset() const
    {
        if (_impl->_dataset == nullptr)
        {
            throw CoreError("Raster map layers do not have a vector dataset");
        }
        return *_impl->_dataset;
    }

    RasterDataset* MapLayer::RasterSource() const noexcept
    {
        return _impl->_raster.get();
    }

    bool MapLayer::IsRaster() const noexcept
    {
        return _impl->_raster != nullptr;
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
        const Extent extent = IsRaster()
            ? _impl->_raster->GetExtent()
            : _impl->_source->GetExtent();
        if (!extent.IsValid())
        {
            return {};
        }

        const std::string sourceWkt = IsRaster()
            ? _impl->_raster->ProjectionWkt()
            : _impl->_source->ProjectionWkt();
        detail::CoordinateTransformation transform(sourceWkt, _impl->_mapProjectionWkt);
        if (!transform.IsNeeded())
        {
            return extent;
        }

        Extent result;
        constexpr int subdivisions = 32;
        for (int index = 0; index <= subdivisions; ++index)
        {
            const double fraction = static_cast<double>(index) / subdivisions;
            const double x = extent.xMin + (extent.xMax - extent.xMin) * fraction;
            const double y = extent.yMin + (extent.yMax - extent.yMin) * fraction;
            const double edgeX[] = {x, extent.xMax, x, extent.xMin};
            const double edgeY[] = {extent.yMin, y, extent.yMax, y};
            for (int edge = 0; edge < 4; ++edge)
            {
                double transformedX = edgeX[edge];
                double transformedY = edgeY[edge];
                if (!transform.Apply(transformedX, transformedY) ||
                    !std::isfinite(transformedX) || !std::isfinite(transformedY))
                {
                    throw CoreError("Cannot transform the layer extent into the map coordinate system");
                }
                result = result.Union({transformedX, transformedY, transformedX, transformedY});
            }
        }
        return result;
    }

    class Map::Impl
    {
    public:
        [[nodiscard]] std::vector<std::unique_ptr<MapLayer>>::iterator Find(
            const LayerId id) noexcept
        {
            return std::find_if(
                _layers.begin(),
                _layers.end(),
                [id](const std::unique_ptr<MapLayer>& layer)
                {
                    return layer->Id() == id;
                });
        }

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
        std::string _coordinateSystemWkt;
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

        const std::string sourceWkt = dataset->Layer(layerIndex).ProjectionWkt();
        const std::string& mapWkt = _impl->_layers.empty()
            ? sourceWkt
            : _impl->_coordinateSystemWkt;
        detail::CoordinateTransformation validateTransform(sourceWkt, mapWkt);

        const LayerId id{_impl->_lastId + 1};
        auto layer = std::unique_ptr<MapLayer>(
            new MapLayer(std::make_unique<MapLayer::Impl>(
                id, std::move(dataset), layerIndex, mapWkt)));
        _impl->_layers.push_back(std::move(layer));
        if (_impl->_layers.size() == 1)
        {
            _impl->_coordinateSystemWkt = sourceWkt;
        }
        _impl->_lastId = id.value;
        return id;
    }

    LayerId Map::AddRasterLayer(std::shared_ptr<RasterDataset> dataset)
    {
        if (dataset == nullptr)
        {
            throw InvalidArgument("Cannot add a layer without a raster dataset");
        }
        if (_impl->_lastId == std::numeric_limits<std::uint64_t>::max())
        {
            throw CoreError("Map layer identity space is exhausted");
        }

        const std::string sourceWkt = dataset->ProjectionWkt();
        const std::string& mapWkt = _impl->_layers.empty()
            ? sourceWkt
            : _impl->_coordinateSystemWkt;
        detail::CoordinateTransformation validateTransform(sourceWkt, mapWkt);

        const LayerId id{_impl->_lastId + 1};
        auto layer = std::unique_ptr<MapLayer>(
            new MapLayer(std::make_unique<MapLayer::Impl>(id, std::move(dataset), mapWkt)));
        _impl->_layers.push_back(std::move(layer));
        if (_impl->_layers.size() == 1)
        {
            _impl->_coordinateSystemWkt = sourceWkt;
        }
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
        if (_impl->_layers.empty())
        {
            _impl->_coordinateSystemWkt.clear();
        }
        return true;
    }

    void Map::Clear() noexcept
    {
        _impl->_layers.clear();
        _impl->_coordinateSystemWkt.clear();
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

    bool Map::MoveLayer(const LayerId id, const int position)
    {
        if (position < 0 || position >= LayerCount())
        {
            throw InvalidArgument("Map layer position out of range: " + std::to_string(position));
        }

        const auto layer = _impl->Find(id);
        if (layer == _impl->_layers.end())
        {
            return false;
        }

        const auto currentPosition =
            static_cast<int>(layer - _impl->_layers.begin());
        if (currentPosition == position)
        {
            return false;
        }

        auto target = _impl->_layers.begin() + position;
        if (currentPosition < position)
        {
            std::rotate(layer, layer + 1, target + 1);
        }
        else
        {
            std::rotate(target, layer, layer + 1);
        }
        return true;
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

    std::string Map::CoordinateSystemWkt() const
    {
        return _impl->_coordinateSystemWkt;
    }
    }
