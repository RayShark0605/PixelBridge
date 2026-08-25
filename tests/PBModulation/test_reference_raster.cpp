#include "modulation_test_helpers.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace pbmodtest;
using namespace pbmodulation;
using pbmodulation::ModulationErrorCode;

namespace {

// Encodes a payload into a fresh canonical frame (dies on encoder failure).
std::vector<std::byte> EncodePayload(
    const GoldenFramePayload& payload)
{
    return EncodeFrameOrDie(MakeFrameInput(payload));
}

// Fills one 8x8 lane symbol block at (x, y) with the given level.
void FillLaneSymbol(
    const std::span<std::byte> bgra,
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint8_t level)
{
    for (std::uint32_t row = 0; row < 8; row++)
    {
        for (std::uint32_t col = 0; col < 8; col++)
        {
            SetFramePixel(bgra, x + col, y + row, level, level, level,
                kReferenceAlphaValue);
        }
    }
}

// Fills one 4x4 data tile at (x, y) with the given level.
void FillDataTile(
    const std::span<std::byte> bgra,
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint8_t level)
{
    for (std::uint32_t row = 0; row < 4; row++)
    {
        for (std::uint32_t col = 0; col < 4; col++)
        {
            SetFramePixel(bgra, x + col, y + row, level, level, level,
                kReferenceAlphaValue);
        }
    }
}

// Reads the luma of the pixel at (x, y).
std::uint8_t PixelLuma(
    const std::span<const std::byte> bgra,
    const std::uint32_t x,
    const std::uint32_t y)
{
    const std::size_t base =
        (static_cast<std::size_t>(y) * 1920u + x) * 4u;
    return std::to_integer<std::uint8_t>(bgra[base]);
}

} // namespace

TEST_CASE("Full payload round-trips through the reference raster",
    "[pbmodulation][raster][roundtrip]")
{
    const GoldenFramePayload canonical = MakeCanonicalGoldenPayload();
    const std::vector<std::byte> frame = EncodePayload(canonical);
    const DecodedReferenceFrame decoded = DecodeFrameOrDie(frame);
    CHECK(decoded.bootstrapRecord == canonical.bootstrap);
    CHECK(decoded.controlWindow == canonical.control);
    CHECK(decoded.data.size() == kReferenceDataRegionBytes);
    CHECK(decoded.data == canonical.data);

    // Deterministic pseudo-random payload (all three lanes active).
    GoldenFramePayload randomPayload = MakeZeroGoldenPayload();
    SplitMix64 random(0xB0B0C0DEu);
    for (std::byte& byteValue : randomPayload.bootstrap)
    {
        byteValue = Byte(random.Next() & 0xFFu);
    }
    for (std::byte& byteValue : randomPayload.control)
    {
        byteValue = Byte(random.Next() & 0xFFu);
    }
    for (std::byte& byteValue : randomPayload.data)
    {
        byteValue = Byte(random.Next() & 0xFFu);
    }
    const std::vector<std::byte> randomFrame = EncodePayload(randomPayload);
    const DecodedReferenceFrame randomDecoded =
        DecodeFrameOrDie(randomFrame);
    CHECK(randomDecoded.bootstrapRecord == randomPayload.bootstrap);
    CHECK(randomDecoded.controlWindow == randomPayload.control);
    CHECK(randomDecoded.data == randomPayload.data);
}

TEST_CASE("Reference frame encoding is deterministic",
    "[pbmodulation][raster][determinism]")
{
    const GoldenFramePayload payload = MakeCanonicalGoldenPayload();
    const std::vector<std::byte> firstFrame = EncodePayload(payload);
    const std::vector<std::byte> secondFrame = EncodePayload(payload);
    CHECK(firstFrame == secondFrame);
    // The raster is a fixed point of encode o decode o encode.
    const DecodedReferenceFrame decoded = DecodeFrameOrDie(firstFrame);
    const GoldenFramePayload decodedPayload{
        decoded.bootstrapRecord,
        decoded.controlWindow,
        decoded.data};
    const std::vector<std::byte> reEncoded = EncodePayload(decodedPayload);
    CHECK(reEncoded == firstFrame);
}

