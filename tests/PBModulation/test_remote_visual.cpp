#include "pbdesktoplevels/reference_channel.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbinterleave/tile_permutation.h"
#include "pbmodulation/remote_visual.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace
{

std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeRecord(const std::uint64_t sequence,
    const std::uint64_t sessionTag = 0x1020304050607080ULL)
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = pbmodulation::kRemoteVisualLayoutVersion;
    record.visualProfileId = pbmodulation::kRemoteVisualProfileId;
    record.sessionTag.value = sessionTag;
    record.frameSequence = sequence;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bytes{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
    return bytes;
}

pbmodulation::LumaView MakeView(const std::span<const std::byte> pixels)
{
    return {pixels, pbmodulation::kLocalDesktopCanvasWidth, pbmodulation::kLocalDesktopCanvasHeight,
        pbmodulation::kLocalDesktopCanvasWidth * 4, pbmodulation::LumaPixelFormat::Bgra8};
}

std::vector<std::byte> MakeData(const std::span<const std::byte> record)
{
    std::vector<std::byte> data(pbmodulation::kRemoteVisualDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, data));
    return data;
}

void FillTile(const std::span<std::byte> pixels, const pbmodulation::LocalDesktopRegion region,
    const std::uint8_t first, const std::uint8_t second)
{
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        for (std::uint32_t column = 0; column < region.width; column++)
        {
            const std::uint8_t level = ((row + column) & 1) == 0 ? first : second;
            const std::size_t offset = (static_cast<std::size_t>(region.y + row) * pbmodulation::kLocalDesktopCanvasWidth +
                region.x + column) * 4;
            pixels[offset] = static_cast<std::byte>(level);
            pixels[offset + 1] = static_cast<std::byte>(level);
            pixels[offset + 2] = static_cast<std::byte>(level);
            pixels[offset + 3] = std::byte{255};
        }
    }
}

void FillRect(const std::span<std::byte> pixels, const pbmodulation::LocalDesktopRegion region,
    const std::uint8_t level)
{
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        for (std::uint32_t column = 0; column < region.width; column++)
        {
            const std::size_t offset = (static_cast<std::size_t>(region.y + row) * pbmodulation::kLocalDesktopCanvasWidth +
                region.x + column) * 4;
            pixels[offset] = static_cast<std::byte>(level);
            pixels[offset + 1] = static_cast<std::byte>(level);
            pixels[offset + 2] = static_cast<std::byte>(level);
            pixels[offset + 3] = std::byte{255};
        }
    }
}

void CorruptBlockPerimeter(const std::span<std::byte> pixels, const pbmodulation::LocalDesktopRegion region,
    const std::uint32_t inset)
{
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        for (std::uint32_t column = 0; column < region.width; column++)
        {
            if (row >= inset && row < region.height - inset && column >= inset && column < region.width - inset)
            {
                continue;
            }
            const std::uint8_t level = ((row + column) & 1) == 0 ? 0 : 255;
            const std::size_t offset = (static_cast<std::size_t>(region.y + row) * pbmodulation::kLocalDesktopCanvasWidth +
                region.x + column) * 4;
            pixels[offset] = static_cast<std::byte>(level);
            pixels[offset + 1] = static_cast<std::byte>(level);
            pixels[offset + 2] = static_cast<std::byte>(level);
            pixels[offset + 3] = std::byte{255};
        }
    }
}

void AddLumaNeutralChromaNoise(const std::span<std::byte> pixels, const pbmodulation::LocalDesktopRegion region,
    const std::uint32_t inset)
{
    REQUIRE(region.x + region.width <= pbmodulation::kLocalDesktopCanvasWidth);
    REQUIRE(region.y + region.height <= pbmodulation::kLocalDesktopCanvasHeight);
    REQUIRE(inset * 2 < region.width);
    REQUIRE(inset * 2 < region.height);
    for (std::uint32_t row = inset; row < region.height - inset; row++)
    {
        for (std::uint32_t column = inset; column < region.width - inset; column++)
        {
            const std::size_t offset = (static_cast<std::size_t>(region.y + row) *
                pbmodulation::kLocalDesktopCanvasWidth + region.x + column) * 4;
            const int level = std::to_integer<std::uint8_t>(pixels[offset + 1]);
            const int phase = ((row + column) & 1) == 0 ? 1 : -1;
            const int blue = std::clamp(level + phase * 24, 1, 254);
            const int red = std::clamp(level - phase * 8, 1, 254);
            pixels[offset] = static_cast<std::byte>(blue);
            pixels[offset + 2] = static_cast<std::byte>(red);
        }
    }
}

