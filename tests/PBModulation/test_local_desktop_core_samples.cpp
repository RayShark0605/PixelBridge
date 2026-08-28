#include "local_desktop_resample_fixtures.h"
#include "local_desktop_test_fixtures.h"
#include "pbmodulation/local_desktop_decode.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

using namespace localdesktoptest;
using namespace pbmodulation;

namespace
{

using Erasure = LocalDesktopErasureReason;

struct CellOrigin
{
    std::uint32_t x;
    std::uint32_t y;
};

// Literal independent layout: these fixtures never obtain a sample position,
// a decoded hard bit, or an expected gray count from the production decoder.
constexpr std::array<CellOrigin, 9> timingOrigins{{
    {96, 160}, {896, 160}, {1696, 160},
    {96, 476}, {896, 476}, {1696, 476},
    {96, 792}, {896, 792}, {1696, 792}}};
constexpr std::array<CellOrigin, 2> copyOrigins{{{96, 16}, {1216, 1000}}};

void PaintRightCoreSample(GrayImage& image, const CellOrigin origin, const unsigned columns,
                          const unsigned cellCount, const unsigned delta = 96)
{
    REQUIRE(columns > 0);
    REQUIRE(cellCount <= (columns == 16 ? 256u : 608u));
    REQUIRE(delta <= 192);
    for (unsigned cell = 0; cell < cellCount; cell++)
    {
        const std::uint32_t left = origin.x + (cell % columns) * 8;
        const std::uint32_t top = origin.y + (cell / columns) * 8;
        const auto level = image.Read(left + 5, top + 3);
        REQUIRE((level == 32 || level == 224));
        const auto replacement = static_cast<std::uint8_t>(level == 32 ? level + delta : level - delta);
        // At exact 1x, only the right-axis point (left+4.5,top+3.5)
        // uses this column. Its two bilinear weights total 1/2; the four
        // other core samples remain unchanged. Delta96 therefore moves
        // one point to .25/.75 but its cell mean only to .05/.95.
        image.Write(left + 5, top + 3, replacement);
        image.Write(left + 5, top + 4, replacement);
    }
}

void CheckDecodedCopies(const LocalDesktopObservation& observation, const std::array<std::byte, 44>& expected)
{
    CHECK(observation.blackLevel == Catch::Approx(32).margin(1e-10));
    CHECK(observation.whiteLevel == Catch::Approx(224).margin(1e-10));
    CHECK(observation.geometry.originX == Catch::Approx(0).margin(1e-10));
    CHECK(observation.geometry.originY == Catch::Approx(0).margin(1e-10));
    CHECK(observation.geometry.scaleX == Catch::Approx(1).margin(1e-10));
    CHECK(observation.geometry.scaleY == Catch::Approx(1).margin(1e-10));
    for (const auto& copy : observation.copies)
    {
        CHECK(copy.canonical44 == expected);
        CHECK(copy.fecDecoded);
        CHECK(copy.crcValid);
        CHECK(copy.recordValid);
        CHECK(copy.correctedSymbols == 0);
        CHECK(copy.midGrayFraction == 0);
    }
    CHECK(observation.midGrayFraction == 0);
    CHECK(observation.timingBitErrorFraction == 0);
    CHECK(observation.workUnits <= LocalDesktopDecodePolicy{}.maximumWorkUnits);
}

void CheckDecision(const LocalDesktopObservation& observation, const std::array<std::byte, 44>& expected, const bool accepted)
{
    INFO("erasure=" << GetLocalDesktopErasureName(observation.erasure) << " meanGray=" << observation.midGrayFraction <<
         " sampleGray=" << observation.sampleMidGrayFraction << " timingResidual=" << observation.timingResidual <<
         " copy0SampleGray=" << observation.copies[0].sampleMidGrayFraction << " copy1SampleGray=" << observation.copies[1].sampleMidGrayFraction);
    CHECK(observation.IsAccepted() == accepted);
    CHECK(observation.erasure == (accepted ? Erasure::None : Erasure::DoubleImage));
    CHECK(observation.canonical44 == (accepted ? expected : std::array<std::byte, 44>{}));
    if (accepted)
    {
        CHECK(observation.quality > 0);
        CHECK(observation.quality <= 1);
    }
    else
    {
        CHECK(observation.quality == 0);
    }
}

GrayImage ShiftImage(const GrayImage& source, const std::int32_t offsetX, const std::int32_t offsetY)
{
    GrayImage result(source.width, source.height);
    for (std::uint32_t row = 0; row < result.height; row++)
    {
        for (std::uint32_t column = 0; column < result.width; column++)
        {
            result.Write(column, row, source.Read(static_cast<std::int64_t>(column) - offsetX, static_cast<std::int64_t>(row) - offsetY));
        }
    }
    return result;
}

} // namespace

