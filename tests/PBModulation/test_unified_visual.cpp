#include "pbmodulation/unified_visual.h"

#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/protocol_version.h"
#include "pbprotocol/transport_block_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

namespace
{

using namespace pbmodulation;

#ifndef PB_UNIFIED_CPU_GOLDEN_DIR
#error PB_UNIFIED_CPU_GOLDEN_DIR must name the independent Unified CPU oracle Golden directory
#endif

struct Fixture
{
    std::array<std::byte, kLocalDesktopBootstrapRecordBytes> bootstrap{};
    std::array<std::byte, kUnifiedCodedFrameBytes> coded{};
    std::array<UnifiedSlotAssignment, kUnifiedCodewordCount> plan{};
    std::array<std::vector<std::byte>, kUnifiedCodewordCount> expected{};
};

std::array<std::byte, kLocalDesktopBootstrapRecordBytes> MakeBootstrap(const std::uint64_t sequence)
{
    std::array<std::byte, kLocalDesktopBootstrapRecordBytes> bytes{};
    const pbprotocol::BootstrapRecord record{pbprotocol::kBootstrapVersion, pbprotocol::GetProtocolVersion(),
        kUnifiedVisualProfile.productProfile.visualLayoutVersion,
        kUnifiedVisualProfile.productProfile.visualProfileId, pbprotocol::SessionTag{0x1122334455667788ULL},
        sequence, 0x21222324U, 0};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
    return bytes;
}

void EncodeInformation(const std::span<const std::byte> information, const std::uint32_t slot,
    const std::span<std::byte> coded)
{
    REQUIRE(information.size() == kUnifiedInformationBytes);
    REQUIRE(slot < kUnifiedCodewordCount);
    REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information,
        coded.subspan(static_cast<std::size_t>(slot) * kUnifiedCodewordBytes, kUnifiedCodewordBytes)));
}

std::vector<std::byte> MakeTransport(const pbprotocol::SessionTag sessionTag, const std::uint32_t slot)
{
    std::vector<std::byte> payload(48 + slot % 13);
    for (std::size_t index = 0; index < payload.size(); index++)
    {
        payload[index] = static_cast<std::byte>((slot * 37 + index * 11 + 5) & 0xFFU);
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
    const std::array<std::byte, 9> payload{std::byte{1}, std::byte{3}, std::byte{5}, std::byte{7},
        std::byte{9}, std::byte{11}, std::byte{13}, std::byte{15}, std::byte{17}};
    const pbprotocol::ControlRecordView record{pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor, 77, sessionTag, payload};
    const auto size = pbprotocol::GetSerializedSize(record);
    REQUIRE(size);
    std::vector<std::byte> bytes(size.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    return bytes;
}

Fixture BuildFixture(const std::uint64_t sequence)
{
    Fixture fixture;
    fixture.bootstrap = MakeBootstrap(sequence);
    const auto parsed = pbprotocol::ParseBootstrapRecord(fixture.bootstrap);
    REQUIRE(parsed);
    for (std::uint32_t slot = 0; slot < kUnifiedCodewordCount; slot++)
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
        EncodeInformation(information, slot, fixture.coded);
    }
    REQUIRE(ValidateUnifiedMixedSlotPlan(fixture.plan));
    return fixture;
}

const Fixture& GetFixture40()
{
    static const Fixture fixture = BuildFixture(40);
    return fixture;
}

const Fixture& GetFixture41()
{
    static const Fixture fixture = BuildFixture(41);
    return fixture;
}

std::vector<std::byte> Render(const Fixture& fixture)
{
    std::vector<std::byte> pixels(kUnifiedFrameBgraBytes);
    REQUIRE(EncodeUnifiedVisualFrame(fixture.bootstrap, fixture.coded, pixels));
    return pixels;
}

LumaView View(const std::vector<std::byte>& pixels)
{
    return LumaView{pixels, kUnifiedVisualProfile.canvasWidth, kUnifiedVisualProfile.canvasHeight,
        static_cast<std::size_t>(kUnifiedVisualProfile.canvasWidth) * 4, LumaPixelFormat::Bgra8};
}

UnifiedVisualCpuOracle MakeOracle()
{
    auto created = UnifiedVisualCpuOracle::Create(UnifiedVisualCpuOracle::RequiredBytes());
    REQUIRE(created);
    return std::move(created).Value();
}

void FillGray(std::vector<std::byte>& pixels, const UnifiedPixelRegion region, const std::uint8_t level)
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

void NeutralizeChroma(std::vector<std::byte>& pixels)
{
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4)
    {
        const double blue = std::to_integer<std::uint8_t>(pixels[offset]);
        const double green = std::to_integer<std::uint8_t>(pixels[offset + 1]);
        const double red = std::to_integer<std::uint8_t>(pixels[offset + 2]);
        const std::uint8_t luma = static_cast<std::uint8_t>(std::lround(0.0722 * blue + 0.7152 * green + 0.2126 * red));
        pixels[offset] = static_cast<std::byte>(luma);
        pixels[offset + 1] = static_cast<std::byte>(luma);
        pixels[offset + 2] = static_cast<std::byte>(luma);
    }
}

