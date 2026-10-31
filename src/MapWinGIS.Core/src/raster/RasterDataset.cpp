// SPDX-License-Identifier: MPL-2.0

/**
 * @file RasterDataset.cpp
 * @brief Implements read-only raster access and bounded display-image reads.
 */
#include "mapwingis/raster/RasterDataset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <cpl_conv.h>
#include <gdal_priv.h>
#include <gdalwarper.h>
#include <ogr_spatialref.h>

#include "internal/CoordinateTransformation.h"
#include "internal/Utf8.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"

namespace mwg
{
    namespace
    {
        struct CloseDataset
        {
            void operator()(GDALDataset* dataset) const noexcept
            {
                if (dataset != nullptr)
                {
                    GDALClose(dataset);
                }
            }
        };

        using DatasetPtr = std::unique_ptr<GDALDataset, CloseDataset>;

        [[nodiscard]] std::string PathUtf8(const std::filesystem::path& path)
        {
            const std::u8string utf8 = path.u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }

        [[nodiscard]] Extent DatasetExtent(const GDALDataset& dataset)
        {
            double transform[6]{};
            if (dataset.GetGeoTransform(transform) != CE_None)
            {
                transform[0] = 0.0;
                transform[1] = 1.0;
                transform[2] = 0.0;
                transform[3] = static_cast<double>(dataset.GetRasterYSize());
                transform[4] = 0.0;
                transform[5] = -1.0;
            }

            const double width = static_cast<double>(dataset.GetRasterXSize());
            const double height = static_cast<double>(dataset.GetRasterYSize());
            const double x[] = {
                transform[0],
                transform[0] + width * transform[1],
                transform[0] + width * transform[1] + height * transform[2],
                transform[0] + height * transform[2]};
            const double y[] = {
                transform[3],
                transform[3] + width * transform[4],
                transform[3] + width * transform[4] + height * transform[5],
                transform[3] + height * transform[5]};

            Extent extent;
            for (int index = 0; index < 4; ++index)
            {
                if (!std::isfinite(x[index]) || !std::isfinite(y[index]))
                {
                    throw CoreError("Raster geotransform produced non-finite bounds");
                }
                extent = extent.Union({x[index], y[index], x[index], y[index]});
            }
            return extent;
        }

        [[nodiscard]] double SampleMinimum(GDALRasterBand& band, const GDALDataType type)
        {
            if (type == GDT_Byte)
            {
                return 0.0;
            }
            int success = FALSE;
            const double minimum = band.GetMinimum(&success);
            if (success != FALSE && std::isfinite(minimum))
            {
                return minimum;
            }
            double limits[2]{};
            if (band.ComputeRasterMinMax(TRUE, limits) != CE_None ||
                !std::isfinite(limits[0]))
            {
                throw CoreError("Cannot determine raster band display range");
            }
            return limits[0];
        }

        [[nodiscard]] double SampleMaximum(GDALRasterBand& band, const GDALDataType type)
        {
            if (type == GDT_Byte)
            {
                return 255.0;
            }
            int success = FALSE;
            const double maximum = band.GetMaximum(&success);
            if (success != FALSE && std::isfinite(maximum))
            {
                return maximum;
            }
            double limits[2]{};
            if (band.ComputeRasterMinMax(TRUE, limits) != CE_None ||
                !std::isfinite(limits[1]))
            {
                throw CoreError("Cannot determine raster band display range");
            }
            return limits[1];
        }

        [[nodiscard]] std::vector<float> ReadBand(
            GDALRasterBand& band,
            const int width,
            const int height)
        {
            const std::size_t count = static_cast<std::size_t>(width) *
                static_cast<std::size_t>(height);
            std::vector<float> samples(count);
            if (band.RasterIO(
                    GF_Read,
                    0,
                    0,
                    band.GetXSize(),
                    band.GetYSize(),
                    samples.data(),
                    width,
                    height,
                    GDT_Float32,
                    0,
                    0) != CE_None)
            {
                throw CoreError("Cannot read raster band for display");
            }
            return samples;
        }

        [[nodiscard]] std::uint8_t ScaleSample(
            const float sample,
            const double minimum,
            const double maximum) noexcept
        {
            if (!std::isfinite(sample))
            {
                return 0;
            }
            if (maximum <= minimum)
            {
                return static_cast<std::uint8_t>(
                    std::clamp(std::round(static_cast<double>(sample)), 0.0, 255.0));
            }
            const double normalized = (static_cast<double>(sample) - minimum) /
                (maximum - minimum);
            return static_cast<std::uint8_t>(
                std::clamp(std::round(normalized * 255.0), 0.0, 255.0));
        }

        [[nodiscard]] std::uint8_t ScaleAlphaSample(
            const float sample,
            const double minimum,
            const double maximum) noexcept
        {
            if (maximum <= minimum && minimum >= 0.0 && maximum <= 1.0)
            {
                return static_cast<std::uint8_t>(
                    std::clamp(std::round(static_cast<double>(sample) * 255.0), 0.0, 255.0));
            }
            return ScaleSample(sample, minimum, maximum);
        }
    }

