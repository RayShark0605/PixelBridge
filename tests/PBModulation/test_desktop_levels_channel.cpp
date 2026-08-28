#include "desktop_levels_test_support.h"
#include "local_desktop_resample_fixtures.h"
#include "../../libs/PBModulation/src/desktop_levels_internal.h"
#include "pbinterleave/tile_permutation.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <future>
#include <limits>

using namespace pbmodulation;
using namespace localdesktoptest;

namespace
{
struct Receiver
{
    Receiver() : hard(kDesktopLevelsMaximumDataBytes, std::byte{0xA7}), soft(kDesktopLevelsMaximumBits, 123.25f)
    {
        auto created = DesktopLevelsWorkspace::Create(DesktopLevelsWorkspace::RequiredBytes());
        REQUIRE(created);
        workspace = std::move(created).Value();
    }
    DesktopLevelsObservation Decode(const LumaView& view, const DesktopLevelsDecodePolicy& policy = {})
    {
        return DecodeDesktopLevelsFrame(view, workspace, hard, soft, policy);
    }
    void CheckUnchanged() const
    {
        REQUIRE(std::ranges::all_of(hard, [](const auto value) { return value == std::byte{0xA7}; }));
        REQUIRE(std::ranges::all_of(soft, [](const auto value) { return value == 123.25f; }));
        REQUIRE(workspace.GetMarginHistogram().empty());
    }
    DesktopLevelsWorkspace workspace;
    std::vector<std::byte> hard;
    std::vector<float> soft;
};
} // namespace

TEST_CASE("DesktopLevels phase pilots and ladder gap enforce inclusive strict thresholds", "[desktop-levels][pilot][boundary]")
{
    for (const unsigned axis : {0u, 1u})
    {
        auto pixels = desktoptest::Raster(4);
        for (unsigned row = 0; row < 64; row++)
        {
            for (unsigned column = 0; column < 64; column++)
            {
                const unsigned stripe = axis == 0 ? column : row;
                desktoptest::Paint(pixels, 896 + axis * 64 + column, 16 + row, 1, 1, stripe % 2 == 0 ? 56 : 200);
            }
        }
        Receiver receiver;
        const auto accepted = receiver.Decode(desktoptest::View(pixels));
        REQUIRE(accepted.IsAccepted());
        REQUIRE(accepted.calibration.phaseResidual == 0.125);
        DesktopLevelsDecodePolicy tightened;
        tightened.maximumPhasePilotResidual = std::nextafter(0.125, 0.0);
        Receiver rejected;
        REQUIRE(rejected.Decode(desktoptest::View(pixels), tightened).erasure == DesktopLevelsErasure::PhasePilotMismatch);
        rejected.CheckUnchanged();
        desktoptest::Paint(pixels, 896 + axis * 64, 16, 1, 1, 57);
        REQUIRE(rejected.Decode(desktoptest::View(pixels)).erasure == DesktopLevelsErasure::PhasePilotMismatch);
        rejected.CheckUnchanged();
    }
    auto pixels = desktoptest::Raster(4);
    for (const auto position : std::array<std::array<unsigned, 2>, 4>{{{736, 16}, {1696, 16}, {96, 1000}, {1056, 1000}}})
    {
        desktoptest::Paint(pixels, position[0] + 32, position[1], 32, 64, 64);
    }
    Receiver receiver;
    const auto accepted = receiver.Decode(desktoptest::View(pixels));
    REQUIRE(accepted.IsAccepted());
    REQUIRE(accepted.calibration.minimumGap == 32);
    DesktopLevelsDecodePolicy tightened;
    tightened.minimumLevelGap = std::nextafter(32.0, 255.0);
    Receiver rejected;
    REQUIRE(rejected.Decode(desktoptest::View(pixels), tightened).erasure == DesktopLevelsErasure::PilotOrder);
    rejected.CheckUnchanged();
    desktoptest::Paint(pixels, 736 + 32, 16, 1, 1, 63);
    REQUIRE(rejected.Decode(desktoptest::View(pixels)).erasure == DesktopLevelsErasure::PilotOrder);
    rejected.CheckUnchanged();
}

