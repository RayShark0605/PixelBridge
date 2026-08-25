#include "modulation_test_helpers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace pbmodtest;
using namespace pbmodulation;

namespace {

// Independent partition proof: every canvas pixel is owned by exactly one
// region (built from the frozen table, without reusing the library's own
// validator logic).
[[nodiscard]] std::vector<int> BuildRegionOwnershipGrid()
{
    std::vector<int> ownership(kReferenceCanvasPixelCount, -1);
    for (std::size_t regionIndex = 0;
        regionIndex < kReferenceRegions.size(); regionIndex++)
    {
        const ReferenceRegion& region = kReferenceRegions[regionIndex];
        for (std::uint32_t row = 0; row < region.height; row++)
        {
            for (std::uint32_t col = 0; col < region.width; col++)
            {
                const std::size_t pixelIndex =
                    (static_cast<std::size_t>(region.y + row) *
                        kReferenceCanvasWidth) +
                    (region.x + col);
                CHECK(ownership[pixelIndex] == -1);
                ownership[pixelIndex] = static_cast<int>(regionIndex);
            }
        }
    }
    for (const int owned : ownership)
    {
        CHECK(owned >= 0);
    }
    return ownership;
}

} // namespace

TEST_CASE("Reference profile passes the frozen validation gate",
    "[pbmodulation][profile]")
{
    REQUIRE(ValidateReferenceVisualProfile());
}

TEST_CASE("Reference region table partitions the 1920x1080 canvas exactly",
    "[pbmodulation][profile][partition]")
{
    const std::vector<int> ownership = BuildRegionOwnershipGrid();
    REQUIRE(ownership.size() == kReferenceCanvasPixelCount);
    // Spot-check the frozen geometry of the data-carrying regions.
    std::size_t dataGridIndex = 0;
    std::size_t bootstrapAIndex = 0;
    std::size_t bootstrapBIndex = 0;
    std::size_t controlIndex = 0;
    for (std::size_t i = 0; i < kReferenceRegions.size(); i++)
    {
        const ReferenceRegionType type =
            kReferenceRegions[i].type;
        if (type == ReferenceRegionType::DataGrid)
        {
            dataGridIndex = i;
        }
        if (type == ReferenceRegionType::BootstrapA)
        {
            bootstrapAIndex = i;
        }
        if (type == ReferenceRegionType::BootstrapB)
        {
            bootstrapBIndex = i;
        }
        if (type == ReferenceRegionType::Control)
        {
            controlIndex = i;
        }
    }
    const ReferenceRegion& dataGrid = kReferenceRegions[dataGridIndex];
    CHECK(dataGrid.x == 16);
    CHECK(dataGrid.y == 64);
    CHECK(dataGrid.width == 1888);
    CHECK(dataGrid.height == 952);
    const ReferenceRegion& bootstrapA =
        kReferenceRegions[bootstrapAIndex];
    CHECK(bootstrapA.y == 16);
    CHECK(bootstrapA.height == 8);
    const ReferenceRegion& control = kReferenceRegions[controlIndex];
    CHECK(control.y == 24);
    CHECK(control.height == 16);
    const ReferenceRegion& bootstrapB =
        kReferenceRegions[bootstrapBIndex];
    CHECK(bootstrapB.y == 1032);
    CHECK(bootstrapB.height == 8);
    // Lane / tile counts derived from the frozen geometry.
    CHECK(dataGrid.width % kReferenceDataTileWidth == 0);
    CHECK(dataGrid.height % kReferenceDataTileHeight == 0);
    CHECK(static_cast<std::size_t>(dataGrid.width / kReferenceDataTileWidth) *
        (dataGrid.height / kReferenceDataTileHeight) ==
        kReferenceDataTileCount);
    CHECK(kReferenceDataTileCount == 112336);
    CHECK(kReferenceDataRegionBytes == 56168);
    CHECK(bootstrapA.width % kReferenceLaneSymbolWidth == 0);
    CHECK(bootstrapA.height % kReferenceLaneSymbolHeight == 0);
    CHECK(static_cast<std::size_t>(bootstrapA.width /
        kReferenceLaneSymbolWidth) *
        (bootstrapA.height / kReferenceLaneSymbolHeight) ==
        kReferenceBootstrapLaneSymbols);
    CHECK(kReferenceBootstrapLaneSymbols == 240);
    CHECK(kReferenceControlLaneSymbols == 480);
    CHECK(kReferenceBootstrapRecordSymbols == 88);
    CHECK(kReferenceBootstrapBSymbolOffset == 152);
    CHECK(kReferenceFrameProtocolBytes == 56452);
}

