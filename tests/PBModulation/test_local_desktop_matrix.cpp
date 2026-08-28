#include "local_desktop_resample_fixtures.h"
#include "local_desktop_test_fixtures.h"
#include "pbmodulation/local_desktop_decode.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

using namespace localdesktoptest;
using namespace pbmodulation;

namespace
{

using Erasure = LocalDesktopErasureReason;

struct MatrixTransform
{
    double scaleX;
    double scaleY;
    double originX;
    double originY;
};

constexpr std::array<FixtureFilter, 2> filters{FixtureFilter::Area, FixtureFilter::Bilinear};
constexpr MatrixTransform mixedTransform{0.75, 1.5, 12.25, 13.75};

const char* FilterName(const FixtureFilter filter) noexcept
{
    return filter == FixtureFilter::Area ? "Area" : "Bilinear";
}

GrayImage TransformImage(const GrayImage& source, const MatrixTransform& transform, const FixtureFilter filter)
{
    return Resample(source, transform.scaleX, transform.scaleY, transform.originX, transform.originY, filter);
}

void CheckAccepted(const LocalDesktopObservation& observation, const std::array<std::byte, 44>& expected, const MatrixTransform& transform)
{
    INFO("erasure=" << GetLocalDesktopErasureName(observation.erasure) << " markers=" << observation.markerCandidates <<
         " geometries=" << observation.geometryCandidates << " work=" << observation.workUnits <<
         " origin=" << observation.geometry.originX << ',' << observation.geometry.originY <<
         " scale=" << observation.geometry.scaleX << ',' << observation.geometry.scaleY <<
         " markerResidual=" << observation.geometry.markerResidualPixels << " timingResidual=" << observation.timingResidual <<
         " midGray=" << observation.midGrayFraction);
    CHECK(observation.IsAccepted());
    CHECK(observation.canonical44 == expected);
    for (const auto& copy : observation.copies)
    {
        CHECK(copy.canonical44 == expected);
        CHECK(copy.fecDecoded);
        CHECK(copy.crcValid);
        CHECK(copy.recordValid);
    }
    CHECK(observation.quality > 0);
    CHECK(observation.quality <= 1);
    CHECK(observation.workUnits <= LocalDesktopDecodePolicy{}.maximumWorkUnits);
    CHECK(observation.geometry.originX == Catch::Approx(transform.originX).margin(0.4));
    CHECK(observation.geometry.originY == Catch::Approx(transform.originY).margin(0.4));
    CHECK(observation.geometry.scaleX * 1920 == Catch::Approx(transform.scaleX * 1920).margin(0.4));
    CHECK(observation.geometry.scaleY * 1080 == Catch::Approx(transform.scaleY * 1080).margin(0.4));
    CHECK(observation.geometry.markerResidualPixels <= LocalDesktopDecodePolicy{}.maximumGeometryResidualPixels);
}

void CheckErased(const LocalDesktopObservation& observation, const Erasure reason)
{
    INFO("erasure=" << GetLocalDesktopErasureName(observation.erasure) << " expected=" << GetLocalDesktopErasureName(reason) <<
         " markers=" << observation.markerCandidates << " geometries=" << observation.geometryCandidates <<
         " work=" << observation.workUnits << " timingBits=" << observation.timingBitErrorFraction <<
         " timingResidual=" << observation.timingResidual << " midGray=" << observation.midGrayFraction);
    CHECK_FALSE(observation.IsAccepted());
    CHECK(observation.erasure == reason);
    CHECK(observation.canonical44 == std::array<std::byte, 44>{});
    CHECK(observation.quality == 0);
    CHECK(observation.workUnits <= LocalDesktopDecodePolicy{}.maximumWorkUnits);
}

void CheckIndependentCopies(const LocalDesktopObservation& observation, const std::array<std::byte, 44>& first,
                            const std::array<std::byte, 44>& second)
{
    CHECK(observation.copies[0].canonical44 == first);
    CHECK(observation.copies[1].canonical44 == second);
    for (const auto& copy : observation.copies)
    {
        CHECK(copy.fecDecoded);
        CHECK(copy.crcValid);
        CHECK(copy.recordValid);
    }
}

void CheckHighSequencePair(const std::array<std::byte, 44>& first, const std::array<std::byte, 44>& second)
{
    // The pinned pair differs in FrameSequence bit 48, not a low-bit timing toggle.
    REQUIRE(std::equal(first.begin(), first.begin() + 30, second.begin()));
    REQUIRE(first[30] == std::byte{0x12});
    REQUIRE(second[30] == std::byte{0x13});
    REQUIRE(std::equal(first.begin() + 31, first.begin() + 40, second.begin() + 31));
    REQUIRE_FALSE(std::equal(first.begin() + 40, first.end(), second.begin() + 40));
}

GrayImage MixQuadrants(const GrayImage& first, const GrayImage& second, const unsigned secondMask,
                       const double seamX, const double seamY)
{
    if (first.width != second.width || first.height != second.height || secondMask == 0 || secondMask >= 15)
    {
        throw std::runtime_error("Invalid independent quadrant mixture");
    }
    GrayImage result(first.width, first.height);
    for (std::uint32_t row = 0; row < result.height; row++)
    {
        for (std::uint32_t column = 0; column < result.width; column++)
        {
            const unsigned quadrant = (column + 0.5 >= seamX ? 1u : 0u) + (row + 0.5 >= seamY ? 2u : 0u);
            const auto& source = (secondMask & (1u << quadrant)) != 0 ? second : first;
            result.Write(column, row, source.Read(column, row));
        }
    }
    return result;
}

GrayImage ReplaceRectangle(const GrayImage& first, const GrayImage& second, const double left, const double top,
                           const double right, const double bottom)
{
    if (first.width != second.width || first.height != second.height || left < 0 || top < 0 ||
        right <= left || bottom <= top || right > first.width || bottom > first.height)
    {
        throw std::runtime_error("Invalid independent local replacement");
    }
    auto result = first;
    for (std::uint32_t row = 0; row < result.height; row++)
    {
        if (row + 0.5 < top || row + 0.5 >= bottom)
        {
            continue;
        }
        for (std::uint32_t column = 0; column < result.width; column++)
        {
            if (column + 0.5 >= left && column + 0.5 < right)
            {
                result.Write(column, row, second.Read(column, row));
            }
        }
    }
    return result;
}

GrayImage ShiftPixels(const GrayImage& source, const std::int32_t offsetX, const std::int32_t offsetY)
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

void CheckGhostErased(const LocalDesktopObservation& observation)
{
    INFO("erasure=" << GetLocalDesktopErasureName(observation.erasure) << " markers=" << observation.markerCandidates <<
         " geometries=" << observation.geometryCandidates << " work=" << observation.workUnits <<
         " timingResidual=" << observation.timingResidual << " midGray=" << observation.midGrayFraction);
    CHECK_FALSE(observation.IsAccepted());
    CHECK(observation.canonical44 == std::array<std::byte, 44>{});
    CHECK(observation.quality == 0);
    CHECK(observation.workUnits <= LocalDesktopDecodePolicy{}.maximumWorkUnits);
    // Shifted marker edges can prevent locating a frame before the soft-cell gate.
    // Resource exhaustion, invalid views/policies and sample errors are not mixed-frame detection.
    const bool imageRejected = observation.erasure == Erasure::MarkersNotFound || observation.erasure == Erasure::IncompleteMarkers ||
        observation.erasure == Erasure::InvalidGeometry || observation.erasure == Erasure::AmbiguousGeometry ||
        observation.erasure == Erasure::LowContrast || observation.erasure == Erasure::BootstrapFecFailure ||
        observation.erasure == Erasure::BootstrapCrcFailure || observation.erasure == Erasure::BootstrapMismatch ||
        observation.erasure == Erasure::TimingMismatch || observation.erasure == Erasure::DoubleImage || observation.erasure == Erasure::ExcessResidual;
    CHECK(imageRejected);
}

} // namespace