TEST_CASE("DesktopLevels locator accepts scale but data gate rejects both independent axes and quarter phase", "[desktop-levels][geometry]")
{
    for (const unsigned tile : {2u, 4u})
    {
        const auto source = GrayFromGolden(desktoptest::Raster(tile));
        Receiver receiver;
        for (const double scale : {0.5, 0.75, 1.25, 2.0})
        {
            for (const bool horizontal : {false, true})
            {
                for (const auto filter : {FixtureFilter::Area, FixtureFilter::Bilinear})
                {
                    const auto image = Resample(source, horizontal ? scale : 1, horizontal ? 1 : scale, 12, 13, filter);
                    const auto result = receiver.Decode(image.View());
                    INFO("tile=" << tile << " scale=" << scale << " horizontal=" << horizontal << " erasure=" << GetDesktopLevelsErasureName(result.erasure)
                         << " bootstrap=" << GetLocalDesktopErasureName(result.bootstrap.erasure));
                    REQUIRE(result.bootstrap.IsAccepted());
                    REQUIRE(result.erasure == DesktopLevelsErasure::ScaleOutOfRange);
                    REQUIRE(result.dataWorkUnits == 0);
                    receiver.CheckUnchanged();
                }
            }
        }
        for (const bool horizontal : {false, true})
        {
            const auto shifted = Resample(source, 1, 1, horizontal ? 12.25 : 12, horizontal ? 13 : 13.25, FixtureFilter::Area);
            const auto result = receiver.Decode(shifted.View());
            INFO("quarter phase erasure=" << GetDesktopLevelsErasureName(result.erasure) << " bootstrap=" << GetLocalDesktopErasureName(result.bootstrap.erasure));
            REQUIRE(result.bootstrap.IsAccepted());
            REQUIRE_FALSE(result.IsAccepted());
            REQUIRE((result.erasure == DesktopLevelsErasure::AlignmentOutOfRange || result.erasure == DesktopLevelsErasure::PhasePilotMismatch ||
                     result.erasure == DesktopLevelsErasure::ScaleOutOfRange));
            receiver.CheckUnchanged();
        }
    }
}

TEST_CASE("DesktopLevels integer translation edge ROI padded rows and explicit SDR formats preserve data", "[desktop-levels][view]")
{
    for (const unsigned tile : {2u, 4u})
    {
        const auto expected = desktoptest::Data(tile);
        const auto source = GrayFromGolden(desktoptest::Raster(tile));
        for (const unsigned origin : {0u, 13u})
        {
            const auto image = Resample(source, 1, 1, origin, origin, FixtureFilter::Area, 0);
            Receiver receiver;
            REQUIRE(receiver.Decode(image.View()).IsAccepted());
            REQUIRE(std::equal(expected.begin(), expected.end(), receiver.hard.begin()));
            for (const auto format : {LumaPixelFormat::Bgra8, LumaPixelFormat::R10G10B10A2, LumaPixelFormat::Fp16LinearSdr})
            {
                const auto converted = ConvertFormat(image, format);
                const auto result = receiver.Decode(converted.View());
                INFO("format=" << static_cast<int>(format) << " erasure=" << GetDesktopLevelsErasureName(result.erasure));
                REQUIRE(result.IsAccepted());
                REQUIRE(std::equal(expected.begin(), expected.end(), receiver.hard.begin()));
            }
        }
    }
}