bool GetFixtureBit(const Fixture& fixture, const UnifiedLogicalCarrierBit logical)
{
    REQUIRE(logical.valid);
    const UnifiedLaneCapacity capacity = GetUnifiedLaneCapacity(logical.lane);
    const std::size_t globalBit = static_cast<std::size_t>(capacity.firstCodewordSlot) *
        kUnifiedVisualProfile.innerCodewordBits + logical.logicalBit;
    return ((std::to_integer<std::uint8_t>(fixture.coded[globalBit / 8]) >> (globalBit % 8)) & 1U) != 0;
}

void FlipLumaCarrierBit(const Fixture& fixture, std::vector<std::byte>& pixels,
    const UnifiedPhysicalCarrierSite target, const std::uint64_t frameSequence)
{
    REQUIRE(target.valid);
    REQUIRE(target.carrier == UnifiedCarrier::Luma);
    std::uint8_t lumaLabel = 0;
    for (std::uint8_t bitPlane = 0; bitPlane < 4; bitPlane++)
    {
        const UnifiedLogicalCarrierBit logical = GetUnifiedLogicalCarrierBit(
            UnifiedPhysicalCarrierSite{true, UnifiedCarrier::Luma, target.tileOrdinal, bitPlane}, frameSequence);
        if (logical.valid && GetFixtureBit(fixture, logical))
        {
            lumaLabel = static_cast<std::uint8_t>(lumaLabel | (1U << bitPlane));
        }
    }
    std::uint8_t chromaLabel = 0;
    for (std::uint8_t bitPlane = 0; bitPlane < 2; bitPlane++)
    {
        const UnifiedLogicalCarrierBit logical = GetUnifiedLogicalCarrierBit(
            UnifiedPhysicalCarrierSite{true, UnifiedCarrier::Chroma, target.tileOrdinal, bitPlane}, frameSequence);
        if (logical.valid && GetFixtureBit(fixture, logical))
        {
            chromaLabel = static_cast<std::uint8_t>(chromaLabel | (1U << bitPlane));
        }
    }
    lumaLabel = static_cast<std::uint8_t>(lumaLabel ^ (1U << target.bitPlane));
    const UnifiedDataTile tile = GetUnifiedDataTile(target.tileOrdinal);
    const std::uint16_t mask = kUnifiedSymbolMasksByLabel[lumaLabel];
    const UnifiedChromaState state = kUnifiedChromaStatesByLabel[chromaLabel];
    for (std::uint32_t row = 0; row < 4; row++)
    {
        for (std::uint32_t column = 0; column < 4; column++)
        {
            const std::uint32_t chip = row * 4 + column;
            const std::int32_t base = ((mask >> chip) & 1U) != 0 ? kUnifiedDataHighLuma : kUnifiedDataLowLuma;
            std::byte* const pixel = pixels.data() +
                (static_cast<std::size_t>(tile.bounds.y + row) * kUnifiedVisualProfile.canvasWidth +
                    tile.bounds.x + column) * 4;
            pixel[0] = static_cast<std::byte>(base + state.blueOffset);
            pixel[1] = static_cast<std::byte>(base + state.greenOffset);
            pixel[2] = static_cast<std::byte>(base + state.redOffset);
            pixel[3] = std::byte{255};
        }
    }
}

