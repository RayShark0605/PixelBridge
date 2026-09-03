#include "pbmodulation/unified_visual_profile.h"

#include "pbinnerfec/inner_fec_profile.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/remote_visual.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbmodulation/shape_chroma.h"
#include "pbprotocol/product_visual_profile.h"
#include "pbprotocol/transport_block_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace
{

using namespace pbmodulation;

[[nodiscard]] constexpr std::array<UnifiedSlotAssignment, 31> MakeTransportSlotPlan() noexcept
{
    std::array<UnifiedSlotAssignment, 31> assignments{};
    for (std::size_t slot = 0; slot < assignments.size(); slot++)
    {
        assignments[slot] = UnifiedSlotAssignment{
            static_cast<std::uint32_t>(slot), UnifiedSlotKind::Transport, UnifiedControlPriority::NotApplicable};
    }
    return assignments;
}

[[nodiscard]] constexpr std::uint32_t CountWholeFrameErasureReasons() noexcept
{
    constexpr std::array<UnifiedErasureReason, 11> reasons{
        UnifiedErasureReason::None,
        UnifiedErasureReason::LocatorFailure,
        UnifiedErasureReason::BootstrapFailure,
        UnifiedErasureReason::CanvasClipped,
        UnifiedErasureReason::IdentityConflict,
        UnifiedErasureReason::BaseLumaPilotFailure,
        UnifiedErasureReason::FineLumaPilotFailure,
        UnifiedErasureReason::ChromaPilotFailure,
        UnifiedErasureReason::LocalStaleRegion,
        UnifiedErasureReason::LocalLowDecisionMargin,
        UnifiedErasureReason::LocalSamplingFailure};
    std::uint32_t count = 0;
    for (const UnifiedErasureReason reason : reasons)
    {
        if (GetUnifiedErasureScope(reason) == UnifiedErasureScope::Frame)
        {
            count++;
        }
    }
    return count;
}

[[nodiscard]] bool Overlaps(const UnifiedPixelRegion& left, const UnifiedPixelRegion& right) noexcept
{
    const std::uint64_t leftRight = static_cast<std::uint64_t>(left.x) + left.width;
    const std::uint64_t leftBottom = static_cast<std::uint64_t>(left.y) + left.height;
    const std::uint64_t rightRight = static_cast<std::uint64_t>(right.x) + right.width;
    const std::uint64_t rightBottom = static_cast<std::uint64_t>(right.y) + right.height;
    return left.x < rightRight && right.x < leftRight && left.y < rightBottom && right.y < leftBottom;
}

} // namespace

