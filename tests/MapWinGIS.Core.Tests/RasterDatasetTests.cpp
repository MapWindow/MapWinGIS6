// SPDX-License-Identifier: MPL-2.0

/**
 * @file RasterDatasetTests.cpp
 * @brief Tests read-only raster access and display-image generation.
 */
#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <stdexcept>

#include <gdal_priv.h>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/raster/RasterDataset.h"

using namespace mwg;
using namespace mwg::test;

namespace
{
    struct CloseGdalDataset
    {
        void operator()(GDALDataset* dataset) const noexcept
        {
            if (dataset != nullptr)
            {
                GDALClose(dataset);
            }
        }
    };

    std::filesystem::path MakeRgbaRaster(const std::filesystem::path& directory)
    {
        GdalEnvironment::Initialize();
        const std::filesystem::path path = directory / "rgba.tif";
        GDALDriver* const driver = GetGDALDriverManager()->GetDriverByName("GTiff");
        if (driver == nullptr)
        {
            throw std::runtime_error("GTiff driver is not available");
        }
        std::unique_ptr<GDALDataset, CloseGdalDataset> dataset(
            driver->Create(path.string().c_str(), 2, 2, 4, GDT_Byte, nullptr));
        if (dataset == nullptr)
        {
            throw std::runtime_error("Could not create RGBA test raster");
        }

        const std::array<GDALColorInterp, 4> interpretations{
            GCI_RedBand, GCI_GreenBand, GCI_BlueBand, GCI_AlphaBand};
        const std::array<std::uint8_t, 4> values{200, 100, 50, 128};
        for (int index = 0; index < 4; ++index)
        {
            GDALRasterBand* const band = dataset->GetRasterBand(index + 1);
            band->SetColorInterpretation(interpretations[static_cast<std::size_t>(index)]);
            std::array<std::uint8_t, 4> pixels{};
            pixels.fill(values[static_cast<std::size_t>(index)]);
            if (band->RasterIO(
                    GF_Write, 0, 0, 2, 2, pixels.data(), 2, 2, GDT_Byte, 0, 0) != CE_None)
            {
                throw std::runtime_error("Could not write RGBA test pixels");
            }
        }
        if (dataset->FlushCache() != CE_None)
        {
            throw std::runtime_error("Could not flush RGBA test raster");
        }
        dataset.reset();
        return path;
    }
}

TEST(RasterDatasetTest, ReadsGeoreferencedSingleBandPixelsAsGrayscale)
{
    TempDirectory directory;
    const std::shared_ptr<RasterDataset> raster =
        RasterDataset::Open(MakeRaster(directory.Path()));

    EXPECT_EQ(raster->Width(), 12);
    EXPECT_EQ(raster->Height(), 8);
    EXPECT_EQ(raster->BandCount(), 1);
    EXPECT_TRUE(raster->GetExtent().IsValid());
    EXPECT_DOUBLE_EQ(raster->GetExtent().xMin, 0.0);
    EXPECT_DOUBLE_EQ(raster->GetExtent().yMin, 0.0);
    EXPECT_DOUBLE_EQ(raster->GetExtent().xMax, 12.0);
    EXPECT_DOUBLE_EQ(raster->GetExtent().yMax, 8.0);

    const RasterImage image = raster->ReadImage();
    ASSERT_EQ(image.width, 12);
    ASSERT_EQ(image.height, 8);
    ASSERT_EQ(image.pixels.size(), 96U);
    EXPECT_EQ(image.pixels.front(), 0xFF2A2A2AU);
}

TEST(RasterDatasetTest, ReadsColorInterpretationAndPremultipliesAlpha)
{
    TempDirectory directory;
    const std::shared_ptr<RasterDataset> raster =
        RasterDataset::Open(MakeRgbaRaster(directory.Path()));

    const RasterImage image = raster->ReadImage();
    ASSERT_EQ(image.pixels.size(), 4U);
    EXPECT_EQ(image.pixels.front(), 0x80643219U);
}

TEST(RasterDatasetTest, ReprojectsDisplayImageToTheRequestedCoordinateSystem)
{
    TempDirectory directory;
    const std::shared_ptr<RasterDataset> raster =
        RasterDataset::Open(MakeRaster(directory.Path()));
    const RasterImage image = raster->ReadImage("EPSG:3857");

    ASSERT_TRUE(image.extent.IsValid());
    EXPECT_GT(image.extent.xMax, image.extent.xMin);
    EXPECT_GT(image.extent.yMax, image.extent.yMin);
    EXPECT_NEAR(image.extent.xMin, 0.0, 1e-4);
    EXPECT_GT(image.extent.yMax, 800000.0);
}

TEST(RasterDatasetTest, RejectsInvalidPreviewDimensions)
{
    TempDirectory directory;
    const std::shared_ptr<RasterDataset> raster =
        RasterDataset::Open(MakeRaster(directory.Path()));

    EXPECT_THROW((void)raster->ReadImage({}, 0), InvalidArgument);
}