void CopyRect(const std::span<const std::byte> source, const std::span<std::byte> destination,
    const pbmodulation::LocalDesktopRegion region)
{
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        const std::size_t offset = (static_cast<std::size_t>(region.y + row) * pbmodulation::kLocalDesktopCanvasWidth +
            region.x) * 4;
        std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(offset), static_cast<std::size_t>(region.width) * 4,
            destination.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

void CopyRemoteVisualRegionTiles(const std::span<const std::byte> source, const std::span<std::byte> destination,
    const std::uint16_t regionId)
{
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        pbmodulation::LocalDesktopRegion region;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        REQUIRE(pbmodulation::GetRemoteVisualTile(physical, region));
        if (mapping.regionId == regionId && mapping.role != pbmodulation::RemoteVisualTileRole::Unused)
        {
            CopyRect(source, destination, region);
        }
    }
}

std::string Hex(const std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const std::byte value : bytes)
    {
        const unsigned number = std::to_integer<unsigned>(value);
        result.push_back(digits[number >> 4]);
        result.push_back(digits[number & 15]);
    }
    return result;
}

std::uint32_t FindPhysicalDataTile(const std::uint32_t dataOrdinal)
{
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        if (mapping.role == pbmodulation::RemoteVisualTileRole::Data && mapping.dataOrdinal == dataOrdinal)
        {
            return physical;
        }
    }
    return pbmodulation::kRemoteVisualTileCount;
}

} // namespace

TEST_CASE("RemoteVisual profile has an independent frozen identity and full affine bijection", "[remote-visual][profile][interleave]")
{
    STATIC_REQUIRE(pbmodulation::kRemoteVisualProfileId == 0xED05C2CC0397572AULL);
    STATIC_REQUIRE(pbmodulation::kRemoteVisualLayoutVersion == 6);
    STATIC_REQUIRE(pbmodulation::kRemoteVisualDataBytes == 2025);
    STATIC_REQUIRE(pbmodulation::kRemoteVisualCodedBits == 16200);
    STATIC_REQUIRE(pbmodulation::kRemoteVisualTileCount == 21456);
    STATIC_REQUIRE(pbmodulation::kRemoteVisualDataTileCount == 16723);
    STATIC_REQUIRE(pbmodulation::kRemoteVisualCodewords == 1);
    REQUIRE(pbinterleave::GetDesktopLevelsPermutation(8) == nullptr);
    REQUIRE(pbinterleave::kRemoteVisualPermutation.IsValid());

    std::vector<bool> visited(pbmodulation::kRemoteVisualDataTileCount);
    for (std::uint64_t phase = 0; phase < 16; phase++)
    {
        std::fill(visited.begin(), visited.end(), false);
        for (std::uint32_t logical = 0; logical < pbmodulation::kRemoteVisualDataTileCount; logical++)
        {
            const std::uint32_t physical = pbinterleave::kRemoteVisualPermutation.ToPhysical(logical, phase);
            REQUIRE(physical < pbmodulation::kRemoteVisualDataTileCount);
            REQUIRE_FALSE(visited[physical]);
            visited[physical] = true;
            REQUIRE(pbinterleave::kRemoteVisualPermutation.ToLogical(physical, phase) == logical);
        }
    }

    REQUIRE(pbinterleave::kDesktopLevelsPermutation2.ToPhysical(12345, 7) == 87897);
    REQUIRE(pbinterleave::kDesktopLevelsPermutation4.ToPhysical(12345, 7) == 84873);
}

