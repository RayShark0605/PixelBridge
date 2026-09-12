#pragma once

#include "pbmodulation/unified_visual_profile.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace pbmodulation
{

inline constexpr std::uint32_t kUnifiedMappingVersion = 3;
inline constexpr std::uint32_t kUnifiedMappingSequencePeriod = 16;
inline constexpr std::uint32_t kUnifiedBaseLumaBits = 145800;
inline constexpr std::uint32_t kUnifiedFineLumaBits = 16200;
inline constexpr std::uint32_t kUnifiedChromaBits = 81000;
inline constexpr std::uint32_t kUnifiedLumaCarrierPlanes = 4;
inline constexpr std::uint32_t kUnifiedChromaCarrierPlanes = 2;
inline constexpr std::uint32_t kUnifiedLumaDeficitBits = 3816;
inline constexpr std::uint32_t kUnifiedLumaSurplusBits = 25504;
inline constexpr std::uint32_t kUnifiedUnusedLumaCarrierBits = 5488;
inline constexpr std::uint32_t kUnifiedUnusedChromaCarrierBits = 2744;
// Bit 0 enables a FrameSequence permutation contained inside each codeword,
// bit 1 assigns each Base codeword primarily to its matching freshness region,
// and bit 2 concatenates Chroma sites in kUnifiedChromaRegionOrder.
inline constexpr std::uint32_t kUnifiedMappingContractFlags = 0x00000007U;
inline constexpr std::string_view kUnifiedMappingStreamFrameSequenceZeroBlake3{
    "4b06ec15c338a4f18502b49a7d5947bd33b4e79fd1a7f73c53c682dfa2283639"};

// Label n is encoded by mask[n]. Bit n within a mask addresses the row-major
// 5x5 chip at (n % 5, n / 5); one selects the foreground carrier. The sixth
// row and column are fixed dark separators. Complement pairs and a minimum
// Hamming distance of nine preserve shape identity after lossy resampling.
inline constexpr std::array<std::uint32_t, 16> kUnifiedSymbolMasksByLabel{
    0x0F6A100U, 0x004A5BFU, 0x00F731CU, 0x0073C61U,
    0x0F78E3FU, 0x00218CEU, 0x0319980U, 0x033BDEFU,
    0x1095EFFU, 0x1FB5A40U, 0x1F08CE3U, 0x1F8C39EU,
    0x10871C0U, 0x1FDE731U, 0x1CE667FU, 0x1CC4210U};

struct UnifiedChromaState
{
    std::uint8_t blue = 0;
    std::uint8_t green = 0;
    std::uint8_t red = 0;
    std::uint8_t label = 0;

    bool operator==(const UnifiedChromaState&) const = default;
};

// Strong, approximately iso-luma foreground states separate the two chroma
// bits from the dark background. The values stay away from gamut endpoints so
// video range conversion and ringing do not immediately clip the carrier.
inline constexpr std::array<UnifiedChromaState, 4> kUnifiedChromaStatesByLabel{
    UnifiedChromaState{253, 127, 239, 0},
    UnifiedChromaState{253, 174, 81, 1},
    UnifiedChromaState{67, 193, 81, 2},
    UnifiedChromaState{67, 146, 239, 3}};

// Gray-state variant (experimental layout 12): the same 2 bits per tile ride
// four achromatic foreground levels instead of opponent chroma, so the
// second carrier survives 4:2:0 chroma destruction on remote video links.
// Levels are spaced 56/56/80 apart, stay >= 80 above the dark background 8 and 7
// below gamut end 255, and the minimum state gap comfortably exceeds the
// policy's minimumLumaLevelGate (32) after channel deviation (<= 8).
inline constexpr std::array<UnifiedChromaState, 4> kUnifiedGrayStatesByLabel{
    UnifiedChromaState{56, 56, 56, 0},
    UnifiedChromaState{112, 112, 112, 1},
    UnifiedChromaState{168, 168, 168, 2},
    UnifiedChromaState{248, 248, 248, 3}};

// Gray-state carrier v2 (experimental layout 12): a 64-symbol, 25-chip
// punctured-Hadamard mask codebook (32x32 Sylvester rows plus complements,
// seven columns greedily removed; minimum pairwise Hamming distance 9,
// complement pairs intact, symbol 32 is the all-background mask whose
// tiles self-erase like sampling failures). Six bits per data tile carry
// ALL fifteen codewords on the mask carrier - the level-state encoding
// proved unresolvable at degraded-link SNR (2026-09-12), while the mask
// carrier ran the whole day with zero FEC failures.
inline constexpr std::array<std::uint32_t, 64> kUnifiedGrayMasksBySymbol{
    0x1FFFFFF, 0x0AAAAAA, 0x0666666, 0x1333333, 0x01E1E1E, 0x14B4B4B, 0x1878787, 0x0D2D2D2,
    0x001FE01, 0x154AB54, 0x1986798, 0x0CD32CD, 0x1E01FE0, 0x0B54AB5, 0x0798679, 0x12CD32C,
    0x00001FF, 0x15554AA, 0x1999866, 0x0CCCD33, 0x1E1E01E, 0x0B4B54B, 0x0787987, 0x12D2CD2,
    0x1FE0001, 0x0AB5554, 0x0679998, 0x132CCCD, 0x01FE1E0, 0x14AB4B5, 0x1867879, 0x0D32D2C,
    0x0000000, 0x1555555, 0x1999999, 0x0CCCCCC, 0x1E1E1E1, 0x0B4B4B4, 0x0787878, 0x12D2D2D,
    0x1FE01FE, 0x0AB54AB, 0x0679867, 0x132CD32, 0x01FE01F, 0x14AB54A, 0x1867986, 0x0D32CD3,
    0x1FFFE00, 0x0AAAB55, 0x0666799, 0x13332CC, 0x01E1FE1, 0x14B4AB4, 0x1878678, 0x0D2D32D,
    0x001FFFE, 0x154AAAB, 0x1986667, 0x0CD3332, 0x1E01E1F, 0x0B54B4A, 0x0798786, 0x12CD2D3,
};
// Carrier v3: every data tile carries seven interleaved planes - the six
// punctured-Hadamard mask bits plus one foreground-level bit rendered as
// one of two ladder reference levels (64/232). The ladder is re-measured
// every frame, so the level bit self-calibrates against the provider
// monotone luma remap; the nominal 168-luma gap sits above seven sigmas
// of the glyph-foreground noise measured on degraded links.
inline constexpr std::uint32_t kUnifiedGrayCarrierPlanes = 7;
inline constexpr std::uint32_t kUnifiedGrayCodewordCount = 18;
inline constexpr std::uint64_t kUnifiedGrayCarrierBits =
    static_cast<std::uint64_t>(kUnifiedVisualProfile.dataTileCount) * kUnifiedGrayCarrierPlanes;
inline constexpr std::uint64_t kUnifiedGrayLogicalBits =
    static_cast<std::uint64_t>(kUnifiedGrayCodewordCount) * 16200;
inline constexpr std::uint32_t kUnifiedGrayActiveTiles =
    static_cast<std::uint32_t>((kUnifiedGrayLogicalBits + kUnifiedGrayCarrierPlanes - 1) /
        kUnifiedGrayCarrierPlanes);
inline constexpr std::uint32_t kUnifiedGrayLowForegroundLuma = 64;
inline constexpr std::uint32_t kUnifiedGrayHighForegroundLuma = 232;

struct UnifiedLaneMappingContract
{
    UnifiedLane lane = UnifiedLane::BaseLuma;
    std::uint32_t logicalBits = 0;

    bool operator==(const UnifiedLaneMappingContract&) const = default;
};

inline constexpr std::array<UnifiedLaneMappingContract, 3> kUnifiedLaneMappings{
    UnifiedLaneMappingContract{UnifiedLane::BaseLuma, kUnifiedBaseLumaBits},
    UnifiedLaneMappingContract{UnifiedLane::FineLuma, kUnifiedFineLumaBits},
    UnifiedLaneMappingContract{UnifiedLane::Chroma, kUnifiedChromaBits}};

struct UnifiedCodewordInterleaveContract
{
    std::uint32_t modulus = 0;
    std::uint32_t multiplier = 0;
    std::uint32_t inverse = 0;
    std::uint32_t offset = 0;
    std::uint32_t phaseStep = 0;
    std::uint32_t phaseCount = 0;

    bool operator==(const UnifiedCodewordInterleaveContract&) const = default;
};

inline constexpr UnifiedCodewordInterleaveContract kUnifiedCodewordInterleave{
    16200, 16067, 5603, 7919, 1009, kUnifiedMappingSequencePeriod};

struct UnifiedPhysicalCarrierSite
{
    bool valid = false;
    UnifiedCarrier carrier = UnifiedCarrier::Luma;
    std::uint32_t tileOrdinal = 0;
    std::uint8_t bitPlane = 0;

    bool operator==(const UnifiedPhysicalCarrierSite&) const = default;
};

struct UnifiedLogicalCarrierBit
{
    bool valid = false;
    UnifiedLane lane = UnifiedLane::BaseLuma;
    std::uint32_t logicalBit = 0;

    bool operator==(const UnifiedLogicalCarrierBit&) const = default;
};

inline constexpr std::uint32_t kUnifiedMappingFreshnessRegionCount = 9;
inline constexpr std::array<std::uint32_t, 2> kUnifiedMappingFreshnessColumnBoundaries{560, 1360};
inline constexpr std::array<std::uint32_t, 2> kUnifiedMappingFreshnessRowBoundaries{382, 698};
inline constexpr std::array<std::uint8_t, kUnifiedMappingFreshnessRegionCount> kUnifiedChromaRegionOrder{
    0, 1, 2, 3, 6, 8, 4, 5, 7};

struct UnifiedFreshnessTileCatalog
{
    std::array<std::uint32_t, kUnifiedVisualProfile.dataTileCount> groupedTileOrdinals{};
    std::array<std::uint32_t, kUnifiedVisualProfile.dataTileCount> rankByTileOrdinal{};
    std::array<std::uint8_t, kUnifiedVisualProfile.dataTileCount> regionByTileOrdinal{};
    std::array<std::uint32_t, kUnifiedMappingFreshnessRegionCount> tileCounts{};
    std::array<std::uint32_t, kUnifiedMappingFreshnessRegionCount + 1> groupedOffsets{};
};

[[nodiscard]] consteval std::uint8_t GetUnifiedMappingFreshnessRegion(
    const std::uint32_t centerX, const std::uint32_t centerY) noexcept
{
    const std::uint8_t column = centerX < kUnifiedMappingFreshnessColumnBoundaries[0] ? 0 :
        centerX < kUnifiedMappingFreshnessColumnBoundaries[1] ? 1 : 2;
    const std::uint8_t row = centerY < kUnifiedMappingFreshnessRowBoundaries[0] ? 0 :
        centerY < kUnifiedMappingFreshnessRowBoundaries[1] ? 1 : 2;
    return static_cast<std::uint8_t>(row * 3 + column);
}

[[nodiscard]] consteval UnifiedFreshnessTileCatalog BuildUnifiedFreshnessTileCatalog() noexcept
{
    UnifiedFreshnessTileCatalog catalog;
    std::uint32_t tileOrdinal = 0;
    for (const UnifiedRegionContract& contract : kUnifiedVisualProfile.regions)
    {
        if (contract.kind != UnifiedRegionKind::Data)
        {
            continue;
        }
        const std::uint32_t tilesPerRow = contract.bounds.width / kUnifiedVisualProfile.tileWidth;
        const std::uint32_t tileRows = contract.bounds.height / kUnifiedVisualProfile.tileHeight;
        for (std::uint32_t tileRow = 0; tileRow < tileRows; tileRow++)
        {
            for (std::uint32_t tileColumn = 0; tileColumn < tilesPerRow; tileColumn++)
            {
                const std::uint32_t centerX = contract.bounds.x + tileColumn * kUnifiedVisualProfile.tileWidth +
                    kUnifiedVisualProfile.tileWidth / 2;
                const std::uint32_t centerY = contract.bounds.y + tileRow * kUnifiedVisualProfile.tileHeight +
                    kUnifiedVisualProfile.tileHeight / 2;
                const std::uint8_t region = GetUnifiedMappingFreshnessRegion(centerX, centerY);
                catalog.regionByTileOrdinal[tileOrdinal] = region;
                catalog.rankByTileOrdinal[tileOrdinal] = catalog.tileCounts[region];
                catalog.tileCounts[region]++;
                tileOrdinal++;
            }
        }
    }
    for (std::size_t region = 0; region < catalog.tileCounts.size(); region++)
    {
        catalog.groupedOffsets[region + 1] = catalog.groupedOffsets[region] + catalog.tileCounts[region];
    }
    std::array<std::uint32_t, kUnifiedMappingFreshnessRegionCount> cursors{};
    for (std::size_t region = 0; region < cursors.size(); region++)
    {
        cursors[region] = catalog.groupedOffsets[region];
    }
    for (std::uint32_t sourceTile = 0; sourceTile < tileOrdinal; sourceTile++)
    {
        const std::uint8_t region = catalog.regionByTileOrdinal[sourceTile];
        catalog.groupedTileOrdinals[cursors[region]] = sourceTile;
        cursors[region]++;
    }
    return catalog;
}

inline constexpr UnifiedFreshnessTileCatalog kUnifiedFreshnessTileCatalog = BuildUnifiedFreshnessTileCatalog();
inline constexpr std::array<std::uint32_t, kUnifiedMappingFreshnessRegionCount> kUnifiedExpectedFreshnessTileCounts{
    3850, 5836, 3850, 4291, 6506, 4291, 3773, 5702, 3773};

namespace unified_mapping_detail
{

inline constexpr std::uint32_t kCodewordBits = kUnifiedVisualProfile.innerCodewordBits;

[[nodiscard]] constexpr std::uint32_t MultiplyModulo(
    const std::uint32_t left, const std::uint32_t right, const std::uint32_t modulus) noexcept
{
    return modulus == 0 ? 0 : static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(left) * right) % modulus);
}

