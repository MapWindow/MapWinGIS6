// SPDX-License-Identifier: MPL-2.0

#include <WinSock2.h>

/**
 * @file RasterDatasetTests.cpp
 * @brief Tests read-only raster access and display-image generation.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gdal_priv.h>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/raster/RasterDataset.h"

#pragma comment(lib, "Ws2_32.lib")

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

    std::vector<std::uint8_t> MakePngTile()
    {
        GdalEnvironment::Initialize();
        GDALDriver* const memoryDriver = GetGDALDriverManager()->GetDriverByName("MEM");
        GDALDriver* const pngDriver = GetGDALDriverManager()->GetDriverByName("PNG");
        if (memoryDriver == nullptr || pngDriver == nullptr)
        {
            throw std::runtime_error("Memory or PNG GDAL driver is not available");
        }

        std::unique_ptr<GDALDataset, CloseGdalDataset> image(
            memoryDriver->Create("", 256, 256, 4, GDT_Byte, nullptr));
        if (image == nullptr)
        {
            throw std::runtime_error("Could not create an XYZ tile fixture");
        }
        const std::array<GDALColorInterp, 4> interpretations{
            GCI_RedBand, GCI_GreenBand, GCI_BlueBand, GCI_AlphaBand};
        const std::array<std::uint8_t, 4> colors{220, 80, 40, 255};
        for (int bandIndex = 0; bandIndex < 4; ++bandIndex)
        {
            GDALRasterBand* const band = image->GetRasterBand(bandIndex + 1);
            band->SetColorInterpretation(
                interpretations[static_cast<std::size_t>(bandIndex)]);
            std::vector<std::uint8_t> pixels(
                256U * 256U, colors[static_cast<std::size_t>(bandIndex)]);
            if (band->RasterIO(
                    GF_Write, 0, 0, 256, 256, pixels.data(), 256, 256,
                    GDT_Byte, 0, 0) != CE_None)
            {
                throw std::runtime_error("Could not write XYZ tile fixture pixels");
            }
        }

        constexpr const char* path = "/vsimem/mapwingis6-test-tile.png";
        std::unique_ptr<GDALDataset, CloseGdalDataset> encoded(
            pngDriver->CreateCopy(path, image.get(), FALSE, nullptr, nullptr, nullptr));
        if (encoded == nullptr)
        {
            throw std::runtime_error("Could not encode the XYZ tile fixture");
        }
        vsi_l_offset size = 0;
        const GByte* const data = VSIGetMemFileBuffer(path, &size, FALSE);
        if (data == nullptr || size == 0)
        {
            throw std::runtime_error("Could not read the XYZ tile fixture");
        }
        std::vector<std::uint8_t> result(data, data + size);
        encoded.reset();
        VSIUnlink(path);
        return result;
    }

    class LocalTileServer final
    {
    public:
        explicit LocalTileServer(std::vector<std::uint8_t> tile)
        {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            {
                throw std::runtime_error("Could not initialize Winsock");
            }
            _winsockStarted = true;
            try
            {
                _listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (_listener == INVALID_SOCKET)
                {
                    throw std::runtime_error("Could not create a local tile listener");
                }

                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = 0;
                if (bind(
                        _listener,
                        reinterpret_cast<const sockaddr*>(&address),
                        sizeof(address)) == SOCKET_ERROR ||
                    listen(_listener, SOMAXCONN) == SOCKET_ERROR)
                {
                    throw std::runtime_error("Could not bind a local tile listener");
                }
                int addressLength = sizeof(address);
                if (getsockname(
                        _listener,
                        reinterpret_cast<sockaddr*>(&address),
                        &addressLength) == SOCKET_ERROR)
                {
                    throw std::runtime_error("Could not query the local tile listener");
                }
                _port = ntohs(address.sin_port);
                _thread = std::thread(
                [this, tile = std::move(tile)]() mutable
                {
                    while (!_stopping.load())
                    {
                        fd_set readable{};
                        FD_ZERO(&readable);
                        FD_SET(_listener, &readable);
                        timeval timeout{0, 100000};
                        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
                        if (ready == 0)
                        {
                            continue;
                        }
                        if (ready == SOCKET_ERROR || _stopping.load())
                        {
                            break;
                        }

                        SOCKET client = accept(_listener, nullptr, nullptr);
                        if (client == INVALID_SOCKET)
                        {
                            break;
                        }
                        char request[2048]{};
                        const int received = recv(client, request, sizeof(request) - 1, 0);
                        if (received > 0)
                        {
                            {
                                std::lock_guard lock(_requestMutex);
                                _lastRequest.assign(
                                    request, static_cast<std::size_t>(received));
                            }
                            const std::string header =
                                "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n"
                                "Content-Length: " + std::to_string(tile.size()) +
                                "\r\nConnection: close\r\n\r\n";
                            auto sendAll = [client](
                                               const char* buffer,
                                               std::size_t length)
                            {
                                std::size_t sent = 0;
                                while (sent < length)
                                {
                                    const int count = send(
                                        client, buffer + sent,
                                        static_cast<int>(length - sent), 0);
                                    if (count <= 0)
                                    {
                                        return false;
                                    }
                                    sent += static_cast<std::size_t>(count);
                                }
                                return true;
                            };
                            const bool headerSent =
                                sendAll(header.data(), header.size());
                            if (headerSent)
                            {
                                (void)sendAll(
                                    reinterpret_cast<const char*>(tile.data()),
                                    tile.size());
                            }
                        }
                        closesocket(client);
                    }
                });
            }
            catch (...)
            {
                if (_listener != INVALID_SOCKET)
                {
                    closesocket(_listener);
                    _listener = INVALID_SOCKET;
                }
                WSACleanup();
                _winsockStarted = false;
                throw;
            }
        }

        ~LocalTileServer()
        {
            _stopping.store(true);
            if (_listener != INVALID_SOCKET)
            {
                closesocket(_listener);
            }
            if (_thread.joinable())
            {
                _thread.join();
            }
            if (_winsockStarted)
            {
                WSACleanup();
            }
        }

        LocalTileServer(const LocalTileServer&) = delete;
        LocalTileServer& operator=(const LocalTileServer&) = delete;

        [[nodiscard]] std::string UrlTemplate() const
        {
            return "http://127.0.0.1:" + std::to_string(_port) + "/{z}/{x}/{y}.png";
        }

        [[nodiscard]] std::string LastRequest() const
        {
            std::lock_guard lock(_requestMutex);
            return _lastRequest;
        }

    private:
        SOCKET _listener = INVALID_SOCKET;
        unsigned short _port = 0;
        std::atomic_bool _stopping{false};
        std::thread _thread;
        mutable std::mutex _requestMutex;
        std::string _lastRequest;
        bool _winsockStarted = false;
    };
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

TEST(RasterDatasetTest, ReadsOnlyTheRequestedRasterWindowAtViewportResolution)
{
    TempDirectory directory;
    const std::shared_ptr<RasterDataset> raster =
        RasterDataset::Open(MakeRaster(directory.Path()));

    const RasterImage image = raster->ReadWindow({3.0, 2.0, 9.0, 6.0}, {}, 60, 40);

    ASSERT_EQ(image.width, 60);
    ASSERT_EQ(image.height, 40);
    EXPECT_DOUBLE_EQ(image.extent.xMin, 3.0);
    EXPECT_DOUBLE_EQ(image.extent.yMax, 6.0);
    EXPECT_NE(image.pixels.front(), 0x00000000U);
    EXPECT_NE(image.pixels[10U * 60U + 10U], 0x00000000U);
}

TEST(RasterDatasetTest, ReprojectsOnlyTheRequestedWindow)
{
    TempDirectory directory;
    const std::shared_ptr<RasterDataset> raster =
        RasterDataset::Open(MakeRaster(directory.Path()));
    const Extent projectedExtent = raster->ReadImage("EPSG:3857").extent;

    const RasterImage image =
        raster->ReadWindow(projectedExtent, "EPSG:3857", 120, 80);

    EXPECT_EQ(image.width, 120);
    EXPECT_EQ(image.height, 80);
    EXPECT_NE(
        std::find_if(
            image.pixels.begin(),
            image.pixels.end(),
            [](const std::uint32_t pixel) { return pixel != 0U; }),
        image.pixels.end());
}

TEST(RasterDatasetTest, WindowedReadLeavesPixelsOutsideRasterTransparent)
{
    TempDirectory directory;
    const std::shared_ptr<RasterDataset> raster =
        RasterDataset::Open(MakeRaster(directory.Path()));

    const RasterImage image = raster->ReadWindow({-6.0, -4.0, 6.0, 4.0}, {}, 120, 80);

    ASSERT_EQ(image.width, 120);
    ASSERT_EQ(image.height, 80);
    EXPECT_EQ(image.pixels[20U * 120U + 10U], 0x00000000U);
    EXPECT_NE(image.pixels[20U * 120U + 80U], 0x00000000U);
}

TEST(RasterDatasetTest, OpensXyzTileSourcesAsWebMercatorRasters)
{
    const std::shared_ptr<RasterDataset> raster = RasterDataset::OpenXyzTiles(
        "https://tiles.example.test/{z}/{x}/{y}.png",
        1,
        18);

    EXPECT_GT(raster->Width(), 0);
    EXPECT_GT(raster->Height(), 0);
    EXPECT_NE(raster->ProjectionWkt().find("3857"), std::string::npos);
    EXPECT_TRUE(raster->GetExtent().IsValid());
}

TEST(RasterDatasetTest, FetchesAndReadsTheRequestedXyzTile)
{
    LocalTileServer server(MakePngTile());
    const std::shared_ptr<RasterDataset> raster =
        RasterDataset::OpenXyzTiles(server.UrlTemplate(), 0, 0);
    constexpr double halfWorld = 20037508.342789244;

    const RasterImage image = raster->ReadWindow(
        {-halfWorld, -halfWorld, halfWorld, halfWorld},
        "EPSG:3857",
        256,
        256);

    ASSERT_EQ(image.width, 256);
    ASSERT_EQ(image.height, 256);
    EXPECT_EQ(image.pixels[128U * 256U + 128U], 0xFFDC5028U);
    EXPECT_NE(server.LastRequest().find("/0/0/0.png"), std::string::npos);
}

TEST(RasterDatasetTest, RejectsInvalidXyzTileSourceDescriptions)
{
    EXPECT_THROW(
        (void)RasterDataset::OpenXyzTiles("file:///tiles/{z}/{x}/{y}.png"),
        InvalidArgument);
    EXPECT_THROW(
        (void)RasterDataset::OpenXyzTiles("https://tiles.example.test/{z}/{x}.png"),
        InvalidArgument);
    EXPECT_THROW(
        (void)RasterDataset::OpenXyzTiles(
            "https://tiles.example.test/{z}/{x}/{y}.png", 5, 4),
        InvalidArgument);
}

TEST(RasterDatasetTest, RejectsRasterWindowsThatExceedThePixelBudget)
{
    TempDirectory directory;
    const std::shared_ptr<RasterDataset> raster =
        RasterDataset::Open(MakeRaster(directory.Path()));

    EXPECT_THROW(
        (void)raster->ReadWindow({0.0, 0.0, 1.0, 1.0}, {}, 16384, 16384),
        InvalidArgument);
}