TEST_CASE("RemoteVisual tile manifest is bounded nonoverlapping and preserves explicit guard strips", "[remote-visual][geometry]")
{
    std::vector<bool> occupied(static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight);
    std::uint32_t dataTiles = 0;
    std::uint32_t freshnessTags = 0;
    std::array<bool, pbmodulation::kRemoteVisualFreshnessRegionCount> freshnessRegions{};
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::LocalDesktopRegion region;
        pbmodulation::RemoteVisualTileMapping mapping;
        REQUIRE(pbmodulation::GetRemoteVisualTile(physical, region));
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        REQUIRE(region.width == 8);
        REQUIRE(region.height == 8);
        REQUIRE(region.x + region.width <= pbmodulation::kLocalDesktopCanvasWidth);
        REQUIRE(region.y + region.height <= pbmodulation::kLocalDesktopCanvasHeight);
        for (std::uint32_t row = 0; row < region.height; row++)
        {
            for (std::uint32_t column = 0; column < region.width; column++)
            {
                const std::size_t pixel = static_cast<std::size_t>(region.y + row) * pbmodulation::kLocalDesktopCanvasWidth +
                    region.x + column;
                REQUIRE_FALSE(occupied[pixel]);
                occupied[pixel] = true;
            }
        }
        if (mapping.role == pbmodulation::RemoteVisualTileRole::Data)
        {
            REQUIRE(mapping.dataOrdinal == dataTiles);
            dataTiles++;
        }
        else if (mapping.role == pbmodulation::RemoteVisualTileRole::FreshnessTag)
        {
            REQUIRE(mapping.regionId < freshnessRegions.size());
            freshnessRegions[mapping.regionId] = true;
            freshnessTags++;
            bool expected = false;
            REQUIRE(pbmodulation::GetRemoteVisualFreshnessBit(11, 22, physical, expected));
        }
    }
    REQUIRE(dataTiles == pbmodulation::kRemoteVisualDataTileCount);
    REQUIRE(freshnessTags == 2389);
    REQUIRE(std::count(freshnessRegions.begin(), freshnessRegions.end(), true) ==
        pbmodulation::kRemoteVisualEligibleFreshnessRegions);
    pbmodulation::LocalDesktopRegion sentinel{1, 2, 3, 4};
    REQUIRE_FALSE(pbmodulation::GetRemoteVisualTile(pbmodulation::kRemoteVisualTileCount, sentinel));
    REQUIRE(sentinel == pbmodulation::LocalDesktopRegion{1, 2, 3, 4});
    for (const std::uint32_t y : {472u, 473u, 474u, 475u, 604u, 605u, 606u, 607u})
    {
        for (std::uint32_t x = 96; x < 1824; x++)
        {
            REQUIRE_FALSE(occupied[static_cast<std::size_t>(y) * pbmodulation::kLocalDesktopCanvasWidth + x]);
        }
    }
}

TEST_CASE("RemoteVisual rejects the measured fit-to-window scale while retaining the strict physical 1-to-1 contract",
    "[remote-visual][geometry][remote-codec]")
{
    pbmodulation::LocalDesktopGeometry geometry;
    geometry.scaleX = 1;
    geometry.scaleY = 1;
    REQUIRE(pbmodulation::ValidateRemoteVisualGeometry(geometry) == pbmodulation::RemoteVisualErasure::None);

    geometry.scaleX = 2418.0 / 1920.0;
    geometry.scaleY = 1360.0 / 1080.0;
    REQUIRE(pbmodulation::ValidateRemoteVisualGeometry(geometry) ==
        pbmodulation::RemoteVisualErasure::ScaleOutOfRange);
}

