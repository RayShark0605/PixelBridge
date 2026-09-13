#include "pbmodulation/unified_visual.h"
#include <fstream>
#include <filesystem>
#include <cstring>
#include "pbprotocol/blake3_digest.h"
#include "pbmodulation/unified_visual_mapping.h"

#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/product_visual_profile.h"
#include "pbprotocol/protocol_version.h"
#include "pbprotocol/transport_block_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Gray-state experimental identity (PB-Experimental-GrayStates-1, layout 12):
// the manifest, mapping and slot contract are identical to the SC6-V3 product
// profile, but the four foreground states and the calibration state stripes
// are gray luma levels, so the second carrier's two bits per tile survive
// chroma-destroying (4:2:0) remote links. These tests prove the mechanism and
// its fail-closed boundaries at the modulation layer; the SC6-V3 suite keeps
// proving the color carrier unchanged.
namespace
{

using namespace pbmodulation;

struct GrayFixture
{
    std::array<std::byte, kLocalDesktopBootstrapRecordBytes> bootstrap{};
    std::array<std::byte, kUnifiedGrayCodedFrameBytes> coded{};
    std::array<UnifiedSlotAssignment, kUnifiedGrayFrameCodewordCount> plan{};
    std::array<std::vector<std::byte>, kUnifiedGrayFrameCodewordCount> expected{};
};

std::array<std::byte, kLocalDesktopBootstrapRecordBytes> MakeGrayBootstrap(const std::uint64_t sequence)
{
    std::array<std::byte, kLocalDesktopBootstrapRecordBytes> bytes{};
    const pbprotocol::BootstrapRecord record{pbprotocol::kBootstrapVersion, pbprotocol::GetProtocolVersion(),
        pbprotocol::kGrayStatesExperimentalProfile.visualLayoutVersion,
        pbprotocol::kGrayStatesExperimentalProfile.visualProfileId, pbprotocol::SessionTag{0x8877665544332211ULL},
        sequence, 0x31323334U, 0};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
    return bytes;
}

std::vector<std::byte> MakeTransport(const pbprotocol::SessionTag sessionTag, const std::uint32_t slot)
{
    std::vector<std::byte> payload(48 + slot % 13);
    for (std::size_t index = 0; index < payload.size(); index++)
    {
        payload[index] = static_cast<std::byte>((slot * 41 + index * 17 + 7) & 0xFFU);
    }
    const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData,
        pbprotocol::kTransportProtocolMinor, 0, sessionTag, 900 + slot, 1000 + slot,
        static_cast<std::uint16_t>(payload.size())};
    std::vector<std::byte> block(pbprotocol::GetTransportSerializedSize(header));
    REQUIRE(pbprotocol::SerializeTransportBlock(header, payload, block));
    return block;
}

std::vector<std::byte> MakeControl(const pbprotocol::SessionTag sessionTag)
{
    const std::array<std::byte, 9> payload{std::byte{2}, std::byte{4}, std::byte{6}, std::byte{8},
        std::byte{10}, std::byte{12}, std::byte{14}, std::byte{16}, std::byte{18}};
    const pbprotocol::ControlRecordView record{pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor, 91, sessionTag, payload};
    const auto size = pbprotocol::GetSerializedSize(record);
    REQUIRE(size);
    std::vector<std::byte> bytes(size.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    return bytes;
}

GrayFixture BuildGrayFixture(const std::uint64_t sequence)
{
    GrayFixture fixture;
    fixture.bootstrap = MakeGrayBootstrap(sequence);
    const auto parsed = pbprotocol::ParseBootstrapRecord(fixture.bootstrap);
    REQUIRE(parsed);
    for (std::uint32_t slot = 0; slot < kUnifiedGrayFrameCodewordCount; slot++)
    {
        UnifiedSlotAssignment& assignment = fixture.plan[slot];
        assignment.codewordSlot = slot;
        std::array<std::byte, kUnifiedInformationBytes> information{};
        if (slot == 0)
        {
            assignment.kind = UnifiedSlotKind::Control;
            assignment.controlPriority = UnifiedControlPriority::SessionDescriptor;
            fixture.expected[slot] = MakeControl(parsed.Value().sessionTag);
            std::copy(fixture.expected[slot].begin(), fixture.expected[slot].end(), information.begin());
        }
        else
        {
            assignment.kind = UnifiedSlotKind::Transport;
            assignment.controlPriority = UnifiedControlPriority::NotApplicable;
            fixture.expected[slot] = MakeTransport(parsed.Value().sessionTag, slot);
            REQUIRE(pbprotocol::FrameTransportBlockIntoInfoBlock(fixture.expected[slot], information.size(), information));
        }
        REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information,
            std::span<std::byte>(fixture.coded).subspan(static_cast<std::size_t>(slot) * kUnifiedCodewordBytes,
                kUnifiedCodewordBytes)));
    }
    REQUIRE(ValidateUnifiedGrayMixedSlotPlan(fixture.plan));
    return fixture;
}

std::vector<std::byte> RenderGray(const GrayFixture& fixture)
{
    std::vector<std::byte> pixels(kUnifiedFrameBgraBytes);
    REQUIRE(static_cast<bool>(EncodeUnifiedVisualFrame(fixture.bootstrap, fixture.coded, pixels)));
    return pixels;
}

UnifiedVisualCpuOracle MakeOracle()
{
    auto created = UnifiedVisualCpuOracle::Create(UnifiedVisualCpuOracle::RequiredBytes());
    REQUIRE(static_cast<bool>(created));
    return std::move(created).Value();
}

