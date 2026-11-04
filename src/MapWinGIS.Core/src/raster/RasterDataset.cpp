// SPDX-License-Identifier: MPL-2.0

/**
 * @file RasterDataset.cpp
 * @brief Implements read-only raster access and bounded display-image reads.
 */
#include "mapwingis/raster/RasterDataset.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <cpl_conv.h>
#include <cpl_vsi.h>
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
            const int xOffset,
            const int yOffset,
            const int sourceWidth,
            const int sourceHeight,
            const int width,
            const int height)
        {
            const std::size_t count = static_cast<std::size_t>(width) *
                static_cast<std::size_t>(height);
            std::vector<float> samples(count);
            if (band.RasterIO(
                    GF_Read,
                    xOffset,
                    yOffset,
                    sourceWidth,
                    sourceHeight,
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
            float sample,
            double minimum,
            double maximum) noexcept;
        [[nodiscard]] std::uint8_t ScaleAlphaSample(
            float sample,
            double minimum,
            double maximum) noexcept;

        [[nodiscard]] DatasetPtr CreateWarpedDataset(
            GDALDataset* source,
            const std::string& sourceWkt,
            const std::string& targetWkt,
            const bool rotated)
        {
            const detail::CoordinateTransformation transform(
                sourceWkt,
                targetWkt.empty() ? sourceWkt : targetWkt);
            if (!transform.IsNeeded() && !rotated)
            {
                return {};
            }

            const std::string& destinationWkt =
                targetWkt.empty() ? sourceWkt : targetWkt;
            const char* sourceProjection = sourceWkt.empty() ? nullptr : sourceWkt.c_str();
            const char* destinationProjection =
                destinationWkt.empty() ? nullptr : destinationWkt.c_str();
            DatasetPtr warped(static_cast<GDALDataset*>(GDALAutoCreateWarpedVRT(
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
            return warped;
        }

        [[nodiscard]] std::string EscapeXml(const std::string& value)
        {
            std::string escaped;
            escaped.reserve(value.size());
            for (const char character : value)
            {
                switch (character)
                {
                case '&':
                    escaped += "&amp;";
                    break;
                case '<':
                    escaped += "&lt;";
                    break;
                case '>':
                    escaped += "&gt;";
                    break;
                case '"':
                    escaped += "&quot;";
                    break;
                case '\'':
                    escaped += "&apos;";
                    break;
                default:
                    escaped += character;
                    break;
                }
            }
            return escaped;
        }

        void ReplaceAll(
            std::string& value,
            const std::string_view from,
            const std::string_view to)
        {
            std::size_t position = 0;
            while ((position = value.find(from, position)) != std::string::npos)
            {
                value.replace(position, from.size(), to);
                position += to.size();
            }
        }

        [[nodiscard]] bool HasPlaceholder(
            const std::string& value,
            const std::string_view placeholder)
        {
            return value.find(placeholder) != std::string::npos;
        }

        [[nodiscard]] std::string MakeXyzServiceXml(
            const std::string& urlTemplate,
            const int minimumZoom,
            const int maximumZoom)
        {
            if ((urlTemplate.starts_with("https://") == false &&
                 urlTemplate.starts_with("http://") == false) ||
                !HasPlaceholder(urlTemplate, "{x}") ||
                !HasPlaceholder(urlTemplate, "{y}") ||
                !HasPlaceholder(urlTemplate, "{z}"))
            {
                throw InvalidArgument(
                    "XYZ tile URL must use HTTP or HTTPS and contain {z}, {x}, and {y}");
            }

            std::string serverUrl = urlTemplate;
            ReplaceAll(serverUrl, "{x}", "${x}");
            ReplaceAll(serverUrl, "{y}", "${y}");
            ReplaceAll(serverUrl, "{z}", "${z}");
            const bool png = serverUrl.find(".png") != std::string::npos;
            const bool jpeg = serverUrl.find(".jpg") != std::string::npos ||
                serverUrl.find(".jpeg") != std::string::npos;

            const int tileSize = 256;
            const int tileCount = 1 << minimumZoom;
            const int dimension = tileSize * tileCount;
            const int overviewCount = maximumZoom - minimumZoom + 1;
            const std::string format = jpeg ? "jpg" : "png";
            return
                "<GDAL_WMS>"
                "<Service name=\"TMS\"><ServerUrl>" + EscapeXml(serverUrl) +
                "</ServerUrl><Format>" + format +
                "</Format></Service>"
                "<DataWindow>"
                "<UpperLeftX>-20037508.342789244</UpperLeftX>"
                "<UpperLeftY>20037508.342789244</UpperLeftY>"
                "<LowerRightX>20037508.342789244</LowerRightX>"
                "<LowerRightY>-20037508.342789244</LowerRightY>"
                "<SizeX>" + std::to_string(dimension) + "</SizeX>"
                "<SizeY>" + std::to_string(dimension) + "</SizeY>"
                "<TileLevel>" + std::to_string(minimumZoom) + "</TileLevel>"
                "<TileCountX>1</TileCountX><TileCountY>1</TileCountY>"
                "<YOrigin>top</YOrigin></DataWindow>"
                "<Projection>EPSG:3857</Projection>"
                "<BlockSizeX>256</BlockSizeX><BlockSizeY>256</BlockSizeY>"
                "<OverviewCount>" + std::to_string(overviewCount) + "</OverviewCount>"
                "<BandsCount>" + std::string(png ? "4" : "3") + "</BandsCount>"
                "<DataType>Byte</DataType>"
                "<Cache><Path>/vsimem/mapwingis6-xyz-cache</Path>"
                "<MaxSize>67108864</MaxSize></Cache>"
                "<Timeout>15</Timeout><MaxConnections>4</MaxConnections>"
                "<UserAgent>MapWinGIS6</UserAgent>"
                "</GDAL_WMS>";
        }

        [[nodiscard]] RasterImage ReadRasterWindow(
            GDALDataset& source,
            const Extent& requestedExtent,
            const int width,
            const int height)
        {
            RasterImage image;
            image.width = width;
            image.height = height;
            image.extent = requestedExtent;
            const std::size_t pixelCount = static_cast<std::size_t>(width) *
                static_cast<std::size_t>(height);
            image.pixels.assign(pixelCount, 0U);

            const Extent sourceExtent = DatasetExtent(source);
            const Extent overlap{
                std::max(requestedExtent.xMin, sourceExtent.xMin),
                std::max(requestedExtent.yMin, sourceExtent.yMin),
                std::min(requestedExtent.xMax, sourceExtent.xMax),
                std::min(requestedExtent.yMax, sourceExtent.yMax)};
            if (!overlap.IsValid())
            {
                return image;
            }

            double geotransform[6]{};
            if (source.GetGeoTransform(geotransform) != CE_None)
            {
                geotransform[0] = 0.0;
                geotransform[1] = 1.0;
                geotransform[2] = 0.0;
                geotransform[3] = static_cast<double>(source.GetRasterYSize());
                geotransform[4] = 0.0;
                geotransform[5] = -1.0;
            }
            double inverse[6]{};
            if (GDALInvGeoTransform(geotransform, inverse) == FALSE)
            {
                throw CoreError("Raster window has no invertible geotransform");
            }

            double pixelX[4]{};
            double pixelY[4]{};
            const double cornerX[] = {
                overlap.xMin, overlap.xMax, overlap.xMax, overlap.xMin};
            const double cornerY[] = {
                overlap.yMax, overlap.yMax, overlap.yMin, overlap.yMin};
            for (int corner = 0; corner < 4; ++corner)
            {
                GDALApplyGeoTransform(
                    inverse,
                    cornerX[corner],
                    cornerY[corner],
                    &pixelX[corner],
                    &pixelY[corner]);
            }
            const auto [pixelXMin, pixelXMax] =
                std::minmax_element(std::begin(pixelX), std::end(pixelX));
            const auto [pixelYMin, pixelYMax] =
                std::minmax_element(std::begin(pixelY), std::end(pixelY));
            const int xOffset = std::clamp(
                static_cast<int>(std::floor(*pixelXMin)),
                0,
                source.GetRasterXSize());
            const int yOffset = std::clamp(
                static_cast<int>(std::floor(*pixelYMin)),
                0,
                source.GetRasterYSize());
            const int xEnd = std::clamp(
                static_cast<int>(std::ceil(*pixelXMax)),
                0,
                source.GetRasterXSize());
            const int yEnd = std::clamp(
                static_cast<int>(std::ceil(*pixelYMax)),
                0,
                source.GetRasterYSize());
            const int sourceWidth = xEnd - xOffset;
            const int sourceHeight = yEnd - yOffset;
            if (sourceWidth <= 0 || sourceHeight <= 0)
            {
                return image;
            }

            const int outputLeft = std::clamp(
                static_cast<int>(std::floor(
                    (overlap.xMin - requestedExtent.xMin) /
                    requestedExtent.Width() * width)),
                0,
                width);
            const int outputRight = std::clamp(
                static_cast<int>(std::ceil(
                    (overlap.xMax - requestedExtent.xMin) /
                    requestedExtent.Width() * width)),
                0,
                width);
            const int outputTop = std::clamp(
                static_cast<int>(std::floor(
                    (requestedExtent.yMax - overlap.yMax) /
                    requestedExtent.Height() * height)),
                0,
                height);
            const int outputBottom = std::clamp(
                static_cast<int>(std::ceil(
                    (requestedExtent.yMax - overlap.yMin) /
                    requestedExtent.Height() * height)),
                0,
                height);
            const int outputWidth = outputRight - outputLeft;
            const int outputHeight = outputBottom - outputTop;
            if (outputWidth <= 0 || outputHeight <= 0)
            {
                return image;
            }

            GDALRasterBand* first = source.GetRasterBand(1);
            if (first == nullptr)
            {
                throw CoreError("Raster dataset has no first band");
            }
            GDALColorTable* colorTable = first->GetColorTable();
            const int bandCount = source.GetRasterCount();
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
                GDALRasterBand* band = source.GetRasterBand(index);
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
            GDALRasterBand* redBand = source.GetRasterBand(redIndex);
            GDALRasterBand* greenBand = source.GetRasterBand(greenIndex);
            GDALRasterBand* blueBand = source.GetRasterBand(blueIndex);
            GDALRasterBand* alphaBand =
                alphaIndex != 0 ? source.GetRasterBand(alphaIndex) : nullptr;
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
                alpha = ReadBand(
                    *alphaBand, xOffset, yOffset, sourceWidth, sourceHeight,
                    outputWidth, outputHeight);
            }
            const std::vector<float> red = ReadBand(
                *redBand, xOffset, yOffset, sourceWidth, sourceHeight,
                outputWidth, outputHeight);
            const std::vector<float> green = greenIndex == redIndex
                ? red
                : ReadBand(
                    *greenBand, xOffset, yOffset, sourceWidth, sourceHeight,
                    outputWidth, outputHeight);
            const std::vector<float> blue = blueIndex == redIndex
                ? red
                : blueIndex == greenIndex
                    ? green
                    : ReadBand(
                        *blueBand, xOffset, yOffset, sourceWidth, sourceHeight,
                        outputWidth, outputHeight);

            int hasNoData = FALSE;
            const double noData = first->GetNoDataValue(&hasNoData);
            for (int row = 0; row < outputHeight; ++row)
            {
                for (int column = 0; column < outputWidth; ++column)
                {
                    const std::size_t sampleIndex =
                        static_cast<std::size_t>(row) * outputWidth + column;
                    std::uint8_t r{};
                    std::uint8_t g{};
                    std::uint8_t b{};
                    std::uint8_t a = 255;
                    if (palette)
                    {
                        const int colorIndex =
                            static_cast<int>(std::round(red[sampleIndex]));
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
                        r = ScaleSample(red[sampleIndex], redMin, redMax);
                        g = ScaleSample(green[sampleIndex], greenMin, greenMax);
                        b = ScaleSample(blue[sampleIndex], blueMin, blueMax);
                    }
                    if (hasNoData != FALSE &&
                        red[sampleIndex] == static_cast<float>(noData))
                    {
                        a = 0;
                    }
                    if (!alpha.empty())
                    {
                        const std::uint8_t bandAlpha =
                            ScaleAlphaSample(alpha[sampleIndex], alphaMin, alphaMax);
                        a = static_cast<std::uint8_t>(
                            (static_cast<unsigned int>(a) * bandAlpha + 127U) / 255U);
                    }
                    r = static_cast<std::uint8_t>(
                        (static_cast<unsigned int>(r) * a + 127U) / 255U);
                    g = static_cast<std::uint8_t>(
                        (static_cast<unsigned int>(g) * a + 127U) / 255U);
                    b = static_cast<std::uint8_t>(
                        (static_cast<unsigned int>(b) * a + 127U) / 255U);
                    const std::uint32_t pixel =
                        (static_cast<std::uint32_t>(a) << 24) |
                        (static_cast<std::uint32_t>(r) << 16) |
                        (static_cast<std::uint32_t>(g) << 8) |
                        static_cast<std::uint32_t>(b);
                    const std::size_t outputIndex =
                        static_cast<std::size_t>(outputTop + row) * width +
                        outputLeft + column;
                    image.pixels[outputIndex] = pixel;
                }
            }
            return image;
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

    std::shared_ptr<RasterDataset> RasterDataset::OpenXyzTiles(
        const std::string& urlTemplate,
        const int minimumZoom,
        const int maximumZoom)
    {
        if (minimumZoom < 0 || maximumZoom < minimumZoom || maximumZoom > 22)
        {
            throw InvalidArgument("XYZ tile zoom range must be within [0, 22]");
        }

        GdalEnvironment::Initialize();
        const std::string xml = MakeXyzServiceXml(urlTemplate, minimumZoom, maximumZoom);
        static std::atomic_uint64_t nextConfigurationId{0};
        const std::string configurationPath =
            "/vsimem/mapwingis6_xyz_" +
            std::to_string(nextConfigurationId.fetch_add(1, std::memory_order_relaxed)) +
            ".xml";
        void* xmlBuffer = CPLMalloc(xml.size() + 1U);
        if (xmlBuffer == nullptr)
        {
            throw std::bad_alloc();
        }
        std::memcpy(xmlBuffer, xml.c_str(), xml.size() + 1U);
        if (VSIFileFromMemBuffer(
                configurationPath.c_str(),
                static_cast<GByte*>(xmlBuffer),
                static_cast<vsi_l_offset>(xml.size()),
                TRUE) == nullptr)
        {
            CPLFree(xmlBuffer);
            throw OpenFailed("Cannot create the XYZ tile service configuration");
        }

        static constexpr const char* allowedDrivers[]{"WMS", nullptr};
        DatasetPtr dataset(static_cast<GDALDataset*>(GDALOpenEx(
            configurationPath.c_str(),
            GDAL_OF_RASTER | GDAL_OF_READONLY,
            allowedDrivers,
            nullptr,
            nullptr)));
        VSIUnlink(configurationPath.c_str());
        if (dataset == nullptr)
        {
            throw OpenFailed("Cannot open XYZ tile service: " + urlTemplate);
        }
        if (dataset->GetRasterXSize() <= 0 || dataset->GetRasterYSize() <= 0 ||
            dataset->GetRasterCount() <= 0)
        {
            throw OpenFailed("XYZ tile service has no readable pixels: " + urlTemplate);
        }
        return std::shared_ptr<RasterDataset>(
            new RasterDataset(std::make_unique<Impl>(std::filesystem::path{}, std::move(dataset))));
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
        double geotransform[6]{};
        const bool hasGeoTransform = source->GetGeoTransform(geotransform) == CE_None;
        const bool isRotated = hasGeoTransform &&
            (geotransform[2] != 0.0 || geotransform[4] != 0.0);
        DatasetPtr warped = CreateWarpedDataset(
            source, sourceWkt, targetProjectionWkt, isRotated);
        source = warped != nullptr ? warped.get() : source;

        const int sourceWidth = source->GetRasterXSize();
        const int sourceHeight = source->GetRasterYSize();
        const double scale = std::min(
            1.0,
            static_cast<double>(maxDimension) /
                static_cast<double>(std::max(sourceWidth, sourceHeight)));
        const int width = std::max(1, static_cast<int>(std::floor(sourceWidth * scale)));
        const int height = std::max(1, static_cast<int>(std::floor(sourceHeight * scale)));
        if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) >
            16'000'000ULL)
        {
            throw InvalidArgument("Raster preview exceeds the 16 megapixel limit");
        }

        return ReadRasterWindow(*source, DatasetExtent(*source), width, height);
    }

    RasterImage RasterDataset::ReadWindow(
        const Extent& targetExtent,
        const std::string& targetProjectionWkt,
        const int width,
        const int height) const
    {
        if (!targetExtent.IsValid() || targetExtent.Width() <= 0.0 ||
            targetExtent.Height() <= 0.0)
        {
            throw InvalidArgument("Raster window extent must have positive width and height");
        }
        if (width <= 0 || height <= 0 ||
            static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) >
                16'000'000ULL)
        {
            throw InvalidArgument("Raster window dimensions must be positive and at most 16 megapixels");
        }

        GDALDataset* source = _impl->_dataset.get();
        const std::string sourceWkt = ProjectionWkt();
        double geotransform[6]{};
        const bool hasGeoTransform = source->GetGeoTransform(geotransform) == CE_None;
        const bool isRotated = hasGeoTransform &&
            (geotransform[2] != 0.0 || geotransform[4] != 0.0);
        DatasetPtr warped = CreateWarpedDataset(
            source, sourceWkt, targetProjectionWkt, isRotated);
        source = warped != nullptr ? warped.get() : source;
        return ReadRasterWindow(*source, targetExtent, width, height);
    }
}
