// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalUtilsTests.cpp
 * @brief Tests legacy-named native GDAL compatibility operations.
 *
 * Covers raster utility options, vector conversion and reprojection, overview
 * creation, clipping, and mapping GDAL diagnostics to the legacy error state.
 */
#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <cpl_conv.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "mapwingis/gdal/GdalAlgorithmRunner.h"
#include "mapwingis/gdal/GdalConfig.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/gdal/GdalUtils.h"
#include "TestData.h"

#pragma warning(push)
#pragma warning(disable: 4996)

using namespace mwg;

namespace
{
    static_assert(std::is_same_v<decltype(std::declval<const GdalUtils&>().Errors()),
                                 const std::vector<GdalError>&>);
    static_assert(noexcept(std::declval<const GdalUtils&>().Errors()));

    GdalDatasetPtr OpenDataset(const std::filesystem::path& path, const unsigned int flags)
    {
        const std::u8string pathUtf8 = path.u8string();
        const std::string utf8(reinterpret_cast<const char*>(pathUtf8.data()), pathUtf8.size());
        return GdalDatasetPtr(static_cast<GDALDataset*>(
            GDALOpenEx(utf8.c_str(), flags, nullptr, nullptr, nullptr)));
    }

    std::string ToUtf8(const std::filesystem::path& path)
    {
        const std::u8string pathUtf8 = path.u8string();
        return {reinterpret_cast<const char*>(pathUtf8.data()), pathUtf8.size()};
    }

    std::filesystem::path MakeWgs84Shapefile(const std::filesystem::path& directory)
    {
        const std::filesystem::path path = directory / "wgs84.shp";
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
        if (driver == nullptr)
            return {};

        const std::u8string pathUtf8 = path.u8string();
        const std::string utf8(reinterpret_cast<const char*>(pathUtf8.data()), pathUtf8.size());
        GDALDataset* dataset = driver->Create(utf8.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        if (dataset == nullptr)
            return {};

        OGRSpatialReference spatialReference;
        if (spatialReference.importFromEPSG(4326) != OGRERR_NONE)
        {
            GDALClose(dataset);
            return {};
        }
        spatialReference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        OGRLayer* layer = dataset->CreateLayer("points", &spatialReference, wkbPoint, nullptr);
        if (layer == nullptr)
        {
            GDALClose(dataset);
            return {};
        }

        OGRFieldDefn idField("id", OFTInteger);
        if (layer->CreateField(&idField) != OGRERR_NONE)
        {
            GDALClose(dataset);
            return {};
        }

        OGRFeatureUniquePtr feature(OGRFeature::CreateFeature(layer->GetLayerDefn()));
        feature->SetField("id", 1);
        OGRPoint point(5.1, 52.1);
        feature->SetGeometry(&point);
        if (layer->CreateFeature(feature.get()) != OGRERR_NONE)
        {
            GDALClose(dataset);
            return {};
        }

        GDALClose(dataset);
        return path;
    }

    std::filesystem::path MakeClipOverlay(const std::filesystem::path& directory)
    {
        const std::filesystem::path path = directory / "overlay.gpkg";
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
        if (driver == nullptr)
            return {};

        const std::u8string pathUtf8 = path.u8string();
        const std::string utf8(reinterpret_cast<const char*>(pathUtf8.data()), pathUtf8.size());
        GDALDataset* dataset = driver->Create(utf8.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        if (dataset == nullptr)
            return {};

        OGRSpatialReference spatialReference;
        if (spatialReference.importFromEPSG(28992) != OGRERR_NONE)
        {
            GDALClose(dataset);
            return {};
        }

        OGRLayer* layer = dataset->CreateLayer("overlay", &spatialReference, wkbPolygon, nullptr);
        if (layer == nullptr)
        {
            GDALClose(dataset);
            return {};
        }

        OGRLinearRing ring;
        ring.addPoint(100500, 400250);
        ring.addPoint(101500, 400250);
        ring.addPoint(101500, 400750);
        ring.addPoint(100500, 400750);
        ring.addPoint(100500, 400250);
        OGRPolygon polygon;
        polygon.addRing(&ring);

        OGRFeatureUniquePtr feature(OGRFeature::CreateFeature(layer->GetLayerDefn()));
        feature->SetGeometry(&polygon);
        if (layer->CreateFeature(feature.get()) != OGRERR_NONE)
        {
            GDALClose(dataset);
            return {};
        }

        GDALClose(dataset);
        return path;
    }
}

TEST(GdalUtils, TranslatesRasterWithLegacyTypeResolutionResamplingAndWindowOptions)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input =
        test::MakeRaster(directory.Path(), std::filesystem::path(u8"legacy-入力.tif"));
    const std::filesystem::path output =
        directory.Path() / std::filesystem::path(u8"legacy-出力.tif");

    GdalUtils utils;
    ASSERT_TRUE(utils.GdalRasterTranslate(
        input, output,
        {"-of", "GTiff", "-ot", "Float32", "-tr", "0.5", "0.5",
         "-r", "bilinear", "-projwin", "0", "8", "12", "0"}))
        << utils.DetailedErrorMsg();

    GdalDatasetPtr dataset = OpenDataset(output, GDAL_OF_RASTER | GDAL_OF_READONLY);
    ASSERT_NE(dataset, nullptr);
    EXPECT_EQ(dataset->GetRasterXSize(), 24);
    EXPECT_EQ(dataset->GetRasterYSize(), 16);
    ASSERT_NE(dataset->GetRasterBand(1), nullptr);
    EXPECT_EQ(dataset->GetRasterBand(1)->GetRasterDataType(), GDT_Float32);

    std::array<double, 6> transform{};
    ASSERT_EQ(dataset->GetGeoTransform(transform.data()), CE_None);
    EXPECT_DOUBLE_EQ(transform[1], 0.5);
    EXPECT_DOUBLE_EQ(transform[5], -0.5);
    EXPECT_EQ(utils.LastErrorCode(), 0);
}

TEST(GdalUtils, WarpsRasterUsingLegacyOptions)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input = test::MakeRaster(directory.Path());
    const std::filesystem::path output = directory.Path() / "warped.tif";