TEST_CASE("LocalDesktop matrix covers every independent axis scale with both filters", "[pbmodulation][localdesktop][matrix][geometry]")
{
    const auto source = GrayFromGolden(MakeGoldenRaster("a"));
    const auto expected = LoadGoldenRecord("a");
    constexpr std::array<double, 5> scales{0.5, 0.75, 1.0, 1.5, 2.0};
    unsigned observations = 0;
    for (const auto filter : filters)
    {
        for (const double scaleX : scales)
        {
            for (const double scaleY : scales)
            {
                const MatrixTransform transform{scaleX, scaleY, 12, 13};
                INFO("filter=" << FilterName(filter) << " scale=" << scaleX << ',' << scaleY << " origin=12,13");
                const auto image = TransformImage(source, transform, filter);
                CheckAccepted(DecodeLocalDesktopBootstrap(image.View()), expected, transform);
                observations++;
            }
        }
    }
    CHECK(observations == 50);
}

TEST_CASE("LocalDesktop extreme scales cover the full independent quarter-pixel phase matrix", "[pbmodulation][localdesktop][matrix][geometry]")
{
    const auto source = GrayFromGolden(MakeGoldenRaster("a"));
    const auto expected = LoadGoldenRecord("a");
    constexpr std::array<std::array<double, 2>, 4> scales{{{0.5, 0.5}, {0.5, 2.0}, {2.0, 0.5}, {2.0, 2.0}}};
    constexpr std::array<double, 4> phases{0, 0.25, 0.5, 0.75};
    unsigned observations = 0;
    for (const auto filter : filters)
    {
        for (const auto& scale : scales)
        {
            for (const double phaseX : phases)
            {
                for (const double phaseY : phases)
                {
                    // No leading integer margin: phase also exercises the top/left view boundary.
                    const MatrixTransform transform{scale[0], scale[1], phaseX, phaseY};
                    INFO("filter=" << FilterName(filter) << " scale=" << scale[0] << ',' << scale[1] << " origin=" << phaseX << ',' << phaseY);
                    const auto image = TransformImage(source, transform, filter);
                    CheckAccepted(DecodeLocalDesktopBootstrap(image.View()), expected, transform);
                    observations++;
                }
            }
        }
    }
    CHECK(observations == 128);
}