TEST_CASE("LocalDesktop core-sample gray fraction gates each timing patch independently", "[local-desktop][core-samples]")
{
    const auto source = GrayFromGolden(MakeGoldenRaster());
    const auto expected = LoadGoldenRecord();
    for (std::size_t patch = 0; patch < timingOrigins.size(); patch++)
    {
        for (const unsigned changedCells : {76u, 77u})
        {
            INFO("patch=" << patch << " changedCells=" << changedCells);
            auto image = source;
            PaintRightCoreSample(image, timingOrigins[patch], 16, changedCells);
            const auto observation = DecodeLocalDesktopBootstrap(image.View());
            CheckDecodedCopies(observation, expected);
            const double expectedFraction = static_cast<double>(changedCells) / 1280;
            CHECK(observation.sampleMidGrayFraction == Catch::Approx(expectedFraction).margin(1e-12));
            CHECK(observation.timingResidual == Catch::Approx(static_cast<double>(changedCells) * 0.05 / 256).margin(1e-12));
            CHECK(observation.timingResidual < LocalDesktopDecodePolicy{}.maximumTimingResidual);
            CHECK(observation.copies[0].sampleMidGrayFraction == 0);
            CHECK(observation.copies[1].sampleMidGrayFraction == 0);
            CheckDecision(observation, expected, changedCells == 76);
        }
    }
}

TEST_CASE("LocalDesktop core-sample gray fraction cannot be averaged across A and B", "[local-desktop][core-samples]")
{
    const auto source = GrayFromGolden(MakeGoldenRaster());
    const auto expected = LoadGoldenRecord();
    for (std::size_t copy = 0; copy < copyOrigins.size(); copy++)
    {
        for (const unsigned changedCells : {182u, 183u})
        {
            INFO("copy=" << copy << " changedCells=" << changedCells);
            auto image = source;
            PaintRightCoreSample(image, copyOrigins[copy], 76, changedCells);
            const auto observation = DecodeLocalDesktopBootstrap(image.View());
            CheckDecodedCopies(observation, expected);
            const double expectedFraction = static_cast<double>(changedCells) / 3040;
            CHECK(observation.sampleMidGrayFraction == Catch::Approx(expectedFraction).margin(1e-12));
            CHECK(observation.copies[copy].sampleMidGrayFraction == Catch::Approx(expectedFraction).margin(1e-12));
            CHECK(observation.copies[1 - copy].sampleMidGrayFraction == 0);
            CHECK(observation.copies[copy].residual == Catch::Approx(static_cast<double>(changedCells) * 0.05 / 608).margin(1e-12));
            CHECK(observation.copies[copy].residual < LocalDesktopDecodePolicy{}.maximumBootstrapResidual);
            CheckDecision(observation, expected, changedCells == 182);
        }
    }
}

TEST_CASE("LocalDesktop original core points retain the frozen gray classification boundary", "[local-desktop][core-samples]")
{
    const auto source = GrayFromGolden(MakeGoldenRaster());
    const auto expected = LoadGoldenRecord();
    std::uint64_t previousWork = 0;
    for (const unsigned delta : {69u, 70u})
    {
        INFO("delta=" << delta);
        auto image = source;
        PaintRightCoreSample(image, timingOrigins[4], 16, 77, delta);
        const auto observation = DecodeLocalDesktopBootstrap(image.View());
        CheckDecodedCopies(observation, expected);
        // The two physical byte cases straddle .18 without requesting a
        // permissive policy: 69/384=.1796875 and 70/384=.1822916667.
        CHECK(observation.sampleMidGrayFraction == Catch::Approx(delta == 69 ? 0.0 : 77.0 / 1280).margin(1e-12));
        CHECK(observation.timingResidual == Catch::Approx(77.0 * delta / (1920.0 * 256)).margin(1e-12));
        CHECK(observation.timingResidual < LocalDesktopDecodePolicy{}.maximumTimingResidual);
        CheckDecision(observation, expected, delta == 69);
        if (previousWork != 0)
        {
            CHECK(observation.workUnits == previousWork);
        }
        previousWork = observation.workUnits;
    }
}