TEST_CASE("Unified LC4 manifest pins every product and geometry constant", "[pbmodulation][unified][profile]")
{
    using namespace pbmodulation;
    constexpr std::array<UnifiedRegionContract, 31> expectedRegions{
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
        UnifiedRegionContract{UnifiedRegionKind::Data, UnifiedPilotContent::None, UnifiedPixelRegion{96, 920, 1728, 64}}};
    constexpr std::array<UnifiedCarrierContract, 2> expectedCarriers{
        UnifiedCarrierContract{UnifiedCarrier::Luma, 4},
        UnifiedCarrierContract{UnifiedCarrier::Chroma, 2}};
    constexpr std::array<UnifiedLaneContract, 3> expectedLanes{
        UnifiedLaneContract{UnifiedLane::BaseLuma, UnifiedCarrier::Luma, 0, 17},
        UnifiedLaneContract{UnifiedLane::FineLuma, UnifiedCarrier::Luma, 17, 4},
        UnifiedLaneContract{UnifiedLane::Chroma, UnifiedCarrier::Chroma, 21, 10}};
    constexpr UnifiedPresentationContract expectedPresentation{3, 4, 2, 1, 1, 15, 60,
        UnifiedViewportFilter::Point, UnifiedViewportFit::PreserveAspectRatio,
        UnifiedViewportPlacement::CenteredLetterbox, UnifiedUndersizeBehavior::NeutralMattePauseWithoutBootstrap};

    STATIC_REQUIRE(kUnifiedVisualProfile.productProfile.name == std::string_view{"PB-Unified-LC4-V1"});
    STATIC_REQUIRE(kUnifiedVisualProfile.productProfile.visualProfileId == 0x5042554E494C4331ULL);
    STATIC_REQUIRE(kUnifiedVisualProfile.productProfile.visualLayoutVersion == 8);
    STATIC_REQUIRE(kUnifiedVisualProfile.pixelFormat == UnifiedPixelFormat::Bgra8UnormSdr);
    STATIC_REQUIRE(kUnifiedVisualProfile.canvasWidth == 1920);
    STATIC_REQUIRE(kUnifiedVisualProfile.canvasHeight == 1080);
    STATIC_REQUIRE(kUnifiedVisualProfile.alphaCodeValue == 255);
    STATIC_REQUIRE(kUnifiedVisualProfile.tileWidth == 4);
    STATIC_REQUIRE(kUnifiedVisualProfile.tileHeight == 4);
    STATIC_REQUIRE(kUnifiedVisualProfile.dataTileCount == 86688);
    STATIC_REQUIRE(kUnifiedVisualProfile.innerFecProfileId == 0x36BC661265E826C3ULL);
    STATIC_REQUIRE(kUnifiedVisualProfile.innerCodewordBits == 16200);
    STATIC_REQUIRE(kUnifiedVisualProfile.innerInformationBits == 10800);
    STATIC_REQUIRE(kUnifiedVisualProfile.transportOverheadBytes == 36);
    STATIC_REQUIRE(kUnifiedVisualProfile.dataGridBounds == UnifiedPixelRegion{96, 96, 1728, 888});
    STATIC_REQUIRE(kUnifiedVisualProfile.regions == expectedRegions);
    STATIC_REQUIRE(kUnifiedVisualProfile.carriers == expectedCarriers);
    STATIC_REQUIRE(kUnifiedVisualProfile.lanes == expectedLanes);
    STATIC_REQUIRE(kUnifiedVisualProfile.mixedSlots ==
        UnifiedMixedSlotContract{UnifiedControlSlotRegion{UnifiedLane::BaseLuma, 0, 17}, 1});
    STATIC_REQUIRE(kUnifiedVisualProfile.presentation == expectedPresentation);
    STATIC_REQUIRE(kUnifiedVisualProfileCatalog.size() == 1);
    STATIC_REQUIRE(kUnifiedVisualProfileCatalog.front() == &kUnifiedVisualProfile);
    STATIC_REQUIRE(FindUnifiedVisualProfile(0x5042554E494C4331ULL, 8) == &kUnifiedVisualProfile);
    STATIC_REQUIRE(FindUnifiedVisualProfile(0x5042554E494C4331ULL, 7) == nullptr);
    STATIC_REQUIRE(FindUnifiedVisualProfile(kRemoteVisualLowFpsProfileId, kRemoteVisualLowFpsLayoutVersion) == nullptr);
    STATIC_REQUIRE(HasUnifiedPilotContent(UnifiedPilotContent::CalibrationReferences,
        UnifiedPilotContent::BlackWhiteMidGray));
    STATIC_REQUIRE(HasUnifiedPilotContent(UnifiedPilotContent::CalibrationReferences,
        UnifiedPilotContent::LumaLevelLadder));
    STATIC_REQUIRE(HasUnifiedPilotContent(UnifiedPilotContent::CalibrationReferences,
        UnifiedPilotContent::ActiveChromaStates));
    STATIC_REQUIRE(HasUnifiedPilotContent(UnifiedPilotContent::CalibrationReferences,
        UnifiedPilotContent::NeutralChroma));
    STATIC_REQUIRE_FALSE(HasUnifiedPilotContent(UnifiedPilotContent::CalibrationReferences,
        UnifiedPilotContent::PhaseChecker));
    STATIC_REQUIRE(ValidateUnifiedVisualProfileStaticContract());
    REQUIRE(ValidateUnifiedVisualProfile());
}