TEST_CASE("LocalDesktop high-sequence four-quadrant mosaics cannot assemble a codeword", "[pbmodulation][localdesktop][matrix][mixed]")
{
    const auto first = GrayFromGolden(MakeGoldenRaster("a"));
    const auto second = GrayFromGolden(MakeGoldenRaster("e"));
    const auto firstRecord = LoadGoldenRecord("a");
    const auto secondRecord = LoadGoldenRecord("e");
    CheckHighSequencePair(firstRecord, secondRecord);
    unsigned observations = 0;
    for (const auto filter : filters)
    {
        const auto firstScaled = TransformImage(first, mixedTransform, filter);
        const auto secondScaled = TransformImage(second, mixedTransform, filter);
        CheckAccepted(DecodeLocalDesktopBootstrap(firstScaled.View()), firstRecord, mixedTransform);
        CheckAccepted(DecodeLocalDesktopBootstrap(secondScaled.View()), secondRecord, mixedTransform);
        // Bit 0/1/2/3 selects e in TL/TR/BL/BR. All 14 nontrivial mosaics include
        // single foreign quadrants and both checkerboards, not just half-frame tears.
        for (unsigned secondMask = 1; secondMask < 15; secondMask++)
        {
            INFO("filter=" << FilterName(filter) << " mask=" << secondMask << " scale=.75,1.5 origin=12.25,13.75");
            const auto& expectedFirst = (secondMask & 1u) != 0 ? secondRecord : firstRecord;
            const auto& expectedSecond = (secondMask & 8u) != 0 ? secondRecord : firstRecord;
            const auto reason = expectedFirst != expectedSecond ? Erasure::BootstrapMismatch : Erasure::TimingMismatch;
            {
                INFO("order=quadrant-mix-then-scale");
                const auto mixed = MixQuadrants(first, second, secondMask, 960, 540);
                const auto image = TransformImage(mixed, mixedTransform, filter);
                const auto observation = DecodeLocalDesktopBootstrap(image.View());
                CheckErased(observation, reason);
                CheckIndependentCopies(observation, expectedFirst, expectedSecond);
                observations++;
            }
            {
                INFO("order=scale-then-quadrant-mix");
                const auto image = MixQuadrants(firstScaled, secondScaled, secondMask,
                    mixedTransform.originX + mixedTransform.scaleX * 960, mixedTransform.originY + mixedTransform.scaleY * 540);
                const auto observation = DecodeLocalDesktopBootstrap(image.View());
                CheckErased(observation, reason);
                CheckIndependentCopies(observation, expectedFirst, expectedSecond);
                observations++;
            }
        }
    }
    CHECK(observations == 56);
}

