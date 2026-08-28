#include "local_desktop_resample_fixtures.h"
#include "local_desktop_test_fixtures.h"
#include "pbmodulation/local_desktop_decode.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

using namespace localdesktoptest;
using namespace pbmodulation;

namespace
{

using Erasure = LocalDesktopErasureReason;

void RequireAccepted(const LocalDesktopObservation& observation, const std::array<std::byte, 44>& record)
{
    INFO("erasure=" << GetLocalDesktopErasureName(observation.erasure) << " markers=" << observation.markerCandidates <<
         " geometries=" << observation.geometryCandidates << " work=" << observation.workUnits <<
         " origin=" << observation.geometry.originX << ',' << observation.geometry.originY <<
         " scale=" << observation.geometry.scaleX << ',' << observation.geometry.scaleY <<
         " levels=" << observation.blackLevel << ',' << observation.whiteLevel <<
         " timingResidual=" << observation.timingResidual << " midGray=" << observation.midGrayFraction);
    REQUIRE(observation.IsAccepted());
    CHECK(observation.erasure == Erasure::None);
    CHECK(observation.canonical44 == record);
    CHECK(observation.copies[0].canonical44 == record);
    CHECK(observation.copies[1].canonical44 == record);
    for (const auto& copy : observation.copies)
    {
        CHECK(copy.fecDecoded);
        CHECK(copy.crcValid);
        CHECK(copy.recordValid);
    }
    CHECK(observation.quality > 0);
    CHECK(observation.quality <= 1);
    CHECK(observation.workUnits <= LocalDesktopDecodePolicy{}.maximumWorkUnits);
}

void RequireErased(const LocalDesktopObservation& observation, const Erasure reason)
{
    INFO("erasure=" << GetLocalDesktopErasureName(observation.erasure) << " expected=" << GetLocalDesktopErasureName(reason) <<
         " markers=" << observation.markerCandidates << " geometries=" << observation.geometryCandidates << " work=" << observation.workUnits);
    REQUIRE_FALSE(observation.IsAccepted());
    CHECK(observation.erasure == reason);
    CHECK(observation.canonical44 == std::array<std::byte, 44>{});
    CHECK(observation.quality == 0);
}

std::uint8_t TimingLevel(const std::vector<std::byte>& timing, const std::uint32_t patch, const std::uint32_t cell)
{
    return timing[patch * 256 + cell] == std::byte{0} ? std::uint8_t{32} : std::uint8_t{224};
}

GrayImage TwoFrames(const GrayImage& first, const GrayImage& second)
{
    const auto firstHalf = Resample(first, 0.5, 0.5, 0, 0, FixtureFilter::Area, 0);
    const auto secondHalf = Resample(second, 0.5, 0.5, 0, 0, FixtureFilter::Area, 0);
    GrayImage result(1960, 564);
    CopyBlock(firstHalf, result, 0, 0, 960, 540, 8, 12);
    CopyBlock(secondHalf, result, 0, 0, 960, 540, 992, 12);
    return result;
}

} // namespace

static_assert(std::is_trivially_copyable_v<LumaView>);
static_assert(std::is_trivially_copyable_v<LocalDesktopObservation>);
static_assert(sizeof(LocalDesktopObservation) < 1024);