void CopyRegion(const std::vector<std::byte>& source, std::vector<std::byte>& destination,
    const UnifiedPixelRegion region)
{
    const std::size_t rowBytes = static_cast<std::size_t>(region.width) * 4;
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        const std::size_t offset =
            (static_cast<std::size_t>(region.y + row) * kUnifiedVisualProfile.canvasWidth + region.x) * 4;
        std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(offset), rowBytes,
            destination.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

void CopyFreshnessData(const std::vector<std::byte>& source, std::vector<std::byte>& destination,
    const std::uint8_t freshnessRegion)
{
    for (std::uint32_t tileOrdinal = 0; tileOrdinal < kUnifiedVisualProfile.dataTileCount; tileOrdinal++)
    {
        const UnifiedDataTile tile = GetUnifiedDataTile(tileOrdinal);
        if (tile.freshnessRegion == freshnessRegion)
        {
            CopyRegion(source, destination, tile.bounds);
        }
    }
}

void ReencodeSlot(std::array<std::byte, kUnifiedCodedFrameBytes>& coded, const std::uint32_t slot)
{
    std::array<std::byte, kUnifiedInformationBytes> information{};
    const std::size_t offset = static_cast<std::size_t>(slot) * kUnifiedCodewordBytes;
    std::copy_n(coded.begin() + static_cast<std::ptrdiff_t>(offset), information.size(), information.begin());
    EncodeInformation(information, slot, coded);
}

std::vector<std::byte> ReadGolden(const char* const name)
{
    const std::filesystem::path path = std::filesystem::path(PB_UNIFIED_CPU_GOLDEN_DIR) / name;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    REQUIRE(input);
    const std::streampos end = input.tellg();
    REQUIRE(end >= 0);
    const auto size = static_cast<std::uint64_t>(end);
    REQUIRE(size <= std::numeric_limits<std::size_t>::max());
    std::vector<std::byte> content(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!content.empty())
    {
        input.read(reinterpret_cast<char*>(content.data()), static_cast<std::streamsize>(content.size()));
        REQUIRE(input);
    }
    return content;
}

std::uint16_t ReadLe16(const std::span<const std::byte> bytes, const std::size_t offset)
{
    REQUIRE(offset <= bytes.size());
    REQUIRE(bytes.size() - offset >= 2);
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U));
}

void RequireRasterDigest(const std::span<const std::byte> digests, const std::size_t variant,
    const std::span<const std::byte> pixels)
{
    CAPTURE(variant);
    REQUIRE(digests.size() == 6 * pbprotocol::kDigestBytes);
    const std::array<std::byte, pbprotocol::kDigestBytes> actual = pbprotocol::ComputeBlake3Digest(pixels);
    REQUIRE(std::equal(actual.begin(), actual.end(), digests.begin() +
        static_cast<std::ptrdiff_t>(variant * actual.size())));
}

} // namespace

TEST_CASE("Unified CPU raster freezes data geometry and complete tile ownership", "[unified][cpu][modulation]")
{
    std::array<std::uint32_t, kUnifiedDataRegionCount> regionCounts{};
    std::array<std::uint32_t, kUnifiedFreshnessRegionCount> freshnessCounts{};
    std::vector<std::uint32_t> tileOrigins;
    tileOrigins.reserve(kUnifiedVisualProfile.dataTileCount);
    for (std::uint32_t tileOrdinal = 0; tileOrdinal < kUnifiedVisualProfile.dataTileCount; tileOrdinal++)
    {
        const UnifiedDataTile tile = GetUnifiedDataTile(tileOrdinal);
        REQUIRE(tile.valid);
        REQUIRE(tile.dataRegion < regionCounts.size());
        REQUIRE(tile.freshnessRegion < freshnessCounts.size());
        REQUIRE(tile.bounds.width == 4);
        REQUIRE(tile.bounds.height == 4);
        regionCounts[tile.dataRegion]++;
        freshnessCounts[tile.freshnessRegion]++;
        tileOrigins.push_back(tile.bounds.y * kUnifiedVisualProfile.canvasWidth + tile.bounds.x);
    }
    REQUIRE_FALSE(GetUnifiedDataTile(kUnifiedVisualProfile.dataTileCount).valid);
    std::ranges::sort(tileOrigins);
    REQUIRE(std::ranges::adjacent_find(tileOrigins) == tileOrigins.end());
    REQUIRE(std::accumulate(regionCounts.begin(), regionCounts.end(), 0U) == kUnifiedVisualProfile.dataTileCount);
    REQUIRE(std::ranges::all_of(regionCounts, [](const std::uint32_t count) { return count > 0; }));
    REQUIRE(std::ranges::all_of(freshnessCounts, [](const std::uint32_t count) { return count > 0; }));
    REQUIRE(GetUnifiedPhasePilotLabel(false, 0, 0) == 0);
    REQUIRE(GetUnifiedPhasePilotLabel(false, 7, 0) == 7);
    REQUIRE(GetUnifiedPhasePilotLabel(true, 0, 0) == 0);
    REQUIRE(GetUnifiedPhasePilotLabel(true, 1, 0) == 8);
}

