// SPDX-License-Identifier: MPL-2.0

/**
 * @file TestData.cpp
 * @brief Implements shared vector test fixture helpers.
 *
 * Locates and copies committed datasets, manages temporary directories, and
 * creates sample polygon shapefiles for the core tests.
 */
#include "TestData.h"

#include <random>
#include <stdexcept>

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

    const std::filesystem::path& DataDirectory()
    {
        static const std::filesystem::path directory = [] {
#ifdef MWG_TEST_DATA_DIR
            std::filesystem::path configured(MWG_TEST_DATA_DIR);
            if (std::filesystem::is_directory(configured)) return configured;
#endif
            // Fallback for a relocated binary: walk up looking for tests/data.
            std::filesystem::path dir = std::filesystem::current_path();
            for (int i = 0; i < 8; ++i)
            {
                if (std::filesystem::is_directory(dir / "tests" / "data"))
                    return dir / "tests" / "data";
                if (!dir.has_parent_path() || dir.parent_path() == dir) break;
                dir = dir.parent_path();
            }
            throw std::runtime_error("tests/data was not found");
        }();
        return directory;
    }

    std::filesystem::path CopyDataset(const std::filesystem::path& destination,
                                      const std::string& fileName)
    {
        const std::filesystem::path source = DataDirectory() / fileName;
        if (!std::filesystem::exists(source))
            throw std::runtime_error("Test data is missing: " + source.string());

        std::filesystem::create_directories(destination);

        static constexpr const char* kShapefileParts[] = {".shp", ".shx", ".dbf", ".prj",
                                                          ".cpg", ".sbn", ".sbx", ".qix"};

        auto copy = [&destination](const std::filesystem::path& file)
        {
            std::filesystem::copy_file(file, destination / file.filename(),
                                       std::filesystem::copy_options::overwrite_existing);
        };

        if (source.extension() == ".shp")
        {
            for (const char* extension : kShapefileParts)
            {
                std::filesystem::path part = source;
                part.replace_extension(extension);
                if (std::filesystem::exists(part)) copy(part);
            }
        }
        else
        {
            copy(source);
        }

        return destination / source.filename();
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