    GdalUtils utils;
    ASSERT_TRUE(utils.GdalRasterWarp(input, output, {"-t_srs", "EPSG:3857", "-r", "bilinear"}))
        << utils.DetailedErrorMsg();

    GdalDatasetPtr dataset = OpenDataset(output, GDAL_OF_RASTER | GDAL_OF_READONLY);
    ASSERT_NE(dataset, nullptr);
    ASSERT_NE(dataset->GetSpatialRef(), nullptr);
    EXPECT_STREQ(dataset->GetSpatialRef()->GetAuthorityCode(nullptr), "3857");
}

TEST(GdalUtils, TranslatesVectorAndPreservesFeatures)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input =
        test::MakeProvincesDataset(directory.Path(), "input.gpkg");
    const std::filesystem::path output = directory.Path() / "converted.geojson";

    GdalUtils utils;
    ASSERT_TRUE(utils.GdalVectorTranslate(input, output, {"-f", "GeoJSON", "-nln", "converted"}))
        << utils.DetailedErrorMsg();

    GdalDatasetPtr dataset = OpenDataset(output, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    ASSERT_NE(dataset, nullptr);
    ASSERT_EQ(dataset->GetLayerCount(), 1);
    OGRLayer* layer = dataset->GetLayer(0);
    ASSERT_NE(layer, nullptr);
    EXPECT_STREQ(layer->GetName(), "converted");
    EXPECT_EQ(layer->GetFeatureCount(), test::Provinces::kFeatureCount);
}

TEST(GdalUtils, ReprojectsWgs84ShapefileToEpsg28992)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input = MakeWgs84Shapefile(directory.Path());
    ASSERT_FALSE(input.empty());
    const std::filesystem::path output = directory.Path() / "projected.gpkg";

    GdalUtils utils;
    ASSERT_TRUE(utils.GdalVectorReproject(input, output, 4326, 28992))
        << utils.DetailedErrorMsg();
    ASSERT_TRUE(utils.GdalVectorReproject(input, output, 4326, 28992))
        << utils.DetailedErrorMsg();

    GdalDatasetPtr dataset = OpenDataset(output, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    ASSERT_NE(dataset, nullptr);
    ASSERT_EQ(dataset->GetLayerCount(), 1);
    OGRLayer* layer = dataset->GetLayer(0);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(layer->GetSpatialRef(), nullptr);
    EXPECT_STREQ(layer->GetSpatialRef()->GetAuthorityCode(nullptr), "28992");
    EXPECT_EQ(layer->GetFeatureCount(), 1);
}