TEST_CASE("Every constellation level round-trips in the data lane",
    "[pbmodulation][raster][levels]")
{
    GoldenFramePayload payload = MakeZeroGoldenPayload();
    // Cycle through all 16 symbols; the tile of symbol s renders at
    // level L_gray4(s).
    for (std::size_t tileIndex = 0;
        tileIndex < kReferenceDataTileCount; tileIndex++)
    {
        const std::uint8_t symbol =
            static_cast<std::uint8_t>(tileIndex % 16);
        const std::size_t byteIndex = tileIndex / 2;
        if ((tileIndex % 2) == 0)
        {
            payload.data[byteIndex] =
                std::byte{static_cast<std::uint8_t>(symbol)};
        }
        else
        {
            const std::uint8_t combinedByte = static_cast<std::uint8_t>(
                std::to_integer<std::uint8_t>(payload.data[byteIndex]) |
                (static_cast<std::uint8_t>(symbol) << 4));
            payload.data[byteIndex] = std::byte{combinedByte};
        }
    }
    const std::vector<std::byte> frame = EncodePayload(payload);

    // Independent raster check: every tile pixel is at the expected level
    // and carries the frozen alpha.
    std::size_t checkedPixels = 0;
    for (std::uint32_t tileRow = 0; tileRow < kReferenceDataGridRows;
        tileRow++)
    {
        for (std::uint32_t tileColumn = 0;
            tileColumn < kReferenceDataGridColumns; tileColumn++)
        {
            const std::size_t tileIndex =
                static_cast<std::size_t>(tileRow) *
                kReferenceDataGridColumns + tileColumn;
            const std::uint8_t symbol =
                static_cast<std::uint8_t>(tileIndex % 16);
            const std::uint8_t expectedLevel =
                GetReferenceLevelValue(GrayCode4(symbol));
            const auto [originX, originY] =
                GetDataTileOrigin(tileColumn, tileRow);
            for (std::uint32_t row = 0; row < 4; row++)
            {
                for (std::uint32_t col = 0; col < 4; col++)
                {
                    CHECK(PixelLuma(frame, originX + col, originY + row) ==
                        expectedLevel);
                    const std::size_t base =
                        (static_cast<std::size_t>(originY + row) * 1920u +
                            originX + col) * 4u;
                    CHECK(std::to_integer<std::uint8_t>(frame[base + 3]) ==
                        kReferenceAlphaValue);
                    checkedPixels++;
                }
            }
        }
    }
    REQUIRE(checkedPixels == kReferenceDataTileCount * 16);

    const DecodedReferenceFrame decoded = DecodeFrameOrDie(frame);
    CHECK(decoded.data == payload.data);
}
namespace {

// Counts the pixels at which two frames differ (geometry pinning for the
// mutation sweep; avoids per-pixel CHECK overhead).
std::size_t CountDifferingPixels(
    const std::span<const std::byte> firstFrame,
    const std::span<const std::byte> secondFrame)
{
    std::size_t differingPixels = 0;
    for (std::size_t pixelIndex = 0;
        pixelIndex < kReferenceCanvasPixelCount; pixelIndex++)
    {
        const std::size_t offset = pixelIndex * 4;
        if (firstFrame[offset] != secondFrame[offset] ||
            firstFrame[offset + 1] != secondFrame[offset + 1] ||
            firstFrame[offset + 2] != secondFrame[offset + 2] ||
            firstFrame[offset + 3] != secondFrame[offset + 3])
        {
            differingPixels++;
        }
    }
    return differingPixels;
}

// Expected sync luma at (x, y) from the frozen global-coordinate checker
// rule (independent of the library implementation).
std::uint8_t ExpectedSyncLuma(
    const std::uint32_t x,
    const std::uint32_t y)
{
    return (((x / 8) + (y / 8)) % 2) == 0 ? 0 : 255;
}

// Expected pilot luma at (x, y) from the frozen pilot raster (local
// coordinates relative to the pilot region origin).
std::uint8_t ExpectedPilotLuma(
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint32_t pilotOriginY)
{
    const std::uint32_t localX = x;
    if (localX < 256)
    {
        return GetReferenceLevelValue(
            static_cast<std::uint8_t>(localX / 16u));
    }
    if (localX < 272)
    {
        return 0;
    }
    if (localX < 288)
    {
        return 255;
    }
    if (localX < 304)
    {
        return 128;
    }
    if (localX < 336)
    {
        const std::uint32_t cellX = (localX - 304) / 8;
        const std::uint32_t cellY = (y - pilotOriginY) / 8;
        return ((cellX + cellY) % 2) == 0 ? 0 : 255;
    }
    return 128;
}

} // namespace