TEST_CASE("Reference constellation is the frozen 16-level luma set",
    "[pbmodulation][profile][constellation]")
{
    std::uint8_t previous = 0;
    for (std::uint8_t levelIndex = 0;
        levelIndex < kReferenceLevelCount; levelIndex++)
    {
        const std::uint8_t value = GetReferenceLevelValue(levelIndex);
        CHECK(value == 8 + 16 * levelIndex);
        if (levelIndex > 0)
        {
            CHECK(value > previous);
        }
        previous = value;
    }
    CHECK(GetReferenceLevelValue(0) == 8);
    CHECK(GetReferenceLevelValue(15) == 248);
    // Guard band: every level is at least 7 steps from both range endpoints.
    for (std::uint8_t levelIndex = 0;
        levelIndex < kReferenceLevelCount; levelIndex++)
    {
        const int value = GetReferenceLevelValue(levelIndex);
        CHECK(value - 0 >= 7);
        CHECK(255 - value >= 7);
    }
    // Gray code: bijection on the 4-bit domain and distinct images.
    std::uint8_t seen[16] = {0};
    for (std::uint8_t symbol = 0; symbol < 16; symbol++)
    {
        const std::uint8_t gray = GrayCode4(symbol);
        CHECK(gray < 16);
        CHECK(GrayDecode4(gray) == symbol);
        CHECK(seen[gray] == 0);
        seen[gray] = 1;
    }
}

namespace {

// Tamper helper: flip one byte of a canonical manifest (returns the
// modified manifest).
std::array<std::byte, kReferenceManifestBytes> FlipManifestByte(
    const std::size_t offset,
    const std::uint8_t newValue)
{
    std::array<std::byte, kReferenceManifestBytes> manifest =
        SerializeReferenceRegionManifest();
    manifest[offset] = std::byte{newValue};
    return manifest;
}

} // namespace

TEST_CASE("Reference manifest round-trips and validates",
    "[pbmodulation][profile][manifest]")
{
    const std::array<std::byte, kReferenceManifestBytes> manifest =
        SerializeReferenceRegionManifest();
    const auto parsedResult = ParseReferenceRegionManifest(
        std::span<const std::byte>(manifest));
    REQUIRE(parsedResult);
    const ReferenceRegionManifest& parsed = parsedResult.Value();
    CHECK(parsed.canvasWidth == kReferenceCanvasWidth);
    CHECK(parsed.canvasHeight == kReferenceCanvasHeight);
    CHECK(parsed.regions == kReferenceRegions);
    CHECK(parsed.bitsPerSymbol == kReferenceBitsPerSymbol);
    CHECK(parsed.levelCount == kReferenceLevelCount);
    CHECK(parsed.levelBase == kReferenceLevelBase);
    CHECK(parsed.levelStep == kReferenceLevelStep);
    CHECK(parsed.dataTileWidth == kReferenceDataTileWidth);
    CHECK(parsed.dataTileHeight == kReferenceDataTileHeight);
    CHECK(parsed.laneSymbolWidth == kReferenceLaneSymbolWidth);
    CHECK(parsed.laneSymbolHeight == kReferenceLaneSymbolHeight);
}