pbmodulation::LumaView View(const std::span<const std::byte> pixels)
{
    return pbmodulation::LumaView{pixels, kUnifiedVisualProfile.canvasWidth, kUnifiedVisualProfile.canvasHeight,
        kUnifiedVisualProfile.canvasWidth * 4, pbmodulation::LumaPixelFormat::Bgra8};
}

pbmodulation::UnifiedExpectedFrameIdentity GrayIdentity()
{
    return pbmodulation::UnifiedExpectedFrameIdentity{false, {}, false, {},
        pbprotocol::kGrayStatesExperimentalProfile.visualProfileId,
        pbprotocol::kGrayStatesExperimentalProfile.visualLayoutVersion};
}

// Idealized 4:2:0 chroma subsampling: each pixel keeps its exact BT.709 luma
// while the opponent chroma is replaced by the 2x2 block mean. This is the
// transform whose color-plane averaging erases the SC6-V3 color carrier; for
// gray pixels (zero opponent by construction) it is exactly the identity, so
// the gray state levels and every mask chip pass through bit-identically.
std::vector<std::byte> SubsampleChroma420(const std::span<const std::byte> source)
{
    std::vector<std::byte> output(source.size());
    std::copy(source.begin(), source.end(), output.begin());
    const std::uint32_t width = kUnifiedVisualProfile.canvasWidth;
    const std::uint32_t height = kUnifiedVisualProfile.canvasHeight;
    for (std::uint32_t blockRow = 0; blockRow < height / 2; blockRow++)
    {
        for (std::uint32_t blockColumn = 0; blockColumn < width / 2; blockColumn++)
        {
            double opponentBlueSum = 0;
            double opponentRedSum = 0;
            std::array<double, 4> luma{};
            std::size_t sample = 0;
            for (std::uint32_t row = 0; row < 2; row++)
            {
                for (std::uint32_t column = 0; column < 2; column++)
                {
                    const std::size_t index =
                        (static_cast<std::size_t>(blockRow * 2 + row) * width + blockColumn * 2 + column) * 4;
                    const double blue = std::to_integer<std::uint8_t>(output[index]);
                    const double green = std::to_integer<std::uint8_t>(output[index + 1]);
                    const double red = std::to_integer<std::uint8_t>(output[index + 2]);
                    luma[sample] = 0.0722 * blue + 0.7152 * green + 0.2126 * red;
                    opponentBlueSum += blue - luma[sample];
                    opponentRedSum += red - luma[sample];
                    sample++;
                }
            }
            const double opponentBlueMean = opponentBlueSum / 4.0;
            const double opponentRedMean = opponentRedSum / 4.0;
            sample = 0;
            for (std::uint32_t row = 0; row < 2; row++)
            {
                for (std::uint32_t column = 0; column < 2; column++)
                {
                    const std::size_t index =
                        (static_cast<std::size_t>(blockRow * 2 + row) * width + blockColumn * 2 + column) * 4;
                    const double blue = luma[sample] + opponentBlueMean;
                    const double red = luma[sample] + opponentRedMean;
                    const double green = luma[sample] -
                        (0.0722 * opponentBlueMean + 0.2126 * opponentRedMean) / 0.7152;
                    output[index] = static_cast<std::byte>(std::lround(std::clamp(blue, 0.0, 255.0)));
                    output[index + 1] = static_cast<std::byte>(std::lround(std::clamp(green, 0.0, 255.0)));
                    output[index + 2] = static_cast<std::byte>(std::lround(std::clamp(red, 0.0, 255.0)));
                    sample++;
                }
            }
        }
    }
    return output;
}

// Replace the four active state stripes of every calibration pilot with one
// common level so the calibrated gray centroids collapse and the state lane
// must erase itself (fail-closed pilot gate).
void CollapseStateStripes(const std::span<std::byte> pixels)
{
    for (const UnifiedRegionContract& contract : kUnifiedVisualProfile.regions)
    {
        if (contract.kind != UnifiedRegionKind::Pilot ||
            !HasUnifiedPilotContent(contract.pilotContent, UnifiedPilotContent::ActiveChromaStates))
        {
            continue;
        }
        for (std::uint32_t row = kUnifiedCalibrationLumaRows + kUnifiedCalibrationNeutralRows;
             row < contract.bounds.height; row++)
        {
            for (std::uint32_t column = 0; column < contract.bounds.width; column++)
            {
                std::byte* const pixel = pixels.data() +
                    (static_cast<std::size_t>(contract.bounds.y + row) * kUnifiedVisualProfile.canvasWidth +
                        contract.bounds.x + column) * 4;
                pixel[0] = std::byte{kUnifiedNeutralLuma};
                pixel[1] = std::byte{kUnifiedNeutralLuma};
                pixel[2] = std::byte{kUnifiedNeutralLuma};
            }
        }
    }
}

// Deterministic per-pixel luma jitter on data tiles only: every data pixel's
// three channels shift by the same bounded amount, leaving pilots pristine.
void JitterDataTileLuma(const std::span<std::byte> pixels, const std::uint32_t amplitude)
{
    for (std::uint32_t tileOrdinal = 0; tileOrdinal < kUnifiedVisualProfile.dataTileCount; tileOrdinal++)
    {
        const UnifiedDataTile tile = GetUnifiedDataTile(tileOrdinal);
        for (std::uint32_t row = 0; row < kUnifiedVisualProfile.tileHeight; row++)
        {
            for (std::uint32_t column = 0; column < kUnifiedVisualProfile.tileWidth; column++)
            {
                std::byte* const pixel = pixels.data() +
                    (static_cast<std::size_t>(tile.bounds.y + row) * kUnifiedVisualProfile.canvasWidth +
                        tile.bounds.x + column) * 4;
                const std::int32_t luma = std::to_integer<std::uint8_t>(pixel[1]);
                const std::int32_t offset = static_cast<std::int32_t>(
                    (tileOrdinal * 31 + row * 7 + column * 13) % (2 * amplitude + 1)) - static_cast<std::int32_t>(amplitude);
                const std::int32_t jittered = std::clamp(luma + offset, 0, 255);
                pixel[0] = static_cast<std::byte>(jittered);
                pixel[1] = static_cast<std::byte>(jittered);
                pixel[2] = static_cast<std::byte>(jittered);
            }
        }
    }
}