TEST_CASE("LocalDesktop stricter local gray policy also bounds original core points", "[local-desktop][core-samples]")
{
    auto image = GrayFromGolden(MakeGoldenRaster());
    const auto expected = LoadGoldenRecord();
    PaintRightCoreSample(image, timingOrigins[4], 16, 76);
    const auto defaultObservation = DecodeLocalDesktopBootstrap(image.View());
    CheckDecodedCopies(defaultObservation, expected);
    CheckDecision(defaultObservation, expected, true);
    auto policy = LocalDesktopDecodePolicy{};
    policy.maximumMidGrayFraction = 0.05;
    const auto strictObservation = DecodeLocalDesktopBootstrap(image.View(), policy);
    CheckDecodedCopies(strictObservation, expected);
    CHECK(strictObservation.sampleMidGrayFraction == defaultObservation.sampleMidGrayFraction);
    CHECK(strictObservation.workUnits == defaultObservation.workUnits);
    CheckDecision(strictObservation, expected, false);
}

TEST_CASE("LocalDesktop diagonal quarter ghosts cannot hide in cell-core means", "[local-desktop][core-samples][matrix]")
{
    const auto first = GrayFromGolden(MakeGoldenRaster("a"));
    const auto second = GrayFromGolden(MakeGoldenRaster("e"));
    const auto firstRecord = LoadGoldenRecord("a");
    const auto secondRecord = LoadGoldenRecord("e");
    REQUIRE(firstRecord[30] == std::byte{0x12});
    REQUIRE(secondRecord[30] == std::byte{0x13});
    constexpr std::array<FixtureFilter, 2> filters{FixtureFilter::Area, FixtureFilter::Bilinear};
    for (const auto filter : filters)
    {
        for (const unsigned secondQuarters : {1u, 3u})
        {
            const std::int32_t offsetX = secondQuarters == 1 ? 4 : -4;
            const std::int32_t offsetY = -offsetX;
            const auto shifted = ShiftImage(second, offsetX, offsetY);
            for (const bool blendBeforeResize : {true, false})
            {
                INFO("filter=" << (filter == FixtureFilter::Area ? "Area" : "Bilinear") << " secondQuarters=" << secondQuarters <<
                     " offset=" << offsetX << ',' << offsetY << " blendBeforeResize=" << blendBeforeResize);
                const auto image = blendBeforeResize ? Resample(Blend(first, shifted, secondQuarters), 0.75, 1.5, 12.25, 13.75, filter) :
                    Blend(Resample(first, 0.75, 1.5, 12.25, 13.75, filter), Resample(shifted, 0.75, 1.5, 12.25, 13.75, filter), secondQuarters);
                const auto observation = DecodeLocalDesktopBootstrap(image.View());
                INFO("erasure=" << GetLocalDesktopErasureName(observation.erasure) << " meanGray=" << observation.midGrayFraction <<
                     " sampleGray=" << observation.sampleMidGrayFraction << " timingResidual=" << observation.timingResidual <<
                     " copy0SampleGray=" << observation.copies[0].sampleMidGrayFraction << " copy1SampleGray=" << observation.copies[1].sampleMidGrayFraction);
                CHECK(observation.midGrayFraction <= LocalDesktopDecodePolicy{}.maximumMidGrayFraction);
                CHECK(observation.timingResidual <= LocalDesktopDecodePolicy{}.maximumTimingResidual);
                CHECK(observation.sampleMidGrayFraction > LocalDesktopDecodePolicy{}.maximumMidGrayFraction);
                CheckDecision(observation, secondQuarters == 1 ? firstRecord : secondRecord, false);
                CHECK(observation.workUnits <= LocalDesktopDecodePolicy{}.maximumWorkUnits);
            }
        }
    }
}
