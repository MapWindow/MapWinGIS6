// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalEnvironmentTests.cpp
 * @brief Tests GDAL initialization, drivers, runtime data, and diagnostics.
 *
 * Verifies environment setup and the logging and error-reporting behavior
 * exposed by GdalEnvironment.
 */
#include <gtest/gtest.h>

#include <cpl_error.h>

#include "mapwingis/gdal/GdalEnvironment.h"

using namespace mwg;

namespace
{
    // Lower bounds, not exact counts: the driver set depends on the vcpkg
    // manifest feature that was built (core-drivers or full-drivers).
    constexpr int kMinimumRasterDrivers = 50;
    constexpr int kMinimumVectorDrivers = 30;
}

TEST(GdalEnvironment, ReportsVersions)
{
    GdalEnvironment::Initialize();

    EXPECT_FALSE(GdalEnvironment::GdalVersion().empty());
    EXPECT_FALSE(GdalEnvironment::GeosVersion().empty());
    EXPECT_FALSE(GdalEnvironment::ProjVersion().empty());

    EXPECT_TRUE(GdalEnvironment::GdalVersion().starts_with("3."));
}

TEST(GdalEnvironment, InitializeIsIdempotent)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::Initialize();
    EXPECT_TRUE(GdalEnvironment::IsInitialized());
}

TEST(GdalEnvironment, RegistersDrivers)
{
    EXPECT_GE(GdalEnvironment::RasterDriverCount(), kMinimumRasterDrivers);
    EXPECT_GE(GdalEnvironment::VectorDriverCount(), kMinimumVectorDrivers);
}

TEST(GdalEnvironment, HasTheDriversMapWinGisDependsOn)
{
    EXPECT_TRUE(GdalEnvironment::HasVectorDriver("ESRI Shapefile"));
    EXPECT_TRUE(GdalEnvironment::HasVectorDriver("GPKG"));
    EXPECT_TRUE(GdalEnvironment::HasVectorDriver("OpenFileGDB"));
    EXPECT_TRUE(GdalEnvironment::HasVectorDriver("GeoJSON"));
    EXPECT_TRUE(GdalEnvironment::HasVectorDriver("MapInfo File"));
    EXPECT_TRUE(GdalEnvironment::HasVectorDriver("CSV"));
    EXPECT_TRUE(GdalEnvironment::HasVectorDriver("DXF"));

    EXPECT_TRUE(GdalEnvironment::HasRasterDriver("GTiff"));
    EXPECT_TRUE(GdalEnvironment::HasRasterDriver("MEM"));
    EXPECT_TRUE(GdalEnvironment::HasRasterDriver("AAIGrid"));
    EXPECT_TRUE(GdalEnvironment::HasRasterDriver("HFA"));
    EXPECT_TRUE(GdalEnvironment::HasRasterDriver("VRT"));
    EXPECT_TRUE(GdalEnvironment::HasRasterDriver("PNG"));
    EXPECT_TRUE(GdalEnvironment::HasRasterDriver("JPEG"));
}

TEST(GdalEnvironment, DeploysRuntimeDataNextToTheModule)
{
    const std::filesystem::path dir = GdalEnvironment::ModuleDirectory();
    ASSERT_FALSE(dir.empty());

    EXPECT_TRUE(std::filesystem::is_directory(dir / "gdal-data"))
        << "gdal-data is missing from " << dir.string();
    EXPECT_TRUE(std::filesystem::exists(dir / "proj" / "proj.db"))
        << "proj.db is missing from " << dir.string();
}

TEST(GdalEnvironment, CapturesTheLastError)
{
    GdalEnvironment::Initialize();
    GdalEnvironment::ClearLastError();
    EXPECT_TRUE(GdalEnvironment::LastErrorMessage().empty());

    CPLError(CE_Failure, CPLE_AppDefined, "%s", "test failure");
    EXPECT_EQ(GdalEnvironment::LastErrorMessage(), "test failure");

    GdalEnvironment::ClearLastError();
    EXPECT_TRUE(GdalEnvironment::LastErrorMessage().empty());
}

TEST(GdalEnvironment, QuietScopeRestoresTheHandler)
{
    GdalEnvironment::Initialize();

    int seen = 0;
    GdalEnvironment::SetLogHandler([&seen](LogLevel, std::string_view) { ++seen; });

    {
        const GdalEnvironment::QuietScope quiet;
        CPLError(CE_Warning, CPLE_AppDefined, "%s", "suppressed");
    }
    EXPECT_EQ(seen, 0);

    CPLError(CE_Warning, CPLE_AppDefined, "%s", "reported");
    EXPECT_EQ(seen, 1);

    GdalEnvironment::SetLogHandler({});
    GdalEnvironment::ClearLastError();
}