TEST_CASE("Frozen regions render the canonical byte-exact content",
    "[pbmodulation][raster][frozen]")
{
    const std::vector<std::byte> frame =
        EncodePayload(MakeZeroGoldenPayload());

    // Guard regions: pure black with the frozen alpha (full scan,
    // mismatch count instead of per-pixel CHECK).
    std::size_t guardMismatches = 0;
    for (const auto& region : kReferenceRegions)
    {
        if (region.type != ReferenceRegionType::Guard)
        {
            continue;
        }
        for (std::uint32_t row = 0; row < region.height; row++)
        {
            for (std::uint32_t col = 0; col < region.width; col++)
            {
                const std::size_t pixelIndex =
                    (static_cast<std::size_t>(region.y + row) *
                        kReferenceCanvasWidth) +
                    (region.x + col);
                const std::size_t offset = pixelIndex * 4;
                if (frame[offset] != std::byte{0} ||
                    frame[offset + 1] != std::byte{0} ||
                    frame[offset + 2] != std::byte{0} ||
                    frame[offset + 3] != std::byte{kReferenceAlphaValue})
                {
                    guardMismatches++;
                }
            }
        }
    }
    CHECK(guardMismatches == 0);

    // Sync regions: global-coordinate checkerboard, so the top band
    // (y 8..15) and the bottom band (y 1024..1031) carry different
    // phases (127 sync rows apart -> parity flips).
    std::size_t syncMismatches = 0;
    for (const auto& region : kReferenceRegions)
    {
        if (region.type != ReferenceRegionType::Sync)
        {
            continue;
        }
        for (std::uint32_t row = 0; row < region.height; row++)
        {
            for (std::uint32_t col = 0; col < region.width; col++)
            {
                const std::uint32_t x = region.x + col;
                const std::uint32_t y = region.y + row;
                if (PixelLuma(frame, x, y) != ExpectedSyncLuma(x, y))
                {
                    syncMismatches++;
                }
            }
        }
    }
    CHECK(syncMismatches == 0);
    CHECK(PixelLuma(frame, 8, 8) == 0);
    CHECK(PixelLuma(frame, 8, 1024) == 255);

    // Pilot regions: identical top and bottom rasters (local coordinates).
    std::size_t pilotMismatches = 0;
    for (const auto& region : kReferenceRegions)
    {
        if (region.type != ReferenceRegionType::Pilot)
        {
            continue;
        }
        for (std::uint32_t row = 0; row < region.height; row++)
        {
            for (std::uint32_t col = 0; col < region.width; col++)
            {
                const std::uint32_t x = region.x + col;
                const std::uint32_t y = region.y + row;
                if (PixelLuma(frame, x, y) !=
                    ExpectedPilotLuma(x, y, region.y))
                {
                    pilotMismatches++;
                }
            }
        }
    }
    CHECK(pilotMismatches == 0);
    // Ladder / reference spot checks (local x 0..255, 16x16 per level).
    CHECK(PixelLuma(frame, 0, 44) == 8);
    CHECK(PixelLuma(frame, 15, 55) == 8);
    CHECK(PixelLuma(frame, 100, 44) == 104);
    CHECK(PixelLuma(frame, 255, 40) == 248);
    CHECK(PixelLuma(frame, 100, 1044) == 104); // bottom pilot identical
    CHECK(PixelLuma(frame, 264, 44) == 0);
    CHECK(PixelLuma(frame, 280, 44) == 255);
    CHECK(PixelLuma(frame, 296, 44) == 128);
    CHECK(PixelLuma(frame, 1600, 48) == 128);
}

TEST_CASE("Frame size and output buffer contracts are enforced",
    "[pbmodulation][raster][sizes]")
{
    const std::vector<std::byte> frame =
        EncodePayload(MakeZeroGoldenPayload());

    {
        const auto result = DecodeReferenceFrame(
            std::span<const std::byte>(frame).first(kReferenceFrameBgraBytes - 1));
        CHECK_FALSE(result);
        CHECK(result.Error().code == ModulationErrorCode::InvalidInput);
    }
    {
        std::vector<std::byte> oversized(frame.begin(), frame.end());
        oversized.push_back(std::byte{0});
        const auto result = DecodeReferenceFrame(
            std::span<const std::byte>(oversized));
        CHECK_FALSE(result);
        CHECK(result.Error().code == ModulationErrorCode::InvalidInput);
    }

    std::array<std::byte, kReferenceBootstrapRecordBytes> outBootstrap{};
    std::array<std::byte, kReferenceControlWindowBytes> outControl{};
    std::vector<std::byte> outData(kReferenceDataRegionBytes);
    {
        const auto status = DecodeReferenceFrameInto(
            frame, std::span<std::byte>(outBootstrap).first(43),
            std::span<std::byte>(outControl), std::span<std::byte>(outData));
        CHECK_FALSE(status);
        CHECK(status.Error().code ==
            ModulationErrorCode::OutputBufferTooSmall);
    }
    {
        std::array<std::byte, kReferenceControlWindowBytes - 1>
            smallControl{};
        const auto status = DecodeReferenceFrameInto(
            frame, std::span<std::byte>(outBootstrap),
            std::span<std::byte>(smallControl),
            std::span<std::byte>(outData));
        CHECK_FALSE(status);
        CHECK(status.Error().code ==
            ModulationErrorCode::OutputBufferTooSmall);
    }
    {
        std::vector<std::byte> smallData(kReferenceDataRegionBytes - 1);
        const auto status = DecodeReferenceFrameInto(
            frame, std::span<std::byte>(outBootstrap),
            std::span<std::byte>(outControl),
            std::span<std::byte>(smallData));
        CHECK_FALSE(status);
        CHECK(status.Error().code ==
            ModulationErrorCode::OutputBufferTooSmall);
    }
    {
        const auto status = DecodeReferenceFrameInto(
            std::span<const std::byte>(frame).first(kReferenceFrameBgraBytes - 1),
            std::span<std::byte>(outBootstrap),
            std::span<std::byte>(outControl),
            std::span<std::byte>(outData));
        CHECK_FALSE(status);
        CHECK(status.Error().code == ModulationErrorCode::InvalidInput);
    }

    {
        const auto input = MakeZeroGoldenPayload().MakeInput();
        std::vector<std::byte> smallOut(kReferenceFrameBgraBytes - 1);
        const auto status = EncodeReferenceFrame(input, smallOut);
        CHECK_FALSE(status);
        CHECK(status.Error().code ==
            ModulationErrorCode::OutputBufferTooSmall);
    }
}
TEST_CASE("Lane payload sizes are validated on the encode path",
    "[pbmodulation][raster][input-sizes]")
{
    const std::array<std::byte, kReferenceBootstrapRecordBytes>
        bootstrap{};
    std::vector<std::byte> bgra(kReferenceFrameBgraBytes);

    const std::vector<std::size_t> controlSizes = {239, 240, 241};
    for (const std::size_t controlSize : controlSizes)
    {
        std::vector<std::byte> control(controlSize);
        std::vector<std::byte> data(kReferenceDataRegionBytes);
        const ReferenceFrameInput input{
            bootstrap,
            std::span<const std::byte>(control),
            std::span<const std::byte>(data)};
        const auto status = EncodeReferenceFrame(input, bgra);
        if (controlSize == kReferenceControlWindowBytes)
        {
            CHECK(status);
        }
        else
        {
            CHECK_FALSE(status);
            CHECK(status.Error().code ==
                ModulationErrorCode::InvalidInput);
        }
    }

    const std::vector<std::size_t> dataSizes = {
        kReferenceDataRegionBytes - 1,
        kReferenceDataRegionBytes,
        kReferenceDataRegionBytes + 1};
    for (const std::size_t dataSize : dataSizes)
    {
        std::vector<std::byte> control(kReferenceControlWindowBytes);
        std::vector<std::byte> dataRegion(dataSize);
        const ReferenceFrameInput input{
            bootstrap,
            std::span<const std::byte>(control),
            std::span<const std::byte>(dataRegion)};
        const auto status = EncodeReferenceFrame(input, bgra);
        if (dataSize == kReferenceDataRegionBytes)
        {
            CHECK(status);
        }
        else
        {
            CHECK_FALSE(status);
            CHECK(status.Error().code ==
                ModulationErrorCode::InvalidInput);
        }
    }
}