TEST_CASE("LocalDesktop high-sequence local timing substitutions erase after either resampling order", "[pbmodulation][localdesktop][matrix][mixed]")
{
    const auto first = GrayFromGolden(MakeGoldenRaster("a"));
    const auto second = GrayFromGolden(MakeGoldenRaster("e"));
    const auto firstRecord = LoadGoldenRecord("a");
    const auto secondRecord = LoadGoldenRecord("e");
    const auto firstTiming = LoadGoldenBytes("a-timing-bits.bin", 2304);
    const auto secondTiming = LoadGoldenBytes("e-timing-bits.bin", 2304);
    CheckHighSequencePair(firstRecord, secondRecord);
    unsigned observations = 0;
    for (const auto filter : filters)
    {
        const auto firstScaled = TransformImage(first, mixedTransform, filter);
        const auto secondScaled = TransformImage(second, mixedTransform, filter);
        CheckAccepted(DecodeLocalDesktopBootstrap(firstScaled.View()), firstRecord, mixedTransform);
        CheckAccepted(DecodeLocalDesktopBootstrap(secondScaled.View()), secondRecord, mixedTransform);
        for (unsigned quadrant = 0; quadrant < 4; quadrant++)
        {
            const std::uint32_t left = 896 + (quadrant % 2) * 64;
            const std::uint32_t top = 476 + (quadrant / 2) * 64;
            unsigned changedCells = 0;
            for (unsigned row = 0; row < 8; row++)
            {
                for (unsigned column = 0; column < 8; column++)
                {
                    const std::size_t index = 4 * 256 + ((quadrant / 2) * 8 + row) * 16 + (quadrant % 2) * 8 + column;
                    changedCells += firstTiming[index] != secondTiming[index] ? 1u : 0u;
                }
            }
            INFO("filter=" << FilterName(filter) << " timingPatch=4 quarter=" << quadrant << " changedCells=" << changedCells <<
                 " scale=.75,1.5 origin=12.25,13.75");
            REQUIRE(changedCells > 256 * LocalDesktopDecodePolicy{}.maximumTimingBitErrorFraction);
            {
                INFO("order=local-replacement-then-scale");
                auto mixed = first;
                CopyBlock(second, mixed, left, top, 64, 64, left, top);
                const auto image = TransformImage(mixed, mixedTransform, filter);
                const auto observation = DecodeLocalDesktopBootstrap(image.View());
                CheckErased(observation, Erasure::TimingMismatch);
                CheckIndependentCopies(observation, firstRecord, firstRecord);
                CHECK(observation.timingBitErrorFraction > LocalDesktopDecodePolicy{}.maximumTimingBitErrorFraction);
                observations++;
            }
            {
                INFO("order=scale-then-local-replacement");
                const auto image = ReplaceRectangle(firstScaled, secondScaled,
                    mixedTransform.originX + mixedTransform.scaleX * left, mixedTransform.originY + mixedTransform.scaleY * top,
                    mixedTransform.originX + mixedTransform.scaleX * (left + 64), mixedTransform.originY + mixedTransform.scaleY * (top + 64));
                const auto observation = DecodeLocalDesktopBootstrap(image.View());
                CheckErased(observation, Erasure::TimingMismatch);
                CheckIndependentCopies(observation, firstRecord, firstRecord);
                CHECK(observation.timingBitErrorFraction > LocalDesktopDecodePolicy{}.maximumTimingBitErrorFraction);
                observations++;
            }
        }
    }
    CHECK(observations == 16);
}

TEST_CASE("LocalDesktop shifted quarter-weight double images erase before and after filtering", "[pbmodulation][localdesktop][matrix][mixed]")
{
    const auto first = GrayFromGolden(MakeGoldenRaster("a"));
    const auto second = GrayFromGolden(MakeGoldenRaster("e"));
    const auto firstRecord = LoadGoldenRecord("a");
    const auto secondRecord = LoadGoldenRecord("e");
    CheckHighSequencePair(firstRecord, secondRecord);
    constexpr std::array<std::array<std::int32_t, 2>, 4> offsets{{{4, 0}, {0, 4}, {4, -4}, {-4, 4}}};
    unsigned observations = 0;
    for (const auto filter : filters)
    {
        const auto firstScaled = TransformImage(first, mixedTransform, filter);
        CheckAccepted(DecodeLocalDesktopBootstrap(firstScaled.View()), firstRecord, mixedTransform);
        for (const auto& offset : offsets)
        {
            // The e image is shifted on the logical canvas before either operation.
            // These +/-4 logical-pixel offsets become +/-3 X or +/-6 Y capture pixels;
            // no metadata, bilinear phase change, or unshifted blend substitutes for a ghost.
            const auto shifted = ShiftPixels(second, offset[0], offset[1]);
            const auto shiftedScaled = TransformImage(shifted, mixedTransform, filter);
            const MatrixTransform shiftedTransform{mixedTransform.scaleX, mixedTransform.scaleY,
                mixedTransform.originX + mixedTransform.scaleX * offset[0], mixedTransform.originY + mixedTransform.scaleY * offset[1]};
            CheckAccepted(DecodeLocalDesktopBootstrap(shiftedScaled.View()), secondRecord, shiftedTransform);
            for (unsigned secondQuarters = 1; secondQuarters <= 3; secondQuarters++)
            {
                INFO("filter=" << FilterName(filter) << " logicalGhostOffset=" << offset[0] << ',' << offset[1] <<
                     " captureGhostOffset=" << mixedTransform.scaleX * offset[0] << ',' << mixedTransform.scaleY * offset[1] <<
                     " secondPercent=" << secondQuarters * 25 << " scale=.75,1.5 origin=12.25,13.75");
                {
                    INFO("order=blend-then-scale");
                    const auto mixed = Blend(first, shifted, secondQuarters);
                    const auto image = TransformImage(mixed, mixedTransform, filter);
                    CheckGhostErased(DecodeLocalDesktopBootstrap(image.View()));
                    observations++;
                }
                {
                    INFO("order=scale-then-blend");
                    const auto image = Blend(firstScaled, shiftedScaled, secondQuarters);
                    CheckGhostErased(DecodeLocalDesktopBootstrap(image.View()));
                    observations++;
                }
            }
        }
    }
    CHECK(observations == 48);
}
