// SPDX-License-Identifier: MPL-2.0

/**
 * @file TestData.cpp
 * @brief Implements shared raster and vector test fixture helpers.
 *
 * Manages temporary directories and creates georeferenced rasters and polygon
 * datasets with attributes, null values, and projections for the core tests.
 */
#include "TestData.h"

#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <gdal_priv.h>
#include <cpl_error.h>
#include <ogrsf_frmts.h>

#include "mapwingis/gdal/GdalEnvironment.h"

namespace mwg::test
{
    namespace
    {
        std::string ToUtf8(const std::filesystem::path& path)
        {
            const std::u8string utf8 = path.u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }

        std::string RandomName()
        {
            static std::mt19937_64 engine{std::random_device{}()};
            std::uniform_int_distribution<std::uint64_t> distribution;
            return "mwg-test-" + std::to_string(distribution(engine));
        }

        class VectorReadTestLayer final : public OGRLayer
        {
        public:
            explicit VectorReadTestLayer(VectorReadTestDriver& state)
                : _state(state),
                  _definition(OGRFeatureDefn::CreateFeatureDefn(state.layerName.c_str()))
            {
                _definition->Reference();
                _definition->SetGeomType(wkbPolygon);
                OGRFieldDefn nameField("NAME", OFTString);
                _definition->AddFieldDefn(&nameField);
                OGRFieldDefn valueField("VALUE", OFTInteger);
                _definition->AddFieldDefn(&valueField);
            }

            ~VectorReadTestLayer() override
            {
                _definition->Release();
            }

            void ResetReading() override
            {
                _state.nextFeature = 0;
            }

            OGRFeature* GetNextFeature() override
            {
                ++_state.readCalls;
                if (_state.failOnSecondRead && _state.nextFeature == 1)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Simulated feature read failure");
                    return nullptr;
                }
                if (_state.emptyLayer || _state.nextFeature == 3)
                {
                    return nullptr;
                }
                return GetFeature(_state.nextFeature++);
            }

            OGRFeature* GetFeature(const GIntBig id) override
            {
                OGRFeatureUniquePtr feature(OGRFeature::CreateFeature(_definition));
                feature->SetFID(id);
                feature->SetField(0, _state.storedName.c_str());
                feature->SetField(1, _state.storedValue);
                if (!_state.ignoredGeometry && id != 1)
                {
                    OGRPolygon polygon;
                    if (id == 0 && !_state.onlyEmptyGeometries)
                    {
                        OGRLinearRing ring;
                        ring.addPoint(0.0, 0.0);
                        ring.addPoint(1.0, 0.0);
                        ring.addPoint(1.0, 1.0);
                        ring.addPoint(0.0, 1.0);
                        ring.addPoint(0.0, 0.0);
                        polygon.addRing(&ring);
                    }
                    if (feature->SetGeometry(&polygon) != OGRERR_NONE)
                    {
                        throw std::runtime_error("Cannot create test geometry");
                    }
                }
                return feature.release();
            }

            GIntBig GetFeatureCount(int) override
            {
                ++_state.countCalls;
                return _state.unknownCount ? -1 : (_state.emptyLayer ? 0 : 3);
            }

            OGRErr SetIgnoredFields(CSLConstList fields) override
            {
                _state.ignoredFields = fields != nullptr;
                _state.ignoredGeometry = false;
                for (const char* const* field = fields; field != nullptr && *field != nullptr; ++field)
                {
                    if (std::string_view(*field) == "OGR_GEOMETRY")
                        _state.ignoredGeometry = true;
                }
                return OGRERR_NONE;
            }

            const OGRFeatureDefn* GetLayerDefn() const override
            {
                return _definition;
            }

            int TestCapability(const char* capability) const override
            {
                return capability != nullptr &&
                               std::string_view(capability) == OLCRandomWrite
                           ? TRUE
                           : FALSE;
            }

        protected:
            OGRErr ISetFeature(OGRFeature* feature) override
            {
                if (_state.failSetFeature)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Simulated feature write failure");
                    return OGRERR_FAILURE;
                }
                _state.storedName = feature->GetFieldAsString(0);
                _state.storedValue = feature->GetFieldAsInteger(1);
                return OGRERR_NONE;
            }

