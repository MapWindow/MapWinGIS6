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
#include <vector>

#include <gdal_priv.h>
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