TEST_CASE("Unified CPU oracle recovers every clean mixed Control and Transport slot", "[unified][cpu][fec][transport][control]")
{
    const Fixture& fixture = GetFixture41();
    const std::vector<std::byte> pixels = Render(fixture);
    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedExpectedFrameIdentity identity{true, pbprotocol::SessionTag{0x1122334455667788ULL}, true, 41};
    const UnifiedVisualObservation observation = oracle.Decode(View(pixels), fixture.plan, identity);
    REQUIRE(observation.IsFrameAvailable());
    REQUIRE(observation.baseLuma.IsAvailable());
    REQUIRE(observation.fineLuma.IsAvailable());
    REQUIRE(observation.chroma.IsAvailable());
    REQUIRE(std::ranges::all_of(observation.freshness,
        [](const UnifiedFreshnessObservation& value) { return value.current; }));
    REQUIRE(observation.acceptedBlocks == kUnifiedCodewordCount);
    REQUIRE(observation.acceptedControlRecords == 1);
    REQUIRE(observation.acceptedTransportBlocks == kUnifiedCodewordCount - 1);
    const std::span<const UnifiedAcceptedBlock> accepted = oracle.GetAcceptedBlocks();
    REQUIRE(accepted.size() == kUnifiedCodewordCount);
    for (std::size_t slot = 0; slot < accepted.size(); slot++)
    {
        REQUIRE(observation.slots[slot].fecValid);
        REQUIRE(observation.slots[slot].paddingValid);
        REQUIRE(observation.slots[slot].crcValid);
        REQUIRE(observation.slots[slot].identityValid);
        REQUIRE(observation.slots[slot].accepted);
        REQUIRE(accepted[slot].codewordSlot == slot);
        REQUIRE(accepted[slot].size == fixture.expected[slot].size());
        REQUIRE(std::equal(fixture.expected[slot].begin(), fixture.expected[slot].end(), accepted[slot].bytes.begin()));
    }
    const std::span<const UnifiedSoftMetric> metrics = oracle.GetSoftMetrics();
    REQUIRE(metrics.size() == kUnifiedSoftMetricCount);
    REQUIRE(std::ranges::all_of(metrics, [](const UnifiedSoftMetric& metric)
    {
        return metric.value != 0 && metric.erasureReason == UnifiedErasureReason::None &&
            metric.codewordSlot < kUnifiedCodewordCount && metric.dataRegion < kUnifiedDataRegionCount &&
            metric.freshnessRegion < kUnifiedFreshnessRegionCount;
    }));
}

TEST_CASE("Unified CPU raster round-trips all 16 FrameSequence mapping phases", "[unified][cpu][mapping]")
{
    Fixture fixture = GetFixture41();
    UnifiedVisualCpuOracle oracle = MakeOracle();
    for (std::uint64_t sequence = 0; sequence < kUnifiedMappingPhaseCount; sequence++)
    {
        fixture.bootstrap = MakeBootstrap(sequence);
        const std::vector<std::byte> pixels = Render(fixture);
        const UnifiedExpectedFrameIdentity identity{true, pbprotocol::SessionTag{0x1122334455667788ULL}, true, sequence};
        const UnifiedVisualObservation observation = oracle.Decode(View(pixels), fixture.plan, identity);
        CAPTURE(sequence);
        REQUIRE(observation.IsFrameAvailable());
        REQUIRE(observation.acceptedBlocks == kUnifiedCodewordCount);
        REQUIRE(oracle.GetAcceptedBlocks().size() == kUnifiedCodewordCount);
    }
}

