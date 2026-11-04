// SPDX-License-Identifier: MPL-2.0

/**
 * @file Map.h
 * @brief Declares the map model: an ordered collection of map layers.
 *
 * The map owns its layer collection and map-level layer state, such as
 * visibility, and calculates the combined extent of its layers. It holds no
 * windowing, COM, or rendering-backend state.
 *
 * The first layer with a declared coordinate reference system sets the map
 * coordinate system. Layers in another declared system are reprojected into
 * it; layers with a missing coordinate system can only be combined with
 * similarly unreferenced layers.
 */
#pragma once

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "mapwingis/core/Api.h"
#include "mapwingis/core/Extent.h"

namespace mwg
{
    class Map;
    class RasterDataset;
    class VectorDataset;
    class VectorLayer;

    /// Stable identity of a layer within one Map.
    ///
    /// Assigned by Map::AddLayer, starting at 1 and increasing. An identity is
    /// never reused by the same map, not even after the layer is removed or
    /// the map is cleared, so a stale identity can never select another layer.
    /// The default value (0) identifies no layer.
    struct LayerId
    {
        std::uint64_t value = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0; }

        friend constexpr auto operator<=>(const LayerId&, const LayerId&) noexcept = default;
    };

    /// Which layers contribute to a map-level extent.
    enum class ExtentScope : int
    {
        /// Only visible layers. This is the extent used for zoom-to-map.
        VisibleLayers,
        /// All layers, including hidden ones.
        AllLayers,
    };

    /// One vector or raster layer of a Map plus map-level state.
    ///
    /// Owned by its Map. A MapLayer reference stays valid until that layer is
    /// removed, the map is cleared, or the map is destroyed; adding, removing,
    /// or clearing other layers does not invalidate it. The MapLayer shares
    /// ownership of its dataset, so the source layer stays valid for the
    /// MapLayer's lifetime even when the caller releases the dataset.
    ///
    /// Not thread-safe, consistent with VectorLayer.
    class MWG_CORE_API MapLayer
    {
    public:
        ~MapLayer();

        MapLayer(const MapLayer&) = delete;
        MapLayer& operator=(const MapLayer&) = delete;

        [[nodiscard]] LayerId Id() const noexcept;

        /// The vector layer displayed by this map layer.
        /// Vector source. Throws CoreError when this is a raster layer.
        [[nodiscard]] VectorLayer& Source() const;

        /// The dataset that owns Source().
        /// Dataset owning Source(). Throws CoreError for raster layers.
        [[nodiscard]] VectorDataset& Dataset() const;

        /// Raster source, or nullptr for a vector layer.
        [[nodiscard]] RasterDataset* RasterSource() const noexcept;

        [[nodiscard]] bool IsRaster() const noexcept;

        /// 0-based index of Source() within Dataset().
        [[nodiscard]] int SourceLayerIndex() const noexcept;

        /// Layers are visible when added. Change visibility through
        /// Map::SetLayerVisible.
        [[nodiscard]] bool IsVisible() const noexcept;

        /// Extent in the map coordinate system, read from the source layer
        /// on every call so edits are reflected. Empty when the layer has no
        /// geometry or its extent is not valid (see Extent::IsValid).
        /// May read the whole layer for formats that don't store an extent;
        /// the source layer caches the result.
        [[nodiscard]] Extent GetExtent() const;

    private:
        friend class Map;

        class Impl;
        explicit MapLayer(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> _impl;
    };

    /// The map model: an ordered collection of layers and map-level state.
    ///
    /// Ordering: position 0 is the bottom layer, drawn first; the last
    /// position is the top layer, drawn last. AddLayer appends a layer on top.
    /// Positions shift when layers are removed, so use LayerId to refer to a
    /// layer over time.
    ///
    /// Duplicates: the same source layer may be added more than once. Each
    /// addition is a separate map layer with its own identity and state.
    ///
    /// Extents: map-level extents are computed from the layers on every call,
    /// so they reflect visibility changes, added and removed layers, and edits
    /// to source geometry without explicit invalidation. Layers whose extent
    /// is empty or not valid are ignored. A map without any valid layer extent
    /// reports an empty Extent.
    ///
    /// Not thread-safe; see VectorLayer.
    class MWG_CORE_API Map
    {
    public:
        Map();
        ~Map();

        Map(const Map&) = delete;
        Map& operator=(const Map&) = delete;

        /// Adds layer layerIndex of dataset on top of the map and returns its
        /// new identity. The map shares ownership of the dataset; the dataset
        /// is released when its last map layer is removed and no other owner
        /// remains. Raster source ownership follows the same rule.
        /// \throws InvalidArgument when dataset is null or layerIndex is out of
        ///         range.
        /// \throws CoreError when the map has exhausted its layer identity space.
        LayerId AddLayer(std::shared_ptr<VectorDataset> dataset, int layerIndex = 0);

        /// Adds a raster source on top of the map and returns its identity.
        /// Its data is shared with the map until the layer is removed.
        /// \throws InvalidArgument when dataset is null or its coordinate
        ///         system cannot be combined with the map's.
        /// \throws CoreError when the map has exhausted its identity space.
        LayerId AddRasterLayer(std::shared_ptr<RasterDataset> dataset);

        /// Removes the layer with this identity. Returns false, and changes
        /// nothing, when the map has no such layer.
        bool RemoveLayer(LayerId id) noexcept;

        /// Removes all layers. Identities of removed layers are not reused.
        void Clear() noexcept;

        [[nodiscard]] int LayerCount() const noexcept;

        /// Layer at a 0-based position; see the ordering rules above.
        /// \throws InvalidArgument when position is out of range.
        [[nodiscard]] const MapLayer& Layer(int position) const;

        /// nullptr when the map has no layer with this identity.
        [[nodiscard]] const MapLayer* FindLayer(LayerId id) const noexcept;

        /// Current 0-based position of a layer, or std::nullopt when the map
        /// has no layer with this identity.
        [[nodiscard]] std::optional<int> LayerPosition(LayerId id) const noexcept;

        /// Moves a layer to a new 0-based position. Returns false when the
        /// layer is unknown or already at that position.
        /// \throws InvalidArgument when position is outside the collection.
        bool MoveLayer(LayerId id, int position);

        /// Shows or hides a layer. Returns false when the map has no layer
        /// with this identity.
        bool SetLayerVisible(LayerId id, bool visible) noexcept;

        /// Union of the valid extents of the layers in scope, or an empty
        /// Extent when none of them has a valid extent. Deterministic for a
        /// given set of layer extents. Reads each layer's extent; see
        /// MapLayer::GetExtent for the cost.
        [[nodiscard]] Extent GetExtent(ExtentScope scope = ExtentScope::VisibleLayers) const;

        /// Coordinate reference system of the first added layer, as WKT.
        /// Empty when the map is empty or that layer has no CRS.
        [[nodiscard]] std::string CoordinateSystemWkt() const;

    private:
        class Impl;
        std::unique_ptr<Impl> _impl;
    };
}