TEST_CASE("RemoteVisual freshness resolution is deterministic and failure atomic", "[remote-visual][freshness][bounds]")
{
    constexpr std::uint64_t sessionTag = 0x1020304050607080ULL;
    constexpr std::uint64_t currentSequence = 41;
    constexpr std::uint64_t previousSequence = 40;
    std::vector<float> physicalMetrics(pbmodulation::kRemoteVisualTileCount, 0.5f);
    std::array<std::uint32_t, pbmodulation::kRemoteVisualFreshnessRegionCount> changedTags{};
    std::array<bool, pbmodulation::kRemoteVisualFreshnessRegionCount> eligibleRegions{};
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        if (mapping.role == pbmodulation::RemoteVisualTileRole::FreshnessTag)
        {
            bool currentBit = false;
            bool previousBit = false;
            REQUIRE(pbmodulation::GetRemoteVisualFreshnessBit(sessionTag, currentSequence, physical, currentBit));
            REQUIRE(pbmodulation::GetRemoteVisualFreshnessBit(sessionTag, previousSequence, physical, previousBit));
            physicalMetrics[physical] = currentBit ? -1.0f : 1.0f;
            eligibleRegions[mapping.regionId] = true;
            changedTags[mapping.regionId] += static_cast<std::uint32_t>(currentBit != previousBit);
        }
        else if (mapping.role == pbmodulation::RemoteVisualTileRole::Data)
        {
            const float magnitude = 0.25f + static_cast<float>(physical % 13) * 0.05f;
            physicalMetrics[physical] = (physical & 1) == 0 ? magnitude : -magnitude;
        }
    }
    for (std::size_t region = 0; region < eligibleRegions.size(); region++)
    {
        if (eligibleRegions[region])
        {
            REQUIRE(changedTags[region] >= 4);
        }
    }

    std::vector<float> logicalMetrics(pbmodulation::kRemoteVisualCodedBits, 42.0f);
    const auto resolution = pbmodulation::ResolveRemoteVisualPhysicalMetrics(physicalMetrics, sessionTag,
        currentSequence, logicalMetrics);
    REQUIRE(resolution.valid);
    REQUIRE(resolution.freshnessRegions == pbmodulation::kRemoteVisualEligibleFreshnessRegions);
    REQUIRE(resolution.staleRegions == 0);
    REQUIRE(resolution.freshnessTagMismatches == 0);
    REQUIRE(resolution.freshnessTagErasures == 0);
    REQUIRE(resolution.erasedDataMetrics == 0);
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        if (mapping.role == pbmodulation::RemoteVisualTileRole::Data)
        {
            const std::uint32_t logical = pbinterleave::kRemoteVisualPermutation.ToLogical(mapping.dataOrdinal,
                currentSequence);
            if (logical < pbmodulation::kRemoteVisualCodedBits)
            {
                REQUIRE(logicalMetrics[logical] == physicalMetrics[physical]);
            }
        }
    }

    const std::vector<float> preservedMetrics = logicalMetrics;
    physicalMetrics[0] = (std::numeric_limits<float>::infinity)();
    const auto nonFinite = pbmodulation::ResolveRemoteVisualPhysicalMetrics(physicalMetrics, sessionTag,
        currentSequence, logicalMetrics);
    REQUIRE_FALSE(nonFinite.valid);
    REQUIRE(logicalMetrics == preservedMetrics);

    std::vector<float> overlappingStorage(pbmodulation::kRemoteVisualTileCount +
        pbmodulation::kRemoteVisualCodedBits, 0.5f);
    const auto overlappingPhysical = std::span<const float>(overlappingStorage).first(
        pbmodulation::kRemoteVisualTileCount);
    const auto overlappingLogical = std::span<float>(overlappingStorage).subspan(128,
        pbmodulation::kRemoteVisualCodedBits);
    const auto overlapping = pbmodulation::ResolveRemoteVisualPhysicalMetrics(overlappingPhysical, sessionTag,
        currentSequence, overlappingLogical);
    REQUIRE_FALSE(overlapping.valid);
    REQUIRE(std::ranges::all_of(overlappingStorage, [](const float value) { return value == 0.5f; }));

    bool sentinel = true;
    REQUIRE_FALSE(pbmodulation::GetRemoteVisualFreshnessBit(sessionTag, currentSequence,
        pbmodulation::kRemoteVisualTileCount, sentinel));
    REQUIRE(sentinel);
}