[[nodiscard]] constexpr std::uint32_t AddModulo(
    const std::uint32_t left, const std::uint32_t right, const std::uint32_t modulus) noexcept
{
    return modulus == 0 ? 0 : static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(left) + right) % modulus);
}

[[nodiscard]] constexpr std::uint32_t PermuteLaneCodewordBit(
    const std::uint32_t logicalBit, const std::uint64_t frameSequence) noexcept
{
    const std::uint32_t codewordSlot = logicalBit / kCodewordBits;
    const std::uint32_t codewordBit = logicalBit % kCodewordBits;
    const std::uint32_t phase = static_cast<std::uint32_t>(frameSequence % kUnifiedCodewordInterleave.phaseCount);
    const std::uint32_t phaseOffset = AddModulo(kUnifiedCodewordInterleave.offset,
        MultiplyModulo(phase, kUnifiedCodewordInterleave.phaseStep, kCodewordBits), kCodewordBits);
    const std::uint32_t domainBit = AddModulo(
        MultiplyModulo(codewordBit, kUnifiedCodewordInterleave.multiplier, kCodewordBits), phaseOffset, kCodewordBits);
    return codewordSlot * kCodewordBits + domainBit;
}

[[nodiscard]] constexpr std::uint32_t InvertLaneCodewordBit(
    const std::uint32_t domainLogicalBit, const std::uint64_t frameSequence) noexcept
{
    const std::uint32_t codewordSlot = domainLogicalBit / kCodewordBits;
    const std::uint32_t domainBit = domainLogicalBit % kCodewordBits;
    const std::uint32_t phase = static_cast<std::uint32_t>(frameSequence % kUnifiedCodewordInterleave.phaseCount);
    const std::uint32_t phaseOffset = AddModulo(kUnifiedCodewordInterleave.offset,
        MultiplyModulo(phase, kUnifiedCodewordInterleave.phaseStep, kCodewordBits), kCodewordBits);
    const std::uint32_t shifted = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(domainBit) + kCodewordBits - phaseOffset) % kCodewordBits);
    return codewordSlot * kCodewordBits + MultiplyModulo(
        shifted, kUnifiedCodewordInterleave.inverse, kCodewordBits);
}