TEST_CASE("Unified LC4 capacity is exact and codewords never cross lanes", "[pbmodulation][unified][capacity]")
{
    using namespace pbmodulation;
    STATIC_REQUIRE(kUnifiedFrameCapacity.valid);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.dataPixels == 1387008);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.dataTiles == 86688);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.rawLumaBits == 346752);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.rawChromaBits == 173376);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.rawVisualBits == 520128);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.codewordCount == 31);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.usedLumaBits == 340200);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.usedChromaBits == 162000);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.usedCodedBits == 502200);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.reservedLumaBits == 6552);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.reservedChromaBits == 11376);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.reservedVisualBits == 17928);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.codedBytes == 62775);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.informationBytes == 41850);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.transportOverheadBytes == 1116);
    STATIC_REQUIRE(kUnifiedFrameCapacity.capacity.transportPayloadBytes == 40734);
    STATIC_REQUIRE(kUnifiedVisualProfile.innerCodewordBits / 8 == 2025);
    STATIC_REQUIRE(kUnifiedVisualProfile.innerInformationBits / 8 == 1350);
    STATIC_REQUIRE(kUnifiedVisualProfile.innerInformationBits / 8 - kUnifiedVisualProfile.transportOverheadBytes == 1314);

    STATIC_REQUIRE(kUnifiedLaneCapacities[0] == UnifiedLaneCapacity{
        true, UnifiedLane::BaseLuma, 0, 17, 275400, 34425, 22950, 22338});
    STATIC_REQUIRE(kUnifiedLaneCapacities[1] == UnifiedLaneCapacity{
        true, UnifiedLane::FineLuma, 17, 4, 64800, 8100, 5400, 5256});
    STATIC_REQUIRE(kUnifiedLaneCapacities[2] == UnifiedLaneCapacity{
        true, UnifiedLane::Chroma, 21, 10, 162000, 20250, 13500, 13140});
    STATIC_REQUIRE(GetUnifiedMaximumControlSlots() == 16);
    STATIC_REQUIRE(FindUnifiedLaneForCodewordSlot(0)->lane == UnifiedLane::BaseLuma);
    STATIC_REQUIRE(FindUnifiedLaneForCodewordSlot(16)->lane == UnifiedLane::BaseLuma);
    STATIC_REQUIRE(FindUnifiedLaneForCodewordSlot(17)->lane == UnifiedLane::FineLuma);
    STATIC_REQUIRE(FindUnifiedLaneForCodewordSlot(20)->lane == UnifiedLane::FineLuma);
    STATIC_REQUIRE(FindUnifiedLaneForCodewordSlot(21)->lane == UnifiedLane::Chroma);
    STATIC_REQUIRE(FindUnifiedLaneForCodewordSlot(30)->lane == UnifiedLane::Chroma);
    STATIC_REQUIRE(FindUnifiedLaneForCodewordSlot(31) == nullptr);

    const pbinnerfec::InnerFecProfile* const robust =
        pbinnerfec::GetInnerFecProfile(pbinnerfec::kInnerFecProfileIdRobust);
    REQUIRE(robust != nullptr);
    CHECK(kUnifiedVisualProfile.innerFecProfileId == robust->profileId);
    CHECK(kUnifiedVisualProfile.innerCodewordBits == robust->nBits);
    CHECK(kUnifiedVisualProfile.innerInformationBits == robust->kBits);
    CHECK(kUnifiedVisualProfile.innerCodewordBits / 8 == robust->GetCodewordByteCount());
    CHECK(kUnifiedVisualProfile.innerInformationBits / 8 == robust->GetInfoByteCount());
    CHECK(kUnifiedVisualProfile.transportOverheadBytes == pbprotocol::kTransportMinimumBlockBytes);
}

