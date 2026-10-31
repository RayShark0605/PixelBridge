#include "qt_raster_paint.h"
#include "qt_raster_test_support.h"
#include "presentation_backend.h"
#include "pbmodulation/experimental_pam4.h"
#include "pbmodulation/experimental_pam4_wide.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <catch2/catch_test_macros.hpp>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
using namespace pbrenderd3d;

// Independent CPU oracle: pixel centers, scalar aspect fit and nearest source
// index. It deliberately never calls the production viewport or Qt resampler.
std::vector<std::byte> ExpectedPixels(const CanonicalBgraFrameView& frame, const std::uint32_t width, const std::uint32_t height)
{
    std::vector<std::byte> output(static_cast<std::size_t>(width) * height * 4);
    const double scale = (std::min)({static_cast<double>(width) / frame.width, static_cast<double>(height) / frame.height, 2.0});
    const double left = (width - frame.width * scale) / 2;
    const double top = (height - frame.height * scale) / 2;
    for (std::uint32_t y = 0; y < height; y++)
    {
        for (std::uint32_t x = 0; x < width; x++)
        {
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
            const double sourceX = (x + 0.5 - left) / scale;
            const double sourceY = (y + 0.5 - top) / scale;
            if (scale >= 1 && sourceX >= 0 && sourceX < frame.width && sourceY >= 0 && sourceY < frame.height)
            {
                const auto sourceOffset = static_cast<std::size_t>(sourceY) * frame.rowPitch + static_cast<std::size_t>(sourceX) * 4;
                std::copy_n(frame.pixels.data() + sourceOffset, 3, output.data() + offset);
            }
            else
            {
                std::fill_n(output.data() + offset, 3, std::byte{128});
            }
            output[offset + 3] = std::byte{255};
        }
    }
    return output;
}

std::vector<std::byte> ImageBytes(const QImage& image)
{
    std::vector<std::byte> bytes(static_cast<std::size_t>(image.width()) * image.height() * 4);
    for (int row = 0; row < image.height(); row++)
    {
        std::memcpy(bytes.data() + static_cast<std::size_t>(row) * image.width() * 4, image.constScanLine(row), static_cast<std::size_t>(image.width()) * 4);
    }
    return bytes;
}

QImage Paint(const DataWindowConfig& config, const CanonicalBgraFrameView& frame, const std::uint32_t width,
    const std::uint32_t height, const double devicePixelRatio, QImage& scratch)
{
    QImage output(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGB32);
    output.fill(0xff123456U);
    output.setDevicePixelRatio(devicePixelRatio);
    QPainter painter(&output);
    REQUIRE(pbencoder::PaintQtRasterFrame(painter, config, frame, width, height, devicePixelRatio, scratch));
    REQUIRE(painter.end());
    return output;
}
}

TEST_CASE("Qt raster preserves canonical pixels at fractional DPR", "[qt-raster][pixels]")
{
    pbencoder::test::EnsureApplication();
    DataWindowConfig config;
    config.presentationModel = PresentationModel::QtRaster;
    config.width = 4;
    config.height = 2;
    std::array<std::byte, 36> pixels{};
    for (std::size_t index = 0; index < pixels.size(); index++)
    {
        pixels[index] = static_cast<std::byte>((index * 17 + 29) & 255);
    }
    const auto original = pixels;
    const CanonicalBgraFrameView frame{pixels, 4, 2, 20, 17, 1};
    QImage scratch;
    constexpr std::array<std::array<std::uint32_t, 2>, 8> sizes{{{4, 2}, {6, 3}, {8, 4}, {11, 7}, {3, 2}, {9, 4}, {7, 5}, {20, 9}}};
    for (const double ratio : {1.0, 1.25, 1.5, 2.0})
    {
        for (const auto& size : sizes)
        {
            INFO(ratio << ": " << size[0] << "x" << size[1]);
            const auto output = Paint(config, frame, size[0], size[1], ratio, scratch);
            CHECK(ImageBytes(output) == ExpectedPixels(frame, size[0], size[1]));
            CHECK(pixels == original);
            const auto* const reused = scratch.constBits();
            const auto repeated = Paint(config, frame, size[0], size[1], ratio, scratch);
            CHECK(scratch.constBits() == reused);
            CHECK(ImageBytes(repeated) == ImageBytes(output));
        }
    }
}