void RequireFullRecovery(UnifiedVisualCpuOracle& oracle, const UnifiedVisualObservation& observation,
    const GrayFixture& fixture, const std::uint32_t frameCount = 1)
{
    REQUIRE(observation.IsFrameAvailable());
    REQUIRE(observation.bootstrapRecord.visualProfileId == pbprotocol::kGrayStatesExperimentalProfile.visualProfileId);
    REQUIRE(observation.bootstrapRecord.visualLayoutVersion ==
        pbprotocol::kGrayStatesExperimentalProfile.visualLayoutVersion);
    REQUIRE(observation.baseLuma.IsAvailable());
    REQUIRE(observation.fineLuma.IsAvailable());
    REQUIRE(observation.chroma.IsAvailable());
    REQUIRE(observation.acceptedBlocks == frameCount * kUnifiedGrayFrameCodewordCount);
    REQUIRE(observation.acceptedControlRecords == frameCount);
    REQUIRE(observation.acceptedTransportBlocks == frameCount * (kUnifiedGrayFrameCodewordCount - 1));
    const std::span<const UnifiedAcceptedBlock> accepted = oracle.GetAcceptedBlocks();
    REQUIRE(accepted.size() == frameCount * kUnifiedGrayFrameCodewordCount);
    for (std::uint32_t slot = 0; slot < kUnifiedGrayFrameCodewordCount; slot++)
    {
        REQUIRE(observation.slots[slot].accepted);
        REQUIRE(accepted[frameCount - 1 + slot].size == fixture.expected[slot].size());
        REQUIRE(std::equal(fixture.expected[slot].begin(), fixture.expected[slot].end(),
            accepted[frameCount - 1 + slot].bytes.begin()));
    }
}

} // namespace

// Gray-fast experimental identity (PB-Experimental-GrayFast-1, layout 13):
// the layout-12 seven-plane raster with the DVB-S2 Short Fast inner FEC
// (37/45). Each codeword carries 1665 information bytes (1629-byte Transport
// payload); raster, mapping and slot contracts are identical to layout 12.
namespace
{

struct GrayFastFixture
{
    std::array<std::byte, kLocalDesktopBootstrapRecordBytes> bootstrap{};
    std::array<std::byte, kUnifiedGrayCodedFrameBytes> coded{};
    std::array<UnifiedSlotAssignment, kUnifiedGrayFrameCodewordCount> plan{};
    std::array<std::vector<std::byte>, kUnifiedGrayFrameCodewordCount> expected{};
};

std::vector<std::byte> MakeLargeTransport(const pbprotocol::SessionTag sessionTag, const std::uint32_t slot)
{
    // Near-ceiling payloads prove the 1629-byte Transport payload actually
    // fits and round-trips through the Fast information block.
    std::vector<std::byte> payload(kUnifiedGrayFastTransportPayloadBytes - 4 - slot % 7);
    for (std::size_t index = 0; index < payload.size(); index++)
    {
        payload[index] = static_cast<std::byte>((slot * 53 + index * 29 + 11) & 0xFFU);
    }
    const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData,
        pbprotocol::kTransportProtocolMinor, 0, sessionTag, 700 + slot, 800 + slot,
        static_cast<std::uint16_t>(payload.size())};
    std::vector<std::byte> block(pbprotocol::GetTransportSerializedSize(header));
    REQUIRE(pbprotocol::SerializeTransportBlock(header, payload, block));
    return block;
}

GrayFastFixture BuildGrayFastFixture(const std::uint64_t sequence)
{
    GrayFastFixture fixture;
    const pbprotocol::BootstrapRecord record{pbprotocol::kBootstrapVersion, pbprotocol::GetProtocolVersion(),
        pbprotocol::kGrayFastExperimentalProfile.visualLayoutVersion,
        pbprotocol::kGrayFastExperimentalProfile.visualProfileId,
        pbprotocol::SessionTag{0x8877665544332211ULL}, sequence, 0x31323334U, 0};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, fixture.bootstrap));
    const auto parsed = pbprotocol::ParseBootstrapRecord(fixture.bootstrap);
    REQUIRE(parsed);
    std::array<std::byte, kUnifiedGrayFastInformationBytes> information{};
    for (std::uint32_t slot = 0; slot < kUnifiedGrayFrameCodewordCount; slot++)
    {
        UnifiedSlotAssignment& assignment = fixture.plan[slot];
        assignment.codewordSlot = slot;
        if (slot == 0)
        {
            assignment.kind = UnifiedSlotKind::Control;
            assignment.controlPriority = UnifiedControlPriority::SessionDescriptor;
            fixture.expected[slot] = MakeControl(parsed.Value().sessionTag);
            std::copy(fixture.expected[slot].begin(), fixture.expected[slot].end(), information.begin());
        }
        else
        {
            assignment.kind = UnifiedSlotKind::Transport;
            assignment.controlPriority = UnifiedControlPriority::NotApplicable;
            fixture.expected[slot] = MakeLargeTransport(parsed.Value().sessionTag, slot);
            REQUIRE(pbprotocol::FrameTransportBlockIntoInfoBlock(fixture.expected[slot], information.size(), information));
        }
        REQUIRE(fixture.expected[slot].size() <= kUnifiedGrayFastInformationBytes);
        REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdFast, information,
            std::span<std::byte>(fixture.coded).subspan(static_cast<std::size_t>(slot) * kUnifiedCodewordBytes,
                kUnifiedCodewordBytes)));
    }
    REQUIRE(ValidateUnifiedGrayMixedSlotPlan(fixture.plan));
    return fixture;
}