TEST_CASE("Unified capacity calculation rejects overflow and impossible packing", "[pbmodulation][unified][capacity][negative]")
{
    using namespace pbmodulation;
    UnifiedVisualProfileManifest overflow = kUnifiedVisualProfile;
    overflow.regions[21].bounds.width = std::numeric_limits<std::uint32_t>::max();
    overflow.regions[21].bounds.height = std::numeric_limits<std::uint32_t>::max();
    CHECK_FALSE(CalculateUnifiedFrameCapacity(overflow).valid);

    UnifiedVisualProfileManifest zeroTile = kUnifiedVisualProfile;
    zeroTile.tileWidth = 0;
    CHECK_FALSE(CalculateUnifiedFrameCapacity(zeroTile).valid);

    UnifiedVisualProfileManifest overfilledLuma = kUnifiedVisualProfile;
    overfilledLuma.lanes[0].codewordCount = std::numeric_limits<std::uint32_t>::max();
    CHECK_FALSE(CalculateUnifiedFrameCapacity(overfilledLuma).valid);

    UnifiedVisualProfileManifest oversizedTransport = kUnifiedVisualProfile;
    oversizedTransport.transportOverheadBytes = kUnifiedVisualProfile.innerInformationBits / 8 + 1;
    CHECK_FALSE(CalculateUnifiedFrameCapacity(oversizedTransport).valid);

    UnifiedVisualProfileManifest impossibleFec = kUnifiedVisualProfile;
    impossibleFec.innerInformationBits = impossibleFec.innerCodewordBits;
    CHECK_FALSE(CalculateUnifiedFrameCapacity(impossibleFec).valid);

    UnifiedVisualProfileManifest missingCarrierCapacity = kUnifiedVisualProfile;
    missingCarrierCapacity.carriers[1].bitsPerTile = 0;
    CHECK_FALSE(CalculateUnifiedFrameCapacity(missingCarrierCapacity).valid);

    UnifiedVisualProfileManifest unknownLaneCarrier = kUnifiedVisualProfile;
    unknownLaneCarrier.lanes[2].carrier = static_cast<UnifiedCarrier>(0xFF);
    CHECK_FALSE(CalculateUnifiedFrameCapacity(unknownLaneCarrier).valid);
}

TEST_CASE("Unified region catalog independently partitions the data grid", "[pbmodulation][unified][regions]")
{
    using namespace pbmodulation;
    const UnifiedPixelRegion grid = kUnifiedVisualProfile.dataGridBounds;
    std::vector<std::uint8_t> ownership(static_cast<std::size_t>(grid.width) * grid.height, 0);
    std::uint64_t dataPixels = 0;
    std::uint64_t timingPixels = 0;
    bool overlappingOwnership = false;

    for (std::size_t regionIndex = 0; regionIndex < kUnifiedVisualProfile.regions.size(); regionIndex++)
    {
        const UnifiedRegionContract& region = kUnifiedVisualProfile.regions[regionIndex];
        CHECK(region.bounds.x + region.bounds.width <= kUnifiedVisualProfile.canvasWidth);
        CHECK(region.bounds.y + region.bounds.height <= kUnifiedVisualProfile.canvasHeight);
        for (std::size_t previousIndex = 0; previousIndex < regionIndex; previousIndex++)
        {
            CHECK_FALSE(Overlaps(region.bounds, kUnifiedVisualProfile.regions[previousIndex].bounds));
        }

        const bool data = region.kind == UnifiedRegionKind::Data;
        const bool timing = region.kind == UnifiedRegionKind::Pilot &&
            region.pilotContent == UnifiedPilotContent::TimingFreshness;
        if (!data && !timing)
        {
            CHECK_FALSE(Overlaps(region.bounds, grid));
            continue;
        }
        REQUIRE(region.bounds.x >= grid.x);
        REQUIRE(region.bounds.y >= grid.y);
        REQUIRE(region.bounds.x + region.bounds.width <= grid.x + grid.width);
        REQUIRE(region.bounds.y + region.bounds.height <= grid.y + grid.height);
        for (std::uint32_t row = 0; row < region.bounds.height; row++)
        {
            for (std::uint32_t column = 0; column < region.bounds.width; column++)
            {
                const std::size_t offset = static_cast<std::size_t>(region.bounds.y - grid.y + row) * grid.width +
                    (region.bounds.x - grid.x + column);
                if (ownership[offset] != 0)
                {
                    overlappingOwnership = true;
                }
                ownership[offset] = data ? 1 : 2;
            }
        }
        const std::uint64_t pixels = static_cast<std::uint64_t>(region.bounds.width) * region.bounds.height;
        if (data)
        {
            dataPixels += pixels;
        }
        else
        {
            timingPixels += pixels;
        }
    }

    bool fullyOwned = true;
    for (const std::uint8_t owner : ownership)
    {
        if (owner != 1 && owner != 2)
        {
            fullyOwned = false;
            break;
        }
    }
    REQUIRE_FALSE(overlappingOwnership);
    REQUIRE(fullyOwned);
    CHECK(dataPixels == 1387008);
    CHECK(timingPixels == 147456);
    CHECK(dataPixels + timingPixels == static_cast<std::uint64_t>(grid.width) * grid.height);
    CHECK(dataPixels / (kUnifiedVisualProfile.tileWidth * kUnifiedVisualProfile.tileHeight) == 86688);
}