TEST_CASE("Unified CPU oracle corrects a raster bit before truth admission", "[unified][cpu][fec][admission]")
{
    const Fixture& fixture = GetFixture41();
    std::vector<std::byte> pixels = Render(fixture);
    const UnifiedPhysicalCarrierSite site = GetUnifiedPhysicalCarrierSite(UnifiedLane::BaseLuma, 100, 41);
    FlipLumaCarrierBit(fixture, pixels, site, 41);
    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedVisualObservation observation = oracle.Decode(View(pixels), fixture.plan);
    REQUIRE(observation.IsFrameAvailable());
    REQUIRE(observation.acceptedBlocks == kUnifiedCodewordCount);
    REQUIRE(observation.slots[0].fecValid);
    REQUIRE(observation.slots[0].iterationsUsed > 0);
    REQUIRE(observation.slots[0].accepted);
    const std::span<const UnifiedAcceptedBlock> accepted = oracle.GetAcceptedBlocks();
    REQUIRE(accepted.size() == kUnifiedCodewordCount);
    REQUIRE(accepted[0].size == fixture.expected[0].size());
    REQUIRE(std::equal(fixture.expected[0].begin(), fixture.expected[0].end(), accepted[0].bytes.begin()));
}

TEST_CASE("Unified lane pilot failures erase only their own lane", "[unified][cpu][erasure][lane]")
{
    const Fixture& fixture = GetFixture41();
    const std::vector<std::byte> clean = Render(fixture);
    UnifiedVisualCpuOracle oracle = MakeOracle();

    SECTION("neutral active chroma pilots erase Chroma only")
    {
        std::vector<std::byte> pixels = clean;
        NeutralizeChroma(pixels);
        const UnifiedVisualObservation observation = oracle.Decode(View(pixels), fixture.plan);
        REQUIRE(observation.IsFrameAvailable());
        REQUIRE(observation.baseLuma.IsAvailable());
        REQUIRE(observation.fineLuma.IsAvailable());
        REQUIRE_FALSE(observation.chroma.IsAvailable());
        REQUIRE(observation.chroma.erasureReason == UnifiedErasureReason::ChromaPilotFailure);
        REQUIRE(observation.acceptedBlocks == 21);
        REQUIRE(observation.acceptedControlRecords == 1);
        REQUIRE(observation.acceptedTransportBlocks == 20);
    }

    SECTION("Base checker failure preserves Fine and Chroma")
    {
        std::vector<std::byte> pixels = clean;
        FillGray(pixels, kUnifiedVisualProfile.regions[19].bounds, kUnifiedNeutralLuma);
        const UnifiedVisualObservation observation = oracle.Decode(View(pixels), fixture.plan);
        REQUIRE_FALSE(observation.baseLuma.IsAvailable());
        REQUIRE(observation.fineLuma.IsAvailable());
        REQUIRE(observation.chroma.IsAvailable());
        REQUIRE(observation.acceptedBlocks == 14);
    }

    SECTION("shared luma calibration loss erases luma lanes but preserves Chroma")
    {
        std::vector<std::byte> pixels = clean;
        for (const UnifiedRegionContract& contract : kUnifiedVisualProfile.regions)
        {
            if (contract.kind == UnifiedRegionKind::Pilot &&
                HasUnifiedPilotContent(contract.pilotContent, UnifiedPilotContent::CalibrationReferences))
            {
                FillGray(pixels, UnifiedPixelRegion{contract.bounds.x, contract.bounds.y,
                    contract.bounds.width, kUnifiedCalibrationLumaRows}, kUnifiedNeutralLuma);
            }
        }
        const UnifiedVisualObservation observation = oracle.Decode(View(pixels), fixture.plan);
        REQUIRE_FALSE(observation.baseLuma.IsAvailable());
        REQUIRE_FALSE(observation.fineLuma.IsAvailable());
        REQUIRE(observation.chroma.IsAvailable());
        REQUIRE(observation.acceptedBlocks == 10);
    }

    SECTION("Fine checker failure never clears Base")
    {
        std::vector<std::byte> pixels = clean;
        FillGray(pixels, kUnifiedVisualProfile.regions[20].bounds, kUnifiedNeutralLuma);
        const UnifiedVisualObservation observation = oracle.Decode(View(pixels), fixture.plan);
        REQUIRE(observation.baseLuma.IsAvailable());
        REQUIRE_FALSE(observation.fineLuma.IsAvailable());
        REQUIRE(observation.chroma.IsAvailable());
        REQUIRE(observation.acceptedBlocks == 27);
    }
}