[[nodiscard]] constexpr std::uint32_t GetPrimaryLumaBits(const std::uint32_t region) noexcept
{
    const std::uint32_t capacity = kUnifiedFreshnessTileCatalog.tileCounts[region] * kUnifiedLumaCarrierPlanes;
    return capacity < kCodewordBits ? capacity : kCodewordBits;
}

[[nodiscard]] constexpr std::uint32_t GetLumaDeficitPrefix(const std::uint32_t endRegion) noexcept
{
    std::uint32_t result = 0;
    for (std::uint32_t region = 0; region < endRegion; region++)
    {
        result += kCodewordBits - GetPrimaryLumaBits(region);
    }
    return result;
}

[[nodiscard]] constexpr std::uint32_t GetLumaSurplusPrefix(const std::uint32_t endRegion) noexcept
{
    std::uint32_t result = 0;
    for (std::uint32_t region = 0; region < endRegion; region++)
    {
        const std::uint32_t capacity = kUnifiedFreshnessTileCatalog.tileCounts[region] * kUnifiedLumaCarrierPlanes;
        result += capacity > kCodewordBits ? capacity - kCodewordBits : 0;
    }
    return result;
}

[[nodiscard]] constexpr UnifiedPhysicalCarrierSite GetRegionLumaSite(
    const std::uint32_t region, const std::uint32_t regionBit) noexcept
{
    const std::uint32_t tileRank = regionBit / kUnifiedLumaCarrierPlanes;
    if (region >= kUnifiedMappingFreshnessRegionCount ||
        tileRank >= kUnifiedFreshnessTileCatalog.tileCounts[region])
    {
        return {};
    }
    const std::uint32_t tileOrdinal = kUnifiedFreshnessTileCatalog.groupedTileOrdinals[
        kUnifiedFreshnessTileCatalog.groupedOffsets[region] + tileRank];
    return {true, UnifiedCarrier::Luma, tileOrdinal,
        static_cast<std::uint8_t>(regionBit % kUnifiedLumaCarrierPlanes)};
}

