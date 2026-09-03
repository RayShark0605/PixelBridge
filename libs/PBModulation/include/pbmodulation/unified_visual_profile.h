#pragma once

#include "pbprotocol/product_visual_profile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace pbmodulation
{

enum class UnifiedPixelFormat : std::uint8_t
{
    Bgra8UnormSdr = 1
};

enum class UnifiedCarrier : std::uint8_t
{
    Luma,
    Chroma
};

enum class UnifiedLane : std::uint8_t
{
    BaseLuma,
    FineLuma,
    Chroma
};

enum class UnifiedRegionKind : std::uint8_t
{
    Locator,
    BootstrapA,
    BootstrapB,
    Pilot,
    Data
};

enum class UnifiedPilotContent : std::uint16_t
{
    None = 0,
    BlackWhiteMidGray = 1U << 0U,
    LumaLevelLadder = 1U << 1U,
    ActiveChromaStates = 1U << 2U,
    NeutralChroma = 1U << 3U,
    PhaseChecker = 1U << 4U,
    TimingFreshness = 1U << 5U,
    CalibrationReferences = (1U << 0U) | (1U << 1U) | (1U << 2U) | (1U << 3U)
};

[[nodiscard]] constexpr bool HasUnifiedPilotContent(
    const UnifiedPilotContent value, const UnifiedPilotContent expected) noexcept
{
    return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(expected)) ==
        static_cast<std::uint16_t>(expected);
}

struct UnifiedPixelRegion
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const UnifiedPixelRegion&) const = default;
};

struct UnifiedRegionContract
{
    UnifiedRegionKind kind = UnifiedRegionKind::Locator;
    UnifiedPilotContent pilotContent = UnifiedPilotContent::None;
    UnifiedPixelRegion bounds;

    bool operator==(const UnifiedRegionContract&) const = default;
};

struct UnifiedLaneContract
{
    UnifiedLane lane = UnifiedLane::BaseLuma;
    UnifiedCarrier carrier = UnifiedCarrier::Luma;
    std::uint32_t firstCodewordSlot = 0;
    std::uint32_t codewordCount = 0;

    bool operator==(const UnifiedLaneContract&) const = default;
};

struct UnifiedCarrierContract
{
    UnifiedCarrier carrier = UnifiedCarrier::Luma;
    std::uint32_t bitsPerTile = 0;

    bool operator==(const UnifiedCarrierContract&) const = default;
};

struct UnifiedControlSlotRegion
{
    UnifiedLane lane = UnifiedLane::BaseLuma;
    std::uint32_t firstCodewordSlot = 0;
    std::uint32_t codewordSlotCount = 0;

    bool operator==(const UnifiedControlSlotRegion&) const = default;
};

struct UnifiedMixedSlotContract
{
    UnifiedControlSlotRegion controlRegion;
    std::uint32_t minimumTransportSlots = 0;

    bool operator==(const UnifiedMixedSlotContract&) const = default;
};

enum class UnifiedViewportFilter : std::uint8_t
{
    Point
};

enum class UnifiedViewportFit : std::uint8_t
{
    PreserveAspectRatio
};

enum class UnifiedViewportPlacement : std::uint8_t
{
    CenteredLetterbox
};

enum class UnifiedUndersizeBehavior : std::uint8_t
{
    NeutralMattePauseWithoutBootstrap
};

struct UnifiedPresentationContract
{
    std::uint32_t minimumScaleNumerator = 0;
    std::uint32_t minimumScaleDenominator = 0;
    std::uint32_t maximumScaleNumerator = 0;
    std::uint32_t maximumScaleDenominator = 0;
    std::uint32_t minimumLogicalFramesPerSecond = 0;
    std::uint32_t defaultLogicalFramesPerSecond = 0;
    std::uint32_t maximumLogicalFramesPerSecond = 0;
    UnifiedViewportFilter filter = UnifiedViewportFilter::Point;
    UnifiedViewportFit fit = UnifiedViewportFit::PreserveAspectRatio;
    UnifiedViewportPlacement placement = UnifiedViewportPlacement::CenteredLetterbox;
    UnifiedUndersizeBehavior undersizeBehavior = UnifiedUndersizeBehavior::NeutralMattePauseWithoutBootstrap;

    bool operator==(const UnifiedPresentationContract&) const = default;
};

struct UnifiedVisualProfileManifest
{
    pbprotocol::ProductVisualProfile productProfile;
    UnifiedPixelFormat pixelFormat = UnifiedPixelFormat::Bgra8UnormSdr;
    std::uint32_t canvasWidth = 0;
    std::uint32_t canvasHeight = 0;
    std::uint8_t alphaCodeValue = 0;
    std::uint32_t tileWidth = 0;
    std::uint32_t tileHeight = 0;
    std::uint32_t dataTileCount = 0;
    std::uint64_t innerFecProfileId = 0;
    std::uint32_t innerCodewordBits = 0;
    std::uint32_t innerInformationBits = 0;
    std::uint32_t transportOverheadBytes = 0;
    UnifiedPixelRegion dataGridBounds;
    std::array<UnifiedRegionContract, 31> regions;
    std::array<UnifiedCarrierContract, 2> carriers;
    std::array<UnifiedLaneContract, 3> lanes;
    UnifiedMixedSlotContract mixedSlots;
    UnifiedPresentationContract presentation;