            OGRErr IGetExtent(int, OGREnvelope* extent, bool) override
            {
                ++_state.extentCalls;
                if (_state.failExtent)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Simulated extent failure");
                    return OGRERR_FAILURE;
                }
                if (_state.warningExtentFailure)
                {
                    CPLError(CE_Warning, CPLE_NotSupported, "Simulated unavailable extent");
                    return OGRERR_FAILURE;
                }
                if (_state.silentExtentFailure || _state.emptyLayer ||
                    _state.onlyEmptyGeometries)
                    return OGRERR_FAILURE;
                extent->MinX = 0.0;
                extent->MinY = 0.0;
                extent->MaxX = 1.0;
                extent->MaxY = 1.0;
                return OGRERR_NONE;
            }

        private:
            VectorReadTestDriver& _state;
            OGRFeatureDefn* _definition;
        };

        class VectorReadTestDataset final : public GDALDataset
        {
        public:
            explicit VectorReadTestDataset(VectorReadTestDriver& state)
                : _state(state), _layer(state)
            {
                if (state.simulateShapefile)
                    poDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
            }

            int GetLayerCount() const override
            {
                return 1;
            }

            const OGRLayer* GetLayer(const int index) const override
            {
                return index == 0 ? &_layer : nullptr;
            }

            OGRLayer* ExecuteSQL(const char* statement, OGRGeometry*, const char*) override
            {
                _state.recomputeExtentStatement = statement;
                if (!std::string_view(statement).starts_with("RECOMPUTE EXTENT ON "))
                    throw std::runtime_error("Unexpected test SQL statement");
                ++_state.recomputeExtentCalls;
                if (_state.failRecomputeExtent)
                    CPLError(CE_Failure, CPLE_AppDefined, "Simulated extent recompute failure");
                return nullptr;
            }

        private:
            VectorReadTestDriver& _state;
            VectorReadTestLayer _layer;
        };
    }

    VectorReadTestDriver::VectorReadTestDriver()
        : _driver(std::make_unique<GDALDriver>())
    {
        GdalEnvironment::Initialize();
        _driver->SetDescription("MWGVectorReadTest");
        _driver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
        _driver->SetMetadataItem(GDAL_DCAP_OPEN, "YES");
        _driver->SetMetadataItem(GDAL_DCAP_UPDATE, "YES");
        _driver->pDriverData = this;
        _driver->pfnIdentify = [](GDALOpenInfo* info)
        {
            return std::string_view(info->pszFilename) == kPath ? TRUE : FALSE;
        };
        _driver->pfnOpen = [](GDALOpenInfo* info) -> GDALDataset*
        {
            if (std::string_view(info->pszFilename) != kPath)
            {
                return nullptr;
            }
            GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("MWGVectorReadTest");
            return new VectorReadTestDataset(
                *static_cast<VectorReadTestDriver*>(driver->pDriverData));
        };
        GetGDALDriverManager()->RegisterDriver(_driver.get());
    }

    VectorReadTestDriver::~VectorReadTestDriver()
    {
        GetGDALDriverManager()->DeregisterDriver(_driver.get());
    }

    TempDirectory::TempDirectory()
        : _path(std::filesystem::temp_directory_path() / RandomName())
    {
        std::filesystem::create_directories(_path);
    }

    TempDirectory::~TempDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(_path, ignored);
    }

    std::filesystem::path MakeRaster(const std::filesystem::path& directory,
                                     const std::filesystem::path& fileName,
                                     const double originX,
                                     const std::uint8_t pixelValue)
    {
        GdalEnvironment::Initialize();
        constexpr int kWidth = 12;
        constexpr int kHeight = 8;
        const std::filesystem::path path = directory / fileName;
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
        if (driver == nullptr)
            throw std::runtime_error("GTiff driver is not available");

        GDALDatasetUniquePtr dataset(
            driver->Create(ToUtf8(path).c_str(), kWidth, kHeight, 1, GDT_Byte, nullptr));
        if (dataset == nullptr)
            throw std::runtime_error("Could not create the test raster");

        double transform[] = {originX, 1, 0, 8, 0, -1};
        OGRSpatialReference srs;
        if (srs.importFromEPSG(4326) != OGRERR_NONE ||
            dataset->SetSpatialRef(&srs) != CE_None ||
            dataset->SetGeoTransform(transform) != CE_None)
        {
            throw std::runtime_error("Could not georeference the test raster");
        }

        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth * kHeight), pixelValue);
        if (dataset->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                                pixels.data(), kWidth, kHeight,
                                                GDT_Byte, 0, 0) != CE_None)
        {
            throw std::runtime_error("Could not write the test raster");
        }
        if (dataset->FlushCache() != CE_None)
            throw std::runtime_error("Could not flush the test raster");
        return path;
    }

    std::filesystem::path MakeProvincesDataset(const std::filesystem::path& directory,
                                               const std::string& fileName)
    {
        GdalEnvironment::Initialize();

        const std::filesystem::path path = directory / fileName;
        const bool shapefile = path.extension() == ".shp";
        if (!shapefile && path.extension() != ".gpkg")
            throw std::invalid_argument("Test dataset must have a .shp or .gpkg extension");

        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName(
            shapefile ? "ESRI Shapefile" : "GPKG");
        if (driver == nullptr) throw std::runtime_error("Test dataset driver is not available");

        GDALDatasetUniquePtr dataset(
            driver->Create(ToUtf8(path).c_str(), 0, 0, 0, GDT_Unknown, nullptr));
        if (!dataset) throw std::runtime_error("Cannot create " + ToUtf8(path));

        OGRSpatialReference srs;
        if (srs.importFromEPSG(Provinces::kEpsg) != OGRERR_NONE)
            throw std::runtime_error("Cannot create test projection");

        const char* options[] = {"ENCODING=UTF-8", nullptr};
        OGRLayer* layer = dataset->CreateLayer(Provinces::kLayerName, &srs, wkbPolygon,
                                               shapefile ? options : nullptr);
        if (layer == nullptr) throw std::runtime_error("Cannot create layer");

        OGRFieldDefn idField(Provinces::kIdField, OFTInteger);
        OGRFieldDefn nameField(Provinces::kNameField, OFTString);
        nameField.SetWidth(32);
        OGRFieldDefn areaField(Provinces::kAreaField, OFTReal);
        areaField.SetWidth(18);
        areaField.SetPrecision(4);
        OGRFieldDefn surveyedField(Provinces::kSurveyedField, OFTDate);
        OGRFieldDefn activeField(Provinces::kActiveField, OFTInteger);
        activeField.SetSubType(OFSTBoolean);

        if (layer->CreateField(&idField) != OGRERR_NONE ||
            layer->CreateField(&nameField) != OGRERR_NONE ||
            layer->CreateField(&areaField) != OGRERR_NONE ||
            layer->CreateField(&surveyedField) != OGRERR_NONE ||
            layer->CreateField(&activeField) != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot create fields");
        }

        static constexpr const char* kNames[] = {"Noord", "Oost", Provinces::kNonAsciiName};
        static constexpr int kDates[][3] = {{2020, 1, 15}, {2021, 6, 30}, {2022, 11, 1}};

        for (int i = 0; i < Provinces::kFeatureCount; ++i)
        {
            OGRFeatureUniquePtr feature(OGRFeature::CreateFeature(layer->GetLayerDefn()));
            feature->SetField(Provinces::kIdField, i + 1);
            if (i != Provinces::kNullFeature)
            {
                feature->SetField(Provinces::kNameField, kNames[i]);
                feature->SetField(Provinces::kSurveyedField,
                                  kDates[i][0], kDates[i][1], kDates[i][2]);
            }
            feature->SetField(Provinces::kAreaField, 1.0);
            feature->SetField(Provinces::kActiveField, i == 2 ? 0 : 1);

            const double x = Provinces::kMinX + 1000.0 * i;
            OGRLinearRing ring;
            ring.addPoint(x, Provinces::kMinY);
            ring.addPoint(x + 1000.0, Provinces::kMinY);
            ring.addPoint(x + 1000.0, Provinces::kMaxY);
            ring.addPoint(x, Provinces::kMaxY);
            ring.closeRings();

            OGRPolygon polygon;
            polygon.addRing(&ring);
            if (feature->SetGeometry(&polygon) != OGRERR_NONE ||
                layer->CreateFeature(feature.get()) != OGRERR_NONE)
            {
                throw std::runtime_error("Cannot write feature " + std::to_string(i));
            }
        }

        dataset.reset();
        return path;
    }

    std::filesystem::path MakeRectanglesDataset(const std::filesystem::path& directory,
                                                const std::string& fileName,
                                                const std::span<const Extent> rectangles)
    {
        GdalEnvironment::Initialize();

        const std::filesystem::path path = directory / fileName;
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
        if (driver == nullptr) throw std::runtime_error("GPKG driver is not available");

        GDALDatasetUniquePtr dataset(
            driver->Create(ToUtf8(path).c_str(), 0, 0, 0, GDT_Unknown, nullptr));
        if (!dataset) throw std::runtime_error("Cannot create " + ToUtf8(path));

        OGRLayer* layer = dataset->CreateLayer("rectangles", nullptr, wkbPolygon, nullptr);
        if (layer == nullptr) throw std::runtime_error("Cannot create layer");

        for (const Extent& rectangle : rectangles)
        {
            OGRLinearRing ring;
            ring.addPoint(rectangle.xMin, rectangle.yMin);
            ring.addPoint(rectangle.xMax, rectangle.yMin);
            ring.addPoint(rectangle.xMax, rectangle.yMax);
            ring.addPoint(rectangle.xMin, rectangle.yMax);
            ring.closeRings();

            OGRPolygon polygon;
            polygon.addRing(&ring);
            OGRFeatureUniquePtr feature(OGRFeature::CreateFeature(layer->GetLayerDefn()));
            if (feature->SetGeometry(&polygon) != OGRERR_NONE ||
                layer->CreateFeature(feature.get()) != OGRERR_NONE)
            {
                throw std::runtime_error("Cannot write rectangle feature");
            }
        }

        dataset.reset();
        return path;
    }

    std::filesystem::path MakeTwoLayerDataset(const std::filesystem::path& directory,
                                              const std::string& fileName)
    {
        GdalEnvironment::Initialize();

        const std::filesystem::path path = directory / fileName;
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
        if (driver == nullptr) throw std::runtime_error("GPKG driver is not available");

        GDALDatasetUniquePtr dataset(
            driver->Create(ToUtf8(path).c_str(), 0, 0, 0, GDT_Unknown, nullptr));
        if (!dataset) throw std::runtime_error("Cannot create " + ToUtf8(path));

        if (dataset->CreateLayer("first", nullptr, wkbPolygon, nullptr) == nullptr ||
            dataset->CreateLayer("second", nullptr, wkbPolygon, nullptr) == nullptr)
        {
            throw std::runtime_error("Cannot create the two test layers");
        }

        dataset.reset();
        return path;
    }

    std::filesystem::path MakeSampleShapefile(const std::filesystem::path& directory,
                                              const std::string& baseName)
    {
        GdalEnvironment::Initialize();

        const std::filesystem::path shp = directory / (baseName + ".shp");

        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
        if (driver == nullptr) throw std::runtime_error("ESRI Shapefile driver is not available");

        GDALDatasetUniquePtr dataset(
            driver->Create(ToUtf8(shp).c_str(), 0, 0, 0, GDT_Unknown, nullptr));
        if (!dataset) throw std::runtime_error("Cannot create " + ToUtf8(shp));

        OGRSpatialReference srs;
        srs.importFromEPSG(28992);

        OGRLayer* layer = dataset->CreateLayer(baseName.c_str(), &srs, wkbPolygon, nullptr);
        if (layer == nullptr) throw std::runtime_error("Cannot create layer");

        OGRFieldDefn idField(SampleLayer::kIdField, OFTInteger);
        OGRFieldDefn nameField(SampleLayer::kNameField, OFTString);
        nameField.SetWidth(32);
        OGRFieldDefn areaField(SampleLayer::kAreaField, OFTReal);
        areaField.SetWidth(18);
        areaField.SetPrecision(4);

        if (layer->CreateField(&idField) != OGRERR_NONE ||
            layer->CreateField(&nameField) != OGRERR_NONE ||
            layer->CreateField(&areaField) != OGRERR_NONE)
        {
            throw std::runtime_error("Cannot create fields");
        }

        static constexpr const char* kNames[] = {"first", "second", nullptr};

        for (int i = 0; i < SampleLayer::kFeatureCount; ++i)
        {
            OGRFeatureUniquePtr feature(OGRFeature::CreateFeature(layer->GetLayerDefn()));
            feature->SetField(SampleLayer::kIdField, i + 1);
            if (kNames[i] != nullptr) feature->SetField(SampleLayer::kNameField, kNames[i]);
            feature->SetField(SampleLayer::kAreaField, 1.0 + i);

            OGRLinearRing ring;
            ring.addPoint(static_cast<double>(i), 0.0);
            ring.addPoint(static_cast<double>(i) + 1.0, 0.0);
            ring.addPoint(static_cast<double>(i) + 1.0, 1.0);
            ring.addPoint(static_cast<double>(i), 1.0);
            ring.closeRings();

            OGRPolygon polygon;
            polygon.addRing(&ring);
            feature->SetGeometry(&polygon);

            if (layer->CreateFeature(feature.get()) != OGRERR_NONE)
                throw std::runtime_error("Cannot write feature " + std::to_string(i));
        }

        dataset.reset();
        return shp;
    }
}
