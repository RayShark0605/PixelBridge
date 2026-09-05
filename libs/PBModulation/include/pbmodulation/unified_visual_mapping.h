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

inline constexpr std::uint32_t kUnifiedMappingVersion = 2;
inline constexpr std::uint32_t kUnifiedMappingPhaseCount = 16;
inline constexpr std::uint32_t kUnifiedBaseDedicatedLumaBits = 125616;
inline constexpr std::uint32_t kUnifiedBaseSharedLumaBits = 20184;
inline constexpr std::uint32_t kUnifiedFinePlaneThreeFirstPosition = 20184;
inline constexpr std::uint32_t kUnifiedFinePlaneThreeEndPosition = 36384;
inline constexpr std::uint32_t kUnifiedChromaUsedTiles = 40500;
inline constexpr std::string_view kUnifiedMappingStreamFrameSequenceZeroBlake3{
    "8718eb1c1b43764160f8a79b437cec0ac5cee46072aeb7d08ef0930fbe450fbc"};

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

struct UnifiedTilePermutation
{
    std::uint32_t modulus = 0;
    std::uint32_t multiplier = 0;
    std::uint32_t inverse = 0;
    std::uint32_t offset = 0;

    bool operator==(const UnifiedTilePermutation&) const = default;
};

inline constexpr UnifiedTilePermutation kUnifiedPlaneThreeTileOrder{41872, 31439, 6879, 34120};
inline constexpr UnifiedTilePermutation kUnifiedChromaTileOrder{41872, 23661, 41557, 9489};

struct UnifiedLaneInterleaveContract
{
    UnifiedLane lane = UnifiedLane::BaseLuma;
    std::uint32_t logicalBits = 0;
    std::uint32_t multiplier = 0;
    std::uint32_t inverse = 0;
    std::uint32_t offset = 0;
    std::uint32_t phaseStep = 0;
    std::uint32_t phaseCount = 0;
    std::uint32_t sequenceOffset = 0;

    bool operator==(const UnifiedLaneInterleaveContract&) const = default;
};

inline constexpr std::array<UnifiedLaneInterleaveContract, 3> kUnifiedLaneInterleaves{
    UnifiedLaneInterleaveContract{UnifiedLane::BaseLuma, 145800, 48467, 38003, 17811, 84229, 16, 3},
    UnifiedLaneInterleaveContract{UnifiedLane::FineLuma, 16200, 2159, 14039, 4198, 9953, 16, 14},
    UnifiedLaneInterleaveContract{UnifiedLane::Chroma, 81000, 71357, 67493, 71539, 50837, 16, 15}};

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

namespace unified_mapping_detail
{

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

[[nodiscard]] constexpr std::uint32_t ResolvePhase(
    const UnifiedLaneInterleaveContract& contract, const std::uint64_t frameSequence) noexcept
{
    return contract.phaseCount == 0 ? 0 : static_cast<std::uint32_t>(
        (frameSequence % contract.phaseCount + contract.sequenceOffset) % contract.phaseCount);
}

[[nodiscard]] constexpr std::uint32_t PermuteTile(
    const UnifiedTilePermutation& permutation, const std::uint32_t position) noexcept
{
    return AddModulo(MultiplyModulo(position, permutation.multiplier, permutation.modulus),
        permutation.offset, permutation.modulus);
}

[[nodiscard]] constexpr std::uint32_t InvertTile(
    const UnifiedTilePermutation& permutation, const std::uint32_t tileOrdinal) noexcept
{
    const std::uint32_t shifted = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(tileOrdinal) + permutation.modulus - permutation.offset) % permutation.modulus);
    return MultiplyModulo(shifted, permutation.inverse, permutation.modulus);
}

[[nodiscard]] constexpr const UnifiedLaneInterleaveContract* FindLane(const UnifiedLane lane) noexcept
{
    for (const UnifiedLaneInterleaveContract& contract : kUnifiedLaneInterleaves)
    {
        if (contract.lane == lane)
        {
            return &contract;
        }
    }
    return nullptr;
}

