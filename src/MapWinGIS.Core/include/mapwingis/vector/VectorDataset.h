// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorDataset.h
 * @brief Declares the vector dataset and layer APIs.
 *
 * Provides OGR-backed access to vector data, including feature attributes,
 * geometry, and updates. Feature indexes are 0-based for MapWinGIS 5
 * compatibility; dataset and layer access is not thread-safe.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mapwingis/core/Api.h"
#include "mapwingis/core/Enums.h"
#include "mapwingis/core/Extent.h"
#include "mapwingis/vector/FieldInfo.h"

namespace mwg
{
    class VectorDataset;

    enum class OpenMode
    {
        ReadOnly,
        Update,
    };

    /// One vector layer of an open dataset.
    ///
    /// Layers are owned by their VectorDataset and stay valid for as long as it
    /// does. Feature indexes are 0-based, matching MapWinGIS 5 shape indexes.
    ///
    /// Not thread-safe, not even for the const methods: they cache the most
    /// recently read feature, the feature count and the extent. Use one
    /// VectorDataset per thread, or serialise access.
    ///
    /// The first indexed feature access costs one pass over the layer to map
    /// indexes onto feature ids. Later accesses are direct lookups.
    class MWG_CORE_API VectorLayer
    {
    public:
        ~VectorLayer();

        VectorLayer(const VectorLayer&) = delete;
        VectorLayer& operator=(const VectorLayer&) = delete;

        [[nodiscard]] std::string Name() const;

        /// Geometry type, mapped onto the shapefile types MapWinGIS 5 exposes.
        [[nodiscard]] ShapeType Type() const;

        /// Exact count. May read the whole layer for formats that don't keep one.
        [[nodiscard]] std::int64_t FeatureCount() const;

        [[nodiscard]] Extent GetExtent() const;

        [[nodiscard]] int FieldCount() const;
        [[nodiscard]] const std::vector<FieldInfo>& Fields() const;
        [[nodiscard]] const FieldInfo& Field(int index) const;
        /// -1 when there is no field with that name (case-insensitive).
        [[nodiscard]] int FieldIndex(const std::string& name) const;

        /// Layer projection as WKT, or "" when the layer has none.
        [[nodiscard]] std::string ProjectionWkt() const;

        [[nodiscard]] bool IsCellNull(int fieldIndex, std::int64_t featureIndex) const;
        /// Attribute formatted as text, which is what MapWinGIS 5 CellValue yields
        /// for display purposes.
        [[nodiscard]] std::string CellString(int fieldIndex, std::int64_t featureIndex) const;
        [[nodiscard]] std::optional<double> CellDouble(int fieldIndex, std::int64_t featureIndex) const;
        [[nodiscard]] std::optional<std::int64_t> CellInteger(int fieldIndex, std::int64_t featureIndex) const;

        /// ISO WKT of the feature geometry, or "" when the feature has none.
        [[nodiscard]] std::string GeometryWkt(std::int64_t featureIndex) const;
        [[nodiscard]] Extent GeometryExtent(std::int64_t featureIndex) const;

        /// True when the dataset was opened with OpenMode::Update.
        [[nodiscard]] bool IsWritable() const;

        /// Attribute and geometry edits. Each writes the feature back
        /// immediately; the file is complete once the dataset is closed.
        /// \throws CoreError when the dataset is read-only or the index is invalid.
        void SetCellString(int fieldIndex, std::int64_t featureIndex, const std::string& value);
        void SetCellDouble(int fieldIndex, std::int64_t featureIndex, double value);
        void SetCellInteger(int fieldIndex, std::int64_t featureIndex, std::int64_t value);
        void SetCellNull(int fieldIndex, std::int64_t featureIndex);
        void SetGeometryWkt(std::int64_t featureIndex, const std::string& wkt);

    private:
        friend class VectorDataset;

        void ValidateField(int fieldIndex) const;

        class Impl;
        explicit VectorLayer(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> _impl;
    };

    /// An open vector data source: a shapefile, a GeoPackage, a database
    /// connection, anything OGR can read.
    ///
    /// Not thread-safe; see VectorLayer.
    class MWG_CORE_API VectorDataset
    {
    public:
        ~VectorDataset();

        VectorDataset(const VectorDataset&) = delete;
        VectorDataset& operator=(const VectorDataset&) = delete;

        /// Opens a data source.
        /// \throws OpenFailed when no driver can read it, or when update was
        ///         requested and the driver or file does not allow it.
        [[nodiscard]] static std::unique_ptr<VectorDataset> Open(const std::filesystem::path& path,
                                                                 OpenMode mode = OpenMode::ReadOnly);

        /// Returns nullptr instead of throwing.
        [[nodiscard]] static std::unique_ptr<VectorDataset> TryOpen(const std::filesystem::path& path,
                                                                    OpenMode mode = OpenMode::ReadOnly);

        [[nodiscard]] std::string DriverName() const;
        [[nodiscard]] std::filesystem::path Path() const;
        [[nodiscard]] bool IsWritable() const;

        /// Writes pending changes to disk without closing. Closing the dataset
        /// does this as well.
        void Flush();

        [[nodiscard]] int LayerCount() const;
        [[nodiscard]] VectorLayer& Layer(int index) const;
        /// nullptr when there is no layer with that name.
        [[nodiscard]] VectorLayer* LayerByName(const std::string& name) const;

    private:
        class Impl;
        explicit VectorDataset(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> _impl;
    };
}