TEST_CASE("Reference manifest golden digest is stable",
    "[pbmodulation][profile][manifest][golden]")
{
    const std::array<std::byte, kReferenceManifestBytes> manifest =
        SerializeReferenceRegionManifest();
    // Pin: BLAKE3-256 over the canonical 275-byte manifest. Generated once
    // with the reference implementation after the independent structural
    // cross-checks above; see docs/REFERENCE_RASTER.md.
    constexpr char kExpectedManifestDigestHex[] =
        "a7e31cbd7cfa6865f8bc029a78d58d065990e732f6386613329f2dd07602003b";
    const auto digest = pbprotocol::ComputeBlake3Digest(
        std::span<const std::byte>(manifest));
    CHECK(ToHex(digest) == kExpectedManifestDigestHex);
    CHECK(manifest.size() == 275);
}

TEST_CASE("Reference manifest rejects malformed variants",
    "[pbmodulation][profile][manifest][malformed]")
{
    const std::array<std::byte, kReferenceManifestBytes> canonical =
        SerializeReferenceRegionManifest();
    const std::span<const std::byte> canonicalSpan =
        std::span<const std::byte>(canonical);

    // Wrong total size (fixed-size container: any truncation or growth is
    // rejected before any field is read).
    CHECK_FALSE(ParseReferenceRegionManifest(canonicalSpan.first(274)));
    CHECK_FALSE(ParseReferenceRegionManifest(canonicalSpan.first(273)));
    std::vector<std::byte> oversized(canonical.begin(), canonical.end());
    oversized.push_back(std::byte{0});
    CHECK_FALSE(ParseReferenceRegionManifest(
        std::span<const std::byte>(oversized)));
    CHECK_FALSE(ParseReferenceRegionManifest(
        std::span<const std::byte>{}));

    // Magic.
    {
        auto manifest = FlipManifestByte(0, 0x00);
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code == ModulationErrorCode::InvalidMagic);
    }

    // Version and layout version.
    {
        auto manifest = FlipManifestByte(4, 2);
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::UnsupportedVersion);
    }
    {
        auto manifest = FlipManifestByte(5, 2);
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::UnsupportedVersion);
    }

    // Header reserved u16.
    {
        auto manifest = FlipManifestByte(6, 1);
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::NonZeroReservedByte);
    }

    // Canvas size.
    {
        auto manifest = FlipManifestByte(8, static_cast<std::uint8_t>(
            kReferenceCanvasWidth & 0xFFu) + 1);
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::ManifestValidationFailed);
    }
    {
        auto manifest = FlipManifestByte(12, 5);
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::ManifestValidationFailed);
    }

    // Region count.
    {
        auto manifest = FlipManifestByte(16, 13);
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::ManifestValidationFailed);
    }

    // Each frozen parameter byte.
    for (std::size_t parameterOffset = 17; parameterOffset <= 24;
        parameterOffset++)
    {
        auto manifest = FlipManifestByte(parameterOffset, 1);
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::ManifestValidationFailed);
    }

    // Reserved 8-byte tail of the header.
    for (std::size_t reservedOffset = 25; reservedOffset < 33;
        reservedOffset++)
    {
        auto manifest = FlipManifestByte(reservedOffset, 1);
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::ManifestValidationFailed);
    }

    // Region table: first entry's type and each of its geometry fields.
    {
        auto manifest = FlipManifestByte(33,
            static_cast<std::uint8_t>(
                static_cast<std::uint8_t>(ReferenceRegionType::BootstrapB) +
                1u));
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::ManifestValidationFailed);
    }
    {
        auto manifest = FlipManifestByte(34,
            static_cast<std::uint8_t>(kReferenceRegions[0].x + 1u));
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::ManifestValidationFailed);
    }
    {
        auto manifest = FlipManifestByte(38,
            static_cast<std::uint8_t>(kReferenceRegions[0].width + 1u));
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::ManifestValidationFailed);
    }

    // CRC: canonical content with a corrupted CRC field.
    {
        auto manifest = FlipManifestByte(274,
            static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(
                canonical[274]) ^ 0xFFu));
        const auto result = ParseReferenceRegionManifest(
            std::span<const std::byte>(manifest));
        CHECK_FALSE(result);
        CHECK(result.Error().code == ModulationErrorCode::CrcMismatch);
    }
}