TEST_CASE("RemoteVisual canonical raster round trips one unchanged Robust codeword", "[remote-visual][golden][fec]")
{
    for (const std::uint64_t sequence : {0ULL, 7ULL, 15ULL})
    {
        const auto record = MakeRecord(sequence);
        const auto data = MakeData(record);
        std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes, std::byte{0xA5});
        REQUIRE(pbmodulation::EncodeRemoteVisualFrame(record, data, pixels));
        if (sequence == 7)
        {
            REQUIRE(Hex(pbprotocol::ComputeBlake3Digest(pixels)) ==
                "697cf6d56e6406b6d0c6b210ce6bd548c846ff5c5582b4377eac89982a33d706");
        }

        auto workspaceResult = pbmodulation::RemoteVisualWorkspace::Create(16 * 1024 * 1024);
        REQUIRE(workspaceResult);
        auto workspace = std::move(workspaceResult).Value();
        std::array<std::byte, pbmodulation::kRemoteVisualDataBytes> hard{};
        std::array<float, pbmodulation::kRemoteVisualCodedBits> soft{};
        const auto observation = pbmodulation::DecodeRemoteVisualFrame(MakeView(pixels), workspace, hard, soft);
        INFO(pbmodulation::GetRemoteVisualErasureName(observation.erasure));
        REQUIRE(observation.IsAccepted());
        REQUIRE(observation.dataBytes == pbmodulation::kRemoteVisualDataBytes);
        REQUIRE(observation.unreliableTiles == 0);
        REQUIRE(std::equal(hard.begin(), hard.end(), data.begin(), data.end()));
        REQUIRE(observation.margin.samples == pbmodulation::kRemoteVisualCodedBits);
        REQUIRE(observation.margin.minimum > 0.99);

        auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
        REQUIRE(channelResult);
        auto channel = std::move(channelResult).Value();
        const auto channelObservation = channel.DecodeRemoteVisual(MakeView(pixels));
        REQUIRE(channelObservation.modulation.IsAccepted());
        REQUIRE(channelObservation.evaluation.IsVerified());
        REQUIRE(channelObservation.evaluation.falseAcceptedCodewords == 0);
    }
}

TEST_CASE("RemoteVisual converts a locally corrupted coarse tile into a bounded soft erasure", "[remote-visual][erasure][fec]")
{
    const auto record = MakeRecord(9);
    const auto data = MakeData(record);
    std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualFrame(record, data, pixels));

    std::size_t selectedBit = 0;
    while (selectedBit < pbmodulation::kRemoteVisualCodedBits &&
        (data[selectedBit / 8] & static_cast<std::byte>(1u << (selectedBit % 8))) == std::byte{0})
    {
        selectedBit++;
    }
    REQUIRE(selectedBit < pbmodulation::kRemoteVisualCodedBits);
    const std::uint32_t dataOrdinal = pbinterleave::kRemoteVisualPermutation.ToPhysical(
        static_cast<std::uint32_t>(selectedBit), 9);
    const std::uint32_t physical = FindPhysicalDataTile(dataOrdinal);
    REQUIRE(physical < pbmodulation::kRemoteVisualTileCount);
    pbmodulation::LocalDesktopRegion region;
    REQUIRE(pbmodulation::GetRemoteVisualTile(physical, region));
    FillTile(pixels, region, 0, 255);

    auto workspaceResult = pbmodulation::RemoteVisualWorkspace::Create(16 * 1024 * 1024);
    REQUIRE(workspaceResult);
    auto workspace = std::move(workspaceResult).Value();
    std::array<std::byte, pbmodulation::kRemoteVisualDataBytes> hard{};
    std::array<float, pbmodulation::kRemoteVisualCodedBits> soft{};
    const auto observation = pbmodulation::DecodeRemoteVisualFrame(MakeView(pixels), workspace, hard, soft);
    REQUIRE(observation.IsAccepted());
    REQUIRE(observation.unreliableTiles == 1);
    REQUIRE(soft[selectedBit] == 0.0f);

    auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(channelResult);
    auto channel = std::move(channelResult).Value();
    const auto channelObservation = channel.DecodeRemoteVisual(MakeView(pixels));
    REQUIRE(channelObservation.modulation.IsAccepted());
    REQUIRE(channelObservation.evaluation.IsVerified());
    REQUIRE(channelObservation.evaluation.falseAcceptedCodewords == 0);
}

