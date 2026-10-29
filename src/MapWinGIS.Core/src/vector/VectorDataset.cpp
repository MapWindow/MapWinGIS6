// SPDX-License-Identifier: MPL-2.0

/**
 * @file VectorDataset.cpp
 * @brief Implements OGR-backed vector datasets and layers.
 *
 * Handles feature and field access, geometry and attribute conversion,
 * indexed feature lookup, and writable dataset updates.
 */
#include "mapwingis/vector/VectorDataset.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal_priv.h>
#include <ogr_api.h>
#include <ogrsf_frmts.h>

#include "internal/Utf8.h"
#include "internal/VectorLayerAccess.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"

namespace mwg
{
    using detail::ToUtf8;

    namespace
    {
        ShapeType ToShapeType(const OGRwkbGeometryType type)
        {
            const OGRwkbGeometryType flat = wkbFlatten(type);
            const bool hasZ = wkbHasZ(type) != FALSE;
            const bool hasM = wkbHasM(type) != FALSE;

            auto pick = [hasZ, hasM](const ShapeType plain, const ShapeType z, const ShapeType m)
            {
                if (hasZ) return z;
                if (hasM) return m;
                return plain;
            };

            switch (flat)
            {
            case wkbPoint:
                return pick(ShapeType::Point, ShapeType::PointZ, ShapeType::PointM);
            case wkbMultiPoint:
                return pick(ShapeType::MultiPoint, ShapeType::MultiPointZ, ShapeType::MultiPointM);
            case wkbLineString:
            case wkbMultiLineString:
            case wkbCircularString:
            case wkbCompoundCurve:
            case wkbMultiCurve:
                return pick(ShapeType::PolyLine, ShapeType::PolyLineZ, ShapeType::PolyLineM);
            case wkbPolygon:
            case wkbMultiPolygon:
            case wkbCurvePolygon:
            case wkbMultiSurface:
                return pick(ShapeType::Polygon, ShapeType::PolygonZ, ShapeType::PolygonM);
            default:
                return ShapeType::NullShape;
            }
        }

        FieldInfo ToFieldInfo(const OGRFieldDefn& defn)
        {
            FieldInfo info;
            info.name = defn.GetNameRef() != nullptr ? defn.GetNameRef() : "";
            info.width = defn.GetWidth();
            info.precision = defn.GetPrecision();

            switch (defn.GetType())
            {
            case OFTInteger:
                info.type = defn.GetSubType() == OFSTBoolean ? FieldType::Boolean : FieldType::Integer;
                break;
            case OFTInteger64:
                info.type = FieldType::Integer;
                break;
            case OFTReal:
                info.type = FieldType::Double;
                break;
            case OFTDate:
            case OFTDateTime:
            case OFTTime:
                info.type = FieldType::Date;
                break;
            default:
                info.type = FieldType::String;
                break;
            }
            return info;
        }