TEST_CASE("Failed demodulation leaves the caller output untouched",
    "[pbmodulation][raster][failure-immutable]")
{
    std::vector<std::byte> frame =
        EncodePayload(MakeZeroGoldenPayload());
    // Corrupt one guard pixel so the whole frame is rejected.
    SetFramePixel(frame, 0, 0, 0, 0, 0, kReferenceAlphaValue - 1);

    std::array<std::byte, kReferenceBootstrapRecordBytes> outBootstrap{};
    std::array<std::byte, kReferenceControlWindowBytes> outControl{};
    std::vector<std::byte> outData(kReferenceDataRegionBytes);
    std::fill(outBootstrap.begin(), outBootstrap.end(), std::byte{0xAB});
    std::fill(outControl.begin(), outControl.end(), std::byte{0xAB});
    std::fill(outData.begin(), outData.end(), std::byte{0xAB});

    const auto status = DecodeReferenceFrameInto(
        frame, std::span<std::byte>(outBootstrap),
        std::span<std::byte>(outControl), std::span<std::byte>(outData));
    CHECK_FALSE(status);
    CHECK(status.Error().code ==
        ModulationErrorCode::FrozenRegionMismatch);

    std::size_t mutatedBytes = 0;
    for (const auto value : outBootstrap)
    {
        if (value != std::byte{0xAB})
        {
            mutatedBytes++;
        }
    }
    for (const auto value : outControl)
    {
        if (value != std::byte{0xAB})
        {
            mutatedBytes++;
        }
    }
    for (const auto value : outData)
    {
        if (value != std::byte{0xAB})
        {
            mutatedBytes++;
        }
    }
    CHECK(mutatedBytes == 0);

    // Encode failures leave the destination raster untouched as well.
    std::vector<std::byte> outBgra(kReferenceFrameBgraBytes);
    std::fill(outBgra.begin(), outBgra.end(), std::byte{0xCD});
    std::vector<std::byte> shortControl(239);
    std::vector<std::byte> data(kReferenceDataRegionBytes);
    const ReferenceFrameInput invalidInput{
        std::array<std::byte, kReferenceBootstrapRecordBytes>{},
        std::span<const std::byte>(shortControl),
        std::span<const std::byte>(data)};
    const auto encodeStatus =
        EncodeReferenceFrame(invalidInput, outBgra);
    CHECK_FALSE(encodeStatus);
    std::size_t sentinelMismatches = 0;
    for (const auto value : outBgra)
    {
        if (value != std::byte{0xCD})
        {
            sentinelMismatches++;
        }
    }
    CHECK(sentinelMismatches == 0);
}