void RequireGrayFastRecovery(UnifiedVisualCpuOracle& oracle, const UnifiedVisualObservation& observation,
    const GrayFastFixture& fixture)
{
    REQUIRE(observation.IsFrameAvailable());
    REQUIRE(observation.bootstrapRecord.visualProfileId == pbprotocol::kGrayFastExperimentalProfile.visualProfileId);
    REQUIRE(observation.bootstrapRecord.visualLayoutVersion ==
        pbprotocol::kGrayFastExperimentalProfile.visualLayoutVersion);
    REQUIRE(observation.acceptedBlocks == kUnifiedGrayFrameCodewordCount);
    REQUIRE(observation.acceptedControlRecords == 1);
    REQUIRE(observation.acceptedTransportBlocks == kUnifiedGrayFrameCodewordCount - 1);
    const std::span<const UnifiedAcceptedBlock> accepted = oracle.GetAcceptedBlocks();
    REQUIRE(accepted.size() == kUnifiedGrayFrameCodewordCount);
    for (std::uint32_t slot = 0; slot < kUnifiedGrayFrameCodewordCount; slot++)
    {
        REQUIRE(observation.slots[slot].accepted);
        REQUIRE(accepted[slot].size == fixture.expected[slot].size());
        REQUIRE(std::equal(fixture.expected[slot].begin(), fixture.expected[slot].end(),
            accepted[slot].bytes.begin()));
    }
}

} // namespace

TEST_CASE("Gray-fast frames recover 1629-byte payloads at 1:1 and through 4:2:0",
    "[unified][graystates][grayfast][channel]")
{
    const GrayFastFixture fixture = BuildGrayFastFixture(19);
    std::vector<std::byte> pixels(kUnifiedFrameBgraBytes);
    REQUIRE(static_cast<bool>(EncodeUnifiedVisualFrame(fixture.bootstrap, fixture.coded, pixels)));
    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedExpectedFrameIdentity fastIdentity{false, {}, false, {},
        pbprotocol::kGrayFastExperimentalProfile.visualProfileId,
        pbprotocol::kGrayFastExperimentalProfile.visualLayoutVersion};
    {
        const UnifiedVisualObservation observation = oracle.Decode(View(pixels), fixture.plan, fastIdentity);
        REQUIRE(observation.inputValid);
        RequireGrayFastRecovery(oracle, observation, fixture);
    }
    {
        const std::vector<std::byte> subsampled = SubsampleChroma420(pixels);
        const UnifiedVisualObservation observation = oracle.Decode(View(subsampled), fixture.plan, fastIdentity);
        REQUIRE(observation.inputValid);
        RequireGrayFastRecovery(oracle, observation, fixture);
    }
}

TEST_CASE("Gray-fast identity stays distinct from layout 12 and the product pair",
    "[unified][graystates][grayfast][identity]")
{
    REQUIRE(IsUnifiedGrayCarrierPair(pbprotocol::kGrayFastExperimentalProfile.visualProfileId,
        pbprotocol::kGrayFastExperimentalProfile.visualLayoutVersion));
    REQUIRE_FALSE(IsUnifiedGrayStatesProfilePair(pbprotocol::kGrayFastExperimentalProfile.visualProfileId,
        pbprotocol::kGrayFastExperimentalProfile.visualLayoutVersion));
    // A layout-12 expected identity must not accept a layout-13 frame: the
    // codeword information differs even though the raster shape matches.
    const GrayFastFixture fixture = BuildGrayFastFixture(23);
    std::vector<std::byte> pixels(kUnifiedFrameBgraBytes);
    REQUIRE(static_cast<bool>(EncodeUnifiedVisualFrame(fixture.bootstrap, fixture.coded, pixels)));
    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedVisualObservation observation = oracle.Decode(View(pixels), fixture.plan, GrayIdentity());
    // The oracle is deliberately identity-transparent at the carrier level:
    // the observed Bootstrap record selects the FEC dims (Fast for layout 13)
    // regardless of the caller's default expectation, so the frame fully
    // recovers. Cross-identity rejection is enforced by the owning profile
    // binding in the application runtime, not by the oracle.
    REQUIRE(observation.bootstrap.IsAccepted());
    REQUIRE(observation.bootstrapRecord.visualLayoutVersion ==
        pbprotocol::kGrayFastExperimentalProfile.visualLayoutVersion);
    RequireGrayFastRecovery(oracle, observation, fixture);
}