[[nodiscard]] constexpr std::uint32_t PermuteLaneBit(
    const UnifiedLaneInterleaveContract& contract, const std::uint32_t logicalBit,
    const std::uint64_t frameSequence) noexcept
{
    const std::uint32_t phase = ResolvePhase(contract, frameSequence);
    const std::uint32_t phaseOffset = AddModulo(contract.offset,
        MultiplyModulo(phase, contract.phaseStep, contract.logicalBits), contract.logicalBits);
    return AddModulo(MultiplyModulo(logicalBit, contract.multiplier, contract.logicalBits),
        phaseOffset, contract.logicalBits);
}

[[nodiscard]] constexpr std::uint32_t InvertLaneBit(
    const UnifiedLaneInterleaveContract& contract, const std::uint32_t domainBit,
    const std::uint64_t frameSequence) noexcept
{
    const std::uint32_t phase = ResolvePhase(contract, frameSequence);
    const std::uint32_t phaseOffset = AddModulo(contract.offset,
        MultiplyModulo(phase, contract.phaseStep, contract.logicalBits), contract.logicalBits);
    const std::uint32_t shifted = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(domainBit) + contract.logicalBits - phaseOffset) % contract.logicalBits);
    return MultiplyModulo(shifted, contract.inverse, contract.logicalBits);
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

[[nodiscard]] constexpr const UnifiedLaneInterleaveContract* GetUnifiedLaneInterleave(
    const UnifiedLane lane) noexcept
{
    return unified_mapping_detail::FindLane(lane);
}

[[nodiscard]] constexpr UnifiedPhysicalCarrierSite GetUnifiedPhysicalCarrierSite(
    const UnifiedLane lane, const std::uint32_t logicalBit, const std::uint64_t frameSequence) noexcept
{
    const UnifiedLaneInterleaveContract* const contract = unified_mapping_detail::FindLane(lane);
    if (contract == nullptr || logicalBit >= contract->logicalBits)
    {
        return {};
    }
    const std::uint32_t domainBit = unified_mapping_detail::PermuteLaneBit(*contract, logicalBit, frameSequence);
    if (lane == UnifiedLane::BaseLuma)
    {
        if (domainBit < kUnifiedBaseDedicatedLumaBits)
        {
            return {true, UnifiedCarrier::Luma, domainBit / 3, static_cast<std::uint8_t>(domainBit % 3)};
        }
        const std::uint32_t planeThreePosition = domainBit - kUnifiedBaseDedicatedLumaBits;
        return {true, UnifiedCarrier::Luma,
            unified_mapping_detail::PermuteTile(kUnifiedPlaneThreeTileOrder, planeThreePosition), 3};
    }
    if (lane == UnifiedLane::FineLuma)
    {
        const std::uint32_t planeThreePosition = domainBit + kUnifiedFinePlaneThreeFirstPosition;
        return {true, UnifiedCarrier::Luma,
            unified_mapping_detail::PermuteTile(kUnifiedPlaneThreeTileOrder, planeThreePosition), 3};
    }
    const std::uint32_t usedTilePosition = domainBit / 2;
    return {true, UnifiedCarrier::Chroma,
        unified_mapping_detail::PermuteTile(kUnifiedChromaTileOrder, usedTilePosition),
        static_cast<std::uint8_t>(domainBit % 2)};
}

