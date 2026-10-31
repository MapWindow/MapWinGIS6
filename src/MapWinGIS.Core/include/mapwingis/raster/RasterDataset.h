// SPDX-License-Identifier: MPL-2.0

/**
 * @file RasterDataset.h
 * @brief Declares the read-only raster dataset and display-image API.
 *
 * Raster datasets expose their georeferenced bounds and a bounded RGBA
 * preview for map rendering. GDAL and raster-driver types stay private.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "mapwingis/core/Api.h"
#include "mapwingis/core/Extent.h"

namespace mwg
{
    /// Top-down RGBA pixels and their map-coordinate bounds.
    struct MWG_CORE_API RasterImage
    {
        int width = 0;
        int height = 0;
        Extent extent;
        /// Premultiplied-alpha pixels are 0xAARRGGBB; row zero is the top row.
        std::vector<std::uint32_t> pixels;
    };

    /// An open read-only GDAL raster data source.
    ///
    /// ReadImage limits its longest output edge to maxDimension and uses
    /// nearest-neighbour resampling. Single-band data is displayed as
    /// grayscale, paletted data uses its color table, and multiband data uses
    /// RGB color interpretations or the first three bands. Nodata and alpha
    /// bands are transparent. A non-empty target WKT reprojects the preview
    /// before sampling.
    class MWG_CORE_API RasterDataset
    {
    public:
        ~RasterDataset();
        RasterDataset(const RasterDataset&) = delete;
        RasterDataset& operator=(const RasterDataset&) = delete;

        [[nodiscard]] static std::shared_ptr<RasterDataset> Open(
            const std::filesystem::path& path);

        [[nodiscard]] std::filesystem::path Path() const;
        [[nodiscard]] int Width() const noexcept;
        [[nodiscard]] int Height() const noexcept;
        [[nodiscard]] int BandCount() const noexcept;
        [[nodiscard]] Extent GetExtent() const;
        [[nodiscard]] std::string ProjectionWkt() const;

        [[nodiscard]] RasterImage ReadImage(
            const std::string& targetProjectionWkt = {},
            int maxDimension = 2048) const;

    private:
        class Impl;
        explicit RasterDataset(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> _impl;
    };
}