TEST_CASE("DesktopLevels independent calibration rejects collapse inversion noise clipping and spatial drift", "[desktop-levels][calibration]")
{
    auto pixels = desktoptest::Raster(4);
    Receiver receiver;
    SECTION("nonlinear monotonic channel learns pilots without payload fitting")
    {
        constexpr std::array<unsigned, 4> levels{40, 100, 150, 212};
        for (std::size_t offset = 0; offset < pixels.size(); offset += 4)
        {
            for (std::size_t index = 0; index < 4; index++)
            {
                if (pixels[offset] == static_cast<std::byte>(kDesktopLevelsLuma[index]))
                {
                    pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = static_cast<std::byte>(levels[index]);
                    break;
                }
            }
        }
        const auto result = receiver.Decode(desktoptest::View(pixels));
        REQUIRE(result.IsAccepted());
        REQUIRE(result.calibration.centroids == std::array<double, 4>{40, 100, 150, 212});
        REQUIRE(result.calibration.minimumGap == 50);
        for (std::size_t ladder = 0; ladder < 4; ladder++)
        {
            REQUIRE(result.calibration.ladderCentroids[ladder] == std::array<double, 4>{40, 100, 150, 212});
            REQUIRE(result.calibration.ladderVariances[ladder] == std::array<double, 4>{0, 0, 0, 0});
        }
        const auto expected = desktoptest::Data(4);
        REQUIRE(std::equal(expected.begin(), expected.end(), receiver.hard.begin()));
        return;
    }
    DesktopLevelsErasure expected = DesktopLevelsErasure::PilotOrder;
    SECTION("collapsed adjacent level")
    {
        desktoptest::Paint(pixels, 768, 16, 32, 64, 32);
    }
    SECTION("inverted adjacent level")
    {
        desktoptest::Paint(pixels, 768, 16, 32, 64, 16);
    }
    SECTION("clipped ladder sample")
    {
        desktoptest::Paint(pixels, 736, 16, 1, 1, 0);
        expected = DesktopLevelsErasure::PilotClipping;
    }
    SECTION("clipped phase sample")
    {
        desktoptest::Paint(pixels, 896, 16, 1, 1, 0);
        expected = DesktopLevelsErasure::PilotClipping;
    }
    SECTION("single clipped RGB channel is not hidden by Luma averaging")
    {
        pixels[(16 * 1920 + 736) * 4] = std::byte{255};
        expected = DesktopLevelsErasure::PilotClipping;
    }
    SECTION("pilot noise just above stddev eight")
    {
        for (unsigned column = 0; column < 32; column++)
        {
            desktoptest::Paint(pixels, 736 + column, 16, 1, 64, column % 2 == 0 ? 23 : 41);
        }
        expected = DesktopLevelsErasure::PilotVariance;
    }
    SECTION("cross-region full centroid range exceeds eight")
    {
        desktoptest::Paint(pixels, 736, 16, 32, 64, 41);
        expected = DesktopLevelsErasure::PilotSpatialMismatch;
    }
    SECTION("phase mismatch along one axis")
    {
        desktoptest::Paint(pixels, 960, 16, 64, 64, 128);
        expected = DesktopLevelsErasure::PhasePilotMismatch;
    }
    const auto result = receiver.Decode(desktoptest::View(pixels));
    INFO(GetDesktopLevelsErasureName(result.erasure));
    REQUIRE(result.bootstrap.IsAccepted());
    REQUIRE(result.erasure == expected);
    receiver.CheckUnchanged();
}

TEST_CASE("DesktopLevels pilot thresholds and per-tile low confidence are not hidden by averaging", "[desktop-levels][metrics][boundary]")
{
    auto pixels = desktoptest::Raster(4);
    Receiver receiver;
    for (unsigned column = 0; column < 32; column++)
    {
        desktoptest::Paint(pixels, 736 + column, 16, 1, 64, column % 2 == 0 ? 24 : 40);
    }
    desktoptest::Paint(pixels, 1696, 16, 32, 64, 40);
    auto result = receiver.Decode(desktoptest::View(pixels));
    REQUIRE(result.IsAccepted());
    REQUIRE(result.calibration.spatialDeviation == 8);
    DesktopLevelsDecodePolicy stricter;
    stricter.maximumPilotSpatialDeviation = std::nextafter(8.0, 0.0);
    REQUIRE(receiver.Decode(desktoptest::View(pixels), stricter).erasure == DesktopLevelsErasure::PilotSpatialMismatch);
    stricter = {};
    stricter.maximumPilotStandardDeviation = std::nextafter(8.0, 0.0);
    REQUIRE(receiver.Decode(desktoptest::View(pixels), stricter).erasure == DesktopLevelsErasure::PilotVariance);

    pixels = desktoptest::Raster(4);
    desktoptest::Paint(pixels, 96, 96, 2, 4, 48);
    desktoptest::Paint(pixels, 98, 96, 2, 4, 80); // mean64, stddev16: allowed threshold
    result = receiver.Decode(desktoptest::View(pixels));
    REQUIRE(result.IsAccepted());
    REQUIRE(result.unreliableTiles == 0);
    REQUIRE(receiver.soft[0] == 0); // exact midpoint: ambiguous bit0
    REQUIRE(receiver.soft[1] > 0);
    REQUIRE(result.margin.minimum == 0);
    desktoptest::Paint(pixels, 96, 96, 2, 4, 47);
    desktoptest::Paint(pixels, 98, 96, 2, 4, 81);
    result = receiver.Decode(desktoptest::View(pixels));
    REQUIRE(result.IsAccepted());
    REQUIRE(result.unreliableTiles == 1);
    REQUIRE(receiver.soft[0] == 0);
    REQUIRE(receiver.soft[1] == 0);
    desktoptest::Paint(pixels, 96, 96, 4, 4, 255);
    result = receiver.Decode(desktoptest::View(pixels));
    REQUIRE(result.IsAccepted());
    REQUIRE(result.unreliableTiles == 1);
    REQUIRE(receiver.soft[0] == 0);
    REQUIRE(receiver.soft[1] == 0);
    pixels = desktoptest::Raster(4);
    pixels[(96 * 1920 + 96) * 4] = std::byte{255};
    result = receiver.Decode(desktoptest::View(pixels));
    REQUIRE(result.IsAccepted());
    REQUIRE(result.unreliableTiles == 1);
    REQUIRE(receiver.soft[0] == 0);
    REQUIRE(receiver.soft[1] == 0);
}