    bool operator==(const UnifiedVisualProfileManifest&) const = default;
};

// The region table reuses only the historical 4x4 Shape geometry, not its
// experimental masks, labels, lane mapping or interleave. The ten Data entries
// and nine TimingFreshness entries exactly partition dataGridBounds. All canvas
// pixels not listed here are reserved guard/matte space; the Unified raster
// contract freezes their exact signal values. Control has no separate pixel rectangle in layout 8: it is
// reserved in Base Luma codeword slot space by mixedSlots, so its capacity is
// counted exactly once.
inline constexpr UnifiedVisualProfileManifest kUnifiedVisualProfile{
    pbprotocol::kUnifiedProductVisualProfile,
    UnifiedPixelFormat::Bgra8UnormSdr,
    1920,
    1080,
    255,
    4,
    4,
    86688,
    0x36BC661265E826C3ULL,
    16200,
    10800,
    36,
    UnifiedPixelRegion{96, 96, 1728, 888},
    std::array<UnifiedRegionContract, 31>{
        UnifiedRegionContract{UnifiedRegionKind::Locator, UnifiedPilotContent::None, UnifiedPixelRegion{16, 16, 64, 64}},
        UnifiedRegionContract{UnifiedRegionKind::Locator, UnifiedPilotContent::None, UnifiedPixelRegion{1840, 16, 64, 64}},
        UnifiedRegionContract{UnifiedRegionKind::Locator, UnifiedPilotContent::None, UnifiedPixelRegion{16, 1000, 64, 64}},
        UnifiedRegionContract{UnifiedRegionKind::Locator, UnifiedPilotContent::None, UnifiedPixelRegion{1840, 1000, 64, 64}},
        UnifiedRegionContract{UnifiedRegionKind::BootstrapA, UnifiedPilotContent::None, UnifiedPixelRegion{96, 16, 608, 64}},
        UnifiedRegionContract{UnifiedRegionKind::BootstrapB, UnifiedPilotContent::None, UnifiedPixelRegion{1216, 1000, 608, 64}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::TimingFreshness, UnifiedPixelRegion{96, 160, 128, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::TimingFreshness, UnifiedPixelRegion{896, 160, 128, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::TimingFreshness, UnifiedPixelRegion{1696, 160, 128, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::TimingFreshness, UnifiedPixelRegion{96, 476, 128, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::TimingFreshness, UnifiedPixelRegion{896, 476, 128, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::TimingFreshness, UnifiedPixelRegion{1696, 476, 128, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::TimingFreshness, UnifiedPixelRegion{96, 792, 128, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::TimingFreshness, UnifiedPixelRegion{896, 792, 128, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::TimingFreshness, UnifiedPixelRegion{1696, 792, 128, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::CalibrationReferences, UnifiedPixelRegion{736, 16, 128, 64}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::CalibrationReferences, UnifiedPixelRegion{1696, 16, 128, 64}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::CalibrationReferences, UnifiedPixelRegion{96, 1000, 128, 64}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::CalibrationReferences, UnifiedPixelRegion{1056, 1000, 128, 64}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::PhaseChecker, UnifiedPixelRegion{896, 16, 128, 64}},
        UnifiedRegionContract{UnifiedRegionKind::Pilot, UnifiedPilotContent::PhaseChecker, UnifiedPixelRegion{896, 1000, 128, 64}},
        UnifiedRegionContract{UnifiedRegionKind::Data, UnifiedPilotContent::None, UnifiedPixelRegion{96, 96, 1728, 64}},
        UnifiedRegionContract{UnifiedRegionKind::Data, UnifiedPilotContent::None, UnifiedPixelRegion{224, 160, 672, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Data, UnifiedPilotContent::None, UnifiedPixelRegion{1024, 160, 672, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Data, UnifiedPilotContent::None, UnifiedPixelRegion{96, 288, 1728, 188}},
        UnifiedRegionContract{UnifiedRegionKind::Data, UnifiedPilotContent::None, UnifiedPixelRegion{224, 476, 672, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Data, UnifiedPilotContent::None, UnifiedPixelRegion{1024, 476, 672, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Data, UnifiedPilotContent::None, UnifiedPixelRegion{96, 604, 1728, 188}},
        UnifiedRegionContract{UnifiedRegionKind::Data, UnifiedPilotContent::None, UnifiedPixelRegion{224, 792, 672, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Data, UnifiedPilotContent::None, UnifiedPixelRegion{1024, 792, 672, 128}},
        UnifiedRegionContract{UnifiedRegionKind::Data, UnifiedPilotContent::None, UnifiedPixelRegion{96, 920, 1728, 64}}},
    std::array<UnifiedCarrierContract, 2>{
        UnifiedCarrierContract{UnifiedCarrier::Luma, 4},
        UnifiedCarrierContract{UnifiedCarrier::Chroma, 2}},
    std::array<UnifiedLaneContract, 3>{
        UnifiedLaneContract{UnifiedLane::BaseLuma, UnifiedCarrier::Luma, 0, 17},
        UnifiedLaneContract{UnifiedLane::FineLuma, UnifiedCarrier::Luma, 17, 4},
        UnifiedLaneContract{UnifiedLane::Chroma, UnifiedCarrier::Chroma, 21, 10}},
    UnifiedMixedSlotContract{UnifiedControlSlotRegion{UnifiedLane::BaseLuma, 0, 17}, 1},
    UnifiedPresentationContract{3, 4, 2, 1, 1, 15, 60, UnifiedViewportFilter::Point,
        UnifiedViewportFit::PreserveAspectRatio, UnifiedViewportPlacement::CenteredLetterbox,
        UnifiedUndersizeBehavior::NeutralMattePauseWithoutBootstrap}};

// Pointer catalog keeps one authoritative manifest object rather than copying
// its region table into per-module registries.
inline constexpr std::array<const UnifiedVisualProfileManifest*, 1> kUnifiedVisualProfileCatalog{
    &kUnifiedVisualProfile};

[[nodiscard]] constexpr const UnifiedVisualProfileManifest* FindUnifiedVisualProfile(
    const std::uint64_t visualProfileId, const std::uint8_t visualLayoutVersion) noexcept
{
    for (const UnifiedVisualProfileManifest* const profile : kUnifiedVisualProfileCatalog)
    {
        if (profile != nullptr && profile->productProfile.visualProfileId == visualProfileId &&
            profile->productProfile.visualLayoutVersion == visualLayoutVersion)
        {
            return profile;
        }
    }
    return nullptr;
}

struct UnifiedFrameCapacity
{
    std::uint64_t dataPixels = 0;
    std::uint64_t dataTiles = 0;
    std::uint64_t rawLumaBits = 0;
    std::uint64_t rawChromaBits = 0;
    std::uint64_t rawVisualBits = 0;
    std::uint64_t codewordCount = 0;
    std::uint64_t usedLumaBits = 0;
    std::uint64_t usedChromaBits = 0;
    std::uint64_t usedCodedBits = 0;
    std::uint64_t reservedLumaBits = 0;
    std::uint64_t reservedChromaBits = 0;
    std::uint64_t reservedVisualBits = 0;
    std::uint64_t codedBytes = 0;
    std::uint64_t informationBytes = 0;
    std::uint64_t transportOverheadBytes = 0;
    // Upper bound when every codeword slot carries Transport. Mixed Control
    // assignments consume Base Luma slots and reduce the realized payload.
    std::uint64_t transportPayloadBytes = 0;

    bool operator==(const UnifiedFrameCapacity&) const = default;
};

struct UnifiedFrameCapacityCalculation
{
    bool valid = false;
    UnifiedFrameCapacity capacity;
};

namespace unified_detail
{

[[nodiscard]] constexpr bool CheckedAdd(const std::uint64_t left, const std::uint64_t right, std::uint64_t& output) noexcept
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
    {
        return false;
    }
    output = left + right;
    return true;
}

[[nodiscard]] constexpr bool CheckedMultiply(
    const std::uint64_t left, const std::uint64_t right, std::uint64_t& output) noexcept
{
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
    {
        return false;
    }
    output = left * right;
    return true;
}

[[nodiscard]] constexpr bool TryGetRegionArea(const UnifiedPixelRegion& region, std::uint64_t& area) noexcept
{
    return region.width != 0 && region.height != 0 && CheckedMultiply(region.width, region.height, area);
}

[[nodiscard]] constexpr bool IsRegionWithin(
    const UnifiedPixelRegion& region, const UnifiedPixelRegion& container) noexcept
{
    std::uint64_t regionRight = 0;
    std::uint64_t regionBottom = 0;
    std::uint64_t containerRight = 0;
    std::uint64_t containerBottom = 0;
    return region.width != 0 && region.height != 0 && CheckedAdd(region.x, region.width, regionRight) &&
        CheckedAdd(region.y, region.height, regionBottom) && CheckedAdd(container.x, container.width, containerRight) &&
        CheckedAdd(container.y, container.height, containerBottom) && region.x >= container.x && region.y >= container.y &&
        regionRight <= containerRight && regionBottom <= containerBottom;
}

[[nodiscard]] constexpr bool RegionsOverlap(const UnifiedPixelRegion& left, const UnifiedPixelRegion& right) noexcept
{
    const std::uint64_t leftRight = static_cast<std::uint64_t>(left.x) + left.width;
    const std::uint64_t leftBottom = static_cast<std::uint64_t>(left.y) + left.height;
    const std::uint64_t rightRight = static_cast<std::uint64_t>(right.x) + right.width;
    const std::uint64_t rightBottom = static_cast<std::uint64_t>(right.y) + right.height;
    return left.x < rightRight && right.x < leftRight && left.y < rightBottom && right.y < leftBottom;
}

[[nodiscard]] constexpr const UnifiedCarrierContract* FindCarrierContract(
    const UnifiedVisualProfileManifest& profile, const UnifiedCarrier carrier) noexcept
{
    for (const UnifiedCarrierContract& contract : profile.carriers)
    {
        if (contract.carrier == carrier)
        {
            return &contract;
        }
    }
    return nullptr;
}

} // namespace unified_detail

[[nodiscard]] constexpr UnifiedFrameCapacityCalculation CalculateUnifiedFrameCapacity(
    const UnifiedVisualProfileManifest& profile) noexcept
{
    UnifiedFrameCapacityCalculation result;
    UnifiedFrameCapacity& capacity = result.capacity;
    if (profile.innerCodewordBits == 0 || profile.innerInformationBits == 0 ||
        profile.innerInformationBits >= profile.innerCodewordBits || profile.innerCodewordBits % 8 != 0 ||
        profile.innerInformationBits % 8 != 0)
    {
        return result;
    }
    for (const UnifiedRegionContract& region : profile.regions)
    {
        if (region.kind != UnifiedRegionKind::Data)
        {
            continue;
        }
        std::uint64_t regionArea = 0;
        std::uint64_t nextDataPixels = 0;
        if (!unified_detail::TryGetRegionArea(region.bounds, regionArea) ||
            !unified_detail::CheckedAdd(capacity.dataPixels, regionArea, nextDataPixels))
        {
            return result;
        }
        capacity.dataPixels = nextDataPixels;
    }

    std::uint64_t tilePixels = 0;
    if (!unified_detail::CheckedMultiply(profile.tileWidth, profile.tileHeight, tilePixels) || tilePixels == 0 ||
        capacity.dataPixels % tilePixels != 0)
    {
        return result;
    }
    capacity.dataTiles = capacity.dataPixels / tilePixels;

    const UnifiedCarrierContract* const luma = unified_detail::FindCarrierContract(profile, UnifiedCarrier::Luma);
    const UnifiedCarrierContract* const chroma = unified_detail::FindCarrierContract(profile, UnifiedCarrier::Chroma);
    if (luma == nullptr || chroma == nullptr || luma->bitsPerTile == 0 || chroma->bitsPerTile == 0 ||
        !unified_detail::CheckedMultiply(capacity.dataTiles, luma->bitsPerTile, capacity.rawLumaBits) ||
        !unified_detail::CheckedMultiply(capacity.dataTiles, chroma->bitsPerTile, capacity.rawChromaBits) ||
        !unified_detail::CheckedAdd(capacity.rawLumaBits, capacity.rawChromaBits, capacity.rawVisualBits))
    {
        return result;
    }

    for (const UnifiedLaneContract& lane : profile.lanes)
    {
        std::uint64_t nextCodewordCount = 0;
        std::uint64_t laneBits = 0;
        if ((lane.carrier != UnifiedCarrier::Luma && lane.carrier != UnifiedCarrier::Chroma) ||
            !unified_detail::CheckedAdd(capacity.codewordCount, lane.codewordCount, nextCodewordCount) ||
            !unified_detail::CheckedMultiply(lane.codewordCount, profile.innerCodewordBits, laneBits))
        {
            return result;
        }
        capacity.codewordCount = nextCodewordCount;
        std::uint64_t* const carrierBits = lane.carrier == UnifiedCarrier::Luma ? &capacity.usedLumaBits : &capacity.usedChromaBits;
        std::uint64_t nextCarrierBits = 0;
        if (!unified_detail::CheckedAdd(*carrierBits, laneBits, nextCarrierBits))
        {
            return result;
        }
        *carrierBits = nextCarrierBits;
    }

    if (!unified_detail::CheckedAdd(capacity.usedLumaBits, capacity.usedChromaBits, capacity.usedCodedBits) ||
        capacity.usedLumaBits > capacity.rawLumaBits || capacity.usedChromaBits > capacity.rawChromaBits)
    {
        return result;
    }
    capacity.reservedLumaBits = capacity.rawLumaBits - capacity.usedLumaBits;
    capacity.reservedChromaBits = capacity.rawChromaBits - capacity.usedChromaBits;
    if (!unified_detail::CheckedAdd(capacity.reservedLumaBits, capacity.reservedChromaBits, capacity.reservedVisualBits))
    {
        return result;
    }

    const std::uint64_t codewordBytes = profile.innerCodewordBits / 8;
    const std::uint64_t informationBytes = profile.innerInformationBits / 8;
    if (profile.transportOverheadBytes > informationBytes ||
        !unified_detail::CheckedMultiply(capacity.codewordCount, codewordBytes, capacity.codedBytes) ||
        !unified_detail::CheckedMultiply(capacity.codewordCount, informationBytes, capacity.informationBytes) ||
        !unified_detail::CheckedMultiply(capacity.codewordCount, profile.transportOverheadBytes, capacity.transportOverheadBytes) ||
        !unified_detail::CheckedMultiply(capacity.codewordCount, informationBytes - profile.transportOverheadBytes,
            capacity.transportPayloadBytes))
    {
        return result;
    }
    result.valid = true;
    return result;
}

inline constexpr UnifiedFrameCapacityCalculation kUnifiedFrameCapacity = CalculateUnifiedFrameCapacity(kUnifiedVisualProfile);

struct UnifiedLaneCapacity
{
    bool valid = false;
    UnifiedLane lane = UnifiedLane::BaseLuma;
    std::uint32_t firstCodewordSlot = 0;
    std::uint32_t codewordCount = 0;
    std::uint64_t codedBits = 0;
    std::uint64_t codedBytes = 0;
    std::uint64_t informationBytes = 0;
    // Upper bound when every slot in this lane carries Transport.
    std::uint64_t transportPayloadBytes = 0;

    bool operator==(const UnifiedLaneCapacity&) const = default;
};

[[nodiscard]] constexpr UnifiedLaneCapacity GetUnifiedLaneCapacity(const UnifiedLane laneValue) noexcept
{
    UnifiedLaneCapacity capacity;
    capacity.lane = laneValue;
    for (const UnifiedLaneContract& lane : kUnifiedVisualProfile.lanes)
    {
        if (lane.lane != laneValue)
        {
            continue;
        }
        capacity.firstCodewordSlot = lane.firstCodewordSlot;
        capacity.codewordCount = lane.codewordCount;
        capacity.codedBits = static_cast<std::uint64_t>(lane.codewordCount) * kUnifiedVisualProfile.innerCodewordBits;
        capacity.codedBytes = capacity.codedBits / 8;
        capacity.informationBytes = static_cast<std::uint64_t>(lane.codewordCount) * (kUnifiedVisualProfile.innerInformationBits / 8);
        capacity.transportPayloadBytes = static_cast<std::uint64_t>(lane.codewordCount) *
            (kUnifiedVisualProfile.innerInformationBits / 8 - kUnifiedVisualProfile.transportOverheadBytes);
        capacity.valid = true;
        return capacity;
    }
    return capacity;
}

inline constexpr std::array<UnifiedLaneCapacity, 3> kUnifiedLaneCapacities{
    GetUnifiedLaneCapacity(UnifiedLane::BaseLuma),
    GetUnifiedLaneCapacity(UnifiedLane::FineLuma),
    GetUnifiedLaneCapacity(UnifiedLane::Chroma)};

[[nodiscard]] constexpr const UnifiedLaneContract* FindUnifiedLaneForCodewordSlot(const std::uint32_t codewordSlot) noexcept
{
    for (const UnifiedLaneContract& lane : kUnifiedVisualProfile.lanes)
    {
        if (codewordSlot >= lane.firstCodewordSlot && codewordSlot - lane.firstCodewordSlot < lane.codewordCount)
        {
            return &lane;
        }
    }
    return nullptr;
}

[[nodiscard]] constexpr std::uint32_t GetUnifiedMaximumControlSlots() noexcept
{
    const UnifiedMixedSlotContract& mixedSlots = kUnifiedVisualProfile.mixedSlots;
    return mixedSlots.controlRegion.codewordSlotCount >= mixedSlots.minimumTransportSlots ?
        mixedSlots.controlRegion.codewordSlotCount - mixedSlots.minimumTransportSlots : 0;
}

enum class UnifiedSlotKind : std::uint8_t
{
    Transport,
    Control
};

enum class UnifiedControlPriority : std::uint8_t
{
    SessionDescriptor,
    FinalManifest,
    CurrentSegmentDescriptor,
    NotApplicable = 0xFF
};

struct UnifiedSlotAssignment
{
    std::uint32_t codewordSlot = 0;
    UnifiedSlotKind kind = UnifiedSlotKind::Transport;
    UnifiedControlPriority controlPriority = UnifiedControlPriority::NotApplicable;

    bool operator==(const UnifiedSlotAssignment&) const = default;
};

[[nodiscard]] constexpr bool IsUnifiedControlPriority(const UnifiedControlPriority priority) noexcept
{
    return priority == UnifiedControlPriority::SessionDescriptor || priority == UnifiedControlPriority::FinalManifest ||
        priority == UnifiedControlPriority::CurrentSegmentDescriptor;
}

[[nodiscard]] constexpr bool ValidateUnifiedMixedSlotPlan(const std::span<const UnifiedSlotAssignment> assignments) noexcept
{
    if (!kUnifiedFrameCapacity.valid || assignments.size() != kUnifiedFrameCapacity.capacity.codewordCount)
    {
        return false;
    }
    std::array<bool, static_cast<std::size_t>(kUnifiedFrameCapacity.capacity.codewordCount)> seenSlots{};
    std::uint32_t controlSlots = 0;
    for (const UnifiedSlotAssignment& assignment : assignments)
    {
        if (assignment.codewordSlot >= kUnifiedFrameCapacity.capacity.codewordCount || seenSlots[assignment.codewordSlot])
        {
            return false;
        }
        seenSlots[assignment.codewordSlot] = true;
        const UnifiedLaneContract* const lane = FindUnifiedLaneForCodewordSlot(assignment.codewordSlot);
        if (lane == nullptr)
        {
            return false;
        }
        if (assignment.kind == UnifiedSlotKind::Transport)
        {
            if (assignment.controlPriority != UnifiedControlPriority::NotApplicable)
            {
                return false;
            }
            continue;
        }
        const UnifiedControlSlotRegion& controlRegion = kUnifiedVisualProfile.mixedSlots.controlRegion;
        if (assignment.kind != UnifiedSlotKind::Control || lane->lane != controlRegion.lane ||
            assignment.codewordSlot < controlRegion.firstCodewordSlot ||
            assignment.codewordSlot - controlRegion.firstCodewordSlot >= controlRegion.codewordSlotCount ||
            !IsUnifiedControlPriority(assignment.controlPriority))
        {
            return false;
        }
        controlSlots++;
    }
    return controlSlots <= GetUnifiedMaximumControlSlots();
}

enum class UnifiedViewportDisposition : std::uint8_t
{
    InvalidClientArea,
    PausedBelowMinimumScale,
    Active,
    ActiveClampedToMaximumScale
};

struct UnifiedViewportGeometry
{
    UnifiedViewportDisposition disposition = UnifiedViewportDisposition::InvalidClientArea;
    double originX = 0;
    double originY = 0;
    double width = 0;
    double height = 0;
    double scale = 0;

    bool operator==(const UnifiedViewportGeometry&) const = default;
};

[[nodiscard]] constexpr UnifiedViewportGeometry ResolveUnifiedViewport(
    const std::uint32_t clientWidth, const std::uint32_t clientHeight) noexcept
{
    if (clientWidth == 0 || clientHeight == 0 || kUnifiedVisualProfile.presentation.minimumScaleDenominator == 0 ||
        kUnifiedVisualProfile.presentation.maximumScaleDenominator == 0)
    {
        return {};
    }
    const double widthScale = static_cast<double>(clientWidth) / kUnifiedVisualProfile.canvasWidth;
    const double heightScale = static_cast<double>(clientHeight) / kUnifiedVisualProfile.canvasHeight;
    const double availableScale = widthScale < heightScale ? widthScale : heightScale;
    const double minimumScale = static_cast<double>(kUnifiedVisualProfile.presentation.minimumScaleNumerator) /
        kUnifiedVisualProfile.presentation.minimumScaleDenominator;
    if (availableScale < minimumScale)
    {
        return UnifiedViewportGeometry{UnifiedViewportDisposition::PausedBelowMinimumScale};
    }
    const double maximumScale = static_cast<double>(kUnifiedVisualProfile.presentation.maximumScaleNumerator) /
        kUnifiedVisualProfile.presentation.maximumScaleDenominator;
    const bool clamped = availableScale > maximumScale;
    const double scale = clamped ? maximumScale : availableScale;
    const double viewportWidth = kUnifiedVisualProfile.canvasWidth * scale;
    const double viewportHeight = kUnifiedVisualProfile.canvasHeight * scale;
    return UnifiedViewportGeometry{clamped ? UnifiedViewportDisposition::ActiveClampedToMaximumScale :
        UnifiedViewportDisposition::Active, (static_cast<double>(clientWidth) - viewportWidth) / 2.0,
        (static_cast<double>(clientHeight) - viewportHeight) / 2.0, viewportWidth, viewportHeight, scale};
}

[[nodiscard]] constexpr bool CanAdvanceUnifiedFrameSequence(const UnifiedViewportDisposition disposition) noexcept
{
    return disposition == UnifiedViewportDisposition::Active ||
        disposition == UnifiedViewportDisposition::ActiveClampedToMaximumScale;
}

enum class UnifiedErasureScope : std::uint8_t
{
    None,
    DataRegion,
    Lane,
    Frame,
    Invalid
};

enum class UnifiedErasureReason : std::uint8_t
{
    None,
    LocatorFailure,
    BootstrapFailure,
    CanvasClipped,
    IdentityConflict,
    BaseLumaPilotFailure,
    FineLumaPilotFailure,
    ChromaPilotFailure,
    LocalStaleRegion,
    LocalLowDecisionMargin,
    LocalSamplingFailure
};

[[nodiscard]] constexpr bool IsKnownUnifiedErasureReason(const UnifiedErasureReason reason) noexcept
{
    return static_cast<std::uint8_t>(reason) <= static_cast<std::uint8_t>(UnifiedErasureReason::LocalSamplingFailure);
}

[[nodiscard]] constexpr UnifiedErasureScope GetUnifiedErasureScope(const UnifiedErasureReason reason) noexcept
{
    switch (reason)
    {
    case UnifiedErasureReason::None: return UnifiedErasureScope::None;
    case UnifiedErasureReason::LocatorFailure:
    case UnifiedErasureReason::BootstrapFailure:
    case UnifiedErasureReason::CanvasClipped:
    case UnifiedErasureReason::IdentityConflict: return UnifiedErasureScope::Frame;
    case UnifiedErasureReason::BaseLumaPilotFailure:
    case UnifiedErasureReason::FineLumaPilotFailure:
    case UnifiedErasureReason::ChromaPilotFailure: return UnifiedErasureScope::Lane;
    case UnifiedErasureReason::LocalStaleRegion:
    case UnifiedErasureReason::LocalLowDecisionMargin:
    case UnifiedErasureReason::LocalSamplingFailure: return UnifiedErasureScope::DataRegion;
    }
    return UnifiedErasureScope::Invalid;
}

template <UnifiedLane LaneValue>
struct UnifiedLaneObservation
{
    static constexpr UnifiedLane lane = LaneValue;
    UnifiedErasureScope erasureScope = UnifiedErasureScope::None;
    UnifiedErasureReason erasureReason = UnifiedErasureReason::None;

    [[nodiscard]] constexpr bool IsAvailable() const noexcept
    {
        return erasureScope == UnifiedErasureScope::None && erasureReason == UnifiedErasureReason::None;
    }

    bool operator==(const UnifiedLaneObservation&) const = default;
};

using UnifiedBaseLumaObservation = UnifiedLaneObservation<UnifiedLane::BaseLuma>;
using UnifiedFineLumaObservation = UnifiedLaneObservation<UnifiedLane::FineLuma>;
using UnifiedChromaObservation = UnifiedLaneObservation<UnifiedLane::Chroma>;

template <UnifiedLane LaneValue>
[[nodiscard]] constexpr bool ValidateUnifiedLaneObservation(const UnifiedLaneObservation<LaneValue>& observation) noexcept
{
    if (!IsKnownUnifiedErasureReason(observation.erasureReason) ||
        observation.erasureScope != GetUnifiedErasureScope(observation.erasureReason))
    {
        return false;
    }
    if (observation.erasureReason == UnifiedErasureReason::BaseLumaPilotFailure)
    {
        return LaneValue == UnifiedLane::BaseLuma;
    }
    if (observation.erasureReason == UnifiedErasureReason::FineLumaPilotFailure)
    {
        return LaneValue == UnifiedLane::FineLuma;
    }
    if (observation.erasureReason == UnifiedErasureReason::ChromaPilotFailure)
    {
        return LaneValue == UnifiedLane::Chroma;
    }
    return true;
}

[[nodiscard]] constexpr bool ValidateUnifiedVisualProfileStaticContract() noexcept
{
    const UnifiedVisualProfileManifest& profile = kUnifiedVisualProfile;
    if (profile.productProfile != pbprotocol::kUnifiedProductVisualProfile ||
        pbprotocol::ValidateProductVisualProfile(profile.productProfile.visualProfileId, profile.productProfile.visualLayoutVersion) !=
            pbprotocol::ProductVisualProfileAdmission::Accepted ||
        profile.canvasWidth == 0 || profile.canvasHeight == 0 || profile.tileWidth == 0 || profile.tileHeight == 0 ||
        profile.innerFecProfileId == 0 || profile.innerCodewordBits == 0 || profile.innerInformationBits == 0 ||
        profile.innerInformationBits >= profile.innerCodewordBits ||
        profile.carriers[0].carrier == profile.carriers[1].carrier || !kUnifiedFrameCapacity.valid ||
        kUnifiedFrameCapacity.capacity.dataTiles != profile.dataTileCount ||
        kUnifiedFrameCapacity.capacity.reservedLumaBits % 8 != 0 ||
        kUnifiedFrameCapacity.capacity.reservedChromaBits % 8 != 0)
    {
        return false;
    }

    const UnifiedPixelRegion canvas{0, 0, profile.canvasWidth, profile.canvasHeight};
    if (!unified_detail::IsRegionWithin(profile.dataGridBounds, canvas))
    {
        return false;
    }
    std::uint32_t locatorRegions = 0;
    std::uint32_t bootstrapARegions = 0;
    std::uint32_t bootstrapBRegions = 0;
    std::uint32_t timingPilotRegions = 0;
    std::uint32_t calibrationPilotRegions = 0;
    std::uint32_t phasePilotRegions = 0;
    std::uint32_t dataRegions = 0;
    std::uint64_t dataGridOwnedPixels = 0;
    for (std::size_t regionIndex = 0; regionIndex < profile.regions.size(); regionIndex++)
    {
        const UnifiedRegionContract& region = profile.regions[regionIndex];
        if (!unified_detail::IsRegionWithin(region.bounds, canvas))
        {
            return false;
        }
        for (std::size_t previousIndex = 0; previousIndex < regionIndex; previousIndex++)
        {
            if (unified_detail::RegionsOverlap(region.bounds, profile.regions[previousIndex].bounds))
            {
                return false;
            }
        }

        const bool ownsDataGrid = region.kind == UnifiedRegionKind::Data ||
            (region.kind == UnifiedRegionKind::Pilot && region.pilotContent == UnifiedPilotContent::TimingFreshness);
        if (ownsDataGrid)
        {
            std::uint64_t area = 0;
            std::uint64_t nextArea = 0;
            if (!unified_detail::IsRegionWithin(region.bounds, profile.dataGridBounds) ||
                !unified_detail::TryGetRegionArea(region.bounds, area) ||
                !unified_detail::CheckedAdd(dataGridOwnedPixels, area, nextArea))
            {
                return false;
            }
            dataGridOwnedPixels = nextArea;
        }
        else if (unified_detail::RegionsOverlap(region.bounds, profile.dataGridBounds))
        {
            return false;
        }

        switch (region.kind)
        {
        case UnifiedRegionKind::Locator:
            if (region.pilotContent != UnifiedPilotContent::None)
            {
                return false;
            }
            locatorRegions++;
            break;
        case UnifiedRegionKind::BootstrapA:
            if (region.pilotContent != UnifiedPilotContent::None)
            {
                return false;
            }
            bootstrapARegions++;
            break;
        case UnifiedRegionKind::BootstrapB:
            if (region.pilotContent != UnifiedPilotContent::None)
            {
                return false;
            }
            bootstrapBRegions++;
            break;
        case UnifiedRegionKind::Pilot:
            if (region.pilotContent == UnifiedPilotContent::TimingFreshness)
            {
                if (region.bounds.x % profile.tileWidth != 0 || region.bounds.y % profile.tileHeight != 0 ||
                    region.bounds.width % profile.tileWidth != 0 || region.bounds.height % profile.tileHeight != 0)
                {
                    return false;
                }
                timingPilotRegions++;
            }
            else if (region.pilotContent == UnifiedPilotContent::CalibrationReferences)
            {
                calibrationPilotRegions++;
            }
            else if (region.pilotContent == UnifiedPilotContent::PhaseChecker)
            {
                phasePilotRegions++;
            }
            else
            {
                return false;
            }
            break;
        case UnifiedRegionKind::Data:
            if (region.pilotContent != UnifiedPilotContent::None || region.bounds.x % profile.tileWidth != 0 ||
                region.bounds.y % profile.tileHeight != 0 || region.bounds.width % profile.tileWidth != 0 ||
                region.bounds.height % profile.tileHeight != 0)
            {
                return false;
            }
            dataRegions++;
            break;
        }
    }

    std::uint64_t dataGridPixels = 0;
    if (!unified_detail::TryGetRegionArea(profile.dataGridBounds, dataGridPixels) || dataGridOwnedPixels != dataGridPixels ||
        locatorRegions != 4 || bootstrapARegions != 1 || bootstrapBRegions != 1 || timingPilotRegions != 9 ||
        calibrationPilotRegions != 4 || phasePilotRegions != 2 || dataRegions != 10)
    {
        return false;
    }

    std::uint32_t nextSlot = 0;
    for (std::size_t laneIndex = 0; laneIndex < profile.lanes.size(); laneIndex++)
    {
        const UnifiedLaneContract& lane = profile.lanes[laneIndex];
        if (lane.lane != static_cast<UnifiedLane>(laneIndex) || lane.firstCodewordSlot != nextSlot || lane.codewordCount == 0 ||
            (lane.carrier != UnifiedCarrier::Luma && lane.carrier != UnifiedCarrier::Chroma) ||
            (lane.lane == UnifiedLane::Chroma) != (lane.carrier == UnifiedCarrier::Chroma) ||
            lane.codewordCount > std::numeric_limits<std::uint32_t>::max() - nextSlot)
        {
            return false;
        }
        nextSlot += lane.codewordCount;
    }
    const UnifiedControlSlotRegion& controlRegion = profile.mixedSlots.controlRegion;
    const UnifiedLaneCapacity controlLane = GetUnifiedLaneCapacity(controlRegion.lane);
    if (!controlLane.valid || controlRegion.lane != UnifiedLane::BaseLuma ||
        controlRegion.firstCodewordSlot != controlLane.firstCodewordSlot ||
        controlRegion.codewordSlotCount != controlLane.codewordCount ||
        profile.mixedSlots.minimumTransportSlots == 0 ||
        profile.mixedSlots.minimumTransportSlots >= controlLane.codewordCount || GetUnifiedMaximumControlSlots() == 0)
    {
        return false;
    }

    const UnifiedPresentationContract& presentation = profile.presentation;
    if (presentation.minimumScaleNumerator == 0 || presentation.minimumScaleDenominator == 0 ||
        presentation.maximumScaleNumerator == 0 || presentation.maximumScaleDenominator == 0 ||
        static_cast<std::uint64_t>(presentation.minimumScaleNumerator) * presentation.maximumScaleDenominator >=
            static_cast<std::uint64_t>(presentation.maximumScaleNumerator) * presentation.minimumScaleDenominator ||
        presentation.minimumLogicalFramesPerSecond == 0 ||
        presentation.minimumLogicalFramesPerSecond > presentation.defaultLogicalFramesPerSecond ||
        presentation.defaultLogicalFramesPerSecond > presentation.maximumLogicalFramesPerSecond)
    {
        return false;
    }
    return true;
}

// Cross-library validation additionally binds this manifest to PBInnerFec and
// PBProtocol's frozen Robust/Transport implementations.
[[nodiscard]] bool ValidateUnifiedVisualProfile() noexcept;

static_assert(ValidateUnifiedVisualProfileStaticContract());
static_assert(kUnifiedFrameCapacity.capacity.rawVisualBits ==
    kUnifiedFrameCapacity.capacity.usedCodedBits + kUnifiedFrameCapacity.capacity.reservedVisualBits);
static_assert(kUnifiedLaneCapacities[0].firstCodewordSlot + kUnifiedLaneCapacities[0].codewordCount ==
    kUnifiedLaneCapacities[1].firstCodewordSlot);
static_assert(kUnifiedLaneCapacities[1].firstCodewordSlot + kUnifiedLaneCapacities[1].codewordCount ==
    kUnifiedLaneCapacities[2].firstCodewordSlot);
static_assert(kUnifiedLaneCapacities[2].firstCodewordSlot + kUnifiedLaneCapacities[2].codewordCount ==
    kUnifiedFrameCapacity.capacity.codewordCount);
static_assert(GetUnifiedErasureScope(UnifiedErasureReason::LocatorFailure) == UnifiedErasureScope::Frame);
static_assert(GetUnifiedErasureScope(UnifiedErasureReason::ChromaPilotFailure) == UnifiedErasureScope::Lane);
static_assert(GetUnifiedErasureScope(UnifiedErasureReason::LocalStaleRegion) == UnifiedErasureScope::DataRegion);

} // namespace pbmodulation