[[nodiscard]] constexpr UnifiedPhysicalCarrierSite GetLumaSurplusSite(std::uint32_t surplusBit) noexcept
{
    for (std::uint32_t region = 0; region < kUnifiedMappingFreshnessRegionCount; region++)
    {
        const std::uint32_t capacity = kUnifiedFreshnessTileCatalog.tileCounts[region] * kUnifiedLumaCarrierPlanes;
        const std::uint32_t surplus = capacity > kCodewordBits ? capacity - kCodewordBits : 0;
        if (surplusBit < surplus)
        {
            return GetRegionLumaSite(region, kCodewordBits + surplusBit);
        }
        surplusBit -= surplus;
    }
    return {};
}

[[nodiscard]] constexpr UnifiedPhysicalCarrierSite GetRegionLocalizedBaseSite(
    const std::uint32_t logicalBit) noexcept
{
    const std::uint32_t codewordSlot = logicalBit / kCodewordBits;
    const std::uint32_t codewordBit = logicalBit % kCodewordBits;
    if (codewordSlot >= kUnifiedMappingFreshnessRegionCount)
    {
        return {};
    }
    const std::uint32_t primaryBits = GetPrimaryLumaBits(codewordSlot);
    if (codewordBit < primaryBits)
    {
        return GetRegionLumaSite(codewordSlot, codewordBit);
    }
    return GetLumaSurplusSite(GetLumaDeficitPrefix(codewordSlot) + codewordBit - primaryBits);
}