        bool EqualsIgnoreCase(const std::string& a, const std::string& b)
        {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(),
                              [](const unsigned char x, const unsigned char y)
                              {
                                  return std::tolower(x) == std::tolower(y);
                              });
        }
    }

    // ---------------------------------------------------------------- Layer

    class VectorLayer::Impl
    {
    public:
        Impl(OGRLayer* layer, const bool writable)
            : _layer(layer), _writable(writable)
        {
            if (const OGRFeatureDefn* defn = _layer->GetLayerDefn())
            {
                _fields.reserve(static_cast<std::size_t>(defn->GetFieldCount()));
                for (int i = 0; i < defn->GetFieldCount(); ++i)
                    _fields.push_back(ToFieldInfo(*defn->GetFieldDefn(i)));
            }
        }

        /// Borrowed pointer, valid until the next call. Throws when the index is
        /// out of range or the feature cannot be read.
        ///
        /// Callers read several fields of the same feature in a row, so the most
        /// recently used feature is kept. Without it, reading five attributes of
        /// one feature would mean five reads from the file.
        [[nodiscard]] OGRFeature* GetFeature(const std::int64_t index) const
        {
            if (index < 0)
                throw CoreError("Feature index out of range: " + std::to_string(index));
            if (_cachedIndex == index && _cachedFeature) return _cachedFeature.get();

            EnsureFeatureIds();
            if (std::cmp_greater_equal(index, _featureIds.size()))
                throw CoreError("Feature index out of range: " + std::to_string(index));

            GdalEnvironment::ClearLastError();
            _cachedFeature.reset(_layer->GetFeature(_featureIds[static_cast<std::size_t>(index)]));
            _cachedIndex = _cachedFeature ? index : -1;
            if (!_cachedFeature)
            {
                std::string message = "Cannot read feature at index " + std::to_string(index);
                if (const std::string detail = GdalEnvironment::LastErrorMessage(); !detail.empty())
                    message += " (" + detail + ")";
                throw CoreError(message);
            }
            return _cachedFeature.get();
        }

        [[nodiscard]] OGRFeature* GetFeatureForEdit(const std::int64_t index) const
        {
            if (!_writable)
                throw CoreError("The dataset was not opened for update");

            return GetFeature(index);
        }

        void Save(OGRFeature* feature) const
        {
            if (_layer->SetFeature(feature) != OGRERR_NONE)
            {
                std::string message = "Cannot write feature " + std::to_string(feature->GetFID());
                if (const std::string detail = GdalEnvironment::LastErrorMessage(); !detail.empty())
                    message += " (" + detail + ")";
                throw CoreError(message);
            }
            _extent.reset();
        }

        [[nodiscard]] std::int64_t FeatureCount() const
        {
            if (!_featureCount)
                _featureCount = static_cast<std::int64_t>(_layer->GetFeatureCount(TRUE));
            return *_featureCount;
        }

        [[nodiscard]] Extent GetExtent() const
        {
            if (!_extent)
            {
                OGREnvelope envelope;
                _extent = _layer->GetExtent(&envelope, TRUE) == OGRERR_NONE
                              ? Extent{envelope.MinX, envelope.MinY, envelope.MaxX, envelope.MaxY}
                              : Extent{};
            }
            return *_extent;
        }

        OGRLayer* _layer;
        bool _writable;
        std::vector<FieldInfo> _fields;

    private:
        /// MapWinGIS addresses features by 0-based index; OGR addresses them by
        /// feature id, which is not the same thing: shapefile ids start at 0,
        /// GeoPackage ids at 1, and either can have gaps. Building the mapping
        /// costs one pass over the layer, so it is deferred until an indexed
        /// lookup actually happens, and the pass asks the driver for ids only.
        void EnsureFeatureIds() const
        {
            if (_featureIdsValid) return;

            // Keep expected diagnostics suppressed for the entire scan, not
            // just while SetIgnoredFields is configured.
            const GdalEnvironment::QuietScope quiet;
            const IgnoreAllButIds ignoreScope(*_layer, _fields);

            _featureIds.clear();

            // Do not force a full count before scanning every feature for its
            // ID. Some drivers implement GetFeatureCount(TRUE) by scanning the
            // entire layer, which would make this index build perform two passes.
            // Reserve only when the driver can provide a count without forcing it.
            const GIntBig count = _layer->GetFeatureCount(FALSE);
            if (count > 0 && std::cmp_less_equal(count, _featureIds.max_size()))
            {
                _featureIds.reserve(static_cast<std::size_t>(count));
            }

            _layer->ResetReading();
            while (const OGRFeatureUniquePtr feature{_layer->GetNextFeature()})
                _featureIds.push_back(feature->GetFID());
            _layer->ResetReading();

            _featureIdsValid = true;
        }

        /// Tells the driver to skip attributes and geometry for the duration of
        /// the id scan, and restores the layer afterwards.
        class IgnoreAllButIds
        {
        public:
            IgnoreAllButIds(OGRLayer& layer, const std::vector<FieldInfo>& fields)
                : _layer(layer)
            {
                std::vector<const char*> ignored;
                ignored.reserve(fields.size() + 2);
                for (const FieldInfo& field : fields) ignored.push_back(field.name.c_str());
                ignored.push_back("OGR_GEOMETRY");
                ignored.push_back(nullptr);

                _applied = _layer.SetIgnoredFields(ignored.data()) == OGRERR_NONE;
            }

            ~IgnoreAllButIds()
            {
                if (_applied) _layer.SetIgnoredFields(nullptr);
            }

            IgnoreAllButIds(const IgnoreAllButIds&) = delete;
            IgnoreAllButIds& operator=(const IgnoreAllButIds&) = delete;

        private:
            OGRLayer& _layer;
            bool _applied = false;
        };

        mutable std::vector<GIntBig> _featureIds;
        mutable bool _featureIdsValid = false;
        mutable OGRFeatureUniquePtr _cachedFeature;
        mutable std::int64_t _cachedIndex = -1;
        mutable std::optional<std::int64_t> _featureCount;
        mutable std::optional<Extent> _extent;
    };

    void detail::VectorLayerAccess::ForEachGeometry(
        const VectorLayer& layer,
        const std::function<void(const OGRGeometry&)>& callback)
    {
        OGRLayer& ogrLayer = *layer._impl->_layer;
        ogrLayer.ResetReading();
        struct ReadingReset final
        {
            OGRLayer& layer;

            ~ReadingReset()
            {
                layer.ResetReading();
            }
        } readingReset{ogrLayer};

        while (true)
        {
            CPLErrorReset();
            OGRFeatureUniquePtr feature{ogrLayer.GetNextFeature()};
            if (feature == nullptr)
            {
                if (CPLGetLastErrorType() >= CE_Failure)
                {
                    std::string message = "Failed to read vector layer features";
                    const char* const detail = CPLGetLastErrorMsg();
                    if (detail != nullptr && detail[0] != '\0')
                    {
                        message += ": ";
                        message += detail;
                    }
                    throw CoreError(message);
                }
                break;
            }

            const OGRGeometry* geometry = feature->GetGeometryRef();
            if (geometry != nullptr && !geometry->IsEmpty())
            {
                callback(*geometry);
            }
        }
    }

    VectorLayer::VectorLayer(std::unique_ptr<Impl> impl)
        : _impl(std::move(impl))
    {
    }

    VectorLayer::~VectorLayer() = default;

    std::string VectorLayer::Name() const
    {
        const char* name = _impl->_layer->GetName();
        return name != nullptr ? name : std::string{};
    }

    ShapeType VectorLayer::Type() const
    {
        return ToShapeType(_impl->_layer->GetGeomType());
    }

    std::int64_t VectorLayer::FeatureCount() const
    {
        return _impl->FeatureCount();
    }

    Extent VectorLayer::GetExtent() const
    {
        return _impl->GetExtent();
    }

    int VectorLayer::FieldCount() const
    {
        return static_cast<int>(_impl->_fields.size());
    }

    const std::vector<FieldInfo>& VectorLayer::Fields() const
    {
        return _impl->_fields;
    }

    const FieldInfo& VectorLayer::Field(const int index) const
    {
        if (index < 0 || index >= FieldCount())
            throw CoreError("Field index out of range: " + std::to_string(index));
        return _impl->_fields[static_cast<std::size_t>(index)];
    }

    int VectorLayer::FieldIndex(const std::string& name) const
    {
        for (std::size_t i = 0; i < _impl->_fields.size(); ++i)
        {
            if (EqualsIgnoreCase(_impl->_fields[i].name, name)) return static_cast<int>(i);
        }
        return -1;
    }

    std::string VectorLayer::ProjectionWkt() const
    {
        const OGRSpatialReference* srs = _impl->_layer->GetSpatialRef();
        if (srs == nullptr) return {};

        char* wkt = nullptr;
        if (srs->exportToWkt(&wkt) != OGRERR_NONE || wkt == nullptr)
        {
            CPLFree(wkt);
            return {};
        }

        std::string result(wkt);
        CPLFree(wkt);
        return result;
    }

    bool VectorLayer::IsCellNull(const int fieldIndex, const std::int64_t featureIndex) const
    {
        ValidateField(fieldIndex);
        const OGRFeature* feature = _impl->GetFeature(featureIndex);
        return !feature->IsFieldSetAndNotNull(fieldIndex);
    }

    std::string VectorLayer::CellString(const int fieldIndex, const std::int64_t featureIndex) const
    {
        ValidateField(fieldIndex);
        const OGRFeature* feature = _impl->GetFeature(featureIndex);
        if (!feature->IsFieldSetAndNotNull(fieldIndex)) return {};

        const char* value = feature->GetFieldAsString(fieldIndex);
        return value != nullptr ? value : std::string{};
    }

    std::optional<double> VectorLayer::CellDouble(const int fieldIndex, const std::int64_t featureIndex) const
    {
        ValidateField(fieldIndex);
        const OGRFeature* feature = _impl->GetFeature(featureIndex);
        if (!feature->IsFieldSetAndNotNull(fieldIndex)) return std::nullopt;
        return feature->GetFieldAsDouble(fieldIndex);
    }

    std::optional<std::int64_t> VectorLayer::CellInteger(const int fieldIndex, const std::int64_t featureIndex) const
    {
        ValidateField(fieldIndex);
        const OGRFeature* feature = _impl->GetFeature(featureIndex);
        if (!feature->IsFieldSetAndNotNull(fieldIndex)) return std::nullopt;
        return static_cast<std::int64_t>(feature->GetFieldAsInteger64(fieldIndex));
    }

    std::string VectorLayer::GeometryWkt(const std::int64_t featureIndex) const
    {
        const OGRFeature* feature = _impl->GetFeature(featureIndex);

        const OGRGeometry* geometry = feature->GetGeometryRef();
        if (geometry == nullptr) return {};

        char* wkt = nullptr;
        if (geometry->exportToWkt(&wkt, wkbVariantIso) != OGRERR_NONE || wkt == nullptr)
        {
            CPLFree(wkt);
            return {};
        }

        std::string result(wkt);
        CPLFree(wkt);
        return result;
    }

    Extent VectorLayer::GeometryExtent(const std::int64_t featureIndex) const
    {
        const OGRFeature* feature = _impl->GetFeature(featureIndex);

        const OGRGeometry* geometry = feature->GetGeometryRef();
        if (geometry == nullptr || geometry->IsEmpty()) return {};

        OGREnvelope envelope;
        geometry->getEnvelope(&envelope);
        return {envelope.MinX, envelope.MinY, envelope.MaxX, envelope.MaxY};
    }

    bool VectorLayer::IsWritable() const
    {
        return _impl->_writable;
    }

    void VectorLayer::ValidateField(const int fieldIndex) const
    {
        if (fieldIndex < 0 || fieldIndex >= FieldCount())
            throw CoreError("Field index out of range: " + std::to_string(fieldIndex));
    }

    void VectorLayer::SetCellString(const int fieldIndex, const std::int64_t featureIndex,
                                    const std::string& value)
    {
        ValidateField(fieldIndex);
        OGRFeature* feature = _impl->GetFeatureForEdit(featureIndex);
        feature->SetField(fieldIndex, value.c_str());
        _impl->Save(feature);
    }

    void VectorLayer::SetCellDouble(const int fieldIndex, const std::int64_t featureIndex,
                                    const double value)
    {
        ValidateField(fieldIndex);
        OGRFeature* feature = _impl->GetFeatureForEdit(featureIndex);
        feature->SetField(fieldIndex, value);
        _impl->Save(feature);
    }

    void VectorLayer::SetCellInteger(const int fieldIndex, const std::int64_t featureIndex,
                                     const std::int64_t value)
    {
        ValidateField(fieldIndex);
        OGRFeature* feature = _impl->GetFeatureForEdit(featureIndex);
        feature->SetField(fieldIndex, static_cast<GIntBig>(value));
        _impl->Save(feature);
    }

    void VectorLayer::SetCellNull(const int fieldIndex, const std::int64_t featureIndex)
    {
        ValidateField(fieldIndex);
        OGRFeature* feature = _impl->GetFeatureForEdit(featureIndex);
        feature->SetFieldNull(fieldIndex);
        _impl->Save(feature);
    }

    void VectorLayer::SetGeometryWkt(const std::int64_t featureIndex, const std::string& wkt)
    {
        OGRFeature* feature = _impl->GetFeatureForEdit(featureIndex);

        OGRGeometry* parsed = nullptr;
        const char* text = wkt.c_str();
        if (OGRGeometryFactory::createFromWkt(&text, _impl->_layer->GetSpatialRef(), &parsed) != OGRERR_NONE ||
            parsed == nullptr)
        {
            throw CoreError("Cannot parse WKT: " + wkt);
        }

        if (feature->SetGeometryDirectly(parsed) != OGRERR_NONE)
            throw CoreError("Cannot assign geometry to feature");

        _impl->Save(feature);
    }

    // -------------------------------------------------------------- Dataset

    class VectorDataset::Impl
    {
    public:
        Impl(GDALDatasetUniquePtr dataset, std::filesystem::path path, const bool writable)
            : _dataset(std::move(dataset)), _path(std::move(path)), _writable(writable)
        {
            const int count = _dataset->GetLayerCount();
            _layers.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i)
            {
                OGRLayer* layer = _dataset->GetLayer(i);
                _layers.push_back(std::unique_ptr<VectorLayer>(
                    new VectorLayer(std::make_unique<VectorLayer::Impl>(layer, writable))));
            }
        }

        GDALDatasetUniquePtr _dataset;
        std::filesystem::path _path;
        bool _writable;
        std::vector<std::unique_ptr<VectorLayer>> _layers;
    };

    VectorDataset::VectorDataset(std::unique_ptr<Impl> impl)
        : _impl(std::move(impl))
    {
    }

    VectorDataset::~VectorDataset() = default;

    std::unique_ptr<VectorDataset> VectorDataset::TryOpen(const std::filesystem::path& path,
                                                          const OpenMode mode)
    {
        GdalEnvironment::Initialize();
        GdalEnvironment::ClearLastError();

        const bool writable = mode == OpenMode::Update;
        const unsigned int flags =
            GDAL_OF_VECTOR | GDAL_OF_VERBOSE_ERROR | (writable ? GDAL_OF_UPDATE : GDAL_OF_READONLY);

        GDALDatasetUniquePtr dataset(
            GDALDataset::Open(ToUtf8(path).c_str(), flags, nullptr, nullptr, nullptr));

        if (!dataset) return nullptr;
        return std::unique_ptr<VectorDataset>(
            new VectorDataset(std::make_unique<Impl>(std::move(dataset), path, writable)));
    }

    std::unique_ptr<VectorDataset> VectorDataset::Open(const std::filesystem::path& path,
                                                       const OpenMode mode)
    {
        if (std::unique_ptr<VectorDataset> dataset = TryOpen(path, mode)) return dataset;

        std::string message = "Cannot open vector data source: " + ToUtf8(path);
        if (const std::string detail = GdalEnvironment::LastErrorMessage(); !detail.empty())
            message += " (" + detail + ")";
        throw OpenFailed(message);
    }

    bool VectorDataset::IsWritable() const
    {
        return _impl->_writable;
    }

    void VectorDataset::Flush()
    {
        GdalEnvironment::ClearLastError();
        if (_impl->_dataset->FlushCache(false) != CE_None)
        {
            std::string message = "Cannot flush vector data source: " + ToUtf8(_impl->_path);
            if (const char* detail = CPLGetLastErrorMsg(); detail != nullptr && *detail != '\0')
                message += " (" + std::string(detail) + ")";
            throw CoreError(message);
        }
    }

    std::string VectorDataset::DriverName() const
    {
        GDALDriver* driver = _impl->_dataset->GetDriver();
        if (driver == nullptr) return {};
        const char* name = driver->GetDescription();
        return name != nullptr ? name : std::string{};
    }

    std::filesystem::path VectorDataset::Path() const
    {
        return _impl->_path;
    }

    int VectorDataset::LayerCount() const
    {
        return static_cast<int>(_impl->_layers.size());
    }

    VectorLayer& VectorDataset::Layer(const int index) const
    {
        if (index < 0 || index >= LayerCount())
            throw CoreError("Layer index out of range: " + std::to_string(index));
        return *_impl->_layers[static_cast<std::size_t>(index)];
    }

    VectorLayer* VectorDataset::LayerByName(const std::string& name) const
    {
        for (const std::unique_ptr<VectorLayer>& layer : _impl->_layers)
        {
            if (EqualsIgnoreCase(layer->Name(), name)) return layer.get();
        }
        return nullptr;
    }
}