TEST_CASE("Unified mixed slot plans retain Transport and reject cross-lane Control",
    "[pbmodulation][unified][mixed-slots]")
{
    using namespace pbmodulation;
    constexpr std::array<UnifiedSlotAssignment, 31> allTransport = MakeTransportSlotPlan();
    STATIC_REQUIRE(ValidateUnifiedMixedSlotPlan(allTransport));
    STATIC_REQUIRE(static_cast<std::uint8_t>(UnifiedControlPriority::SessionDescriptor) <
        static_cast<std::uint8_t>(UnifiedControlPriority::FinalManifest));
    STATIC_REQUIRE(static_cast<std::uint8_t>(UnifiedControlPriority::FinalManifest) <
        static_cast<std::uint8_t>(UnifiedControlPriority::CurrentSegmentDescriptor));

    auto mixed = allTransport;
    mixed[0] = UnifiedSlotAssignment{0, UnifiedSlotKind::Control, UnifiedControlPriority::SessionDescriptor};
    mixed[1] = UnifiedSlotAssignment{1, UnifiedSlotKind::Control, UnifiedControlPriority::FinalManifest};
    mixed[2] = UnifiedSlotAssignment{2, UnifiedSlotKind::Control, UnifiedControlPriority::CurrentSegmentDescriptor};
    REQUIRE(ValidateUnifiedMixedSlotPlan(mixed));

    auto allBaseControl = allTransport;
    for (std::uint32_t slot = 0; slot < 17; slot++)
    {
        allBaseControl[slot] = UnifiedSlotAssignment{slot, UnifiedSlotKind::Control,
            UnifiedControlPriority::CurrentSegmentDescriptor};
    }
    REQUIRE_FALSE(ValidateUnifiedMixedSlotPlan(allBaseControl));

    auto fineControl = mixed;
    fineControl[17] = UnifiedSlotAssignment{17, UnifiedSlotKind::Control, UnifiedControlPriority::SessionDescriptor};
    REQUIRE_FALSE(ValidateUnifiedMixedSlotPlan(fineControl));

    auto transportWithPriority = mixed;
    transportWithPriority[30].controlPriority = UnifiedControlPriority::SessionDescriptor;
    REQUIRE_FALSE(ValidateUnifiedMixedSlotPlan(transportWithPriority));

    auto controlWithoutPriority = mixed;
    controlWithoutPriority[0].controlPriority = UnifiedControlPriority::NotApplicable;
    REQUIRE_FALSE(ValidateUnifiedMixedSlotPlan(controlWithoutPriority));

    auto duplicateSlot = mixed;
    duplicateSlot[30].codewordSlot = 29;
    REQUIRE_FALSE(ValidateUnifiedMixedSlotPlan(duplicateSlot));
    auto outOfRangeSlot = mixed;
    outOfRangeSlot[30].codewordSlot = 31;
    REQUIRE_FALSE(ValidateUnifiedMixedSlotPlan(outOfRangeSlot));
    auto unknownKind = mixed;
    unknownKind[0].kind = static_cast<UnifiedSlotKind>(0xFF);
    REQUIRE_FALSE(ValidateUnifiedMixedSlotPlan(unknownKind));
    REQUIRE_FALSE(ValidateUnifiedMixedSlotPlan(std::span<const UnifiedSlotAssignment>(mixed).first(30)));
}