[[nodiscard]] constexpr UnifiedLogicalCarrierBit InvertRegionLocalizedLumaSite(
    const UnifiedPhysicalCarrierSite& site) noexcept
{
    const std::uint32_t region = kUnifiedFreshnessTileCatalog.regionByTileOrdinal[site.tileOrdinal];
    const std::uint32_t rank = kUnifiedFreshnessTileCatalog.rankByTileOrdinal[site.tileOrdinal];
    const std::uint32_t regionBit = rank * kUnifiedLumaCarrierPlanes + site.bitPlane;
    const std::uint32_t primaryBits = GetPrimaryLumaBits(region);
    if (regionBit < primaryBits)
    {
        return {true, UnifiedLane::BaseLuma, region * kCodewordBits + regionBit};
    }
    const std::uint32_t capacity = kUnifiedFreshnessTileCatalog.tileCounts[region] * kUnifiedLumaCarrierPlanes;
    if (regionBit >= capacity || capacity <= kCodewordBits)
    {
        return {};
    }
    const std::uint32_t surplusBit = GetLumaSurplusPrefix(region) + regionBit - kCodewordBits;
    const std::uint32_t totalDeficit = GetLumaDeficitPrefix(kUnifiedMappingFreshnessRegionCount);
    if (surplusBit < totalDeficit)
    {
        std::uint32_t remaining = surplusBit;
        for (std::uint32_t deficitRegion = 0; deficitRegion < kUnifiedMappingFreshnessRegionCount; deficitRegion++)
        {
            const std::uint32_t deficit = kCodewordBits - GetPrimaryLumaBits(deficitRegion);
            if (remaining < deficit)
            {
                return {true, UnifiedLane::BaseLuma,
                    deficitRegion * kCodewordBits + GetPrimaryLumaBits(deficitRegion) + remaining};
            }
            remaining -= deficit;
        }
        return {};
    }
    const std::uint32_t fineBit = surplusBit - totalDeficit;
    return fineBit < kCodewordBits ? UnifiedLogicalCarrierBit{true, UnifiedLane::FineLuma, fineBit} :
        UnifiedLogicalCarrierBit{};
}

[[nodiscard]] constexpr UnifiedPhysicalCarrierSite GetRegionOrderedChromaSite(std::uint32_t logicalBit) noexcept
{
    if (logicalBit >= kUnifiedChromaBits)
    {
        return {};
    }
    for (const std::uint8_t region : kUnifiedChromaRegionOrder)
    {
        const std::uint32_t capacity = kUnifiedFreshnessTileCatalog.tileCounts[region] * kUnifiedChromaCarrierPlanes;
        if (logicalBit < capacity)
        {
            const std::uint32_t tileRank = logicalBit / kUnifiedChromaCarrierPlanes;
            const std::uint32_t tileOrdinal = kUnifiedFreshnessTileCatalog.groupedTileOrdinals[
                kUnifiedFreshnessTileCatalog.groupedOffsets[region] + tileRank];
            return {true, UnifiedCarrier::Chroma, tileOrdinal,
                static_cast<std::uint8_t>(logicalBit % kUnifiedChromaCarrierPlanes)};
        }
        logicalBit -= capacity;
    }
    return {};
}