TEST_CASE("Mid-stream failures leave every caller output untouched",
    "[pbmodulation][raster][failure-immutable]")
{
    // The no-output contract must hold for failures that occur only after
    // the decoder has already demodulated earlier lanes and tiles. Both
    // cases below fail late enough that an incremental writer would have
    // already corrupted the caller buffers:
    //   case 1: the last control symbol (index 479 of 480) sits exactly on
    //           the ambiguous decision boundary between two levels;
    //   case 2: a mid-grid data tile is forced onto the same boundary.
    const std::vector<std::byte> frame =
        EncodePayload(MakeZeroGoldenPayload());

    const auto expectUntouchedOnFailure =
        [](const std::vector<std::byte>& mutated,
            const ModulationErrorCode expectedCode,
            const std::size_t expectedOffset)
    {
        std::array<std::byte, kReferenceBootstrapRecordBytes> outBootstrap{};
        std::array<std::byte, kReferenceControlWindowBytes> outControl{};
        std::vector<std::byte> outData(kReferenceDataRegionBytes);
        std::fill(outBootstrap.begin(), outBootstrap.end(), std::byte{0xAB});
        std::fill(outControl.begin(), outControl.end(), std::byte{0xCD});
        std::fill(outData.begin(), outData.end(), std::byte{0xEF});

        const auto status = DecodeReferenceFrameInto(
            mutated, std::span<std::byte>(outBootstrap),
            std::span<std::byte>(outControl),
            std::span<std::byte>(outData));
        REQUIRE_FALSE(status);
        CHECK(status.Error().code == expectedCode);
        CHECK(status.Error().offset == expectedOffset);

        const std::uint8_t bootstrapSentinel = 0xAB;
        const std::uint8_t controlSentinel = 0xCD;
        const std::uint8_t dataSentinel = 0xEF;
        std::size_t mutatedBytes = 0;
        for (const auto value : outBootstrap)
        {
            if (value != std::byte{bootstrapSentinel})
            {
                mutatedBytes++;
            }
        }
        for (const auto value : outControl)
        {
            if (value != std::byte{controlSentinel})
            {
                mutatedBytes++;
            }
        }
        for (const auto value : outData)
        {
            if (value != std::byte{dataSentinel})
            {
                mutatedBytes++;
            }
        }
        CHECK(mutatedBytes == 0);
    };

    // Case 1: the control region starts at y 24 in the frozen table;
    // symbol 479 of 480 is row 1, column 239.
    {
        std::vector<std::byte> mutated = frame;
        const std::uint32_t originX =
            239 * kReferenceLaneSymbolWidth;
        const std::uint32_t originY =
            24 + 1 * kReferenceLaneSymbolHeight;
        FillLaneSymbol(mutated, originX, originY, 16);
        expectUntouchedOnFailure(
            mutated, ModulationErrorCode::AmbiguousLevel,
            static_cast<std::size_t>(originY) * kReferenceCanvasWidth +
                originX);
    }

    // Case 2: tile column 56 / row 2 (tile index 1000) in the data grid.
    {
        std::vector<std::byte> mutated = frame;
        const auto [originX, originY] = GetDataTileOrigin(56, 2);
        FillDataTile(mutated, originX, originY, 16);
        expectUntouchedOnFailure(
            mutated, ModulationErrorCode::AmbiguousLevel,
            static_cast<std::size_t>(originY) * kReferenceCanvasWidth +
                originX);
    }
}

TEST_CASE("Frozen region tampering fails closed at the exact pixel",
    "[pbmodulation][raster][frozen][malformed]")
{
    const std::vector<std::byte> frame =
        EncodePayload(MakeZeroGoldenPayload());

    struct FrozenTamperCase
    {
        std::uint32_t x;
        std::uint32_t y;
        std::uint8_t tamperLuma;
        std::size_t expectedOffset;
    };
    const std::vector<FrozenTamperCase> cases = {
        // Guard top-left corner.
        {0, 0, 255, 0},
        // Sync top band: global checker (1+1)%2 == 0 -> level 0.
        {8, 8, 255, 8u * kReferenceCanvasWidth + 8},
        // Sync bottom band: different phase -> level 255 expected.
        {8, 1024, 0, 1024u * kReferenceCanvasWidth + 8},
        // Pilot ladder cell 6 (level 104).
        {100, 44, 0, 44u * kReferenceCanvasWidth + 100},
        // Pilot white reference.
        {280, 52, 0, 52u * kReferenceCanvasWidth + 280},
        // Guard bottom band.
        {960, 1070, 255, 1070u * kReferenceCanvasWidth + 960}};
    for (const auto& tamperCase : cases)
    {
        std::vector<std::byte> mutated = frame;
        SetFramePixel(mutated, tamperCase.x, tamperCase.y,
            tamperCase.tamperLuma, tamperCase.tamperLuma,
            tamperCase.tamperLuma, kReferenceAlphaValue);
        const auto result = DecodeReferenceFrame(mutated);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::FrozenRegionMismatch);
        CHECK(result.Error().offset == tamperCase.expectedOffset);
    }
}