    class RasterDataset::Impl
    {
    public:
        Impl(std::filesystem::path path, DatasetPtr dataset)
            : _path(std::move(path)), _dataset(std::move(dataset))
        {
        }

        std::filesystem::path _path;
        DatasetPtr _dataset;
    };

    RasterDataset::RasterDataset(std::unique_ptr<Impl> impl)
        : _impl(std::move(impl))
    {
    }

    RasterDataset::~RasterDataset() = default;

    std::shared_ptr<RasterDataset> RasterDataset::Open(const std::filesystem::path& path)
    {
        GdalEnvironment::Initialize();
        DatasetPtr dataset(static_cast<GDALDataset*>(GDALOpenEx(
            PathUtf8(path).c_str(),
            GDAL_OF_RASTER | GDAL_OF_READONLY,
            nullptr,
            nullptr,
            nullptr)));
        if (dataset == nullptr)
        {
            throw OpenFailed("Cannot open raster dataset: " + PathUtf8(path));
        }
        if (dataset->GetRasterXSize() <= 0 || dataset->GetRasterYSize() <= 0 ||
            dataset->GetRasterCount() <= 0)
        {
            throw OpenFailed("Raster dataset has no readable pixels: " + PathUtf8(path));
        }
        return std::shared_ptr<RasterDataset>(
            new RasterDataset(std::make_unique<Impl>(path, std::move(dataset))));
    }

    std::filesystem::path RasterDataset::Path() const
    {
        return _impl->_path;
    }

    int RasterDataset::Width() const noexcept
    {
        return _impl->_dataset->GetRasterXSize();
    }

    int RasterDataset::Height() const noexcept
    {
        return _impl->_dataset->GetRasterYSize();
    }

    int RasterDataset::BandCount() const noexcept
    {
        return _impl->_dataset->GetRasterCount();
    }

    Extent RasterDataset::GetExtent() const
    {
        return DatasetExtent(*_impl->_dataset);
    }

    std::string RasterDataset::ProjectionWkt() const
    {
        const char* projection = _impl->_dataset->GetProjectionRef();
        return projection != nullptr ? projection : "";
    }