TEST_CASE("Unified stale and wrong-sequence mixtures fail closed without cross-frame stitching", "[unified][cpu][erasure][sequence]")
{
    const Fixture& previousFixture = GetFixture40();
    const Fixture& currentFixture = GetFixture41();
    const std::vector<std::byte> previous = Render(previousFixture);
    const std::vector<std::byte> current = Render(currentFixture);
    UnifiedVisualCpuOracle oracle = MakeOracle();

    SECTION("one stale freshness patch zeros only its assigned metrics")
    {
        const UnifiedVisualObservation previousObservation = oracle.Decode(View(previous), previousFixture.plan);
        REQUIRE(previousObservation.acceptedBlocks == kUnifiedCodewordCount);
        std::vector<std::byte> pixels = current;
        constexpr std::uint8_t staleRegion = 4;
        CopyRegion(previous, pixels, kUnifiedVisualProfile.regions[6 + staleRegion].bounds);
        CopyFreshnessData(previous, pixels, staleRegion);
        const UnifiedVisualObservation observation = oracle.Decode(View(pixels), currentFixture.plan);
        REQUIRE(observation.IsFrameAvailable());
        REQUIRE_FALSE(observation.freshness[staleRegion].current);
        for (std::size_t region = 0; region < observation.freshness.size(); region++)
        {
            if (region != staleRegion)
            {
                REQUIRE(observation.freshness[region].current);
            }
        }
        std::uint64_t staleMetrics = 0;
        for (const UnifiedSoftMetric& metric : oracle.GetSoftMetrics())
        {
            if (metric.freshnessRegion == staleRegion)
            {
                REQUIRE(metric.value == 0);
                REQUIRE(metric.erasureReason == UnifiedErasureReason::LocalStaleRegion);
                staleMetrics++;
            }
            else
            {
                REQUIRE(metric.erasureReason != UnifiedErasureReason::LocalStaleRegion);
            }
        }
        REQUIRE(staleMetrics > 0);
        REQUIRE(staleMetrics < kUnifiedSoftMetricCount);
    }

    SECTION("current Bootstrap with previous-sequence data accepts no block")
    {
        std::vector<std::byte> pixels = current;
        for (const UnifiedRegionContract& contract : kUnifiedVisualProfile.regions)
        {
            if (contract.kind == UnifiedRegionKind::Data)
            {
                CopyRegion(previous, pixels, contract.bounds);
            }
        }
        UnifiedVisualDecodePolicy policy;
        policy.maximumFecIterations = 1;
        const UnifiedVisualObservation observation = oracle.Decode(View(pixels), currentFixture.plan, {}, policy);
        REQUIRE(observation.IsFrameAvailable());
        REQUIRE(observation.acceptedBlocks == 0);
        REQUIRE(observation.acceptedControlRecords == 0);
        REQUIRE(observation.acceptedTransportBlocks == 0);
        REQUIRE(oracle.GetAcceptedBlocks().empty());
    }
}