TEST_CASE("RemoteVisual ignores codec-contaminated tile and calibration perimeters", "[remote-visual][codec][ringing]")
{
    const auto record = MakeRecord(18);
    const auto data = MakeData(record);
    std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualFrame(record, data, pixels));
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::LocalDesktopRegion region;
        REQUIRE(pbmodulation::GetRemoteVisualTile(physical, region));
        CorruptBlockPerimeter(pixels, region, pbmodulation::kRemoteVisualTileSampleInset);
    }
    for (const auto ladder : pbmodulation::kRemoteVisualLadders)
    {
        for (std::uint32_t level = 0; level < 4; level++)
        {
            CorruptBlockPerimeter(pixels, {ladder.x + level * 32, ladder.y, 32, 64},
                pbmodulation::kRemoteVisualCalibrationSampleInset);
        }
    }

    auto workspaceResult = pbmodulation::RemoteVisualWorkspace::Create(16 * 1024 * 1024);
    REQUIRE(workspaceResult);
    auto workspace = std::move(workspaceResult).Value();
    std::array<std::byte, pbmodulation::kRemoteVisualDataBytes> hard{};
    std::array<float, pbmodulation::kRemoteVisualCodedBits> soft{};
    const auto observation = pbmodulation::DecodeRemoteVisualFrame(MakeView(pixels), workspace, hard, soft);
    INFO(pbmodulation::GetRemoteVisualErasureName(observation.erasure));
    REQUIRE(observation.IsAccepted());
    REQUIRE(observation.unreliableTiles == 0);
    REQUIRE(std::equal(hard.begin(), hard.end(), data.begin(), data.end()));

    auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(channelResult);
    auto channel = std::move(channelResult).Value();
    const auto channelObservation = channel.DecodeRemoteVisual(MakeView(pixels));
    REQUIRE(channelObservation.modulation.IsAccepted());
    REQUIRE(channelObservation.evaluation.IsVerified());
    REQUIRE(channelObservation.evaluation.falseAcceptedCodewords == 0);
}

TEST_CASE("RemoteVisual luma carrier survives bounded chroma contamination without accepting a wrong codeword",
    "[remote-visual][codec][chroma][remote-codec]")
{
    const auto record = MakeRecord(29);
    const auto data = MakeData(record);
    std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualFrame(record, data, pixels));
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::LocalDesktopRegion region;
        REQUIRE(pbmodulation::GetRemoteVisualTile(physical, region));
        AddLumaNeutralChromaNoise(pixels, region, pbmodulation::kRemoteVisualTileSampleInset);
    }
    for (const auto ladder : pbmodulation::kRemoteVisualLadders)
    {
        for (std::uint32_t level = 0; level < 4; level++)
        {
            AddLumaNeutralChromaNoise(pixels, {ladder.x + level * 32, ladder.y, 32, 64},
                pbmodulation::kRemoteVisualCalibrationSampleInset);
        }
    }

    auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(channelResult);
    auto channel = std::move(channelResult).Value();
    const auto observation = channel.DecodeRemoteVisual(MakeView(pixels));
    INFO(pbmodulation::GetRemoteVisualErasureName(observation.modulation.erasure));
    REQUIRE(observation.modulation.IsAccepted());
    REQUIRE(observation.modulation.staleRegions == 0);
    REQUIRE(observation.evaluation.evaluated);
    REQUIRE(observation.evaluation.IsVerified());
    REQUIRE(observation.evaluation.falseAcceptedCodewords == 0);
}