    RasterImage RasterDataset::ReadImage(
        const std::string& targetProjectionWkt,
        const int maxDimension) const
    {
        if (maxDimension <= 0)
        {
            throw InvalidArgument("Raster image maximum dimension must be positive");
        }

        GDALDataset* source = _impl->_dataset.get();
        const std::string sourceWkt = ProjectionWkt();
        std::unique_ptr<GDALDataset, CloseDataset> warped;
        const detail::CoordinateTransformation transform(
            sourceWkt,
            targetProjectionWkt.empty() ? sourceWkt : targetProjectionWkt);
        double geotransform[6]{};
        const bool hasGeoTransform = source->GetGeoTransform(geotransform) == CE_None;
        const bool isRotated = hasGeoTransform &&
            (geotransform[2] != 0.0 || geotransform[4] != 0.0);
        if (transform.IsNeeded() || isRotated)
        {
            const std::string& destinationWkt =
                targetProjectionWkt.empty() ? sourceWkt : targetProjectionWkt;
            const char* sourceProjection = sourceWkt.empty() ? nullptr : sourceWkt.c_str();
            const char* destinationProjection =
                destinationWkt.empty() ? nullptr : destinationWkt.c_str();
            warped.reset(static_cast<GDALDataset*>(GDALAutoCreateWarpedVRT(
                source,
                sourceProjection,
                destinationProjection,
                GRA_NearestNeighbour,
                0.0,
                nullptr)));
            if (warped == nullptr)
            {
                throw CoreError("Cannot warp raster into the map coordinate system");
            }
            source = warped.get();
        }

        const int sourceWidth = source->GetRasterXSize();
        const int sourceHeight = source->GetRasterYSize();
        const double scale = std::min(
            1.0,
            static_cast<double>(maxDimension) /
                static_cast<double>(std::max(sourceWidth, sourceHeight)));
        const int width = std::max(1, static_cast<int>(std::floor(sourceWidth * scale)));
        const int height = std::max(1, static_cast<int>(std::floor(sourceHeight * scale)));

        RasterImage image;
        image.width = width;
        image.height = height;
        image.extent = DatasetExtent(*source);
        const std::size_t pixelCount = static_cast<std::size_t>(width) *
            static_cast<std::size_t>(height);
        image.pixels.resize(pixelCount);

        GDALRasterBand* first = source->GetRasterBand(1);
        if (first == nullptr)
        {
            throw CoreError("Raster dataset has no first band");
        }

        GDALColorTable* colorTable = first->GetColorTable();
        const int bandCount = source->GetRasterCount();
        const bool palette = colorTable != nullptr;
        int redIndex = 1;
        int greenIndex = bandCount < 3 ? 1 : 2;
        int blueIndex = bandCount < 3 ? 1 : 3;
        int alphaIndex = 0;
        int interpretedRed = 0;
        int interpretedGreen = 0;
        int interpretedBlue = 0;
        for (int index = 1; index <= bandCount; ++index)
        {
            GDALRasterBand* band = source->GetRasterBand(index);
            if (band == nullptr)
            {
                continue;
            }
            switch (band->GetColorInterpretation())
            {
            case GCI_RedBand:
                interpretedRed = index;
                break;
            case GCI_GreenBand:
                interpretedGreen = index;
                break;
            case GCI_BlueBand:
                interpretedBlue = index;
                break;
            case GCI_AlphaBand:
                alphaIndex = index;
                break;
            default:
                break;
            }
        }
        if (!palette && interpretedRed != 0 && interpretedGreen != 0 &&
            interpretedBlue != 0)
        {
            redIndex = interpretedRed;
            greenIndex = interpretedGreen;
            blueIndex = interpretedBlue;
        }
        GDALRasterBand* redBand = source->GetRasterBand(redIndex);
        GDALRasterBand* greenBand = source->GetRasterBand(greenIndex);
        GDALRasterBand* blueBand = source->GetRasterBand(blueIndex);
        GDALRasterBand* alphaBand =
            alphaIndex != 0 ? source->GetRasterBand(alphaIndex) : nullptr;
        if (redBand == nullptr || greenBand == nullptr || blueBand == nullptr)
        {
            throw CoreError("Raster dataset is missing a display band");
        }

        const GDALDataType redType = redBand->GetRasterDataType();
        const GDALDataType greenType = greenBand->GetRasterDataType();
        const GDALDataType blueType = blueBand->GetRasterDataType();
        const double redMin = SampleMinimum(*redBand, redType);
        const double redMax = SampleMaximum(*redBand, redType);
        const double greenMin = SampleMinimum(*greenBand, greenType);
        const double greenMax = SampleMaximum(*greenBand, greenType);
        const double blueMin = SampleMinimum(*blueBand, blueType);
        const double blueMax = SampleMaximum(*blueBand, blueType);
        std::vector<float> alpha;
        double alphaMin = 0.0;
        double alphaMax = 255.0;
        if (alphaBand != nullptr)
        {
            const GDALDataType alphaType = alphaBand->GetRasterDataType();
            alphaMin = SampleMinimum(*alphaBand, alphaType);
            alphaMax = SampleMaximum(*alphaBand, alphaType);
            alpha = ReadBand(*alphaBand, width, height);
        }
        const std::vector<float> red = ReadBand(*redBand, width, height);
        const std::vector<float> green = greenIndex == redIndex
            ? red
            : ReadBand(*greenBand, width, height);
        const std::vector<float> blue = blueIndex == redIndex
            ? red
            : blueIndex == greenIndex ? green : ReadBand(*blueBand, width, height);

        int hasNoData = FALSE;
        const double noData = first->GetNoDataValue(&hasNoData);
        for (std::size_t index = 0; index < pixelCount; ++index)
        {
            std::uint8_t r{};
            std::uint8_t g{};
            std::uint8_t b{};
            std::uint8_t a = 255;
            if (palette)
            {
                const int colorIndex = static_cast<int>(std::round(red[index]));
                const GDALColorEntry* entry = colorTable->GetColorEntry(colorIndex);
                if (entry == nullptr)
                {
                    a = 0;
                }
                else
                {
                    r = static_cast<std::uint8_t>(std::clamp<int>(entry->c1, 0, 255));
                    g = static_cast<std::uint8_t>(std::clamp<int>(entry->c2, 0, 255));
                    b = static_cast<std::uint8_t>(std::clamp<int>(entry->c3, 0, 255));
                    a = static_cast<std::uint8_t>(std::clamp<int>(entry->c4, 0, 255));
                }
            }
            else
            {
                r = ScaleSample(red[index], redMin, redMax);
                g = ScaleSample(green[index], greenMin, greenMax);
                b = ScaleSample(blue[index], blueMin, blueMax);
            }
            if (hasNoData != FALSE && red[index] == static_cast<float>(noData))
            {
                a = 0;
            }
            if (!alpha.empty())
            {
                const std::uint8_t bandAlpha =
                    ScaleAlphaSample(alpha[index], alphaMin, alphaMax);
                a = static_cast<std::uint8_t>(
                    (static_cast<unsigned int>(a) * bandAlpha + 127U) / 255U);
            }
            r = static_cast<std::uint8_t>((static_cast<unsigned int>(r) * a + 127U) / 255U);
            g = static_cast<std::uint8_t>((static_cast<unsigned int>(g) * a + 127U) / 255U);
            b = static_cast<std::uint8_t>((static_cast<unsigned int>(b) * a + 127U) / 255U);
            image.pixels[index] =
                (static_cast<std::uint32_t>(a) << 24) |
                (static_cast<std::uint32_t>(r) << 16) |
                (static_cast<std::uint32_t>(g) << 8) |
                static_cast<std::uint32_t>(b);
        }
        return image;
    }
}
