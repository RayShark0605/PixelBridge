#include "local_desktop_resample_fixtures.h"
#include "local_desktop_test_fixtures.h"
#include "pbmodulation/local_desktop_decode.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

using namespace localdesktoptest;
using namespace pbmodulation;

namespace
{

using Erasure = LocalDesktopErasureReason;

// Full-scale region geometry, mirroring the frozen layout constants: 76
// columns x 8 rows of 8-pixel cells. Cell (column, row) is bit `row` of
// byte `column` in the frozen LSB-first layout.
struct Region
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};

constexpr std::array<Region, 2> bootstrapRegions{{Region{96, 16}, Region{1216, 1000}}};
constexpr std::uint32_t kRegionCellsX = 76;
constexpr std::uint32_t kRegionCellsY = 8;

// Correlated partial-refresh model. A real partial update refreshes only the
// cells that changed between the old and the new frame, and even that
// refresh can land only partially. The torn pattern is therefore selected
// from the cells where the two frames actually differ, restricted to one
// fixed stride class so that a proper subset of the changed cells stays
// unrefreshed. The torn region is then neither frame's codeword.
constexpr bool PartialRefreshSelected(const std::uint32_t cellColumn, const std::uint32_t cellRow,
                                      const bool changedCell, const std::uint32_t strideClass) noexcept
{
    return changedCell && (cellColumn * 7u + cellRow) % 3u == strideClass;
}

// Replaces the pattern-selected 8x8 cells of one region with the other
// frame's pixels. Returns the number of pixels actually changed.
std::uint32_t GraftPartialRefresh(GrayImage& base, const GrayImage& other, const std::size_t copyIndex,
                                  const std::uint32_t strideClass)
{
    if (copyIndex >= bootstrapRegions.size())
    {
        throw std::runtime_error("Partial-refresh fixture addressed a third bootstrap region");
    }
    const auto& region = bootstrapRegions[copyIndex];
    std::uint32_t changedPixels = 0;
    for (std::uint32_t cellRow = 0; cellRow < kRegionCellsY; cellRow++)
    {
        for (std::uint32_t cellColumn = 0; cellColumn < kRegionCellsX; cellColumn++)
        {
            const auto baseLevel = base.Read(region.x + cellColumn * 8 + 4, region.y + cellRow * 8 + 4);
            const auto otherLevel = other.Read(region.x + cellColumn * 8 + 4, region.y + cellRow * 8 + 4);
            if (!PartialRefreshSelected(cellColumn, cellRow, baseLevel != otherLevel, strideClass))
            {
                continue;
            }
            for (std::uint32_t offsetRow = 0; offsetRow < 8; offsetRow++)
            {
                for (std::uint32_t offsetColumn = 0; offsetColumn < 8; offsetColumn++)
                {
                    const auto foreignLevel = other.Read(region.x + cellColumn * 8 + offsetColumn, region.y + cellRow * 8 + offsetRow);
                    base.Write(region.x + cellColumn * 8 + offsetColumn, region.y + cellRow * 8 + offsetRow, foreignLevel);
                    changedPixels++;
                }
            }
        }
    }
    return changedPixels;
}

// Independent symbol-level oracle computed from the pinned golden codewords
// only (no production RS, raster, or serializer code). For each byte
// position: the torn word differs from the base codeword exactly when some
// selected differing cell landed there, and differs from the other
// codeword exactly when some differing cell did not.
struct PartialRefreshDistance
{
    std::uint32_t distanceToBase = 0;
    std::uint32_t distanceToOther = 0;
};