TEST_CASE("Unified frame conflicts and post-FEC truth gates reject decisive failures", "[unified][cpu][admission]")
{
    const Fixture& previousFixture = GetFixture40();
    const Fixture& cleanFixture = GetFixture41();
    const std::vector<std::byte> previous = Render(previousFixture);
    const std::vector<std::byte> clean = Render(cleanFixture);
    UnifiedVisualCpuOracle oracle = MakeOracle();

    SECTION("cropped canvas is a whole-frame erasure")
    {
        LumaView cropped = View(clean);
        cropped.width--;
        const UnifiedVisualObservation observation = oracle.Decode(cropped, cleanFixture.plan);
        REQUIRE_FALSE(observation.IsFrameAvailable());
        REQUIRE(observation.frameErasure == UnifiedErasureReason::CanvasClipped);
        REQUIRE(oracle.GetAcceptedBlocks().empty());
    }

    SECTION("locator loss is a whole-frame erasure")
    {
        std::vector<std::byte> pixels = clean;
        FillGray(pixels, kUnifiedVisualProfile.regions[0].bounds, kUnifiedNeutralLuma);
        const UnifiedVisualObservation observation = oracle.Decode(View(pixels), cleanFixture.plan);
        REQUIRE_FALSE(observation.IsFrameAvailable());
        REQUIRE(observation.frameErasure == UnifiedErasureReason::LocatorFailure);
        REQUIRE(oracle.GetAcceptedBlocks().empty());
    }

    SECTION("Bootstrap copies from different sequences erase the whole frame")
    {
        std::vector<std::byte> pixels = clean;
        CopyRegion(previous, pixels, kUnifiedVisualProfile.regions[5].bounds);
        const UnifiedVisualObservation observation = oracle.Decode(View(pixels), cleanFixture.plan);
        REQUIRE_FALSE(observation.IsFrameAvailable());
        REQUIRE(observation.frameErasure == UnifiedErasureReason::BootstrapFailure);
        REQUIRE(oracle.GetAcceptedBlocks().empty());
    }

    SECTION("caller identity conflict erases before data admission")
    {
        const UnifiedExpectedFrameIdentity wrongIdentity{true, pbprotocol::SessionTag{9}, true, 41};
        const UnifiedVisualObservation observation = oracle.Decode(View(clean), cleanFixture.plan, wrongIdentity);
        REQUIRE_FALSE(observation.IsFrameAvailable());
        REQUIRE(observation.frameErasure == UnifiedErasureReason::IdentityConflict);
        REQUIRE(oracle.GetAcceptedBlocks().empty());
        REQUIRE(std::ranges::all_of(oracle.GetSoftMetrics(), [](const UnifiedSoftMetric& metric)
        {
            return metric.value == 0 && metric.erasureReason == UnifiedErasureReason::IdentityConflict;
        }));
    }

    SECTION("valid LDPC cannot bypass Control CRC, padding, Transport CRC, or identity")
    {
        Fixture damaged = cleanFixture;
        damaged.coded[26] ^= std::byte{1};
        ReencodeSlot(damaged.coded, 0);
        damaged.coded[static_cast<std::size_t>(1) * kUnifiedCodewordBytes + kUnifiedInformationBytes - 1] = std::byte{1};
        ReencodeSlot(damaged.coded, 1);
        damaged.coded[static_cast<std::size_t>(2) * kUnifiedCodewordBytes + pbprotocol::kTransportPayloadOffset] ^= std::byte{1};
        ReencodeSlot(damaged.coded, 2);
        std::array<std::byte, kUnifiedInformationBytes> wrongIdentityInformation{};
        const std::vector<std::byte> wrongIdentity = MakeTransport(pbprotocol::SessionTag{9}, 3);
        REQUIRE(pbprotocol::FrameTransportBlockIntoInfoBlock(
            wrongIdentity, wrongIdentityInformation.size(), wrongIdentityInformation));
        EncodeInformation(wrongIdentityInformation, 3, damaged.coded);
        const std::vector<std::byte> pixels = Render(damaged);
        const UnifiedVisualObservation observation = oracle.Decode(View(pixels), damaged.plan);
        REQUIRE(observation.slots[0].rejection == UnifiedSlotRejection::ControlCrcFailure);
        REQUIRE(observation.slots[1].rejection == UnifiedSlotRejection::NonCanonicalPadding);
        REQUIRE(observation.slots[2].rejection == UnifiedSlotRejection::TransportCrcFailure);
        REQUIRE(observation.slots[3].rejection == UnifiedSlotRejection::IdentityFailure);
        REQUIRE(observation.acceptedBlocks == kUnifiedCodewordCount - 4);
        REQUIRE(oracle.GetAcceptedBlocks().size() == kUnifiedCodewordCount - 4);
    }
}