TEST_CASE("DesktopLevels malformed views budgets aliases and late pixel failures preserve complete outputs", "[desktop-levels][bounds]")
{
    auto pixels = desktoptest::Raster(4);
    Receiver receiver;
    SECTION("stride overflow")
    {
        auto view = desktoptest::View(pixels);
        view.rowPitch = std::numeric_limits<std::size_t>::max();
        REQUIRE(receiver.Decode(view).erasure == DesktopLevelsErasure::InvalidInput);
    }
    SECTION("short input")
    {
        auto view = desktoptest::View(pixels);
        view.pixels = view.pixels.first(view.pixels.size() - 1);
        REQUIRE(receiver.Decode(view).erasure == DesktopLevelsErasure::InvalidInput);
    }
    SECTION("zero work budget is not an unlimited policy")
    {
        DesktopLevelsDecodePolicy policy;
        policy.maximumDataWorkUnits = 0;
        REQUIRE(receiver.Decode(desktoptest::View(pixels), policy).erasure == DesktopLevelsErasure::InvalidPolicy);
    }
    SECTION("budget exhaustion late in tile scan rolls back both outputs")
    {
        DesktopLevelsDecodePolicy policy;
        policy.maximumDataWorkUnits = 500000;
        REQUIRE(receiver.Decode(desktoptest::View(pixels), policy).erasure == DesktopLevelsErasure::WorkBudgetExceeded);
    }
    SECTION("output capacity checked before data scan")
    {
        const auto result = DecodeDesktopLevelsFrame(desktoptest::View(pixels), receiver.workspace, std::span(receiver.hard).first(21671), receiver.soft);
        REQUIRE(result.erasure == DesktopLevelsErasure::OutputBufferTooSmall);
        REQUIRE(result.dataWorkUnits == 0);
    }
    SECTION("input-output alias fails before any sample changes")
    {
        const auto before = pixels;
        const auto result = DecodeDesktopLevelsFrame(desktoptest::View(pixels), receiver.workspace, std::span(pixels).first(21672), receiver.soft);
        REQUIRE(result.erasure == DesktopLevelsErasure::OverlappingSpans);
        REQUIRE(pixels == before);
    }
    SECTION("hard-soft alias is rejected")
    {
        const auto result = DecodeDesktopLevelsFrame(desktoptest::View(pixels), receiver.workspace, std::as_writable_bytes(std::span(receiver.soft)).first(21672), receiver.soft);
        REQUIRE(result.erasure == DesktopLevelsErasure::OverlappingSpans);
    }
    SECTION("FP16 NaN inside data cannot leave a partial accepted prefix")
    {
        auto converted = ConvertFormat(GrayFromGolden(pixels), LumaPixelFormat::Fp16LinearSdr);
        const auto offset = 97 * converted.pitch + 100 * 8;
        converted.pixels[offset] = std::byte{0};
        converted.pixels[offset + 1] = std::byte{0x7E};
        const auto result = receiver.Decode(converted.View());
        REQUIRE(result.bootstrap.IsAccepted());
        REQUIRE(result.erasure == DesktopLevelsErasure::PixelReadFailure);
        REQUIRE(result.pixelError == LocalDesktopErasureReason::NonFinitePixel);
    }
    receiver.CheckUnchanged();
}