PartialRefreshDistance MeasurePartialRefreshDistance(const std::span<const std::byte> baseWord,
                                                     const std::span<const std::byte> otherWord,
                                                     const std::uint32_t strideClass)
{
    if (baseWord.size() != kLocalDesktopRsCodewordBytes || otherWord.size() != kLocalDesktopRsCodewordBytes)
    {
        throw std::runtime_error("Partial-refresh oracle received malformed codewords");
    }
    PartialRefreshDistance result;
    for (std::uint32_t byteIndex = 0; byteIndex < kRegionCellsX; byteIndex++)
    {
        bool baseHit = false;
        bool otherHit = false;
        for (std::uint32_t bitIndex = 0; bitIndex < kRegionCellsY; bitIndex++)
        {
            const auto baseBit = (std::to_integer<std::uint8_t>(baseWord[byteIndex]) >> bitIndex) & 1u;
            const auto otherBit = (std::to_integer<std::uint8_t>(otherWord[byteIndex]) >> bitIndex) & 1u;
            if (baseBit == otherBit)
            {
                continue;
            }
            if (PartialRefreshSelected(byteIndex, bitIndex, true, strideClass))
            {
                baseHit = true;
            }
            else
            {
                otherHit = true;
            }
        }
        result.distanceToBase += baseHit ? 1u : 0u;
        result.distanceToOther += otherHit ? 1u : 0u;
    }
    return result;
}

void CheckErasedFrame(const LocalDesktopObservation& observation, const char* const label)
{
    INFO("erasure=" << GetLocalDesktopErasureName(observation.erasure) << ' ' << label << " work=" << observation.workUnits);
    CHECK_FALSE(observation.IsAccepted());
    CHECK(observation.canonical44 == std::array<std::byte, kLocalDesktopBootstrapRecordBytes>{});
    CHECK(observation.quality == 0);
    CHECK(observation.workUnits <= LocalDesktopDecodePolicy{}.maximumWorkUnits);
}

void CheckIntactCopy(const LocalDesktopCopyObservation& copy, const std::array<std::byte, 44>& expected, const char* const label)
{
    INFO(label << " corrected=" << copy.correctedSymbols << " residual=" << copy.residual);
    CHECK(copy.fecDecoded);
    CHECK(copy.crcValid);
    CHECK(copy.recordValid);
    CHECK(copy.canonical44 == expected);
    CHECK(copy.correctedSymbols == 0);
}

// The torn region lies outside the 16-symbol correction radius of both
// intact codewords, so its copy must never report the base record. If RS
// "corrects" the torn word to a third codeword, that record is either
// CRC-invalid (flags below) or a different record caught by the A/B
// agreement gate; both outcomes are frame erasures asserted by the caller.
void CheckTornCopyNotBase(const LocalDesktopCopyObservation& copy, const std::array<std::byte, 44>& baseRecord, const char* const label)
{
    INFO(label << " fec=" << copy.fecDecoded << " crc=" << copy.crcValid << " record=" << copy.recordValid);
    CHECK(copy.canonical44 != baseRecord);
}

void CheckPartialRefreshOracle(const std::span<const std::byte> baseWord, const std::span<const std::byte> otherWord,
                               const std::uint32_t strideClass)
{
    const auto distance = MeasurePartialRefreshDistance(baseWord, otherWord, strideClass);
    REQUIRE(distance.distanceToBase > 16);
    REQUIRE(distance.distanceToOther > 16);
}

} // namespace

TEST_CASE("LocalDesktop correlated partial refresh of region A erases while intact B independently recovers",
         "[localdesktop][torn][partial]")
{
    const auto base = GrayFromGolden(MakeGoldenRaster("a"));
    const auto other = GrayFromGolden(MakeGoldenRaster("e"));
    const auto expectedRecord = LoadGoldenRecord("a");
    const auto baseWord = LoadGoldenBytes("a-rs76.bin", 76);
    const auto otherWord = LoadGoldenBytes("e-rs76.bin", 76);
    CheckPartialRefreshOracle(baseWord, otherWord, 0u);

    auto torn = base;
    const auto changedPixels = GraftPartialRefresh(torn, other, 0, 0u);
    REQUIRE(changedPixels > 0);

    const auto observation = DecodeLocalDesktopBootstrap(torn.View());
    CheckErasedFrame(observation, "partial-refresh-A");
    CheckTornCopyNotBase(observation.copies[0], expectedRecord, "torn-A");
    CheckIntactCopy(observation.copies[1], expectedRecord, "intact-B");
}

