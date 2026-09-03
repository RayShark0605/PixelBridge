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

inline constexpr std::uint32_t kUnifiedMappingVersion = 1;
inline constexpr std::uint32_t kUnifiedMappingPhaseCount = 16;
inline constexpr std::uint32_t kUnifiedBaseDedicatedLumaBits = 260064;
inline constexpr std::uint32_t kUnifiedBaseSharedLumaBits = 15336;
inline constexpr std::uint32_t kUnifiedFinePlaneThreeFirstPosition = 15336;
inline constexpr std::uint32_t kUnifiedFinePlaneThreeEndPosition = 80136;
inline constexpr std::uint32_t kUnifiedChromaUsedTiles = 81000;
inline constexpr std::string_view kUnifiedMappingStreamFrameSequenceZeroBlake3{
    "cd8444d1513640cb0b01d58dd5a8b984d54457d78ba49331c7def9f676cd1801"};

// Label n is encoded by mask[n]. Bit n within a mask addresses the row-major
// 4x4 chip at (n % 4, n / 4); one selects high luma. Selection used only the
// sealed Train set to construct candidates/labels and Validation to rank them.
inline constexpr std::array<std::uint16_t, 16> kUnifiedSymbolMasksByLabel{
    0x07DC, 0x08EF, 0x1337, 0x6666, 0x34D3, 0x3BE0, 0x62B9, 0x718E,
    0xF823, 0xF710, 0xECC8, 0x9999, 0xCB2C, 0xC41F, 0x9D46, 0x8E71};

struct UnifiedChromaState
{
    std::int16_t blueOffset = 0;
    std::int16_t greenOffset = 0;
    std::int16_t redOffset = 0;
    std::uint8_t label = 0;

    bool operator==(const UnifiedChromaState&) const = default;
};

// The four approximately iso-luma historical Shape/Chroma states are retained,
// but stored by their two-bit label. Their cycle in capture space is 0,1,3,2.
inline constexpr std::array<UnifiedChromaState, 4> kUnifiedChromaStatesByLabel{
    UnifiedChromaState{-24, 7, -16, 0},
    UnifiedChromaState{24, 2, -16, 1},
    UnifiedChromaState{-24, -2, 16, 2},
    UnifiedChromaState{24, -7, 16, 3}};

struct UnifiedTilePermutation
{
    std::uint32_t modulus = 0;
    std::uint32_t multiplier = 0;
    std::uint32_t inverse = 0;
    std::uint32_t offset = 0;

    bool operator==(const UnifiedTilePermutation&) const = default;
};

inline constexpr UnifiedTilePermutation kUnifiedPlaneThreeTileOrder{86688, 31439, 15215, 76408};
inline constexpr UnifiedTilePermutation kUnifiedChromaTileOrder{86688, 65533, 14965, 51321};

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
    UnifiedLaneInterleaveContract{UnifiedLane::BaseLuma, 275400, 194267, 200003, 163611, 230029, 16, 3},
    UnifiedLaneInterleaveContract{UnifiedLane::FineLuma, 64800, 34559, 30239, 52798, 26153, 16, 14},
    UnifiedLaneInterleaveContract{UnifiedLane::Chroma, 162000, 152357, 148493, 152539, 50837, 16, 15}};

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

[[nodiscard]] constexpr bool HasNoIsolatedCells(const std::uint16_t mask) noexcept
{
    for (std::uint32_t index = 0; index < 16; index++)
    {
        const bool value = ((mask >> index) & 1U) != 0;
        const std::uint32_t x = index % 4;
        const std::uint32_t y = index / 4;
        bool hasEqualNeighbor = false;
        if (x > 0)
        {
            hasEqualNeighbor = hasEqualNeighbor || (((mask >> (index - 1)) & 1U) != 0) == value;
        }
        if (x < 3)
        {
            hasEqualNeighbor = hasEqualNeighbor || (((mask >> (index + 1)) & 1U) != 0) == value;
        }
        if (y > 0)
        {
            hasEqualNeighbor = hasEqualNeighbor || (((mask >> (index - 4)) & 1U) != 0) == value;
        }
        if (y < 3)
        {
            hasEqualNeighbor = hasEqualNeighbor || (((mask >> (index + 4)) & 1U) != 0) == value;
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
    if (!kUnifiedFrameCapacity.valid || kUnifiedVisualProfile.dataTileCount != 86688 ||
        kUnifiedMappingPhaseCount != 16 || kUnifiedBaseDedicatedLumaBits != kUnifiedVisualProfile.dataTileCount * 3 ||
        kUnifiedBaseDedicatedLumaBits + kUnifiedBaseSharedLumaBits != 275400 ||
        kUnifiedFinePlaneThreeFirstPosition != kUnifiedBaseSharedLumaBits ||
        kUnifiedFinePlaneThreeEndPosition != kUnifiedFinePlaneThreeFirstPosition + 64800 ||
        kUnifiedFinePlaneThreeEndPosition > kUnifiedVisualProfile.dataTileCount ||
        kUnifiedChromaUsedTiles * 2 != 162000)
    {
        return false;
    }
    for (std::size_t label = 0; label < kUnifiedSymbolMasksByLabel.size(); label++)
    {
        const std::uint16_t mask = kUnifiedSymbolMasksByLabel[label];
        if (std::popcount(mask) != 8 || !unified_mapping_detail::HasNoIsolatedCells(mask) ||
            (label < 8 && static_cast<std::uint16_t>(mask ^ kUnifiedSymbolMasksByLabel[label + 8]) != 0xFFFFU))
        {
            return false;
        }
        for (std::size_t previous = 0; previous < label; previous++)
        {
            if (std::popcount(static_cast<std::uint16_t>(mask ^ kUnifiedSymbolMasksByLabel[previous])) < 8)
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
static_assert(GetUnifiedPhysicalCarrierSite(UnifiedLane::FineLuma, 64800, 0).valid == false);
static_assert(GetUnifiedPhysicalCarrierSite(UnifiedLane::Chroma, 162000, 0).valid == false);

} // namespace pbmodulation
