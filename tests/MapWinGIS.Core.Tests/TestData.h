// SPDX-License-Identifier: MPL-2.0

/**
 * @file TestData.h
 * @brief Declares shared vector test data and fixture helpers.
 *
 * Provides generated dataset metadata, temporary-directory support, and
 * helpers for creating Shapefile and GeoPackage fixtures.
 */
#pragma once

#include <filesystem>
#include <string>

namespace mwg::test
{
    /// Describes the projected polygon layer written by MakeProvincesDataset().
    struct Provinces
    {
        static constexpr int kFeatureCount = 4;
        static constexpr int kEpsg = 28992;

        static constexpr const char* kLayerName = "provinces";
        static constexpr const char* kIdField = "ID";
        static constexpr const char* kNameField = "NAME";
        static constexpr const char* kAreaField = "AREA_KM2";
        static constexpr const char* kSurveyedField = "SURVEYED";
        static constexpr const char* kActiveField = "ACTIVE";

        static constexpr double kMinX = 100000.0;
        static constexpr double kMinY = 400000.0;
        static constexpr double kMaxX = 104000.0;
        static constexpr double kMaxY = 401000.0;

        /// Feature 3 has a null NAME and a null SURVEYED.
        static constexpr int kNullFeature = 3;
        /// UTF-8; feature 2 exercises non-ASCII attribute values.
        static constexpr const char* kNonAsciiName = "Frysl\u00e2n";
    };

    /// Writes four polygons with all five field types, null values, and UTF-8
    /// names. fileName must have a .shp or .gpkg extension.
    std::filesystem::path MakeProvincesDataset(const std::filesystem::path& directory,
                                               const std::string& fileName);

    /// Describes the sample layer that MakeSampleShapefile() writes, so tests
    /// can assert against it without repeating literals.
    struct SampleLayer
    {
        static constexpr int kFeatureCount = 3;

        static constexpr const char* kIdField = "ID";
        static constexpr const char* kNameField = "NAME";
        static constexpr const char* kAreaField = "AREA";

        // Feature 0..2: unit squares side by side, y in [0, 1].
        static constexpr double kMinX = 0.0;
        static constexpr double kMinY = 0.0;
        static constexpr double kMaxX = 3.0;
        static constexpr double kMaxY = 1.0;
    };

    /// Creates a directory that is deleted when the object goes out of scope.
    class TempDirectory
    {
    public:
        TempDirectory();
        ~TempDirectory();

        TempDirectory(const TempDirectory&) = delete;
        TempDirectory& operator=(const TempDirectory&) = delete;

        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return _path; }

    private:
        std::filesystem::path _path;
    };

    /// Writes a 3-feature polygon shapefile in EPSG:28992 with the fields
    /// described by SampleLayer, and returns the .shp path.
    /// The last feature has a NULL NAME so that null handling is covered.
    std::filesystem::path MakeSampleShapefile(const std::filesystem::path& directory,
                                              const std::string& baseName = "sample");
}