TEST_CASE("RemoteVisual never publishes a partially sampled frame after the data work budget expires",
    "[remote-visual][erasure][budget]")
{
    const auto record = MakeRecord(10);
    const auto data = MakeData(record);
    std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualFrame(record, data, pixels));

    auto workspaceResult = pbmodulation::RemoteVisualWorkspace::Create(16 * 1024 * 1024);
    REQUIRE(workspaceResult);
    auto workspace = std::move(workspaceResult).Value();
    std::array<std::byte, pbmodulation::kRemoteVisualDataBytes> hard{};
    std::array<float, pbmodulation::kRemoteVisualCodedBits> soft{};
    hard.fill(std::byte{0xA5});
    soft.fill(123.0f);
    pbmodulation::RemoteVisualDecodePolicy policy;
    policy.maximumDataWorkUnits = 20000;
    const auto observation = pbmodulation::DecodeRemoteVisualFrame(MakeView(pixels), workspace, hard, soft, policy);
    REQUIRE_FALSE(observation.IsAccepted());
    REQUIRE(observation.erasure == pbmodulation::RemoteVisualErasure::WorkBudgetExceeded);
    REQUIRE(std::ranges::all_of(hard, [](const std::byte value) { return value == std::byte{0xA5}; }));
    REQUIRE(std::ranges::all_of(soft, [](const float value) { return value == 123.0f; }));
}

TEST_CASE("RemoteVisual recovers bounded macroblock loss and fails closed on screenshot-scale stale prediction regions",
    "[remote-visual][codec][block-loss][temporal]")
{
    const auto currentRecord = MakeRecord(32);
    const auto previousRecord = MakeRecord(31);
    const auto currentData = MakeData(currentRecord);
    const auto previousData = MakeData(previousRecord);
    std::vector<std::byte> currentPixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    std::vector<std::byte> previousPixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualFrame(currentRecord, currentData, currentPixels));
    REQUIRE(pbmodulation::EncodeRemoteVisualFrame(previousRecord, previousData, previousPixels));

    std::vector<std::byte> boundedLoss = currentPixels;
    FillRect(boundedLoss, {400, 304, 160, 64}, pbmodulation::kRemoteVisualUnusedLuma);
    auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(channelResult);
    auto channel = std::move(channelResult).Value();
    const auto recovered = channel.DecodeRemoteVisual(MakeView(boundedLoss));
    REQUIRE(recovered.modulation.IsAccepted());
    REQUIRE(recovered.modulation.staleRegions > 0);
    REQUIRE(recovered.modulation.freshnessTagErasures > 0);
    REQUIRE(recovered.evaluation.evaluated);
    REQUIRE(recovered.evaluation.IsVerified());
    REQUIRE(recovered.evaluation.falseAcceptedCodewords == 0);

    std::vector<std::byte> boundedStalePrediction = currentPixels;
    std::array<bool, pbmodulation::kRemoteVisualFreshnessRegionCount> selectedRegions{};
    std::uint32_t selectedRegionCount = 0;
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount && selectedRegionCount < 6; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        if (mapping.role == pbmodulation::RemoteVisualTileRole::FreshnessTag && !selectedRegions[mapping.regionId])
        {
            selectedRegions[mapping.regionId] = true;
            selectedRegionCount++;
            CopyRemoteVisualRegionTiles(previousPixels, boundedStalePrediction, mapping.regionId);
        }
    }
    REQUIRE(selectedRegionCount == 6);
    const auto staleRecovered = channel.DecodeRemoteVisual(MakeView(boundedStalePrediction));
    INFO(pbmodulation::GetRemoteVisualErasureName(staleRecovered.modulation.erasure));
    REQUIRE(staleRecovered.modulation.IsAccepted());
    REQUIRE(staleRecovered.modulation.staleRegions >= 6);
    REQUIRE(staleRecovered.modulation.freshnessTagMismatches > 0);
    REQUIRE(staleRecovered.evaluation.evaluated);
    REQUIRE(staleRecovered.evaluation.IsVerified());
    REQUIRE(staleRecovered.evaluation.falseAcceptedCodewords == 0);

    std::vector<std::byte> stalePrediction = currentPixels;
    CopyRect(previousPixels, stalePrediction, {224, 288, 800, 184});
    CopyRect(previousPixels, stalePrediction, {1024, 608, 672, 184});
    FillRect(stalePrediction, {224, 792, 672, 128}, pbmodulation::kRemoteVisualUnusedLuma);
    const auto rejected = channel.DecodeRemoteVisual(MakeView(stalePrediction));
    REQUIRE(rejected.modulation.IsAccepted());
    REQUIRE(rejected.modulation.staleRegions > staleRecovered.modulation.staleRegions);
    REQUIRE(rejected.modulation.freshnessTagMismatches > 0);
    REQUIRE(rejected.evaluation.evaluated);
    REQUIRE_FALSE(rejected.evaluation.IsVerified());
    REQUIRE(rejected.evaluation.falseAcceptedCodewords == 0);
    REQUIRE(channel.GetAcceptedTransportBlocks().empty());
}

