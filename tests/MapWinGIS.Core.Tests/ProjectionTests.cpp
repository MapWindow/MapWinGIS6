// SPDX-License-Identifier: MPL-2.0

/**
 * @file ProjectionTests.cpp
 * @brief Tests coordinate reference system parsing, identity, and transforms.
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/projection/Projection.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    constexpr std::string_view kAmersfoortEsriWkt =
        R"(PROJCS["Amersfoort_RD_New",GEOGCS["GCS_Amersfoort",DATUM["D_Amersfoort",SPHEROID["Bessel_1841",6377397.155,299.1528128]],PRIMEM["Greenwich",0],UNIT["Degree",0.017453292519943295]],PROJECTION["Double_Stereographic"],PARAMETER["latitude_of_origin",52.15616055555555],PARAMETER["central_meridian",5.38763888888889],PARAMETER["scale_factor",0.9999079],PARAMETER["false_easting",155000],PARAMETER["false_northing",463000],UNIT["Meter",1]])";
}

TEST(ProjectionTest, RoundTripsCommonEpsgCodes)
{
    for (const int code : {28992, 4326, 3857})
    {
        const Projection projection = Projection::FromEpsg(code);
        ASSERT_EQ(projection.Epsg(), code) << code;
        EXPECT_EQ(Projection::FromWkt(projection.ExportToWkt()).Epsg(), code);
        EXPECT_FALSE(projection.IsEmpty());
    }
}

TEST(ProjectionTest, RejectsInvalidInputsAndSupportsTryFactories)
{
    EXPECT_THROW((void)Projection::FromEpsg(0), InvalidArgument);
    EXPECT_THROW((void)Projection::FromEpsg(987654321), InvalidArgument);
    EXPECT_THROW((void)Projection::FromWkt("not WKT"), InvalidArgument);
    EXPECT_THROW((void)Projection::FromProj4("not proj4"), InvalidArgument);
    EXPECT_FALSE(Projection::TryFromWkt("").has_value());
    EXPECT_FALSE(Projection::TryFromWkt("not WKT").has_value());
    EXPECT_FALSE(Projection::TryFromProj4("").has_value());
    EXPECT_FALSE(Projection::TryFromProj4("not proj4").has_value());
}

TEST(ProjectionTest, AutodetectsEpsgFromEsriAmersfoortWkt)
{
    const Projection projection = Projection::FromWkt(kAmersfoortEsriWkt);

    EXPECT_EQ(projection.Epsg(), 28992);
    EXPECT_TRUE(projection.IsProjected());
    EXPECT_FALSE(projection.IsGeographic());
}

TEST(ProjectionTest, ComparesByEpsgAndCrsEquivalence)
{
    const Projection epsg28992 = Projection::FromEpsg(28992);
    const Projection esri28992 = Projection::FromWkt(kAmersfoortEsriWkt);
    const Projection wgs84 = Projection::FromEpsg(4326);
    const Projection webMercator = Projection::FromEpsg(3857);
    const Projection empty;

    EXPECT_TRUE(epsg28992.IsSame(esri28992));
    EXPECT_TRUE(esri28992.IsSame(epsg28992));
    EXPECT_FALSE(wgs84.IsSame(webMercator));
    EXPECT_TRUE(empty.IsSame(Projection{}));
    EXPECT_FALSE(empty.IsSame(wgs84));
    EXPECT_FALSE(wgs84.IsSame(empty));
    EXPECT_NE(epsg28992.ExportToWkt(), std::string(kAmersfoortEsriWkt));
}

TEST(ProjectionTest, ExportsFormatsThatParseBack)
{
    const Projection projection = Projection::FromEpsg(28992);
    const std::string wkt = projection.ExportToWkt();
    const std::string proj4 = projection.ExportToProj4();

    EXPECT_EQ(Projection::FromWkt(wkt).Epsg(), 28992);
    EXPECT_TRUE(Projection::FromProj4(proj4).IsSame(projection));
    EXPECT_TRUE(Projection::TryFromWkt(wkt).has_value());
    EXPECT_TRUE(Projection::TryFromProj4(proj4).has_value());
}

TEST(ProjectionTest, ImportsProj4AndTransformsInTraditionalGisOrder)
{
    const Projection wgs84 = Projection::FromProj4("+proj=longlat +datum=WGS84 +no_defs");
    const Projection mercator = Projection::FromEpsg(3857);

    EXPECT_TRUE(wgs84.IsGeographic());
    double x = 10.0;
    double y = 50.0;
    const ProjectionTransform transform = wgs84.TransformTo(mercator);

    ASSERT_TRUE(transform(x, y));
    EXPECT_NEAR(x, 1113194.9079327357, 0.001);
    EXPECT_NEAR(y, 6446275.84101716, 0.001);
}

TEST(ProjectionTest, ResolvesRealProjectionDatasetWhenOptedIn)
{
    const char* value = std::getenv("MWG_PROJECTION_TEST_DATASET");
    if (value == nullptr || value[0] == '\0')
    {
        GTEST_SKIP() << "Set MWG_PROJECTION_TEST_DATASET to run";
    }

    const std::unique_ptr<VectorDataset> dataset =
        VectorDataset::Open(std::filesystem::path(value));
    ASSERT_EQ(dataset->LayerCount() > 0, true);
    EXPECT_EQ(dataset->Layer(0).Projection().Epsg(), 28992);
}