TEST_CASE("Blank canvases are rejected by the frozen region check",
    "[pbmodulation][raster][blank]")
{
    std::vector<std::byte> allZero(kReferenceFrameBgraBytes);
    {
        const auto result = DecodeReferenceFrame(allZero);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::FrozenRegionMismatch);
        CHECK(result.Error().offset == 0);
    }
    std::vector<std::byte> allWhite(kReferenceFrameBgraBytes,
        std::byte{255});
    {
        const auto result = DecodeReferenceFrame(allWhite);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::FrozenRegionMismatch);
        CHECK(result.Error().offset == 0);
    }
}
TEST_CASE("Bootstrap A/B disagreement rejects the frame as torn",
    "[pbmodulation][raster][torn]")
{
    const std::vector<std::byte> frame =
        EncodePayload(MakeCanonicalGoldenPayload());
    // The frozen torn-frame offset is the origin of the B-side record:
    // lane position 152 -> x 1216, y 1032.
    const std::size_t expectedOffset =
        1032u * kReferenceCanvasWidth + 1216u;

    // Tamper the whole first record symbol of A (level 8 -> 24 flips
    // record byte 0 from 0x50 to 0x10).
    {
        std::vector<std::byte> mutated = frame;
        FillLaneSymbol(mutated, 0, 16, 24);
        const auto result = DecodeReferenceFrame(mutated);
        CHECK_FALSE(result);
        CHECK(result.Error().code == ModulationErrorCode::TornFrame);
        CHECK(result.Error().offset == expectedOffset);
    }
    // Same tamper on the B-side record origin.
    {
        std::vector<std::byte> mutated = frame;
        FillLaneSymbol(mutated, 1216, 1032, 24);
        const auto result = DecodeReferenceFrame(mutated);
        CHECK_FALSE(result);
        CHECK(result.Error().code == ModulationErrorCode::TornFrame);
        CHECK(result.Error().offset == expectedOffset);
    }
    // One pixel inside a record symbol stays inside the frozen margin
    // (mean 759/64 == 11, distance 3 from level 8): the decoded record is
    // unchanged. The reference demod never guesses sub-margin noise.
    {
        std::vector<std::byte> mutated = frame;
        SetFramePixel(mutated, 0, 16, 255, 255, 255, kReferenceAlphaValue);
        const DecodedReferenceFrame decoded = DecodeFrameOrDie(mutated);
        CHECK(BytesEqual(
            std::span<const std::byte>(decoded.bootstrapRecord),
            std::span<const std::byte>(kBootstrapGolden)));
        CHECK(BytesEqual(
            std::span<const std::byte>(decoded.controlWindow).first(
                kControlGolden.size()),
            std::span<const std::byte>(kControlGolden)));
        std::size_t controlTailNonZero = 0;
        for (std::size_t i = kControlGolden.size();
            i < decoded.controlWindow.size(); i++)
        {
            if (decoded.controlWindow[i] != std::byte{0})
            {
                controlTailNonZero++;
            }
        }
        CHECK(controlTailNonZero == 0);
    }
}

TEST_CASE("Symbol means at decision boundaries fail closed",
    "[pbmodulation][raster][decision-boundary]")
{
    // (a) A 4x4 tile split exactly halfway between two levels ->
    // AmbiguousLevel at the tile origin.
    const std::vector<std::pair<std::uint32_t, std::uint8_t>>
        ambiguousTiles = {{0, 8}, {1, 232}};
    for (const auto& [tileColumn, lowLuma] : ambiguousTiles)
    {
        const std::vector<std::byte> frame =
            EncodePayload(MakeZeroGoldenPayload());
        std::vector<std::byte> mutated = frame;
        const auto [originX, originY] =
            GetDataTileOrigin(tileColumn, 0);
        const std::uint8_t highLuma = static_cast<std::uint8_t>(
            lowLuma + kReferenceLevelStep);
        for (std::uint32_t row = 0; row < 4; row++)
        {
            for (std::uint32_t col = 0; col < 2; col++)
            {
                SetFramePixel(mutated, originX + col, originY + row,
                    lowLuma, lowLuma, lowLuma, kReferenceAlphaValue);
            }
            for (std::uint32_t col = 2; col < 4; col++)
            {
                SetFramePixel(mutated, originX + col, originY + row,
                    highLuma, highLuma, highLuma, kReferenceAlphaValue);
            }
        }
        const auto result = DecodeReferenceFrame(mutated);
        CHECK_FALSE(result);
        CHECK(result.Error().code == ModulationErrorCode::AmbiguousLevel);
        CHECK(result.Error().offset ==
            static_cast<std::size_t>(originY) * kReferenceCanvasWidth +
            originX);
    }

    // (b) Uniform off-constellation values decide to the unique level
    // inside the margin (distance <= 7), pinning the exact boundary
    // semantics including the asymmetric 255 side.
    const std::vector<std::pair<std::uint8_t, std::uint8_t>>
        meanToLevelIndex = {{1, 0}, {7, 0}, {15, 0}, {17, 1}, {23, 1},
        {247, 15}, {249, 15}, {255, 15}};
    for (const auto& [meanLuma, levelIndex] : meanToLevelIndex)
    {
        const std::vector<std::byte> frame =
            EncodePayload(MakeZeroGoldenPayload());
        std::vector<std::byte> mutated = frame;
        const auto [originX, originY] = GetDataTileOrigin(0, 0);
        FillDataTile(mutated, originX, originY, meanLuma);
        const DecodedReferenceFrame decoded = DecodeFrameOrDie(mutated);
        CHECK(std::to_integer<std::uint8_t>(decoded.data[0]) ==
            GrayDecode4(levelIndex));
    }

    // (c) Mean 0 is 8 steps from level 8 (outside the margin) ->
    // OffConstellationLevel at the tile origin.
    {
        const std::vector<std::byte> frame =
            EncodePayload(MakeZeroGoldenPayload());
        std::vector<std::byte> mutated = frame;
        const auto [originX, originY] = GetDataTileOrigin(0, 0);
        FillDataTile(mutated, originX, originY, 0);
        const auto result = DecodeReferenceFrame(mutated);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::OffConstellationLevel);
        CHECK(result.Error().offset ==
            static_cast<std::size_t>(originY) * kReferenceCanvasWidth +
            originX);
    }
}