TEST_CASE("Gray-state frames fully recover at 1:1 with gray foreground pixels", "[unified][graystates]")
{
    const GrayFixture fixture = BuildGrayFixture(7);
    const std::vector<std::byte> pixels = RenderGray(fixture);
    // The raster itself must carry the gray identity: a foreground data-tile
    // chip is one of the four gray levels, never a saturated color state.
    // The v2 carrier renders mask symbols at fixed high/low luma; verify a
    // data tile carries both a foreground chip at the calibrated high level
    // and a background chip at the low level.
    bool sawForeground = false;
    bool sawBackground = false;
    {
        const UnifiedDataTile tile = GetUnifiedDataTile(0);
        for (std::uint32_t row = 0; row < 5 && (!sawForeground || !sawBackground); row++)
        {
            for (std::uint32_t column = 0; column < 5 && (!sawForeground || !sawBackground); column++)
            {
                const std::byte* const pixel = pixels.data() +
                    (static_cast<std::size_t>(tile.bounds.y + row) * kUnifiedVisualProfile.canvasWidth +
                        tile.bounds.x + column) * 4;
                const std::uint8_t lumaValue = std::to_integer<std::uint8_t>(pixel[1]);
                sawForeground = sawForeground || lumaValue == kUnifiedGrayLowForegroundLuma ||
                    lumaValue == kUnifiedGrayHighForegroundLuma;
                sawBackground = sawBackground || lumaValue == kUnifiedDataLowLuma;
            }
        }
    }
    REQUIRE(sawForeground);
    REQUIRE(sawBackground);

    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedVisualObservation observation = oracle.Decode(View(pixels), fixture.plan, GrayIdentity());
    REQUIRE(observation.inputValid);
    RequireFullRecovery(oracle, observation, fixture);
}

TEST_CASE("Gray-state second carrier survives 4:2:0 chroma subsampling", "[unified][graystates][channel]")
{
    const GrayFixture fixture = BuildGrayFixture(11);
    const std::vector<std::byte> clean = RenderGray(fixture);
    const std::vector<std::byte> subsampled = SubsampleChroma420(clean);
    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedVisualObservation observation = oracle.Decode(View(subsampled), fixture.plan, GrayIdentity());
    // The whole point of the identity: the transform that erases the SC6-V3
    // Chroma lane leaves every slot, including the five state codewords, live.
    REQUIRE(observation.inputValid);
    RequireFullRecovery(oracle, observation, fixture);
}

TEST_CASE("Gray-state frames are immune to chroma neutralization", "[unified][graystates][channel]")
{
    const GrayFixture fixture = BuildGrayFixture(13);
    const std::vector<std::byte> clean = RenderGray(fixture);
    std::vector<std::byte> neutralized = SubsampleChroma420(clean);
    // A second pass cannot make it "more neutral": gray frames carry no
    // chroma information to lose, so the decode stays complete.
    neutralized = SubsampleChroma420(neutralized);
    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedVisualObservation observation = oracle.Decode(View(neutralized), fixture.plan, GrayIdentity());
    REQUIRE(observation.inputValid);
    RequireFullRecovery(oracle, observation, fixture);
}

// Field-measured degradation model (2026-09-12 ToDesk link): flat calibration
// stripes pass nearly unchanged while tiny glyph foreground chips above the
// provider knee compress toward the tile DC - the top level lost ~34 luma and
// the two top populations became inseparable under the original 88/144/200/248
// set. This case replays that monotone concave remap on the data tiles only
// (pilots stay pristine) against the retuned 56/112/168/248 levels and the
// data-driven mode classifier.
TEST_CASE("Gray-state second carrier survives provider level compression", "[unified][graystates][channel][compression]")
{
    const GrayFixture fixture = BuildGrayFixture(29);
    const std::vector<std::byte> clean = RenderGray(fixture);
    std::vector<std::byte> compressed = clean;
    for (std::uint32_t tileOrdinal = 0; tileOrdinal < kUnifiedVisualProfile.dataTileCount; tileOrdinal++)
    {
        const UnifiedDataTile tile = GetUnifiedDataTile(tileOrdinal);
        for (std::uint32_t row = 0; row < kUnifiedVisualProfile.tileHeight; row++)
        {
            for (std::uint32_t column = 0; column < kUnifiedVisualProfile.tileWidth; column++)
            {
                std::byte* const pixel = compressed.data() +
                    (static_cast<std::size_t>(tile.bounds.y + row) * kUnifiedVisualProfile.canvasWidth +
                        tile.bounds.x + column) * 4;
                const std::int32_t luma = std::to_integer<std::uint8_t>(pixel[1]);
                std::int32_t remapped = luma;
                if (luma > 176)
                {
                    remapped = 176 + static_cast<std::int32_t>((luma - 176) * 0.45);
                }
                const std::int32_t offset = static_cast<std::int32_t>(
                    (tileOrdinal * 17 + row * 3 + column * 7) % 13) - 6;
                const std::int32_t jittered = std::clamp(remapped + offset, 0, 255);
                pixel[0] = static_cast<std::byte>(jittered);
                pixel[1] = static_cast<std::byte>(jittered);
                pixel[2] = static_cast<std::byte>(jittered);
            }
        }
    }
    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedVisualObservation observation = oracle.Decode(View(compressed), fixture.plan, GrayIdentity());
    REQUIRE(observation.inputValid);
    RequireFullRecovery(oracle, observation, fixture);
}

TEST_CASE("Gray-state frames tolerate bounded luma jitter on data tiles", "[unified][graystates][channel]")
{
    const GrayFixture fixture = BuildGrayFixture(17);
    const std::vector<std::byte> clean = RenderGray(fixture);
    std::vector<std::byte> jittered = clean;
    JitterDataTileLuma(jittered, 6);
    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedVisualObservation observation = oracle.Decode(View(jittered), fixture.plan, GrayIdentity());
    REQUIRE(observation.inputValid);
    RequireFullRecovery(oracle, observation, fixture);
}