TEST(GdalUtils, BuildsRasterOverviewsForSelectedBands)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input = test::MakeRaster(directory.Path());

    GdalUtils utils;
    ASSERT_TRUE(utils.GdalBuildOverviews(input, "NEAREST", {2, 4}, {1}))
        << utils.DetailedErrorMsg();

    GdalDatasetPtr dataset = OpenDataset(input, GDAL_OF_RASTER | GDAL_OF_READONLY);
    ASSERT_NE(dataset, nullptr);
    ASSERT_NE(dataset->GetRasterBand(1), nullptr);
    EXPECT_EQ(dataset->GetRasterBand(1)->GetOverviewCount(), 2);
}

TEST(GdalUtils, BuildsAutomaticOverviewsWhenTheOverviewListIsEmpty)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input = directory.Path() / "large.tif";
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(driver, nullptr);
    GdalDatasetPtr created(driver->Create(ToUtf8(input).c_str(), 1024, 768, 1,
                                          GDT_Byte, nullptr));
    ASSERT_NE(created, nullptr);
    created.reset();

    GdalUtils utils;
    ASSERT_TRUE(utils.GdalBuildOverviews(input, "NEAREST", {}, {1}))
        << utils.DetailedErrorMsg();

    GdalDatasetPtr dataset = OpenDataset(input, GDAL_OF_RASTER | GDAL_OF_READONLY);
    ASSERT_NE(dataset, nullptr);
    GDALRasterBand* band = dataset->GetRasterBand(1);
    ASSERT_NE(band, nullptr);
    ASSERT_EQ(band->GetOverviewCount(), 2);
    EXPECT_EQ(band->GetOverview(0)->GetXSize(), 512);
    EXPECT_EQ(band->GetOverview(0)->GetYSize(), 384);
    EXPECT_EQ(band->GetOverview(1)->GetXSize(), 256);
    EXPECT_EQ(band->GetOverview(1)->GetYSize(), 192);
}

TEST(GdalUtils, BuildsExternalOverviewsForReadOnlyPngRasters)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input = directory.Path() / "source.png";
    GDALDriver* memoryDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDriver* pngDriver = GetGDALDriverManager()->GetDriverByName("PNG");
    ASSERT_NE(memoryDriver, nullptr);
    ASSERT_NE(pngDriver, nullptr);
    GdalDatasetPtr memoryRaster(memoryDriver->Create("", 512, 512, 1, GDT_Byte, nullptr));
    ASSERT_NE(memoryRaster, nullptr);
    GdalDatasetPtr pngRaster(
        pngDriver->CreateCopy(ToUtf8(input).c_str(), memoryRaster.get(), FALSE,
                              nullptr, nullptr, nullptr));
    ASSERT_NE(pngRaster, nullptr);
    ASSERT_EQ(GDALClose(pngRaster.release()), CE_None);

    GdalUtils utils;
    ASSERT_TRUE(utils.GdalBuildOverviews(input, "NEAREST", {2}, {1}))
        << utils.DetailedErrorMsg();
    EXPECT_TRUE(std::filesystem::exists(input.string() + ".ovr"));
}

TEST(GdalUtils, RestoresDuplicateConfigurationOptionsInReverseOrder)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input = test::MakeRaster(directory.Path());
    constexpr char kConfigName[] = "MWG_GDAL_UTILS_TEST_OPTION";
    ScopedGdalConfigOption initialValue(kConfigName, "original");

    GdalUtils utils;
    ASSERT_TRUE(utils.GdalBuildOverviews(
        input, "NEAREST", {2}, {1},
        {{kConfigName, "first"}, {kConfigName, "second"}}))
        << utils.DetailedErrorMsg();
    EXPECT_STREQ(CPLGetConfigOption(kConfigName, nullptr), "original");
}