TEST_CASE("Chroma and alpha contract violations fail closed",
    "[pbmodulation][raster][channels]")
{
    const std::vector<std::byte> frame =
        EncodePayload(MakeZeroGoldenPayload());
    const auto [tileX, tileY] = GetDataTileOrigin(10, 5);

    // B != G on the first data tile pixel.
    {
        std::vector<std::byte> mutated = frame;
        SetFramePixel(mutated, tileX, tileY, 8, 9, 8,
            kReferenceAlphaValue);
        const auto result = DecodeReferenceFrame(mutated);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::ChromaChannelMismatch);
        CHECK(result.Error().offset ==
            static_cast<std::size_t>(tileY) * kReferenceCanvasWidth +
            tileX);
    }
    // G != R in a control lane symbol pixel.
    {
        std::vector<std::byte> mutated = frame;
        SetFramePixel(mutated, 0, 24, 8, 8, 9, kReferenceAlphaValue);
        const auto result = DecodeReferenceFrame(mutated);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::ChromaChannelMismatch);
        CHECK(result.Error().offset == 24u * kReferenceCanvasWidth);
    }
    // Alpha != 255 in the data tile.
    {
        std::vector<std::byte> mutated = frame;
        SetFramePixel(mutated, tileX, tileY, 8, 8, 8,
            kReferenceAlphaValue - 1);
        const auto result = DecodeReferenceFrame(mutated);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::AlphaChannelViolation);
        CHECK(result.Error().offset ==
            static_cast<std::size_t>(tileY) * kReferenceCanvasWidth +
            tileX);
    }
    // Alpha violation in a bootstrap lane pixel.
    {
        std::vector<std::byte> mutated = frame;
        SetFramePixel(mutated, 8, 16, 8, 8, 8, kReferenceAlphaValue - 1);
        const auto result = DecodeReferenceFrame(mutated);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::AlphaChannelViolation);
        CHECK(result.Error().offset == 16u * kReferenceCanvasWidth + 8);
    }
    // A frozen-region channel violation surfaces through the byte-exact
    // frozen check at the first canvas pixel.
    {
        std::vector<std::byte> mutated = frame;
        SetFramePixel(mutated, 0, 0, 0, 1, 0, kReferenceAlphaValue);
        const auto result = DecodeReferenceFrame(mutated);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::FrozenRegionMismatch);
        CHECK(result.Error().offset == 0);
    }
}