TEST_CASE("Unified viewport contract centers letterbox and pauses below 0.75x",
    "[pbmodulation][unified][viewport]")
{
    using namespace pbmodulation;
    constexpr UnifiedViewportGeometry exact = ResolveUnifiedViewport(1920, 1080);
    STATIC_REQUIRE(exact == UnifiedViewportGeometry{UnifiedViewportDisposition::Active, 0, 0, 1920, 1080, 1});

    constexpr UnifiedViewportGeometry verticalLetterbox = ResolveUnifiedViewport(1920, 1200);
    STATIC_REQUIRE(verticalLetterbox == UnifiedViewportGeometry{
        UnifiedViewportDisposition::Active, 0, 60, 1920, 1080, 1});
    constexpr UnifiedViewportGeometry horizontalLetterbox = ResolveUnifiedViewport(2560, 1080);
    STATIC_REQUIRE(horizontalLetterbox == UnifiedViewportGeometry{
        UnifiedViewportDisposition::Active, 320, 0, 1920, 1080, 1});
    constexpr UnifiedViewportGeometry minimum = ResolveUnifiedViewport(1440, 810);
    STATIC_REQUIRE(minimum == UnifiedViewportGeometry{
        UnifiedViewportDisposition::Active, 0, 0, 1440, 810, 0.75});
    constexpr UnifiedViewportGeometry oversized = ResolveUnifiedViewport(4000, 2400);
    STATIC_REQUIRE(oversized == UnifiedViewportGeometry{
        UnifiedViewportDisposition::ActiveClampedToMaximumScale, 80, 120, 3840, 2160, 2});
    STATIC_REQUIRE(ResolveUnifiedViewport(1439, 810).disposition == UnifiedViewportDisposition::PausedBelowMinimumScale);
    STATIC_REQUIRE(ResolveUnifiedViewport(1440, 809).disposition == UnifiedViewportDisposition::PausedBelowMinimumScale);
    STATIC_REQUIRE(ResolveUnifiedViewport(0, 1080).disposition == UnifiedViewportDisposition::InvalidClientArea);
    STATIC_REQUIRE_FALSE(CanAdvanceUnifiedFrameSequence(UnifiedViewportDisposition::PausedBelowMinimumScale));
    STATIC_REQUIRE(CanAdvanceUnifiedFrameSequence(UnifiedViewportDisposition::Active));
    STATIC_REQUIRE(CanAdvanceUnifiedFrameSequence(UnifiedViewportDisposition::ActiveClampedToMaximumScale));
}