TEST_CASE("Collapsed gray state stripes fail the state pilot without harming data slots",
    "[unified][graystates][erasure]")
{
    const GrayFixture fixture = BuildGrayFixture(19);
    const std::vector<std::byte> clean = RenderGray(fixture);
    std::vector<std::byte> collapsed = clean;
    CollapseStateStripes(collapsed);
    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedVisualObservation observation = oracle.Decode(View(collapsed), fixture.plan, GrayIdentity());
    REQUIRE(observation.IsFrameAvailable());
    REQUIRE(observation.baseLuma.IsAvailable());
    REQUIRE(observation.fineLuma.IsAvailable());
    REQUIRE_FALSE(observation.chroma.IsAvailable());
    REQUIRE(observation.chroma.erasureReason == UnifiedErasureReason::ChromaPilotFailure);
    // Carrier v3: every slot rides the Base Luma observation and the level
    // bit calibrates from the ladder stripes, so the collapsed state stripes
    // cost the Chroma pilot observation only - no data slot is erased.
    REQUIRE(observation.acceptedBlocks == kUnifiedGrayFrameCodewordCount);
    REQUIRE(observation.acceptedControlRecords == 1);
    REQUIRE(observation.acceptedTransportBlocks == kUnifiedGrayFrameCodewordCount - 1);
}


#ifndef PB_UNIFIED_GRAY_GOLDEN_DIR
#error PB_UNIFIED_GRAY_GOLDEN_DIR must name the independent gray-state CPU oracle Golden directory
#endif

std::vector<std::byte> ReadGrayGolden(const char* const name)
{
    const std::filesystem::path path = std::filesystem::path(PB_UNIFIED_GRAY_GOLDEN_DIR) / name;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    REQUIRE(input);
    const std::streampos end = input.tellg();
    REQUIRE(end >= 0);
    const auto size = static_cast<std::uint64_t>(end);
    REQUIRE(size <= static_cast<std::size_t>(-1));
    std::vector<std::byte> content(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!content.empty())
    {
        input.read(reinterpret_cast<char*>(content.data()), static_cast<std::streamsize>(content.size()));
        REQUIRE(input);
    }
    return content;
}

void CopyPixels(const std::vector<std::byte>& source, std::vector<std::byte>& destination,
    const UnifiedPixelRegion region)
{
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        std::memcpy(destination.data() + (static_cast<std::size_t>(region.y + row) * kUnifiedVisualProfile.canvasWidth +
            region.x) * 4,
            source.data() + (static_cast<std::size_t>(region.y + row) * kUnifiedVisualProfile.canvasWidth +
                region.x) * 4,
            static_cast<std::size_t>(region.width) * 4);
    }
}

void FillGrayRegion(std::vector<std::byte>& pixels, const UnifiedPixelRegion region, const std::uint8_t level)
{
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        for (std::uint32_t column = 0; column < region.width; column++)
        {
            std::byte* const pixel = pixels.data() +
                (static_cast<std::size_t>(region.y + row) * kUnifiedVisualProfile.canvasWidth + region.x + column) * 4;
            pixel[0] = static_cast<std::byte>(level);
            pixel[1] = static_cast<std::byte>(level);
            pixel[2] = static_cast<std::byte>(level);
            pixel[3] = std::byte{255};
        }
    }
}

void CopyFreshnessRegionData(const std::vector<std::byte>& source, std::vector<std::byte>& destination,
    const std::uint8_t freshnessRegion)
{
    for (std::uint32_t tileOrdinal = 0; tileOrdinal < kUnifiedVisualProfile.dataTileCount; tileOrdinal++)
    {
        const UnifiedDataTile tile = GetUnifiedDataTile(tileOrdinal);
        if (!tile.valid || tile.freshnessRegion != freshnessRegion)
        {
            continue;
        }
        CopyPixels(source, destination, tile.bounds);
    }
}

void RequireGrayRasterDigest(const std::span<const std::byte> digests, const std::size_t variant,
    const std::span<const std::byte> pixels)
{
    CAPTURE(variant);
    REQUIRE(digests.size() == 6 * pbprotocol::kDigestBytes);
    const std::array<std::byte, pbprotocol::kDigestBytes> actual = pbprotocol::ComputeBlake3Digest(pixels);
    REQUIRE(std::equal(actual.begin(), actual.end(), digests.begin() +
        static_cast<std::ptrdiff_t>(variant * actual.size())));
}