TEST_CASE("Reserved lane positions must carry the frozen symbol 0",
    "[pbmodulation][raster][reserved]")
{
    const std::vector<std::byte> frame =
        EncodePayload(MakeZeroGoldenPayload());

    // Bootstrap A reserved lane position 100 (x 800, y 16).
    {
        std::vector<std::byte> mutated = frame;
        FillLaneSymbol(mutated, 800, 16, 248);
        const auto result = DecodeReferenceFrame(mutated);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::NonZeroReservedByte);
        CHECK(result.Error().offset ==
            16u * kReferenceCanvasWidth + 800);
    }
    // Bootstrap B reserved lane position 10 (x 80, y 1032).
    {
        std::vector<std::byte> mutated = frame;
        FillLaneSymbol(mutated, 80, 1032, 24);
        const auto result = DecodeReferenceFrame(mutated);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            ModulationErrorCode::NonZeroReservedByte);
        CHECK(result.Error().offset ==
            1032u * kReferenceCanvasWidth + 80);
    }
    // Reserved positions carry the canonical symbol 0 (level 8) in an
    // encoded frame.
    CHECK(PixelLuma(frame, 800, 16) == 8);
    CHECK(PixelLuma(frame, 80, 1032) == 8);
    // The lane tail of A (positions 88..239) is reserved as well.
    CHECK(PixelLuma(frame, 1912, 16) == 8);
}
TEST_CASE("Single-bit payload mutations map to exactly one symbol block",
    "[pbmodulation][raster][bit-flip]")
{
    const GoldenFramePayload base = MakeCanonicalGoldenPayload();
    const std::vector<std::byte> baseFrame = EncodePayload(base);

    // Data lane: every bit of the first 16 payload bytes. Stream bit
    // (byteIndex, bitIndex) belongs to symbol (8*byteIndex + bitIndex) / 4.
    for (std::size_t byteIndex = 0; byteIndex < 16; byteIndex++)
    {
        for (std::uint8_t bitIndex = 0; bitIndex < 8; bitIndex++)
        {
            GoldenFramePayload mutated = base;
            const std::uint8_t bitMask =
                static_cast<std::uint8_t>(1u << bitIndex);
            mutated.data[byteIndex] = std::byte{static_cast<std::uint8_t>(
                std::to_integer<std::uint8_t>(base.data[byteIndex]) ^
                bitMask)};
            const std::vector<std::byte> mutatedFrame =
                EncodePayload(mutated);
            const DecodedReferenceFrame decoded =
                DecodeFrameOrDie(mutatedFrame);
            CHECK(decoded.data == mutated.data);
            CHECK(decoded.bootstrapRecord == base.bootstrap);
            CHECK(decoded.controlWindow == base.control);

            const std::size_t symbolIndex =
                (byteIndex * 8u + static_cast<std::size_t>(bitIndex)) / 4u;
            const auto [tileX, tileY] = GetDataTileOrigin(
                static_cast<std::uint32_t>(
                    symbolIndex % kReferenceDataGridColumns),
                static_cast<std::uint32_t>(
                    symbolIndex / kReferenceDataGridColumns));
            CHECK(CountDifferingPixels(baseFrame, mutatedFrame) == 16);
            for (std::uint32_t row = 0; row < 4; row++)
            {
                for (std::uint32_t col = 0; col < 4; col++)
                {
                    const std::size_t offset =
                        (static_cast<std::size_t>(tileY + row) *
                            kReferenceCanvasWidth) +
                        (tileX + col);
                    CHECK(baseFrame[offset * 4] !=
                        mutatedFrame[offset * 4]);
                }
            }
        }
    }

    // Control lane: every bit of the first 8 window bytes.
    for (std::size_t byteIndex = 0; byteIndex < 8; byteIndex++)
    {
        for (std::uint8_t bitIndex = 0; bitIndex < 8; bitIndex++)
        {
            GoldenFramePayload mutated = base;
            const std::uint8_t bitMask =
                static_cast<std::uint8_t>(1u << bitIndex);
            mutated.control[byteIndex] = std::byte{static_cast<std::uint8_t>(
                std::to_integer<std::uint8_t>(base.control[byteIndex]) ^
                bitMask)};
            const std::vector<std::byte> mutatedFrame =
                EncodePayload(mutated);
            const DecodedReferenceFrame decoded =
                DecodeFrameOrDie(mutatedFrame);
            CHECK(decoded.controlWindow == mutated.control);
            CHECK(decoded.data == base.data);

            const std::size_t symbolIndex =
                (byteIndex * 8u + static_cast<std::size_t>(bitIndex)) / 4u;
            const auto [symbolX, symbolY] = GetLaneSymbolOrigin(
                24u, static_cast<std::uint32_t>(symbolIndex));
            CHECK(CountDifferingPixels(baseFrame, mutatedFrame) == 64);
            for (std::uint32_t row = 0; row < 8; row++)
            {
                for (std::uint32_t col = 0; col < 8; col++)
                {
                    const std::size_t offset =
                        (static_cast<std::size_t>(symbolY + row) *
                            kReferenceCanvasWidth) +
                        (symbolX + col);
                    CHECK(baseFrame[offset * 4] !=
                        mutatedFrame[offset * 4]);
                }
            }
        }
    }

    // Bootstrap record: every bit of the first 4 bytes mutates both the
    // A and the B copy (128 differing pixels).
    for (std::size_t byteIndex = 0; byteIndex < 4; byteIndex++)
    {
        for (std::uint8_t bitIndex = 0; bitIndex < 8; bitIndex++)
        {
            GoldenFramePayload mutated = base;
            const std::uint8_t bitMask =
                static_cast<std::uint8_t>(1u << bitIndex);
            mutated.bootstrap[byteIndex] = std::byte{
                static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(
                    base.bootstrap[byteIndex]) ^ bitMask)};
            const std::vector<std::byte> mutatedFrame =
                EncodePayload(mutated);
            const DecodedReferenceFrame decoded =
                DecodeFrameOrDie(mutatedFrame);
            CHECK(decoded.bootstrapRecord == mutated.bootstrap);
            CHECK(decoded.controlWindow == base.control);

            const std::size_t symbolIndex =
                (byteIndex * 8u + static_cast<std::size_t>(bitIndex)) / 4u;
            const auto [symbolAX, symbolAY] =
                GetLaneSymbolOrigin(16u,
                    static_cast<std::uint32_t>(symbolIndex));
            const std::size_t bSymbol =
                kReferenceBootstrapBSymbolOffset + symbolIndex;
            const auto [symbolBX, symbolBY] =
                GetLaneSymbolOrigin(1032u, static_cast<std::uint32_t>(
                    bSymbol));
            CHECK(CountDifferingPixels(baseFrame, mutatedFrame) == 128);
            for (std::uint32_t row = 0; row < 8; row++)
            {
                for (std::uint32_t col = 0; col < 8; col++)
                {
                    const std::size_t offsetA =
                        (static_cast<std::size_t>(symbolAY + row) *
                            kReferenceCanvasWidth) +
                        (symbolAX + col);
                    const std::size_t offsetB =
                        (static_cast<std::size_t>(symbolBY + row) *
                            kReferenceCanvasWidth) +
                        (symbolBX + col);
                    CHECK(baseFrame[offsetA * 4] !=
                        mutatedFrame[offsetA * 4]);
                    CHECK(baseFrame[offsetB * 4] !=
                        mutatedFrame[offsetB * 4]);
                }
            }
        }
    }
}

TEST_CASE("Allocating and non-allocating decodes agree",
    "[pbmodulation][raster][api-consistency]")
{
    const std::vector<std::byte> frame =
        EncodePayload(MakeCanonicalGoldenPayload());
    const DecodedReferenceFrame decoded = DecodeFrameOrDie(frame);

    std::array<std::byte, kReferenceBootstrapRecordBytes> outBootstrap{};
    std::array<std::byte, kReferenceControlWindowBytes> outControl{};
    std::vector<std::byte> outData(kReferenceDataRegionBytes);
    const auto status = DecodeReferenceFrameInto(
        frame, std::span<std::byte>(outBootstrap),
        std::span<std::byte>(outControl), std::span<std::byte>(outData));
    CHECK(status);
    CHECK(outBootstrap == decoded.bootstrapRecord);
    CHECK(outControl == decoded.controlWindow);
    CHECK(outData == decoded.data);
}