TEST_CASE("LocalDesktop correlated partial refresh of region B erases while intact A independently recovers",
         "[localdesktop][torn][partial]")
{
    const auto base = GrayFromGolden(MakeGoldenRaster("a"));
    const auto other = GrayFromGolden(MakeGoldenRaster("e"));
    const auto expectedRecord = LoadGoldenRecord("a");
    const auto baseWord = LoadGoldenBytes("a-rs76.bin", 76);
    const auto otherWord = LoadGoldenBytes("e-rs76.bin", 76);
    CheckPartialRefreshOracle(baseWord, otherWord, 1u);

    auto torn = base;
    const auto changedPixels = GraftPartialRefresh(torn, other, 1, 1u);
    REQUIRE(changedPixels > 0);

    const auto observation = DecodeLocalDesktopBootstrap(torn.View());
    CheckErasedFrame(observation, "partial-refresh-B");
    CheckIntactCopy(observation.copies[0], expectedRecord, "intact-A");
    CheckTornCopyNotBase(observation.copies[1], expectedRecord, "torn-B");
}

TEST_CASE("LocalDesktop different partial refreshes in both regions erase with no accepted codeword",
         "[localdesktop][torn][partial]")
{
    const auto base = GrayFromGolden(MakeGoldenRaster("a"));
    const auto other = GrayFromGolden(MakeGoldenRaster("e"));
    const auto expectedRecord = LoadGoldenRecord("a");
    const auto baseWord = LoadGoldenBytes("a-rs76.bin", 76);
    const auto otherWord = LoadGoldenBytes("e-rs76.bin", 76);
    CheckPartialRefreshOracle(baseWord, otherWord, 0u);
    CheckPartialRefreshOracle(baseWord, otherWord, 1u);

    auto torn = base;
    const auto changedA = GraftPartialRefresh(torn, other, 0, 0u);
    const auto changedB = GraftPartialRefresh(torn, other, 1, 1u);
    REQUIRE(changedA > 0);
    REQUIRE(changedB > 0);

    const auto observation = DecodeLocalDesktopBootstrap(torn.View());
    CheckErasedFrame(observation, "partial-refresh-both");
    CheckTornCopyNotBase(observation.copies[0], expectedRecord, "torn-A");
    CheckTornCopyNotBase(observation.copies[1], expectedRecord, "torn-B");
}

TEST_CASE("LocalDesktop three corrupted finder modules erase as incomplete markers", "[localdesktop][torn][partial][markers]")
{
    auto base = GrayFromGolden(MakeGoldenRaster("a"));
    // TL marker origin (16,16): module (row, column) occupies the 8x8 block
    // at (16 + 4 + column*8, 16 + 4 + row*8). These three interior modules
    // stay clear of the central finder cross (row 3 / column 3) and of the
    // corner role bits, so the cross is still detected and the failure must
    // surface as marker verification, not as marker absence.
    constexpr std::array<std::array<std::uint32_t, 2>, 3> modules{{{1, 1}, {1, 5}, {5, 1}}};
    for (const auto& module : modules)
    {
        for (std::uint32_t offsetRow = 0; offsetRow < 8; offsetRow++)
        {
            for (std::uint32_t offsetColumn = 0; offsetColumn < 8; offsetColumn++)
            {
                const std::uint32_t pixelX = 16 + 4 + module[1] * 8 + offsetColumn;
                const std::uint32_t pixelY = 16 + 4 + module[0] * 8 + offsetRow;
                const auto level = base.Read(pixelX, pixelY);
                REQUIRE((level == 32u || level == 224u));
                base.Write(pixelX, pixelY, level == 32u ? 224u : 32u);
            }
        }
    }

    const auto observation = DecodeLocalDesktopBootstrap(base.View());
    INFO("erasure=" << GetLocalDesktopErasureName(observation.erasure) << " markers=" << observation.markerCandidates);
    CHECK_FALSE(observation.IsAccepted());
    CHECK(observation.erasure == Erasure::IncompleteMarkers);
    CHECK(observation.canonical44 == std::array<std::byte, kLocalDesktopBootstrapRecordBytes>{});
    CHECK(observation.quality == 0);
    CHECK(observation.workUnits <= LocalDesktopDecodePolicy{}.maximumWorkUnits);
}