TEST_CASE("Gray-state CPU oracle matches the independently regenerated Golden",
    "[unified][graystates][golden]")
{
    const std::vector<std::byte> previousBootstrap = ReadGrayGolden("bootstrap-sequence40.bin");
    const std::vector<std::byte> currentBootstrap = ReadGrayGolden("bootstrap-sequence41.bin");
    const std::vector<std::byte> codewords = ReadGrayGolden("mixed-codewords.bin");
    const std::vector<std::byte> expectedAccepted = ReadGrayGolden("accepted-stream.bin");
    const std::vector<std::byte> digests = ReadGrayGolden("raster-digests.bin");
    REQUIRE(previousBootstrap.size() == kLocalDesktopBootstrapRecordBytes);
    REQUIRE(currentBootstrap.size() == kLocalDesktopBootstrapRecordBytes);
    REQUIRE(codewords.size() == kUnifiedGrayCodedFrameBytes);

    std::vector<std::byte> previous(kUnifiedFrameBgraBytes);
    std::vector<std::byte> current(kUnifiedFrameBgraBytes);
    REQUIRE(static_cast<bool>(EncodeUnifiedVisualFrame(previousBootstrap, codewords, previous)));
    REQUIRE(static_cast<bool>(EncodeUnifiedVisualFrame(currentBootstrap, codewords, current)));
    RequireGrayRasterDigest(digests, 0, current);
    {
        std::vector<std::byte> variant = current;
        CollapseStateStripes(variant);
        RequireGrayRasterDigest(digests, 1, variant);
    }
    {
        std::vector<std::byte> variant = current;
        FillGrayRegion(variant, kUnifiedVisualProfile.regions[19].bounds, kUnifiedNeutralLuma);
        RequireGrayRasterDigest(digests, 2, variant);
    }
    {
        std::vector<std::byte> variant = current;
        FillGrayRegion(variant, kUnifiedVisualProfile.regions[20].bounds, kUnifiedNeutralLuma);
        RequireGrayRasterDigest(digests, 3, variant);
    }
    {
        std::vector<std::byte> variant = current;
        CopyPixels(previous, variant, kUnifiedVisualProfile.regions[10].bounds);
        CopyFreshnessRegionData(previous, variant, 4);
        RequireGrayRasterDigest(digests, 4, variant);
    }
    {
        std::vector<std::byte> variant = current;
        for (const UnifiedRegionContract& contract : kUnifiedVisualProfile.regions)
        {
            if (contract.kind == UnifiedRegionKind::Data)
            {
                CopyPixels(previous, variant, contract.bounds);
            }
        }
        RequireGrayRasterDigest(digests, 5, variant);
    }

    std::array<UnifiedSlotAssignment, kUnifiedGrayFrameCodewordCount> plan{};
    for (std::uint32_t slot = 0; slot < kUnifiedGrayFrameCodewordCount; slot++)
    {
        plan[slot] = slot == 0 ?
            UnifiedSlotAssignment{slot, UnifiedSlotKind::Control, UnifiedControlPriority::SessionDescriptor} :
            UnifiedSlotAssignment{slot, UnifiedSlotKind::Transport, UnifiedControlPriority::NotApplicable};
    }
    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedVisualObservation observation = oracle.Decode(View(current), plan, GrayIdentity());
    REQUIRE(observation.acceptedBlocks == kUnifiedGrayFrameCodewordCount);
    const std::span<const UnifiedAcceptedBlock> accepted = oracle.GetAcceptedBlocks();
    REQUIRE(accepted.size() == kUnifiedGrayFrameCodewordCount);
    std::size_t offset = 0;
    for (std::size_t slot = 0; slot < accepted.size(); slot++)
    {
        REQUIRE(offset <= expectedAccepted.size());
        REQUIRE(expectedAccepted.size() - offset >= 4);
        REQUIRE(std::to_integer<std::uint8_t>(expectedAccepted[offset]) == slot);
        const bool transport = std::to_integer<std::uint8_t>(expectedAccepted[offset + 1]) != 0;
        REQUIRE(accepted[slot].kind == (transport ? UnifiedSlotKind::Transport : UnifiedSlotKind::Control));
        const std::uint16_t size = static_cast<std::uint16_t>(
            std::to_integer<std::uint16_t>(expectedAccepted[offset + 2]) |
            (std::to_integer<std::uint16_t>(expectedAccepted[offset + 3]) << 8U));
        offset += 4;
        REQUIRE(offset <= expectedAccepted.size());
        REQUIRE(expectedAccepted.size() - offset >= size);
        REQUIRE(accepted[slot].size == size);
        REQUIRE(std::equal(expectedAccepted.begin() + static_cast<std::ptrdiff_t>(offset),
            expectedAccepted.begin() + static_cast<std::ptrdiff_t>(offset + size), accepted[slot].bytes.begin()));
        offset += size;
    }
    REQUIRE(offset == expectedAccepted.size());
}

#ifndef PB_UNIFIED_GRAYFAST_GOLDEN_DIR
#error PB_UNIFIED_GRAYFAST_GOLDEN_DIR must name the independent gray-fast CPU oracle Golden directory
#endif