TEST_CASE("RemoteVisual carries unchanged Control bytes through the Robust codeword and rejects identity mismatch",
    "[remote-visual][control][fec][identity]")
{
    constexpr std::uint64_t sessionTag = 0x1020304050607080ULL;
    const auto record = MakeRecord(21, sessionTag);
    const std::array<std::byte, 9> payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5},
        std::byte{6}, std::byte{7}, std::byte{8}, std::byte{9}};
    const pbprotocol::ControlRecordView controlView{pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::FinalManifest, 17, pbprotocol::SessionTag{sessionTag}, payload};
    const auto controlSize = pbprotocol::GetSerializedSize(controlView);
    REQUIRE(controlSize);
    REQUIRE(controlSize.Value() <= pbmodulation::kReferenceControlWindowBytes);
    std::array<std::byte, pbdesktoplevels::kInfoBytes> information{};
    REQUIRE(pbprotocol::SerializeControlRecord(controlView, std::span(information).first(controlSize.Value())));
    std::array<std::byte, pbmodulation::kRemoteVisualDataBytes> codeword{};
    REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information, codeword));
    std::array<float, pbmodulation::kRemoteVisualCodedBits> metrics{};
    for (std::size_t bit = 0; bit < metrics.size(); bit++)
    {
        const bool one = (codeword[bit / 8] & static_cast<std::byte>(1u << (bit % 8))) != std::byte{0};
        metrics[bit] = one ? -1.0f : 1.0f;
    }

    auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(channelResult);
    auto channel = std::move(channelResult).Value();
    const auto evaluation = channel.EvaluateCodewords(record, codeword, metrics,
        pbdesktoplevels::EvaluationMode::Transport);
    REQUIRE(evaluation.evaluated);
    REQUIRE(evaluation.fecFailures == 0);
    REQUIRE(evaluation.crcFailures == 0);
    REQUIRE(evaluation.identityFailures == 0);
    REQUIRE(evaluation.acceptedTransportBlocks == 0);
    REQUIRE(evaluation.acceptedRemoteControlBlocks == 1);
    const auto accepted = channel.GetAcceptedRemoteControlBlocks();
    REQUIRE(accepted.size() == 1);
    REQUIRE(accepted[0].kind == pbdesktoplevels::AcceptedRemoteControlKind::Record);
    REQUIRE(accepted[0].byteCount == controlSize.Value());
    REQUIRE(std::equal(accepted[0].bytes.begin(), accepted[0].bytes.begin() + accepted[0].byteCount,
        information.begin(), information.begin() + accepted[0].byteCount));

    const auto wrongSessionRecord = MakeRecord(21, sessionTag ^ 1ULL);
    const auto rejected = channel.EvaluateCodewords(wrongSessionRecord, codeword, metrics,
        pbdesktoplevels::EvaluationMode::Transport);
    REQUIRE(rejected.evaluated);
    REQUIRE(rejected.fecFailures == 0);
    REQUIRE(rejected.crcFailures == 0);
    REQUIRE(rejected.identityFailures == 1);
    REQUIRE(rejected.acceptedRemoteControlBlocks == 0);
    REQUIRE(channel.GetAcceptedRemoteControlBlocks().empty());
}