TEST_CASE("Qt raster validates padded rows and resource bounds", "[qt-raster][pixels]")
{
    pbencoder::test::EnsureApplication();
    DataWindowConfig config;
    config.presentationModel = PresentationModel::QtRaster;
    config.width = 2;
    config.height = 2;
    config.maximumFrameBytes = 64;
    const std::array<std::byte, 20> pixels{};
    CanonicalBgraFrameView frame{pixels, 2, 2, 12, 1, 1};
    QImage scratch;
    QImage output(4, 4, QImage::Format_RGB32);
    QPainter painter(&output);
    REQUIRE(pbencoder::PaintQtRasterFrame(painter, config, frame, 4, 4, 1.0, scratch));
    frame.rowPitch = 7;
    CHECK_FALSE(pbencoder::PaintQtRasterFrame(painter, config, frame, 4, 4, 1.0, scratch));
    frame.rowPitch = (std::numeric_limits<std::size_t>::max)();
    CHECK_FALSE(pbencoder::PaintQtRasterFrame(painter, config, frame, 4, 4, 1.0, scratch));
    frame.rowPitch = 12;
    frame.pixels = std::span(pixels).first(19);
    CHECK_FALSE(pbencoder::PaintQtRasterFrame(painter, config, frame, 4, 4, 1.0, scratch));
    frame.pixels = pixels;
    for (const auto width : {0U, 5U, 16385U, (std::numeric_limits<std::uint32_t>::max)()})
    {
        CHECK_FALSE(pbencoder::PaintQtRasterFrame(painter, config, frame, width, 4, 1.0, scratch));
    }
    for (const double ratio : {0.0, -1.0, 2.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        CHECK_FALSE(pbencoder::PaintQtRasterFrame(painter, config, frame, 4, 4, ratio, scratch));
    }
    CHECK_FALSE(pbencoder::PaintQtRasterFrame(painter, config, frame, 4, 0, 1.0, scratch));
    config.width = 0;
    CHECK_FALSE(pbencoder::PaintQtRasterFrame(painter, config, frame, 4, 4, 1.0, scratch));
    config.width = 2;
    config.maximumFrameBytes = 19;
    CHECK_FALSE(pbencoder::PaintQtRasterFrame(painter, config, frame, 4, 4, 1.0, scratch));
    REQUIRE(painter.end());
    CHECK_FALSE(pbencoder::PaintQtRasterFrame(painter, config, frame, 4, 4, 1.0, scratch));
}

TEST_CASE("Qt raster preserves four GUI profile rasters and agrees with native WARP point output", "[qt-raster][pixels][warp]")
{
    pbencoder::test::EnsureApplication();
    const std::array profiles{pbprotocol::kUnifiedProductVisualProfile, pbprotocol::kGrayFastExperimentalProfile,
        pbprotocol::kPam4ExperimentalProfile, pbprotocol::kPam4WideExperimentalProfile};
    for (std::size_t mode = 0; mode < profiles.size(); mode++)
    {
        INFO(mode);
        DataWindowConfig config;
        config.presentationModel = PresentationModel::QtRaster;
        pbprotocol::BootstrapRecord bootstrap;
        bootstrap.protocolVersion = pbprotocol::GetProtocolVersion();
        bootstrap.visualProfileId = profiles[mode].visualProfileId;
        bootstrap.visualLayoutVersion = profiles[mode].visualLayoutVersion;
        bootstrap.frameSequence = 17;
        std::array<std::byte, pbprotocol::kBootstrapRecordBytes> serialized{};
        REQUIRE(pbprotocol::SerializeBootstrapRecord(bootstrap, serialized));
        const auto codedBytes = mode == 3 ? pbmodulation::kExperimentalPam4WideCodedFrameBytes : mode == 2 ?
            pbmodulation::kExperimentalPam4CodedFrameBytes : mode == 1 ? pbmodulation::kUnifiedGrayCodedFrameBytes : pbmodulation::kUnifiedCodedFrameBytes;
        std::vector<std::byte> coded(codedBytes);
        for (std::size_t index = 0; index < coded.size(); index++)
        {
            coded[index] = static_cast<std::byte>((index * 29 + 11) & 255);
        }
        std::vector<std::byte> pixels(static_cast<std::size_t>(config.width) * config.height * 4);
        const auto encoded = mode == 3 ? pbmodulation::EncodeExperimentalPam4WideFrame(serialized, coded, pixels) : mode == 2 ?
            pbmodulation::EncodeExperimentalPam4Frame(serialized, coded, pixels) : pbmodulation::EncodeUnifiedVisualFrame(serialized, coded, pixels);
        REQUIRE(encoded);
        const CanonicalBgraFrameView frame{pixels, config.width, config.height, static_cast<std::size_t>(config.width) * 4, 17, 1};
        const std::uint32_t width = mode == 3 ? 2560U : 1920U;
        const std::uint32_t height = mode == 3 ? 1440U : 1080U;
        QImage scratch;
        const auto output = Paint(config, frame, width, height, 1.25, scratch);
        CHECK(ImageBytes(output) == ExpectedPixels(frame, width, height));
        config.presentationModel = PresentationModel::Flip;
        const auto warp = RenderWarpOffscreenForTest(config, pixels, width, height, false);
        REQUIRE(warp);
        CHECK(ImageBytes(output) == warp.Value().pixels);
    }
}