[[nodiscard]] constexpr UnifiedLogicalCarrierBit InvertRegionOrderedChromaSite(
    const UnifiedPhysicalCarrierSite& site) noexcept
{
    const std::uint32_t siteRegion = kUnifiedFreshnessTileCatalog.regionByTileOrdinal[site.tileOrdinal];
    std::uint32_t logicalBit = 0;
    for (const std::uint8_t region : kUnifiedChromaRegionOrder)
    {
        if (region == siteRegion)
        {
            logicalBit += kUnifiedFreshnessTileCatalog.rankByTileOrdinal[site.tileOrdinal] *
                kUnifiedChromaCarrierPlanes +
                site.bitPlane;
            return logicalBit < kUnifiedChromaBits ?
                UnifiedLogicalCarrierBit{true, UnifiedLane::Chroma, logicalBit} : UnifiedLogicalCarrierBit{};
        }
        logicalBit += kUnifiedFreshnessTileCatalog.tileCounts[region] * kUnifiedChromaCarrierPlanes;
    }
    return {};
}

[[nodiscard]] constexpr const UnifiedLaneMappingContract* FindLaneMapping(const UnifiedLane lane) noexcept
{
    for (const UnifiedLaneMappingContract& contract : kUnifiedLaneMappings)
    {
        if (contract.lane == lane)
        {
            return &contract;
        }
    }
    return nullptr;
}

[[nodiscard]] constexpr bool HasNoIsolatedCells(const std::uint32_t mask) noexcept
{
    for (std::uint32_t index = 0; index < 25; index++)
    {
        const bool value = ((mask >> index) & 1U) != 0;
        const std::uint32_t x = index % 5;
        const std::uint32_t y = index / 5;
        bool hasEqualNeighbor = false;
        if (x > 0)
        {
            hasEqualNeighbor = hasEqualNeighbor || (((mask >> (index - 1)) & 1U) != 0) == value;
        }
        if (x < 4)
        {
            hasEqualNeighbor = hasEqualNeighbor || (((mask >> (index + 1)) & 1U) != 0) == value;
        }
        if (y > 0)
        {
            hasEqualNeighbor = hasEqualNeighbor || (((mask >> (index - 5)) & 1U) != 0) == value;
        }
        if (y < 4)
        {
            hasEqualNeighbor = hasEqualNeighbor || (((mask >> (index + 5)) & 1U) != 0) == value;
        }
        if (!hasEqualNeighbor)
        {
            return false;
        }
    }
    return true;
}

} // namespace unified_mapping_detail

// Gray-carrier v2 mapping: logical bit (codeword * 16200 + bit) passes through
// the same per-codeword interleave, then fills the six mask planes of the
// active tiles in flat order. Inverse pair; sites beyond 15 * 16200 are
// invalid by construction.
[[nodiscard]] constexpr UnifiedPhysicalCarrierSite GetUnifiedGrayPhysicalSite(
    const std::uint32_t logicalBit, const std::uint64_t frameSequence) noexcept
{
    if (logicalBit >= kUnifiedGrayLogicalBits)
    {
        return {};
    }
    const std::uint32_t domainBit = unified_mapping_detail::PermuteLaneCodewordBit(logicalBit, frameSequence);
    if (domainBit >= kUnifiedGrayCarrierBits)
    {
        return {};
    }
    return {true, UnifiedCarrier::Luma, domainBit / kUnifiedGrayCarrierPlanes,
        static_cast<std::uint8_t>(domainBit % kUnifiedGrayCarrierPlanes)};
}

[[nodiscard]] constexpr UnifiedLogicalCarrierBit GetUnifiedGrayLogicalBit(
    const UnifiedPhysicalCarrierSite& site, const std::uint64_t frameSequence) noexcept
{
    if (!site.valid || site.tileOrdinal >= kUnifiedGrayActiveTiles ||
        site.carrier != UnifiedCarrier::Luma || site.bitPlane >= kUnifiedGrayCarrierPlanes ||
        site.tileOrdinal >= kUnifiedVisualProfile.dataTileCount)
    {
        return {};
    }
    const std::uint32_t domainBit = site.tileOrdinal * kUnifiedGrayCarrierPlanes + site.bitPlane;
    // InvertLaneCodewordBit already returns the slot-offset global logical bit.
    const std::uint32_t globalLogical = unified_mapping_detail::InvertLaneCodewordBit(domainBit, frameSequence);
    if (globalLogical >= kUnifiedGrayLogicalBits)
    {
        return {};
    }
    // Lane identity is irrelevant on the single gray carrier; report BaseLuma
    // so observation summaries classify these metrics consistently.
    return {true, UnifiedLane::BaseLuma, globalLogical};
}