TEST(GdalUtils, ReportsInvalidOverviewConfigurationThroughTheDocumentedContracts)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input = test::MakeRaster(directory.Path());
    constexpr char kConfigName[] = "MWG_GDAL_UTILS_INVALID_OVERVIEW_OPTION";
    const ScopedGdalConfigOption original(kConfigName, "original");
    const std::string nullName("name\0suffix", 11);
    const std::string nullValue("value\0suffix", 12);

    for (const bool withProgress : {false, true})
    {
        GdalUtils utils;
        const auto build = [&](const std::string_view resampling,
                               const std::vector<std::pair<std::string, std::string>>& config)
        {
            return withProgress
                ? utils.GdalBuildOverviews(input, resampling, {2}, {1}, config, {})
                : utils.GdalBuildOverviews(input, resampling, {2}, {1}, config);
        };

        EXPECT_THROW((void)build("NEAREST", {{kConfigName, "temporary"}, {"", "value"}}),
                     InvalidArgument);
        EXPECT_STREQ(CPLGetConfigOption(kConfigName, nullptr), "original");

        for (const auto& option : std::vector<std::pair<std::string, std::string>>{
                 {nullName, "value"}, {kConfigName, nullValue}})
        {
            EXPECT_FALSE(build("NEAREST", {option}));
            EXPECT_NE(utils.LastErrorCode(), 0);
            EXPECT_NE(utils.DetailedErrorMsg().find("null bytes"), std::string::npos);
            EXPECT_FALSE(utils.Result().Succeeded());
            EXPECT_STREQ(CPLGetConfigOption(kConfigName, nullptr), "original");
        }

        EXPECT_FALSE(build(nullValue, {}));
        EXPECT_NE(utils.LastErrorCode(), 0);
        EXPECT_NE(utils.DetailedErrorMsg().find("resampling"), std::string::npos);
        EXPECT_FALSE(utils.Result().Succeeded());
    }
}

TEST(GdalUtils, ClipsVectorFeaturesWithAnOverlayDataset)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    const std::filesystem::path input =
        test::MakeProvincesDataset(directory.Path(), "input.gpkg");
    const std::filesystem::path overlay = MakeClipOverlay(directory.Path());
    ASSERT_FALSE(overlay.empty());
    const std::filesystem::path output = directory.Path() / "clipped.gpkg";

    GdalUtils utils;
    ASSERT_TRUE(utils.ClipVectorWithVector(input, overlay, output))
        << utils.DetailedErrorMsg();

    GdalDatasetPtr dataset = OpenDataset(output, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    ASSERT_NE(dataset, nullptr);
    ASSERT_EQ(dataset->GetLayerCount(), 1);
    OGRLayer* layer = dataset->GetLayer(0);
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(layer->GetFeatureCount(), 2);
}

TEST(GdalUtils, MapsFailureDiagnosticsToLegacyErrorProperties)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::QuietScope quiet;
    test::TempDirectory directory;
    GdalUtils utils;

    EXPECT_FALSE(utils.GdalRasterTranslate(
        directory.Path() / "missing.tif", directory.Path() / "missing-output.tif", {}));
    EXPECT_NE(utils.LastErrorCode(), 0);
    EXPECT_FALSE(utils.ErrorMsg(utils.LastErrorCode()).empty());
    EXPECT_FALSE(utils.DetailedErrorMsg().empty());
    EXPECT_FALSE(utils.Errors().empty());

    const std::filesystem::path input = test::MakeRaster(directory.Path());
    EXPECT_TRUE(utils.GdalRasterTranslate(
        input, directory.Path() / "valid-output.tif", {}))
        << utils.DetailedErrorMsg();
    EXPECT_EQ(utils.LastErrorCode(), 0);
}

TEST(GdalUtils, PreservesClipDiagnosticsWhenLoggerThrows)
{
    GdalEnvironment::Initialize();
    test::TempDirectory directory;
    GdalUtils utils;
    GdalEnvironment::SetLogHandler([](LogLevel, std::string_view)
    {
        throw std::runtime_error("logger failure");
    });

    EXPECT_THROW((void)utils.ClipVectorWithVector(
        directory.Path() / "missing-subject.gpkg",
        directory.Path() / "missing-overlay.gpkg",
        directory.Path() / "clipped.gpkg"), std::runtime_error);
    GdalEnvironment::SetLogHandler({});

    const GdalOperationResult result = utils.Result();
    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.AllErrors().empty());
    EXPECT_NE(result.LastErrorCode(), 0);
}

TEST(GdalUtils, RejectsOptionStringsContainingNullBytes)
{
    GdalUtils utils;
    std::string invalidOption = "option";
    invalidOption.push_back('\0');
    invalidOption += "truncated";

    EXPECT_FALSE(utils.GdalRasterTranslate("input.tif", "output.tif", {invalidOption}));
    EXPECT_NE(utils.LastErrorCode(), 0);
    EXPECT_NE(utils.DetailedErrorMsg().find("null bytes"), std::string::npos);
}