TEST_CASE("DesktopLevels mixed Bootstrap timing and local occlusion cannot bypass framing", "[desktop-levels][mixed]")
{
    auto first = GrayFromGolden(desktoptest::Raster(4, 0));
    const auto second = GrayFromGolden(desktoptest::Raster(4, 1));
    Receiver receiver;
    SECTION("different independent A B records")
    {
        CopyBlock(second, first, 1216, 1000, 608, 64, 1216, 1000);
    }
    SECTION("distributed timing from another sequence")
    {
        CopyBlock(second, first, 896, 476, 128, 128, 896, 476);
    }
    SECTION("localized marker occlusion")
    {
        PaintBlock(first, 16, 16, 64, 64, 128);
    }
    REQUIRE_FALSE(receiver.Decode(first.View()).IsAccepted());
    receiver.CheckUnchanged();
}

TEST_CASE("DesktopLevels strict geometry includes adjacent representable threshold values", "[desktop-levels][boundary]")
{
    for (const bool horizontal : {false, true})
    {
        const double extent = horizontal ? 1920 : 1080;
        for (const double direction : {-1.0, 1.0})
        {
            double allowed = 1 + direction * 0.125 / extent;
            while (std::abs(allowed - 1) * extent > 0.125)
            {
                allowed = std::nextafter(allowed, 1.0);
            }
            const double forbidden = std::nextafter(allowed, direction < 0 ? 0.0 : 2.0);
            LocalDesktopGeometry geometry{0, 0, 1, 1, 0};
            (horizontal ? geometry.scaleX : geometry.scaleY) = allowed;
            REQUIRE(ValidateDesktopLevelsGeometry(geometry) == DesktopLevelsErasure::None);
            (horizontal ? geometry.scaleX : geometry.scaleY) = forbidden;
            REQUIRE(ValidateDesktopLevelsGeometry(geometry) == DesktopLevelsErasure::ScaleOutOfRange);
        }
        for (const double direction : {-1.0, 1.0})
        {
            LocalDesktopGeometry geometry{12, 13, 1, 1, 0};
            auto& origin = horizontal ? geometry.originX : geometry.originY;
            origin += direction * 0.125;
            REQUIRE(ValidateDesktopLevelsGeometry(geometry) == DesktopLevelsErasure::None);
            origin = std::nextafter(origin, direction < 0 ? 0.0 : 32.0);
            REQUIRE(ValidateDesktopLevelsGeometry(geometry) == DesktopLevelsErasure::AlignmentOutOfRange);
        }
    }
    LocalDesktopGeometry geometry{0.125, -0.125, 1, 1, 0.125};
    REQUIRE(ValidateDesktopLevelsGeometry(geometry) == DesktopLevelsErasure::None);
    geometry.markerResidualPixels = std::nextafter(0.125, 1.0);
    REQUIRE(ValidateDesktopLevelsGeometry(geometry) == DesktopLevelsErasure::AlignmentOutOfRange);
    REQUIRE_FALSE(pbinterleave::DesktopLevelsPermutation{4, 0, 432}.IsValid());
    REQUIRE(pbinterleave::DesktopLevelsPermutation{4, 0, 432}.ToLogical(0, 0) == 0);
}

TEST_CASE("DesktopLevels independent workspace instances decode concurrently", "[desktop-levels][thread]")
{
    const auto Run = [](const unsigned tile)
    {
        const auto pixels = desktoptest::Raster(tile);
        const auto expected = desktoptest::Data(tile);
        auto created = DesktopLevelsWorkspace::Create(DesktopLevelsWorkspace::RequiredBytes());
        if (!created)
        {
            return false;
        }
        auto workspace = std::move(created).Value();
        std::vector<std::byte> hard(expected.size());
        std::vector<float> soft(expected.size() * 8);
        return DecodeDesktopLevelsFrame(desktoptest::View(pixels), workspace, hard, soft).IsAccepted() && hard == expected;
    };
    auto first = std::async(std::launch::async, Run, 2);
    auto second = std::async(std::launch::async, Run, 4);
    REQUIRE(first.get());
    REQUIRE(second.get());
}