[[nodiscard]] constexpr const UnifiedLaneMappingContract* GetUnifiedLaneMapping(
    const UnifiedLane lane) noexcept
{
    return unified_mapping_detail::FindLaneMapping(lane);
}

[[nodiscard]] constexpr UnifiedPhysicalCarrierSite GetUnifiedPhysicalCarrierSite(
    const UnifiedLane lane, const std::uint32_t logicalBit, const std::uint64_t frameSequence) noexcept
{
    const UnifiedLaneMappingContract* const contract = unified_mapping_detail::FindLaneMapping(lane);
    if (contract == nullptr || logicalBit >= contract->logicalBits)
    {
        return {};
    }
    const std::uint32_t domainLogicalBit = unified_mapping_detail::PermuteLaneCodewordBit(
        logicalBit, frameSequence);
    if (lane == UnifiedLane::BaseLuma)
    {
        return unified_mapping_detail::GetRegionLocalizedBaseSite(domainLogicalBit);
    }
    if (lane == UnifiedLane::FineLuma)
    {
        const std::uint32_t totalDeficit = unified_mapping_detail::GetLumaDeficitPrefix(
            kUnifiedMappingFreshnessRegionCount);
        return unified_mapping_detail::GetLumaSurplusSite(totalDeficit + domainLogicalBit);
    }
    return unified_mapping_detail::GetRegionOrderedChromaSite(domainLogicalBit);
}

[[nodiscard]] constexpr UnifiedLogicalCarrierBit GetUnifiedLogicalCarrierBit(
    const UnifiedPhysicalCarrierSite& site, const std::uint64_t frameSequence) noexcept
{
    if (!site.valid || site.tileOrdinal >= kUnifiedVisualProfile.dataTileCount)
    {
        return {};
    }
    UnifiedLogicalCarrierBit domainLogical;
    if (site.carrier == UnifiedCarrier::Luma)
    {
        if (site.bitPlane >= kUnifiedLumaCarrierPlanes)
        {
            return {};
        }
        domainLogical = unified_mapping_detail::InvertRegionLocalizedLumaSite(site);
    }
    else if (site.carrier == UnifiedCarrier::Chroma)
    {
        if (site.bitPlane >= kUnifiedChromaCarrierPlanes)
        {
            return {};
        }
        domainLogical = unified_mapping_detail::InvertRegionOrderedChromaSite(site);
    }
    if (!domainLogical.valid)
    {
        return {};
    }
    domainLogical.logicalBit = unified_mapping_detail::InvertLaneCodewordBit(
        domainLogical.logicalBit, frameSequence);
    return domainLogical;
}

