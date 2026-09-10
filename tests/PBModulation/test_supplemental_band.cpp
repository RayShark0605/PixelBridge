#include "pbmodulation/supplemental_band.h"

#include "pbprotocol/control_plane_receiver.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace
{

using pbmodulation::kSupplementalBands;
using pbmodulation::kSupplementalBandMaximumRecordBytes;
using pbmodulation::kSupplementalBandPatchBytes;

constexpr std::size_t kCanvasBytes = 1920 * 1080 * 4;
constexpr std::size_t kScaledCanvasBytes = 2560 * 1440 * 4;

std::vector<std::byte> MakeControlRecord(const pbprotocol::SessionTag sessionTag, const std::uint64_t sequence)
{
    const std::array<std::byte, 9> payload{std::byte{1}, std::byte{3}, std::byte{5}, std::byte{7},
        std::byte{9}, std::byte{11}, std::byte{13}, std::byte{15}, std::byte{17}};
    const pbprotocol::ControlRecordView record{pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor, sequence, sessionTag, payload};
    const auto size = pbprotocol::GetSerializedSize(record);
    REQUIRE(size);
    std::vector<std::byte> bytes(size.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    return bytes;
}

pbmodulation::LumaView CanvasView(const std::span<const std::byte> pixels, const std::uint32_t width, const std::uint32_t height)
{
    return {pixels, width, height, static_cast<std::size_t>(width) * 4, pbmodulation::LumaPixelFormat::Bgra8};
}

std::vector<std::byte> MatteCanvas(const std::size_t bytes)
{
    std::vector<std::byte> canvas(bytes, std::byte{128});
    for (std::size_t offset = 3; offset < canvas.size(); offset += 4)
    {
        canvas[offset] = std::byte{255};
    }
    return canvas;
}

void ExtractBandDirect(const std::span<const std::byte> canvas, const std::size_t bandIndex, const std::span<std::byte> patch)
{
    const auto& region = kSupplementalBands[bandIndex];
    for (std::size_t row = 0; row < region.height; row++)
    {
        std::copy_n(canvas.begin() + ((region.y + row) * 1920 + region.x) * 4, region.width * 4,
            patch.begin() + row * 608 * 4);
    }
}

// Deterministic half-pixel-center bilinear upscale by exactly 4/3 (the verified
// display transform from the frozen 4/3 experiment).
std::vector<std::byte> ScaleFourThirds(const std::span<const std::byte> source)
{
    REQUIRE(source.size() == kCanvasBytes);
    std::vector<std::byte> output(kScaledCanvasBytes);
    const auto Read = [&](const std::uint32_t column, const std::uint32_t row, const std::size_t channel)
    {
        return std::to_integer<std::uint32_t>(source[(static_cast<std::size_t>(row) * 1920 + column) * 4 + channel]);
    };
    for (std::uint32_t row = 0; row < 1440; row++)
    {
        const auto positionY = std::clamp<std::int32_t>(static_cast<std::int32_t>(6 * row) - 1, 0, 8 * (1080 - 1));
        const auto top = static_cast<std::uint32_t>(positionY / 8);
        const auto bottom = std::min(top + 1, 1079U);
        const auto weightY = static_cast<std::uint32_t>(positionY % 8);
        for (std::uint32_t column = 0; column < 2560; column++)
        {
            const auto positionX = std::clamp<std::int32_t>(static_cast<std::int32_t>(6 * column) - 1, 0, 8 * (1920 - 1));
            const auto left = static_cast<std::uint32_t>(positionX / 8);
            const auto right = std::min(left + 1, 1919U);
            const auto weightX = static_cast<std::uint32_t>(positionX % 8);
            for (std::size_t channel = 0; channel < 4; channel++)
            {
                const std::uint32_t weighted = Read(left, top, channel) * (8 - weightX) * (8 - weightY) +
                    Read(right, top, channel) * weightX * (8 - weightY) +
                    Read(left, bottom, channel) * (8 - weightX) * weightY +
                    Read(right, bottom, channel) * weightX * weightY;
                output[(static_cast<std::size_t>(row) * 2560 + column) * 4 + channel] =
                    static_cast<std::byte>((weighted + 32) / 64);
            }
        }
    }
    return output;
}

// Flips the rendered level of the cell carrying `bit` (224 <-> 32).
void FlipRenderedCell(const std::span<std::byte> patch, const std::size_t bit)
{
    const std::size_t cellX = 8 + (bit % 148) * 4;
    const std::size_t cellY = 4 + (bit / 148) * 4;
    const auto current = std::to_integer<unsigned>(patch[(cellY * 608 + cellX) * 4]);
    const std::byte level = current >= 128 ? std::byte{32} : std::byte{224};
    for (std::size_t row = 0; row < 4; row++)
    {
        for (std::size_t column = 0; column < 4; column++)
        {
            const std::size_t pixel = ((cellY + row) * 608 + cellX + column) * 4;
            patch[pixel] = patch[pixel + 1] = patch[pixel + 2] = level;
        }
    }
}

struct BandFixture
{
    pbprotocol::SessionTag tag{0x0706050403020100ULL};
    std::uint64_t frameSequence = 42;
    std::vector<std::byte> record = MakeControlRecord(pbprotocol::SessionTag{0x0706050403020100ULL}, 77);
    std::vector<std::byte> patch = std::vector<std::byte>(kSupplementalBandPatchBytes);
    std::vector<std::byte> scratch = std::vector<std::byte>(kSupplementalBandPatchBytes);
    std::array<std::byte, kSupplementalBandMaximumRecordBytes> output{};

    BandFixture()
    {
        REQUIRE(pbmodulation::RenderSupplementalBand(record, tag, frameSequence, patch));
    }

    [[nodiscard]] pbmodulation::SupplementalBandDecodeOutcome DecodeFrom(const pbmodulation::LumaView& view,
        const pbmodulation::LocalDesktopGeometry& geometry, const std::size_t bandIndex,
        const pbprotocol::SessionTag expectedTag = pbprotocol::SessionTag{0x0706050403020100ULL},
        const std::uint64_t expectedFrame = 42)
    {
        return pbmodulation::DecodeSupplementalBand(view, geometry, bandIndex, expectedTag, expectedFrame, scratch, output);
    }
};

} // namespace

TEST_CASE("Supplemental band regions stay clear of the unified main map",
    "[pbmodulation][supplemental-band][regions]")
{
    CHECK(pbmodulation::ValidateSupplementalBandRegions());
    CHECK(kSupplementalBands.size() == 2);
    CHECK(kSupplementalBands[0].x + kSupplementalBands[0].width <= 1920);
    CHECK(kSupplementalBands[1].y + kSupplementalBands[1].height <= 1080);
}

TEST_CASE("Supplemental band render and decode round-trip at exact 1:1",
    "[pbmodulation][supplemental-band][roundtrip]")
{
    BandFixture fixture;
    std::vector<std::byte> canvas = MatteCanvas(kCanvasBytes);
    REQUIRE(pbmodulation::BlitSupplementalBand(fixture.patch, canvas, 0));
    REQUIRE(pbmodulation::BlitSupplementalBand(fixture.patch, canvas, 1));
    const pbmodulation::LocalDesktopGeometry geometry{0, 0, 1, 1, 0};
    for (const std::size_t band : {0U, 1U})
    {
        const auto outcome = fixture.DecodeFrom(CanvasView(canvas, 1920, 1080), geometry, band);
        CHECK(outcome.IsAdmitted());
        CHECK(outcome.recordBytes == fixture.record.size());
        CHECK(outcome.correctedSymbols == 0);
        CHECK(std::equal(fixture.record.begin(), fixture.record.end(), fixture.output.begin()));
    }
}

TEST_CASE("Geometry sampling at 1:1 equals a fixed row-copy extraction byte-for-byte",
    "[pbmodulation][supplemental-band][geometry][identity]")
{
    BandFixture fixture;
    std::vector<std::byte> canvas = MatteCanvas(kCanvasBytes);
    REQUIRE(pbmodulation::BlitSupplementalBand(fixture.patch, canvas, 0));
    REQUIRE(pbmodulation::BlitSupplementalBand(fixture.patch, canvas, 1));
    std::vector<std::byte> sampled(kSupplementalBandPatchBytes);
    std::vector<std::byte> extracted(kSupplementalBandPatchBytes);
    const pbmodulation::LocalDesktopGeometry geometry{0, 0, 1, 1, 0};
    for (const std::size_t band : {0U, 1U})
    {
        REQUIRE(pbmodulation::SampleSupplementalBand(CanvasView(canvas, 1920, 1080), geometry, band, sampled));
        ExtractBandDirect(canvas, band, extracted);
        CHECK(std::equal(sampled.begin(), sampled.end(), extracted.begin()));
    }
}

TEST_CASE("Supplemental band decodes through a 4/3 display transform",
    "[pbmodulation][supplemental-band][geometry][scaled]")
{
    BandFixture fixture;
    std::vector<std::byte> canvas = MatteCanvas(kCanvasBytes);
    REQUIRE(pbmodulation::BlitSupplementalBand(fixture.patch, canvas, 0));
    REQUIRE(pbmodulation::BlitSupplementalBand(fixture.patch, canvas, 1));
    const std::vector<std::byte> scaled = ScaleFourThirds(canvas);
    const pbmodulation::LocalDesktopGeometry geometry{0, 0, 4.0 / 3.0, 4.0 / 3.0, 0};
    for (const std::size_t band : {0U, 1U})
    {
        const auto outcome = fixture.DecodeFrom(CanvasView(scaled, 2560, 1440), geometry, band);
        CHECK(outcome.IsAdmitted());
        CHECK(std::equal(fixture.record.begin(), fixture.record.end(), fixture.output.begin()));
    }
}

TEST_CASE("Supplemental band rejects ambiguous center luma without RS correction",
    "[pbmodulation][supplemental-band][negative][ambiguity]")
{
    BandFixture fixture;
    // A matte (128) patch is ambiguous at the very first cell.
    std::vector<std::byte> matte = MatteCanvas(kSupplementalBandPatchBytes);
    std::vector<std::byte> canvas = MatteCanvas(kCanvasBytes);
    REQUIRE(pbmodulation::BlitSupplementalBand(matte, canvas, 0));
    const pbmodulation::LocalDesktopGeometry geometry{0, 0, 1, 1, 0};
    const auto outcome = fixture.DecodeFrom(CanvasView(canvas, 1920, 1080), geometry, 0);
    CHECK(outcome.status == pbmodulation::SupplementalBandDecodeStatus::AmbiguousCell);

    // One mid-gray cell in a rendered band is equally fatal.
    std::vector<std::byte> canvasTwo = MatteCanvas(kCanvasBytes);
    REQUIRE(pbmodulation::BlitSupplementalBand(fixture.patch, canvasTwo, 0));
    const auto& region = kSupplementalBands[0];
    for (std::size_t row = 0; row < 4; row++)
    {
        for (std::size_t column = 0; column < 4; column++)
        {
            const std::size_t pixel = ((region.y + 4 + row) * 1920 + region.x + 8 + column) * 4;
            canvasTwo[pixel] = canvasTwo[pixel + 1] = canvasTwo[pixel + 2] = std::byte{128};
        }
    }
    const auto outcomeTwo = fixture.DecodeFrom(CanvasView(canvasTwo, 1920, 1080), geometry, 0);
    CHECK(outcomeTwo.status == pbmodulation::SupplementalBandDecodeStatus::AmbiguousCell);
}

TEST_CASE("Supplemental band RS corrects up to 16 symbol errors and fails beyond",
    "[pbmodulation][supplemental-band][rs]")
{
    const pbmodulation::LocalDesktopGeometry geometry{0, 0, 1, 1, 0};
    SECTION("16 single-bit errors across distinct bytes correct cleanly")
    {
        BandFixture fixture;
        for (std::size_t byteIndex = 0; byteIndex < 16; byteIndex++)
        {
            FlipRenderedCell(fixture.patch, byteIndex * 8);
        }
        std::vector<std::byte> canvas = MatteCanvas(kCanvasBytes);
        REQUIRE(pbmodulation::BlitSupplementalBand(fixture.patch, canvas, 0));
        const auto outcome = fixture.DecodeFrom(CanvasView(canvas, 1920, 1080), geometry, 0);
        CHECK(outcome.IsAdmitted());
        CHECK(outcome.correctedSymbols == 16);
        CHECK(std::equal(fixture.record.begin(), fixture.record.end(), fixture.output.begin()));
    }
    SECTION("17 symbol errors exceed the correction budget")
    {
        BandFixture fixture;
        for (std::size_t byteIndex = 0; byteIndex < 17; byteIndex++)
        {
            FlipRenderedCell(fixture.patch, byteIndex * 8);
        }
        std::vector<std::byte> canvas = MatteCanvas(kCanvasBytes);
        REQUIRE(pbmodulation::BlitSupplementalBand(fixture.patch, canvas, 0));
        const auto outcome = fixture.DecodeFrom(CanvasView(canvas, 1920, 1080), geometry, 0);
        CHECK(outcome.status == pbmodulation::SupplementalBandDecodeStatus::RsFailure);
    }
}

TEST_CASE("Supplemental band binds to the captured session tag and frame sequence",
    "[pbmodulation][supplemental-band][negative][identity]")
{
    BandFixture fixture;
    std::vector<std::byte> canvas = MatteCanvas(kCanvasBytes);
    REQUIRE(pbmodulation::BlitSupplementalBand(fixture.patch, canvas, 0));
    const pbmodulation::LocalDesktopGeometry geometry{0, 0, 1, 1, 0};
    const auto view = CanvasView(canvas, 1920, 1080);
    CHECK(fixture.DecodeFrom(view, geometry, 0, pbprotocol::SessionTag{0x9999999999999999ULL}, 42).status ==
        pbmodulation::SupplementalBandDecodeStatus::IdentityRejected);
    CHECK(fixture.DecodeFrom(view, geometry, 0, pbprotocol::SessionTag{0x0706050403020100ULL}, 43).status ==
        pbmodulation::SupplementalBandDecodeStatus::IdentityRejected);
}

TEST_CASE("Supplemental band sampling rejects frames the geometry leaves",
    "[pbmodulation][supplemental-band][negative][bounds]")
{
    BandFixture fixture;
    std::vector<std::byte> tiny = MatteCanvas(64 * 64 * 4);
    const pbmodulation::LocalDesktopGeometry geometry{0, 0, 1, 1, 0};
    const auto outcome = fixture.DecodeFrom(CanvasView(tiny, 64, 64), geometry, 0);
    CHECK(outcome.status == pbmodulation::SupplementalBandDecodeStatus::SamplingRejected);
}

TEST_CASE("Supplemental band render rejects oversize and non-control records",
    "[pbmodulation][supplemental-band][negative][render]")
{
    const pbprotocol::SessionTag tag{1};
    std::vector<std::byte> patch(kSupplementalBandPatchBytes);
    const std::vector<std::byte> oversize(kSupplementalBandMaximumRecordBytes + 1, std::byte{0});
    CHECK_FALSE(pbmodulation::RenderSupplementalBand(oversize, tag, 1, patch));
    const std::vector<std::byte> garbage(64, std::byte{0xAB});
    CHECK_FALSE(pbmodulation::RenderSupplementalBand(garbage, tag, 1, patch));
    // A valid record carrying a different tag must also be refused.
    const auto other = MakeControlRecord(pbprotocol::SessionTag{0x9999999999999999ULL}, 5);
    CHECK_FALSE(pbmodulation::RenderSupplementalBand(other, tag, 1, patch));
    // Blit guards the frame contract.
    const auto valid = MakeControlRecord(tag, 5);
    REQUIRE(pbmodulation::RenderSupplementalBand(valid, tag, 1, patch));
    std::vector<std::byte> smallFrame(640 * 480 * 4);
    CHECK_FALSE(pbmodulation::BlitSupplementalBand(patch, smallFrame, 0));
    std::vector<std::byte> fullFrame(kCanvasBytes);
    CHECK_FALSE(pbmodulation::BlitSupplementalBand(patch, fullFrame, 2));
}

TEST_CASE("Supplemental band raster matches the independent python golden",
    "[pbmodulation][supplemental-band][golden]")
{
    // Fixture identity: record = SerializeControlRecord(SessionDescriptor,
    // sequence 77, tag 0x0706050403020100, payload 01 03 .. 11), frame 42.
    // See generate_supplemental_band_golden.py; the record bytes are pinned
    // there as a literal so the oracle never depends on production code.
    BandFixture fixture;
    std::ifstream inputFile(std::filesystem::path(PB_SUPPLEMENTAL_BAND_GOLDEN_DIR) / "render-session.bin",
        std::ios::binary);
    REQUIRE(inputFile.is_open());
    std::vector<std::byte> golden(kSupplementalBandPatchBytes);
    inputFile.read(reinterpret_cast<char*>(golden.data()), static_cast<std::streamsize>(golden.size()));
    REQUIRE(inputFile.gcount() == static_cast<std::streamsize>(golden.size()));
    CHECK(std::equal(golden.begin(), golden.end(), fixture.patch.begin()));
}