[[nodiscard]] constexpr UnifiedLogicalCarrierBit GetUnifiedLogicalCarrierBit(
    const UnifiedPhysicalCarrierSite& site, const std::uint64_t frameSequence) noexcept
{
    if (!site.valid || site.tileOrdinal >= kUnifiedVisualProfile.dataTileCount)
    {
        return {};
    }
    UnifiedLane lane = UnifiedLane::BaseLuma;
    std::uint32_t domainBit = 0;
    if (site.carrier == UnifiedCarrier::Luma)
    {
        if (site.bitPlane < 3)
        {
            lane = UnifiedLane::BaseLuma;
            domainBit = site.tileOrdinal * 3 + site.bitPlane;
        }
        else if (site.bitPlane == 3)
        {
            const std::uint32_t planeThreePosition = unified_mapping_detail::InvertTile(
                kUnifiedPlaneThreeTileOrder, site.tileOrdinal);
            if (planeThreePosition < kUnifiedBaseSharedLumaBits)
            {
                lane = UnifiedLane::BaseLuma;
                domainBit = kUnifiedBaseDedicatedLumaBits + planeThreePosition;
            }
            else if (planeThreePosition < kUnifiedFinePlaneThreeEndPosition)
            {
                lane = UnifiedLane::FineLuma;
                domainBit = planeThreePosition - kUnifiedFinePlaneThreeFirstPosition;
            }
            else
            {
                return {};
            }
        }
        else
        {
            return {};
        }
    }
    else if (site.carrier == UnifiedCarrier::Chroma)
    {
        if (site.bitPlane >= 2)
        {
            return {};
        }
        const std::uint32_t usedTilePosition = unified_mapping_detail::InvertTile(
            kUnifiedChromaTileOrder, site.tileOrdinal);
        if (usedTilePosition >= kUnifiedChromaUsedTiles)
        {
            return {};
        }
        lane = UnifiedLane::Chroma;
        domainBit = usedTilePosition * 2 + site.bitPlane;
    }
    else
    {
        return {};
    }
    const UnifiedLaneInterleaveContract* const contract = unified_mapping_detail::FindLane(lane);
    if (contract == nullptr || domainBit >= contract->logicalBits)
    {
        return {};
    }
    return {true, lane, unified_mapping_detail::InvertLaneBit(*contract, domainBit, frameSequence)};
}

[[nodiscard]] constexpr bool ValidateUnifiedVisualMappingStaticContract() noexcept
{
    if (!kUnifiedFrameCapacity.valid || kUnifiedVisualProfile.dataTileCount != 41872 ||
        kUnifiedMappingPhaseCount != 16 || kUnifiedBaseDedicatedLumaBits != kUnifiedVisualProfile.dataTileCount * 3 ||
        kUnifiedBaseDedicatedLumaBits + kUnifiedBaseSharedLumaBits != 145800 ||
        kUnifiedFinePlaneThreeFirstPosition != kUnifiedBaseSharedLumaBits ||
        kUnifiedFinePlaneThreeEndPosition != kUnifiedFinePlaneThreeFirstPosition + 16200 ||
        kUnifiedFinePlaneThreeEndPosition > kUnifiedVisualProfile.dataTileCount ||
        kUnifiedChromaUsedTiles * 2 != 81000)
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
    for (const UnifiedTilePermutation permutation : {kUnifiedPlaneThreeTileOrder, kUnifiedChromaTileOrder})
    {
        if (permutation.modulus != kUnifiedVisualProfile.dataTileCount || permutation.multiplier == 0 ||
            permutation.inverse == 0 || permutation.offset >= permutation.modulus ||
            unified_mapping_detail::MultiplyModulo(permutation.multiplier, permutation.inverse, permutation.modulus) != 1)
        {
            return false;
        }
    }
    for (std::size_t laneIndex = 0; laneIndex < kUnifiedLaneInterleaves.size(); laneIndex++)
    {
        const UnifiedLaneInterleaveContract& interleave = kUnifiedLaneInterleaves[laneIndex];
        const UnifiedLaneCapacity capacity = GetUnifiedLaneCapacity(static_cast<UnifiedLane>(laneIndex));
        if (!capacity.valid || interleave.lane != static_cast<UnifiedLane>(laneIndex) ||
            interleave.logicalBits != capacity.codedBits || interleave.multiplier == 0 || interleave.inverse == 0 ||
            interleave.offset >= interleave.logicalBits || interleave.phaseStep >= interleave.logicalBits ||
            interleave.phaseCount != kUnifiedMappingPhaseCount || interleave.sequenceOffset >= interleave.phaseCount ||
            unified_mapping_detail::MultiplyModulo(interleave.multiplier, interleave.inverse, interleave.logicalBits) != 1)
        {
            return false;
        }
    }
    return true;
}

static_assert(ValidateUnifiedVisualMappingStaticContract());
static_assert(GetUnifiedPhysicalCarrierSite(UnifiedLane::BaseLuma, 0, 0).valid);
static_assert(GetUnifiedPhysicalCarrierSite(UnifiedLane::FineLuma, 16200, 0).valid == false);
static_assert(GetUnifiedPhysicalCarrierSite(UnifiedLane::Chroma, 81000, 0).valid == false);

} // namespace pbmodulation