[[nodiscard]] constexpr bool ValidateUnifiedVisualMappingStaticContract() noexcept
{
    if (!kUnifiedFrameCapacity.valid || kUnifiedVisualProfile.dataTileCount != 41872 ||
        kUnifiedMappingVersion != 3 || kUnifiedMappingSequencePeriod != 16 ||
        kUnifiedFreshnessTileCatalog.tileCounts != kUnifiedExpectedFreshnessTileCounts ||
        kUnifiedFreshnessTileCatalog.groupedOffsets.back() != kUnifiedVisualProfile.dataTileCount ||
        kUnifiedLumaCarrierPlanes != 4 || kUnifiedChromaCarrierPlanes != 2 ||
        kUnifiedCodewordInterleave.modulus != kUnifiedVisualProfile.innerCodewordBits ||
        kUnifiedCodewordInterleave.multiplier == 0 || kUnifiedCodewordInterleave.inverse == 0 ||
        kUnifiedCodewordInterleave.offset >= kUnifiedCodewordInterleave.modulus ||
        kUnifiedCodewordInterleave.phaseStep == 0 ||
        kUnifiedCodewordInterleave.phaseStep >= kUnifiedCodewordInterleave.modulus ||
        kUnifiedCodewordInterleave.phaseCount != kUnifiedMappingSequencePeriod ||
        unified_mapping_detail::MultiplyModulo(kUnifiedCodewordInterleave.multiplier,
            kUnifiedCodewordInterleave.inverse, kUnifiedCodewordInterleave.modulus) != 1)
    {
        return false;
    }
    for (std::uint32_t tileOrdinal = 0; tileOrdinal < kUnifiedVisualProfile.dataTileCount; tileOrdinal++)
    {
        const std::uint8_t region = kUnifiedFreshnessTileCatalog.regionByTileOrdinal[tileOrdinal];
        const std::uint32_t rank = kUnifiedFreshnessTileCatalog.rankByTileOrdinal[tileOrdinal];
        if (region >= kUnifiedMappingFreshnessRegionCount ||
            rank >= kUnifiedFreshnessTileCatalog.tileCounts[region] ||
            kUnifiedFreshnessTileCatalog.groupedTileOrdinals[
                kUnifiedFreshnessTileCatalog.groupedOffsets[region] + rank] != tileOrdinal)
        {
            return false;
        }
    }
    std::array<bool, kUnifiedMappingFreshnessRegionCount> seenChromaRegions{};
    for (const std::uint8_t region : kUnifiedChromaRegionOrder)
    {
        if (region >= seenChromaRegions.size() || seenChromaRegions[region])
        {
            return false;
        }
        seenChromaRegions[region] = true;
    }
    std::uint32_t lumaDeficitBits = 0;
    std::uint32_t lumaSurplusBits = 0;
    for (std::uint32_t region = 0; region < kUnifiedMappingFreshnessRegionCount; region++)
    {
        const std::uint32_t capacity = kUnifiedFreshnessTileCatalog.tileCounts[region] * kUnifiedLumaCarrierPlanes;
        const std::uint32_t primaryBits = unified_mapping_detail::GetPrimaryLumaBits(region);
        lumaDeficitBits += kUnifiedVisualProfile.innerCodewordBits - primaryBits;
        lumaSurplusBits += capacity - primaryBits;
    }
    const std::uint32_t totalLumaCarrierBits = kUnifiedVisualProfile.dataTileCount * kUnifiedLumaCarrierPlanes;
    const std::uint32_t totalChromaCarrierBits = kUnifiedVisualProfile.dataTileCount * kUnifiedChromaCarrierPlanes;
    if (lumaDeficitBits != kUnifiedLumaDeficitBits || lumaSurplusBits != kUnifiedLumaSurplusBits ||
        kUnifiedLumaDeficitBits + kUnifiedFineLumaBits + kUnifiedUnusedLumaCarrierBits != kUnifiedLumaSurplusBits ||
        kUnifiedBaseLumaBits + kUnifiedFineLumaBits + kUnifiedUnusedLumaCarrierBits != totalLumaCarrierBits ||
        kUnifiedChromaBits + kUnifiedUnusedChromaCarrierBits != totalChromaCarrierBits)
    {
        return false;
    }
    for (std::size_t label = 0; label < kUnifiedSymbolMasksByLabel.size(); label++)
    {
        const std::uint32_t mask = kUnifiedSymbolMasksByLabel[label];
        const int foregroundCells = std::popcount(mask);
        if (foregroundCells < 8 || foregroundCells > 17 || !unified_mapping_detail::HasNoIsolatedCells(mask) ||
            (label < 8 && (mask ^ kUnifiedSymbolMasksByLabel[label + 8]) != 0x1FFFFFFU))
        {
            return false;
        }
        for (std::size_t previous = 0; previous < label; previous++)
        {
            if (std::popcount(mask ^ kUnifiedSymbolMasksByLabel[previous]) < 9)
            {
                return false;
            }
        }
    }
    for (std::size_t label = 0; label < kUnifiedChromaStatesByLabel.size(); label++)
    {
        if (kUnifiedChromaStatesByLabel[label].label != label)
        {
            return false;
        }
    }
    for (std::size_t laneIndex = 0; laneIndex < kUnifiedLaneMappings.size(); laneIndex++)
    {
        const UnifiedLaneMappingContract& mapping = kUnifiedLaneMappings[laneIndex];
        const UnifiedLaneCapacity capacity = GetUnifiedLaneCapacity(static_cast<UnifiedLane>(laneIndex));
        if (!capacity.valid || mapping.lane != static_cast<UnifiedLane>(laneIndex) ||
            mapping.logicalBits != capacity.codedBits)
        {
            return false;
        }
    }
    return true;
}

static_assert(ValidateUnifiedVisualMappingStaticContract());
static_assert(GetUnifiedPhysicalCarrierSite(UnifiedLane::BaseLuma, 0, 0).valid);
static_assert(GetUnifiedPhysicalCarrierSite(UnifiedLane::FineLuma, kUnifiedFineLumaBits, 0).valid == false);
static_assert(GetUnifiedPhysicalCarrierSite(UnifiedLane::Chroma, kUnifiedChromaBits, 0).valid == false);

} // namespace pbmodulation