TEST_CASE("Unified lane observations enforce independent erasure scope", "[pbmodulation][unified][erasure]")
{
    using namespace pbmodulation;
    STATIC_REQUIRE(UnifiedBaseLumaObservation::lane == UnifiedLane::BaseLuma);
    STATIC_REQUIRE(UnifiedFineLumaObservation::lane == UnifiedLane::FineLuma);
    STATIC_REQUIRE(UnifiedChromaObservation::lane == UnifiedLane::Chroma);
    STATIC_REQUIRE(CountWholeFrameErasureReasons() == 4);

    constexpr UnifiedBaseLumaObservation baseAvailable{};
    constexpr UnifiedFineLumaObservation finePilot{
        UnifiedErasureScope::Lane, UnifiedErasureReason::FineLumaPilotFailure};
    constexpr UnifiedChromaObservation chromaPilot{
        UnifiedErasureScope::Lane, UnifiedErasureReason::ChromaPilotFailure};
    constexpr UnifiedChromaObservation chromaStale{
        UnifiedErasureScope::DataRegion, UnifiedErasureReason::LocalStaleRegion};
    constexpr UnifiedBaseLumaObservation frameConflict{
        UnifiedErasureScope::Frame, UnifiedErasureReason::IdentityConflict};
    constexpr UnifiedChromaObservation wrongLanePilot{
        UnifiedErasureScope::Lane, UnifiedErasureReason::BaseLumaPilotFailure};
    constexpr UnifiedFineLumaObservation wrongScope{
        UnifiedErasureScope::Frame, UnifiedErasureReason::FineLumaPilotFailure};
    constexpr UnifiedBaseLumaObservation unknownReason{
        UnifiedErasureScope::Frame, static_cast<UnifiedErasureReason>(0xFF)};

    STATIC_REQUIRE(baseAvailable.IsAvailable());
    STATIC_REQUIRE(ValidateUnifiedLaneObservation(baseAvailable));
    STATIC_REQUIRE(ValidateUnifiedLaneObservation(finePilot));
    STATIC_REQUIRE(ValidateUnifiedLaneObservation(chromaPilot));
    STATIC_REQUIRE(ValidateUnifiedLaneObservation(chromaStale));
    STATIC_REQUIRE(ValidateUnifiedLaneObservation(frameConflict));
    STATIC_REQUIRE_FALSE(ValidateUnifiedLaneObservation(wrongLanePilot));
    STATIC_REQUIRE_FALSE(ValidateUnifiedLaneObservation(wrongScope));
    STATIC_REQUIRE_FALSE(ValidateUnifiedLaneObservation(unknownReason));
    STATIC_REQUIRE(GetUnifiedErasureScope(static_cast<UnifiedErasureReason>(0xFF)) == UnifiedErasureScope::Invalid);
    STATIC_REQUIRE(GetUnifiedErasureScope(UnifiedErasureReason::ChromaPilotFailure) == UnifiedErasureScope::Lane);
    STATIC_REQUIRE(GetUnifiedErasureScope(UnifiedErasureReason::LocalStaleRegion) == UnifiedErasureScope::DataRegion);
}

TEST_CASE("Historical experimental profiles remain explicit but fail product admission",
    "[pbmodulation][unified][internal-profile]")
{
    using namespace pbmodulation;
    constexpr std::array<std::uint64_t, 4> historicalProfiles{
        kDesktopLevels2ProfileId,
        kShapeChromaProfileId,
        kRemoteVisualProfileId,
        kRemoteVisualLowFpsProfileId};
    constexpr std::array<std::uint8_t, 4> historicalLayouts{
        kDesktopLevelsLayoutVersion,
        kShapeChromaLayoutVersion,
        kRemoteVisualLayoutVersion,
        kRemoteVisualLowFpsLayoutVersion};
    for (std::size_t index = 0; index < historicalProfiles.size(); index++)
    {
        CHECK_FALSE(pbprotocol::IsProductSessionVisualProfileId(historicalProfiles[index]));
        CHECK(pbprotocol::ValidateProductVisualProfile(historicalProfiles[index], historicalLayouts[index]) ==
            pbprotocol::ProductVisualProfileAdmission::UnsupportedProfile);
    }

    REQUIRE(GetDesktopLevelsProfile(kDesktopLevels2ProfileId) != nullptr);
    REQUIRE(GetDesktopLevelsProfile(kDesktopLevels4ProfileId) != nullptr);
    LocalDesktopRegion historicalRegion{};
    CHECK(GetLocalDesktopDataTile(kShapeChromaTilePixels, 0, historicalRegion));
    CHECK(GetRemoteVisualTile(0, historicalRegion));
}