TEST_CASE("Unified CPU oracle matches independently regenerated mixed-slot and raster Golden",
    "[unified][cpu][golden]")
{
    const std::vector<std::byte> previousBootstrap = ReadGolden("bootstrap-sequence40.bin");
    const std::vector<std::byte> currentBootstrap = ReadGolden("bootstrap-sequence41.bin");
    const std::vector<std::byte> codewords = ReadGolden("mixed-codewords.bin");
    const std::vector<std::byte> expectedAccepted = ReadGolden("accepted-stream.bin");
    const std::vector<std::byte> digests = ReadGolden("raster-digests.bin");
    REQUIRE(previousBootstrap.size() == kLocalDesktopBootstrapRecordBytes);
    REQUIRE(currentBootstrap.size() == kLocalDesktopBootstrapRecordBytes);
    REQUIRE(codewords.size() == kUnifiedCodedFrameBytes);

    std::vector<std::byte> previous(kUnifiedFrameBgraBytes);
    std::vector<std::byte> current(kUnifiedFrameBgraBytes);
    REQUIRE(EncodeUnifiedVisualFrame(previousBootstrap, codewords, previous));
    REQUIRE(EncodeUnifiedVisualFrame(currentBootstrap, codewords, current));
    RequireRasterDigest(digests, 0, current);
    {
        std::vector<std::byte> variant = current;
        NeutralizeChroma(variant);
        RequireRasterDigest(digests, 1, variant);
    }
    {
        std::vector<std::byte> variant = current;
        FillGray(variant, kUnifiedVisualProfile.regions[19].bounds, kUnifiedNeutralLuma);
        RequireRasterDigest(digests, 2, variant);
    }
    {
        std::vector<std::byte> variant = current;
        FillGray(variant, kUnifiedVisualProfile.regions[20].bounds, kUnifiedNeutralLuma);
        RequireRasterDigest(digests, 3, variant);
    }
    {
        std::vector<std::byte> variant = current;
        CopyRegion(previous, variant, kUnifiedVisualProfile.regions[10].bounds);
        CopyFreshnessData(previous, variant, 4);
        RequireRasterDigest(digests, 4, variant);
    }
    {
        std::vector<std::byte> variant = current;
        for (const UnifiedRegionContract& contract : kUnifiedVisualProfile.regions)
        {
            if (contract.kind == UnifiedRegionKind::Data)
            {
                CopyRegion(previous, variant, contract.bounds);
            }
        }
        RequireRasterDigest(digests, 5, variant);
    }

    std::array<UnifiedSlotAssignment, kUnifiedCodewordCount> plan{};
    for (std::uint32_t slot = 0; slot < kUnifiedCodewordCount; slot++)
    {
        plan[slot] = slot == 0 ?
            UnifiedSlotAssignment{slot, UnifiedSlotKind::Control, UnifiedControlPriority::SessionDescriptor} :
            UnifiedSlotAssignment{slot, UnifiedSlotKind::Transport, UnifiedControlPriority::NotApplicable};
    }
    UnifiedVisualCpuOracle oracle = MakeOracle();
    const UnifiedVisualObservation observation = oracle.Decode(View(current), plan);
    REQUIRE(observation.acceptedBlocks == kUnifiedCodewordCount);
    const std::span<const UnifiedAcceptedBlock> accepted = oracle.GetAcceptedBlocks();
    REQUIRE(accepted.size() == kUnifiedCodewordCount);
    std::size_t offset = 0;
    for (std::size_t slot = 0; slot < accepted.size(); slot++)
    {
        REQUIRE(offset <= expectedAccepted.size());
        REQUIRE(expectedAccepted.size() - offset >= 4);
        REQUIRE(std::to_integer<std::uint8_t>(expectedAccepted[offset]) == slot);
        const bool transport = std::to_integer<std::uint8_t>(expectedAccepted[offset + 1]) != 0;
        REQUIRE(accepted[slot].kind == (transport ? UnifiedSlotKind::Transport : UnifiedSlotKind::Control));
        const std::uint16_t size = ReadLe16(expectedAccepted, offset + 2);
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
