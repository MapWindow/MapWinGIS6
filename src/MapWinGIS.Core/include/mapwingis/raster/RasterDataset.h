// SPDX-License-Identifier: MPL-2.0

/**
 * @file RasterDataset.h
 * @brief Declares the read-only raster dataset and display-image API.
 *
 * Raster datasets expose georeferenced bounds, viewport-sized RGBA reads, and
 * bounded previews. GDAL and raster-driver types stay private.
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
    /// nearest-neighbour resampling. ReadWindow reads only the visible source
    /// window and samples it to the requested output dimensions. Single-band
    /// data is displayed as grayscale, paletted data uses its color table,
    /// and multiband data uses RGB color interpretations or the first three
    /// bands. Nodata and alpha bands are transparent. A non-empty target WKT
    /// reprojects the source before sampling.
    class MWG_CORE_API RasterDataset
    {
    public:
        ~RasterDataset();
        RasterDataset(const RasterDataset&) = delete;
        RasterDataset& operator=(const RasterDataset&) = delete;

        [[nodiscard]] static std::shared_ptr<RasterDataset> Open(
            const std::filesystem::path& path);

        /// Opens an XYZ tile service using a URL containing {z}, {x}, and {y}.
        /// Tiles use the Web Mercator matrix and are cached in memory by GDAL.
        /// \throws InvalidArgument when the URL template or zoom range is invalid.
        /// \throws OpenFailed when the GDAL WMS driver cannot open the service.
        [[nodiscard]] static std::shared_ptr<RasterDataset> OpenXyzTiles(
            const std::string& urlTemplate,
            int minimumZoom = 0,
            int maximumZoom = 19);

        [[nodiscard]] std::filesystem::path Path() const;
        [[nodiscard]] int Width() const noexcept;
        [[nodiscard]] int Height() const noexcept;
        [[nodiscard]] int BandCount() const noexcept;
        [[nodiscard]] Extent GetExtent() const;
        [[nodiscard]] std::string ProjectionWkt() const;

        [[nodiscard]] RasterImage ReadImage(
            const std::string& targetProjectionWkt = {},
            int maxDimension = 2048) const;

        /// Reads an output image for a map-coordinate window. Pixels outside
        /// the raster remain transparent; output dimensions are limited to
        /// 16 million pixels.
        [[nodiscard]] RasterImage ReadWindow(
            const Extent& targetExtent,
            const std::string& targetProjectionWkt,
            int width,
            int height) const;

    private:
        class Impl;
        explicit RasterDataset(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> _impl;
    };
}