TEST(GdalUtils, ReportsProgressCancellationAndResetsOperationState)
{
    GdalEnvironment::Initialize();
    test::TempDirectory directory;
    const auto input = test::MakeRaster(directory.Path());
    GdalUtils utils;
    EXPECT_FALSE(utils.Result().Succeeded());
    EXPECT_FALSE(utils.GdalRasterTranslate(
        input, directory.Path() / "cancelled.tif", {},
        [](double, std::string_view) { return false; }));
    auto cancelled = utils.Result();
    EXPECT_FALSE(cancelled.Succeeded());
    EXPECT_TRUE(cancelled.WasCancelled());
    EXPECT_NE(cancelled.ErrorMsg().find("cancelled"), std::string::npos);
    EXPECT_EQ(cancelled.LastErrorCode(), utils.LastErrorCode());

    std::vector<double> progress;
    EXPECT_TRUE(utils.GdalRasterTranslate(
        input, directory.Path() / "success.tif", {},
        [&](const double value, std::string_view)
        {
            progress.push_back(value);
            return true;
        }));
    EXPECT_TRUE(utils.Result().Succeeded());
    EXPECT_FALSE(utils.Result().WasCancelled());
    EXPECT_EQ(utils.Result().LastErrorCode(), 0);
    EXPECT_TRUE(utils.Result().AllErrors().empty());
    EXPECT_TRUE(cancelled.WasCancelled());
    ASSERT_FALSE(progress.empty());
    EXPECT_DOUBLE_EQ(progress.back(), 1.0);
    auto output = OpenDataset(directory.Path() / "success.tif", GDAL_OF_RASTER);
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(output->GetRasterXSize(), 12);
    std::uint8_t pixel = 0;
    EXPECT_EQ(output->GetRasterBand(1)->RasterIO(
        GF_Read, 0, 0, 1, 1, &pixel, 1, 1, GDT_Byte, 0, 0), CE_None);
    EXPECT_EQ(pixel, 42);
}

TEST(GdalUtils, RejectsPreCancelledOperationsAndReportsCallbackExceptionsAsFailures)
{
    GdalEnvironment::Initialize();
    test::TempDirectory directory;
    const auto input = test::MakeRaster(directory.Path());
    GdalUtils utils;
    GdalCancellationToken token;
    token.Cancel();
    EXPECT_FALSE(utils.GdalRasterWarp(
        input, directory.Path() / "cancelled.tif", {}, {}, &token));
    EXPECT_TRUE(utils.Result().WasCancelled());
    EXPECT_FALSE(std::filesystem::exists(directory.Path() / "cancelled.tif"));
    EXPECT_FALSE(utils.GdalRasterTranslate(
        input, directory.Path() / "failed.tif", {},
        [](double, std::string_view) -> bool { throw std::runtime_error("callback failed"); }));
    EXPECT_FALSE(utils.Result().WasCancelled());
    EXPECT_NE(utils.Result().ErrorMsg().find("callback failed"), std::string::npos);
}

TEST(GdalUtils, PropagatesCancellationThroughVectorAndOverviewOperations)
{
    GdalEnvironment::Initialize();
    test::TempDirectory directory;
    const auto raster = test::MakeRaster(directory.Path());
    const auto vector = test::MakeProvincesDataset(directory.Path(), "source.gpkg");
    const auto overlay = MakeClipOverlay(directory.Path());
    GdalCancellationToken token;
    token.Cancel();
    GdalUtils utils;

    EXPECT_FALSE(utils.GdalVectorTranslate(
        vector, directory.Path() / "translated.gpkg", {}, false, {}, &token));
    EXPECT_TRUE(utils.Result().WasCancelled());
    EXPECT_FALSE(utils.GdalVectorReproject(
        vector, directory.Path() / "reprojected.gpkg", 28992, 4326, false, {}, &token));
    EXPECT_TRUE(utils.Result().WasCancelled());
    EXPECT_FALSE(utils.GdalBuildOverviews(raster, "NEAREST", {2}, {}, {}, {}, &token));
    EXPECT_TRUE(utils.Result().WasCancelled());
    EXPECT_FALSE(utils.ClipVectorWithVector(
        vector, overlay, directory.Path() / "clipped.gpkg", false, {}, &token));
    EXPECT_TRUE(utils.Result().WasCancelled());
    EXPECT_FALSE(std::filesystem::exists(directory.Path() / "translated.gpkg"));
    EXPECT_FALSE(std::filesystem::exists(directory.Path() / "reprojected.gpkg"));
    EXPECT_FALSE(std::filesystem::exists(directory.Path() / "clipped.gpkg"));
}

#pragma warning(pop)