TEST_CASE("Gray-fast CPU oracle matches the independently regenerated Golden",
    "[unified][graystates][grayfast][golden]")
{
    const std::filesystem::path root = PB_UNIFIED_GRAYFAST_GOLDEN_DIR;
    const auto readGolden = [&root](const char* const name)
    {
        std::ifstream input(root / name, std::ios::binary | std::ios::ate);
        REQUIRE(input);
        const auto size = static_cast<std::size_t>(input.tellg());
        std::vector<std::byte> content(size);
        input.seekg(0);
        if (!content.empty())
        {
            input.read(reinterpret_cast<char*>(content.data()),
                static_cast<std::streamsize>(content.size()));
            REQUIRE(input);
        }
        return content;
    };
    const std::vector<std::byte> previousBootstrap = readGolden("bootstrap-sequence40.bin");
    const std::vector<std::byte> currentBootstrap = readGolden("bootstrap-sequence41.bin");
    const std::vector<std::byte> codewords = readGolden("mixed-codewords.bin");
    const std::vector<std::byte> expectedAccepted = readGolden("accepted-stream.bin");
    const std::vector<std::byte> digests = readGolden("raster-digests.bin");
    REQUIRE(previousBootstrap.size() == kLocalDesktopBootstrapRecordBytes);
    REQUIRE(currentBootstrap.size() == kLocalDesktopBootstrapRecordBytes);
    REQUIRE(codewords.size() == kUnifiedGrayCodedFrameBytes);

    // The raster is the layout-12 seven-plane carrier; only the codeword
    // bits differ (Fast parity), so the digest variants reuse the same
    // transforms and the C++ raster must reproduce them byte-for-byte.
    std::vector<std::byte> previous(kUnifiedFrameBgraBytes);
    std::vector<std::byte> current(kUnifiedFrameBgraBytes);
    REQUIRE(static_cast<bool>(EncodeUnifiedVisualFrame(previousBootstrap, codewords, previous)));
    REQUIRE(static_cast<bool>(EncodeUnifiedVisualFrame(currentBootstrap, codewords, current)));
    RequireGrayRasterDigest(digests, 0, current);
    {
        std::vector<std::byte> variant = current;
        CollapseStateStripes(variant);
        RequireGrayRasterDigest(digests, 1, variant);
    }
    {
        std::vector<std::byte> variant = current;
        FillGrayRegion(variant, kUnifiedVisualProfile.regions[19].bounds, kUnifiedNeutralLuma);
        RequireGrayRasterDigest(digests, 2, variant);
    }
    {
        std::vector<std::byte> variant = current;
        FillGrayRegion(variant, kUnifiedVisualProfile.regions[20].bounds, kUnifiedNeutralLuma);
        RequireGrayRasterDigest(digests, 3, variant);
    }
    {
        std::vector<std::byte> variant = current;
        CopyPixels(previous, variant, kUnifiedVisualProfile.regions[10].bounds);
        CopyFreshnessRegionData(previous, variant, 4);
        RequireGrayRasterDigest(digests, 4, variant);
    }
    {
        std::vector<std::byte> variant = current;
        for (const UnifiedRegionContract& contract : kUnifiedVisualProfile.regions)
        {
            if (contract.kind == UnifiedRegionKind::Data)
            {
                CopyPixels(previous, variant, contract.bounds);
            }
        }
        RequireGrayRasterDigest(digests, 5, variant);
    }

    // Decode the golden raster under the gray-fast identity: Fast FEC must
    // recover all eighteen near-ceiling payloads exactly.
    std::array<UnifiedSlotAssignment, kUnifiedGrayFrameCodewordCount> plan{};
    for (std::uint32_t slot = 0; slot < kUnifiedGrayFrameCodewordCount; slot++)
    {
        plan[slot] = slot == 0 ?
            UnifiedSlotAssignment{slot, UnifiedSlotKind::Control, UnifiedControlPriority::SessionDescriptor} :
            UnifiedSlotAssignment{slot, UnifiedSlotKind::Transport, UnifiedControlPriority::NotApplicable};
    }
    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedExpectedFrameIdentity fastIdentity{false, {}, false, {},
        pbprotocol::kGrayFastExperimentalProfile.visualProfileId,
        pbprotocol::kGrayFastExperimentalProfile.visualLayoutVersion};
    const UnifiedVisualObservation observation = oracle.Decode(View(current), plan, fastIdentity);
    REQUIRE(observation.acceptedBlocks == kUnifiedGrayFrameCodewordCount);
    const std::span<const UnifiedAcceptedBlock> accepted = oracle.GetAcceptedBlocks();
    REQUIRE(accepted.size() == kUnifiedGrayFrameCodewordCount);
    std::size_t offset = 0;
    for (std::size_t slot = 0; slot < accepted.size(); slot++)
    {
        REQUIRE(offset <= expectedAccepted.size());
        REQUIRE(expectedAccepted.size() - offset >= 4);
        REQUIRE(std::to_integer<std::uint8_t>(expectedAccepted[offset]) == slot);
        const bool transport = std::to_integer<std::uint8_t>(expectedAccepted[offset + 1]) != 0;
        REQUIRE(accepted[slot].kind == (transport ? UnifiedSlotKind::Transport : UnifiedSlotKind::Control));
        const std::uint16_t size = static_cast<std::uint16_t>(
            std::to_integer<std::uint16_t>(expectedAccepted[offset + 2]) |
            (std::to_integer<std::uint16_t>(expectedAccepted[offset + 3]) << 8U));
        offset += 4;
        REQUIRE(offset <= expectedAccepted.size());
        REQUIRE(expectedAccepted.size() - offset >= size);
        // Near-ceiling Transport payloads: every transport slot exercises
        // the 1629-byte payload contract.
        if (transport)
        {
            REQUIRE(size > 1600);
        }
        REQUIRE(accepted[slot].size == size);
        REQUIRE(std::equal(expectedAccepted.begin() + static_cast<std::ptrdiff_t>(offset),
            expectedAccepted.begin() + static_cast<std::ptrdiff_t>(offset + size), accepted[slot].bytes.begin()));
        offset += size;
    }
    REQUIRE(offset == expectedAccepted.size());
}

TEST_CASE("Gray-state identity rejects the product color pair and vice versa", "[unified][graystates][identity]")
{
    const GrayFixture fixture = BuildGrayFixture(23);
    const std::vector<std::byte> pixels = RenderGray(fixture);
    REQUIRE(pbprotocol::IsProductSessionVisualProfileId(
        pbprotocol::kGrayStatesExperimentalProfile.visualProfileId) == false);
    REQUIRE(IsUnifiedGrayStatesProfilePair(pbprotocol::kGrayStatesExperimentalProfile.visualProfileId,
        pbprotocol::kGrayStatesExperimentalProfile.visualLayoutVersion));
    REQUIRE_FALSE(IsUnifiedGrayStatesProfilePair(pbprotocol::kUnifiedVisualProfileId, 10));
    REQUIRE_FALSE(IsUnifiedGrayStatesProfilePair(pbprotocol::kGrayStatesExperimentalProfile.visualProfileId, 10));
    REQUIRE_FALSE(IsUnifiedGrayStatesProfilePair(pbprotocol::kGrayStatesExperimentalProfile.visualProfileId, 11));
    // Encoding under the product scaffold contract (explicit product binding)
    // still accepts the gray bootstrap pair because the pair gate admits the
    // family; the rendered pixels themselves decide which carrier is drawn.
    std::vector<std::byte> other(kUnifiedFrameBgraBytes);
    REQUIRE(static_cast<bool>(EncodeUnifiedVisualFrame(fixture.bootstrap, fixture.coded, other)));
    REQUIRE(other == pixels);
}