TEST_CASE("LocalDesktop portable samples honor footprint, pitch, format and immutable failures", "[pbmodulation][localdesktop][sampling]")
{
    std::array<std::byte, 9> bytes{std::byte{10}, std::byte{30}, std::byte{0xFF}, std::byte{0xFF},
                                  std::byte{50}, std::byte{90}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
    LumaView view{bytes, 2, 2, 4, LumaPixelFormat::Gray8};
    CHECK(ValidateLumaView(view) == Erasure::None);
    double sample = -17;
    CHECK(SampleLuma(view, 0.5, 0.5, sample) == Erasure::None);
    CHECK(sample == 45);
    CHECK(SampleLuma(view, 1, 1, sample) == Erasure::None);
    CHECK(sample == 90);
    for (const auto point : std::array<std::array<double, 2>, 4>{{{-0.01, 0}, {0, -0.01}, {1.01, 0}, {0, 1.01}}})
    {
        sample = -17;
        CHECK(SampleLuma(view, point[0], point[1], sample) == Erasure::SampleOutOfBounds);
        CHECK(sample == -17);
    }
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
    {
        sample = -17;
        CHECK(SampleLuma(view, invalid, 0, sample) == Erasure::NonFinitePixel);
        CHECK(SampleLuma(view, 0, invalid, sample) == Erasure::NonFinitePixel);
        CHECK(sample == -17);
    }
    view.pixels = std::span<const std::byte>(bytes).first(5);
    sample = -17;
    CHECK(ValidateLumaView(view) == Erasure::InvalidView);
    CHECK(SampleLuma(view, 0, 0, sample) == Erasure::InvalidView);
    CHECK(sample == -17);
    view = {bytes, 2, 2, 1, LumaPixelFormat::Gray8};
    CHECK(ValidateLumaView(view) == Erasure::InvalidView);
    view = {bytes, 1, 3, std::numeric_limits<std::size_t>::max(), LumaPixelFormat::Gray8};
    CHECK(ValidateLumaView(view) == Erasure::InvalidView);
    view = {bytes, std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(),
            std::numeric_limits<std::size_t>::max(), LumaPixelFormat::Fp16LinearSdr};
    CHECK(ValidateLumaView(view) == Erasure::InvalidView);
    view = {bytes, 1, 1, 1, static_cast<LumaPixelFormat>(255)};
    CHECK(ValidateLumaView(view) == Erasure::UnsupportedFormat);
    RequireErased(DecodeLocalDesktopBootstrap(view), Erasure::UnsupportedFormat);
    RequireErased(DecodeLocalDesktopBootstrap({}), Erasure::InvalidView);

    std::array<std::byte, 4> red{std::byte{0}, std::byte{0}, std::byte{255}, std::byte{0}};
    CHECK(SampleLuma({red, 1, 1, 4, LumaPixelFormat::Bgra8}, 0, 0, sample) == Erasure::None);
    CHECK(sample == Catch::Approx(255 * 0.2126).margin(1.0e-12));
    std::array<std::byte, 4> r10Red{std::byte{255}, std::byte{3}, std::byte{0}, std::byte{0}};
    CHECK(SampleLuma({r10Red, 1, 1, 4, LumaPixelFormat::R10G10B10A2}, 0, 0, sample) == Erasure::None);
    CHECK(sample == Catch::Approx(255 * 0.2126).margin(1.0e-12));
}

TEST_CASE("LocalDesktop FP16 rejects every sampled NaN, infinity and out-of-SDR component", "[pbmodulation][localdesktop][sampling]")
{
    constexpr std::array<std::uint16_t, 5> invalidBits{0x7C00, 0xFC00, 0x7E01, 0xBC00, 0x4000};
    for (std::size_t component = 0; component < 4; component++)
    {
        for (const auto bits : invalidBits)
        {
            std::array<std::byte, 8> bytes{};
            bytes[6] = std::byte{0};
            bytes[7] = std::byte{0x3C};
            bytes[component * 2] = static_cast<std::byte>(bits & 255u);
            bytes[component * 2 + 1] = static_cast<std::byte>(bits >> 8);
            double output = 12345;
            const auto reason = (bits & 0x7C00u) == 0x7C00u ? Erasure::NonFinitePixel : Erasure::InvalidPixelValue;
            CHECK(SampleLuma({bytes, 1, 1, 8, LumaPixelFormat::Fp16LinearSdr}, 0, 0, output) == reason);
            CHECK(output == 12345);
        }
    }
    GrayImage neutral(256, 1);
    for (std::uint32_t value = 0; value < 256; value++)
    {
        neutral.Write(value, 0, static_cast<std::uint8_t>(value));
    }
    const auto encoded = ConvertFormat(neutral, LumaPixelFormat::Fp16LinearSdr);
    for (std::uint32_t value = 0; value < 256; value++)
    {
        double output = -1;
        CHECK(SampleLuma(encoded.View(), value, 0, output) == Erasure::None);
        CHECK(output == Catch::Approx(value).margin(0.05));
    }
}

TEST_CASE("LocalDesktop independently painted Golden frames decode all canonical bytes", "[pbmodulation][localdesktop][golden]")
{
    for (const std::string stem : {"a", "b", "c", "d", "e", "f"})
    {
        INFO(stem);
        const auto bgra = MakeGoldenRaster(stem);
        const auto expected = LoadGoldenRecord(stem);
        const auto gray = GrayFromGolden(bgra);
        const auto observation = DecodeLocalDesktopBootstrap(gray.View());
        RequireAccepted(observation, expected);
        CHECK(observation.markerCandidates == 4);
        CHECK(observation.geometryCandidates == 1);
        CHECK(observation.geometry.originX == Catch::Approx(0).margin(1.0e-9));
        CHECK(observation.geometry.originY == Catch::Approx(0).margin(1.0e-9));
        CHECK(observation.geometry.scaleX == Catch::Approx(1).margin(1.0e-9));
        CHECK(observation.geometry.scaleY == Catch::Approx(1).margin(1.0e-9));
        CHECK(observation.blackLevel == 32);
        CHECK(observation.whiteLevel == 224);
        CHECK(observation.copies[0].correctedSymbols == 0);
        CHECK(observation.copies[1].correctedSymbols == 0);
        CHECK(observation.timingResidual == 0);
        CHECK(observation.midGrayFraction == 0);
        const auto repeated = DecodeLocalDesktopBootstrap(gray.View());
        RequireAccepted(repeated, expected);
        CHECK(repeated.geometry == observation.geometry);
        CHECK(repeated.workUnits == observation.workUnits);
    }
}

TEST_CASE("LocalDesktop locator supports anisotropic subpixel area and bilinear sampling", "[pbmodulation][localdesktop][geometry]")
{
    const auto source = GrayFromGolden(MakeGoldenRaster());
    const auto expected = LoadGoldenRecord();
    struct Transform
    {
        double scaleX;
        double scaleY;
        double originX;
        double originY;
    };
    constexpr std::array<Transform, 8> transforms{{{0.5, 0.5, 0, 0}, {0.5, 2.0, 12.25, 13.75},
        {2.0, 0.5, 13.625, 12.125}, {0.625, 1.25, 12.25, 13.375}, {0.73, 1.41, 12.375, 13.625},
        {1.25, 0.75, 12.75, 13.25}, {1.5, 1.5, 12.125, 13.875}, {2.0, 2.0, 12.5, 13.5}}};
    for (const auto filter : {FixtureFilter::Area, FixtureFilter::Bilinear})
    {
        for (const auto& transform : transforms)
        {
            INFO("filter=" << static_cast<unsigned>(filter) << " scale=" << transform.scaleX << ',' << transform.scaleY <<
                 " phase=" << transform.originX << ',' << transform.originY);
            const auto raster = Resample(source, transform.scaleX, transform.scaleY, transform.originX, transform.originY, filter);
            const auto observation = DecodeLocalDesktopBootstrap(raster.View());
            RequireAccepted(observation, expected);
            CHECK(observation.geometry.originX == Catch::Approx(transform.originX).margin(0.4));
            CHECK(observation.geometry.originY == Catch::Approx(transform.originY).margin(0.4));
            CHECK(std::abs(observation.geometry.scaleX - transform.scaleX) * 1920 <= 0.4);
            CHECK(std::abs(observation.geometry.scaleY - transform.scaleY) * 1080 <= 0.4);
            CHECK(observation.geometry.markerResidualPixels <= LocalDesktopDecodePolicy{}.maximumGeometryResidualPixels);
        }
    }
    for (const double outsideScale : {0.49, 2.01})
    {
        const auto raster = Resample(source, outsideScale, 1, 12, 12, FixtureFilter::Area);
        const auto observation = DecodeLocalDesktopBootstrap(raster.View());
        RequireErased(observation, Erasure::InvalidGeometry);
    }
}

TEST_CASE("LocalDesktop direct strided BGRA, R10 and linear-SDR half formats share gates", "[pbmodulation][localdesktop][formats]")
{
    const auto source = GrayFromGolden(MakeGoldenRaster());
    const auto raster = Resample(source, 0.625, 1.25, 12.25, 13.375, FixtureFilter::Area);
    const auto expected = LoadGoldenRecord();
    for (const auto format : {LumaPixelFormat::Bgra8, LumaPixelFormat::R10G10B10A2, LumaPixelFormat::Fp16LinearSdr})
    {
        INFO("format=" << static_cast<unsigned>(format));
        const auto encoded = ConvertFormat(raster, format);
        const auto before = encoded.pixels;
        RequireAccepted(DecodeLocalDesktopBootstrap(encoded.View()), expected);
        CHECK(encoded.pixels == before);
    }
}

TEST_CASE("LocalDesktop A and B independently spend their own sixteen-symbol RS budgets", "[pbmodulation][localdesktop][fec]")
{
    auto raster = GrayFromGolden(MakeGoldenRaster());
    auto first = LoadGoldenBytes("a-rs76.bin", 76);
    auto second = first;
    for (std::size_t index = 0; index < 16; index++)
    {
        first[(index * 5) % 76] ^= std::byte{0xA5};
    }
    for (std::size_t index = 0; index < 12; index++)
    {
        second[(3 + index * 7) % 76] ^= std::byte{0xC3};
    }
    PaintCopy(raster, 0, first);
    PaintCopy(raster, 1, second);
    const auto observation = DecodeLocalDesktopBootstrap(raster.View());
    RequireAccepted(observation, LoadGoldenRecord());
    CHECK(observation.copies[0].correctedSymbols == 16);
    CHECK(observation.copies[1].correctedSymbols == 12);
    CHECK(observation.copies[0].residual == 0);
    CHECK(observation.copies[1].residual == 0);

    auto damaged = GrayFromGolden(MakeGoldenRaster());
    auto excessive = LoadGoldenBytes("a-rs76.bin", 76);
    for (std::size_t index = 0; index < 17; index++)
    {
        excessive[index] ^= std::byte{0xFF};
    }
    PaintCopy(damaged, 0, excessive);
    const auto rejected = DecodeLocalDesktopBootstrap(damaged.View());
    RequireErased(rejected, Erasure::BootstrapFecFailure);
    CHECK_FALSE(rejected.copies[0].fecDecoded);
    CHECK(rejected.copies[1].fecDecoded);
    CHECK(rejected.copies[1].crcValid);
}

TEST_CASE("LocalDesktop CRC and protocol gates do not trust a perfectly valid RS word", "[pbmodulation][localdesktop][crc]")
{
    const auto invalidCrc = GrayFromGolden(MakeGoldenRaster("badcrc"));
    const auto crcObservation = DecodeLocalDesktopBootstrap(invalidCrc.View());
    RequireErased(crcObservation, Erasure::BootstrapCrcFailure);
    for (const auto& copy : crcObservation.copies)
    {
        CHECK(copy.fecDecoded);
        CHECK(copy.correctedSymbols == 0);
        CHECK_FALSE(copy.crcValid);
        CHECK_FALSE(copy.recordValid);
    }
    const auto unsupported = GrayFromGolden(MakeGoldenRaster("unsupported"));
    const auto unsupportedObservation = DecodeLocalDesktopBootstrap(unsupported.View());
    RequireErased(unsupportedObservation, Erasure::UnsupportedRecord);
    for (const auto& copy : unsupportedObservation.copies)
    {
        CHECK(copy.fecDecoded);
        CHECK(copy.crcValid);
        CHECK_FALSE(copy.recordValid);
    }
}

TEST_CASE("LocalDesktop full-44 agreement rejects sequence, session and control-epoch substitution", "[pbmodulation][localdesktop][torn]")
{
    for (const std::string other : {"b", "c", "d", "e", "f"})
    {
        INFO(other);
        auto raster = GrayFromGolden(MakeGoldenRaster());
        const auto otherWord = LoadGoldenBytes(other + "-rs76.bin", 76);
        PaintCopy(raster, 1, otherWord);
        const auto observation = DecodeLocalDesktopBootstrap(raster.View());
        RequireErased(observation, Erasure::BootstrapMismatch);
        CHECK(observation.copies[0].recordValid);
        CHECK(observation.copies[1].recordValid);
        CHECK(observation.copies[0].canonical44 == LoadGoldenRecord());
        CHECK(observation.copies[1].canonical44 == LoadGoldenRecord(other));
    }
}

TEST_CASE("LocalDesktop horizontal, vertical and both diagonal mixed frames erase", "[pbmodulation][localdesktop][torn]")
{
    const auto first = GrayFromGolden(MakeGoldenRaster());
    const auto second = GrayFromGolden(MakeGoldenRaster("b"));
    for (unsigned direction = 0; direction < 6; direction++)
    {
        INFO("direction=" << direction);
        auto mixed = first;
        for (std::uint32_t row = 0; row < mixed.height; row++)
        {
            for (std::uint32_t column = 0; column < mixed.width; column++)
            {
                bool chooseSecond = false;
                switch (direction)
                {
                case 0: chooseSecond = row >= 540; break;
                case 1: chooseSecond = row < 540; break;
                case 2: chooseSecond = column >= 960; break;
                case 3: chooseSecond = column < 960; break;
                case 4: chooseSecond = column * 1080u + row * 1920u >= 1920u * 1080u; break;
                default: chooseSecond = column * 1080u < row * 1920u; break;
                }
                if (chooseSecond)
                {
                    mixed.Write(column, row, second.Read(column, row));
                }
            }
        }
        const auto observation = DecodeLocalDesktopBootstrap(mixed.View());
        REQUIRE_FALSE(observation.IsAccepted());
        CHECK(observation.canonical44 == std::array<std::byte, 44>{});
        CHECK((observation.erasure == Erasure::BootstrapMismatch || observation.erasure == Erasure::BootstrapFecFailure ||
               observation.erasure == Erasure::BootstrapCrcFailure || observation.erasure == Erasure::TimingMismatch));
    }
}

TEST_CASE("LocalDesktop each distributed timing patch binds full sequence and record", "[pbmodulation][localdesktop][timing]")
{
    constexpr std::array<std::uint32_t, 3> originsX{96, 896, 1696};
    constexpr std::array<std::uint32_t, 3> originsY{160, 476, 792};
    const auto baseline = GrayFromGolden(MakeGoldenRaster());
    for (const std::string other : {"b", "d", "e", "f"})
    {
        const auto foreign = GrayFromGolden(MakeGoldenRaster(other));
        const std::uint32_t firstPatch = other == "b" ? 0u : 4u;
        const std::uint32_t lastPatch = other == "b" ? 9u : 5u;
        for (std::uint32_t patch = firstPatch; patch < lastPatch; patch++)
        {
            INFO("record=" << other << " patch=" << patch);
            auto raster = baseline;
            CopyBlock(foreign, raster, originsX[patch % 3], originsY[patch / 3], 128, 128, originsX[patch % 3], originsY[patch / 3]);
            const auto observation = DecodeLocalDesktopBootstrap(raster.View());
            RequireErased(observation, Erasure::TimingMismatch);
            CHECK(observation.copies[0].canonical44 == LoadGoldenRecord());
            CHECK(observation.copies[1].canonical44 == LoadGoldenRecord());
            CHECK(observation.copies[0].recordValid);
            CHECK(observation.copies[1].recordValid);
            CHECK(observation.timingBitErrorFraction > 0.02);
            CHECK(observation.midGrayFraction == 0);
        }
    }
}

TEST_CASE("LocalDesktop 25, 50 and 75 percent blends erase in both filter orders", "[pbmodulation][localdesktop][blend]")
{
    const auto first = GrayFromGolden(MakeGoldenRaster());
    const auto second = GrayFromGolden(MakeGoldenRaster("b"));
    for (const auto filter : {FixtureFilter::Area, FixtureFilter::Bilinear})
    {
        const auto resizedFirst = Resample(first, 0.73, 1.41, 12.375, 13.625, filter);
        const auto resizedSecond = Resample(second, 0.73, 1.41, 12.375, 13.625, filter);
        for (unsigned quarters = 1; quarters <= 3; quarters++)
        {
            INFO("filter=" << static_cast<unsigned>(filter) << " secondQuarters=" << quarters);
            const auto blendThenResize = Resample(Blend(first, second, quarters), 0.73, 1.41, 12.375, 13.625, filter);
            const auto resizeThenBlend = Blend(resizedFirst, resizedSecond, quarters);
            const auto firstOrder = DecodeLocalDesktopBootstrap(blendThenResize.View());
            const auto secondOrder = DecodeLocalDesktopBootstrap(resizeThenBlend.View());
            RequireErased(firstOrder, Erasure::DoubleImage);
            RequireErased(secondOrder, Erasure::DoubleImage);
            CHECK(firstOrder.midGrayFraction > 0.06);
            CHECK(secondOrder.midGrayFraction > 0.06);
        }
    }
}

TEST_CASE("LocalDesktop frozen contrast and soft residual thresholds have neighboring fixtures", "[pbmodulation][localdesktop][threshold]")
{
    const auto baseline = GrayFromGolden(MakeGoldenRaster());
    const auto expected = LoadGoldenRecord();
    const auto timing = LoadGoldenBytes("a-timing-bits.bin", 2304);
    for (const std::uint8_t white : {std::uint8_t{175}, std::uint8_t{176}})
    {
        auto raster = baseline;
        for (std::uint32_t row = 0; row < raster.height; row++)
        {
            for (std::uint32_t column = 0; column < raster.width; column++)
            {
                const auto value = raster.Read(column, row);
                raster.Write(column, row, value == 32 ? std::uint8_t{80} : value == 224 ? white : value);
            }
        }
        const auto observation = DecodeLocalDesktopBootstrap(raster.View());
        if (white == 176)
        {
            RequireAccepted(observation, expected);
            CHECK(observation.whiteLevel - observation.blackLevel == 96);
        }
        else
        {
            RequireErased(observation, Erasure::LowContrast);
        }
    }
    for (const unsigned delta : {12u, 13u})
    {
        auto raster = baseline;
        for (std::uint32_t cell = 0; cell < 256; cell++)
        {
            const auto level = TimingLevel(timing, 4, cell);
            PaintTimingCell(raster, 4, cell, static_cast<std::uint8_t>(level == 32 ? level + delta : level - delta));
        }
        const auto observation = DecodeLocalDesktopBootstrap(raster.View());
        if (delta == 12)
        {
            RequireAccepted(observation, expected);
            CHECK(observation.timingResidual == Catch::Approx(0.0625).margin(1.0e-12));
        }
        else
        {
            RequireErased(observation, Erasure::ExcessResidual);
        }
    }
    for (const unsigned delta : {15u, 16u})
    {
        auto raster = baseline;
        for (const auto origin : std::array<std::array<std::uint32_t, 2>, 2>{{{96, 16}, {1216, 1000}}})
        {
            for (std::uint32_t row = 0; row < 64; row++)
            {
                for (std::uint32_t column = 0; column < 608; column++)
                {
                    const auto level = raster.Read(origin[0] + column, origin[1] + row);
                    raster.Write(origin[0] + column, origin[1] + row, static_cast<std::uint8_t>(level == 32 ? level + delta : level - delta));
                }
            }
        }
        const auto observation = DecodeLocalDesktopBootstrap(raster.View());
        if (delta == 15)
        {
            RequireAccepted(observation, expected);
            CHECK(observation.copies[0].residual == Catch::Approx(15.0 / 192).margin(1.0e-12));
        }
        else
        {
            RequireErased(observation, Erasure::ExcessResidual);
        }
    }
}

TEST_CASE("LocalDesktop independent timing-bit and middle-gray gates keep exact boundaries", "[pbmodulation][localdesktop][threshold]")
{
    const auto baseline = GrayFromGolden(MakeGoldenRaster());
    const auto expected = LoadGoldenRecord();
    const auto timing = LoadGoldenBytes("a-timing-bits.bin", 2304);
    for (const unsigned count : {5u, 6u})
    {
        auto raster = baseline;
        for (std::uint32_t cell = 0; cell < count; cell++)
        {
            PaintTimingCell(raster, 4, cell, static_cast<std::uint8_t>(256 - TimingLevel(timing, 4, cell)));
        }
        const auto observation = DecodeLocalDesktopBootstrap(raster.View());
        CHECK(observation.timingBitErrorFraction == static_cast<double>(count) / 256);
        if (count == 5)
        {
            RequireAccepted(observation, expected);
        }
        else
        {
            RequireErased(observation, Erasure::TimingMismatch);
        }
    }
    for (const unsigned count : {15u, 16u})
    {
        auto raster = baseline;
        unsigned painted = 0;
        for (std::uint32_t cell = 0; cell < 256 && painted < count; cell++)
        {
            if (TimingLevel(timing, 4, cell) == 224)
            {
                PaintTimingCell(raster, 4, cell, 176);
                painted++;
            }
        }
        REQUIRE(painted == count);
        const auto observation = DecodeLocalDesktopBootstrap(raster.View());
        CHECK(observation.timingBitErrorFraction == 0);
        CHECK(observation.midGrayFraction == static_cast<double>(count) / 256);
        if (count == 15)
        {
            RequireAccepted(observation, expected);
        }
        else
        {
            RequireErased(observation, Erasure::DoubleImage);
        }
    }
    for (const std::uint8_t level : {std::uint8_t{66}, std::uint8_t{67}})
    {
        auto raster = baseline;
        unsigned painted = 0;
        for (std::uint32_t cell = 0; cell < 256 && painted < 16; cell++)
        {
            if (TimingLevel(timing, 4, cell) == 32)
            {
                PaintTimingCell(raster, 4, cell, level);
                painted++;
            }
        }
        REQUIRE(painted == 16);
        const auto observation = DecodeLocalDesktopBootstrap(raster.View());
        CHECK(observation.timingBitErrorFraction == 0);
        if (level == 66)
        {
            RequireAccepted(observation, expected);
            CHECK(observation.midGrayFraction == 0);
        }
        else
        {
            RequireErased(observation, Erasure::DoubleImage);
            CHECK(observation.midGrayFraction == 16.0 / 256);
        }
    }
}

TEST_CASE("LocalDesktop all budgets fail closed including multiple valid image locations", "[pbmodulation][localdesktop][budget]")
{
    const auto source = GrayFromGolden(MakeGoldenRaster());
    LocalDesktopDecodePolicy tinyWork;
    tinyWork.maximumWorkUnits = 17;
    const auto exhausted = DecodeLocalDesktopBootstrap(source.View(), tinyWork);
    RequireErased(exhausted, Erasure::WorkBudgetExceeded);
    CHECK(exhausted.workUnits == 17);
    LocalDesktopDecodePolicy oneMarker;
    oneMarker.maximumMarkers = 1;
    RequireErased(DecodeLocalDesktopBootstrap(source.View(), oneMarker), Erasure::MarkerBudgetExceeded);

    for (const std::uint32_t count : {64u, 65u})
    {
        GrayImage markerWall(736, 656);
        for (std::uint32_t index = 0; index < count; index++)
        {
            CopyBlock(source, markerWall, 16, 16, 64, 64, 8 + (index % 9) * 80, 8 + (index / 9) * 80);
        }
        const auto observation = DecodeLocalDesktopBootstrap(markerWall.View());
        RequireErased(observation, count == 64 ? Erasure::IncompleteMarkers : Erasure::MarkerBudgetExceeded);
        CHECK(observation.markerCandidates == 64);
    }
    const auto doubleRaster = TwoFrames(source, source);
    const auto ambiguous = DecodeLocalDesktopBootstrap(doubleRaster.View());
    RequireErased(ambiguous, Erasure::AmbiguousGeometry);
    CHECK(ambiguous.markerCandidates == 8);
    CHECK(ambiguous.geometryCandidates == 2);
    LocalDesktopDecodePolicy oneGeometry;
    oneGeometry.maximumGeometries = 1;
    const auto geometryBudget = DecodeLocalDesktopBootstrap(doubleRaster.View(), oneGeometry);
    RequireErased(geometryBudget, Erasure::GeometryBudgetExceeded);
    CHECK(geometryBudget.geometryCandidates == 1);
}

TEST_CASE("LocalDesktop malformed and weakened policy is never an unlimited fallback", "[pbmodulation][localdesktop][budget]")
{
    const GrayImage blank(8, 8);
    std::array<LocalDesktopDecodePolicy, 14> invalid{};
    invalid[0].maximumWorkUnits = 0;
    invalid[1].maximumWorkUnits = std::numeric_limits<std::uint64_t>::max();
    invalid[2].maximumMarkers = 65;
    invalid[3].maximumGeometries = 33;
    invalid[4].maximumRefinementIterations = 0;
    invalid[5].maximumRefinementIterations = 5;
    invalid[6].minimumContrast = 95;
    invalid[7].minimumScale = 0.49;
    invalid[8].maximumScale = 2.01;
    invalid[9].minimumScale = 1.5;
    invalid[9].maximumScale = 1;
    invalid[10].maximumTimingResidual = std::numeric_limits<double>::quiet_NaN();
    invalid[11].maximumBootstrapResidual = std::numeric_limits<double>::infinity();
    invalid[12].maximumMidGrayFraction = 0.061;
    invalid[13].midGrayBoundary = 0.181;
    for (const auto& policy : invalid)
    {
        const auto observation = DecodeLocalDesktopBootstrap(blank.View(), policy);
        RequireErased(observation, Erasure::InvalidPolicy);
        CHECK(observation.workUnits == 0);
    }
    RequireErased(DecodeLocalDesktopBootstrap(blank.View()), Erasure::MarkersNotFound);
}
