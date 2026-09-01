#include "local_desktop_resample_fixtures.h"

#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{

std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeLowFpsRecord(const std::uint64_t sequence,
    const std::uint64_t sessionTag = 0x6D3C2B1A90785634ULL)
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = pbmodulation::kRemoteVisualLowFpsLayoutVersion;
    record.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    record.sessionTag.value = sessionTag;
    record.frameSequence = sequence;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bytes{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
    return bytes;
}

std::vector<std::byte> MakeLowFpsData()
{
    std::vector<std::byte> data(pbmodulation::kRemoteVisualLowFpsDataBytes);
    std::uint64_t state = 0xD1B54A32D192ED03ULL;
    for (std::byte& value : data)
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        value = static_cast<std::byte>((state * 0x2545F4914F6CDD1DULL) >> 56);
    }
    return data;
}

std::vector<std::byte> MakeLowFpsDiagnosticData(const std::span<const std::byte> record)
{
    std::vector<std::byte> data(pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, data));
    return data;
}

void CopyLowFpsRegion(const std::span<const std::byte> source, const std::span<std::byte> destination,
    const std::uint16_t regionId)
{
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        pbmodulation::LocalDesktopRegion region;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        REQUIRE(pbmodulation::GetRemoteVisualTile(physical, region));
        if (mapping.regionId != regionId || mapping.role == pbmodulation::RemoteVisualTileRole::Unused)
        {
            continue;
        }
        for (std::uint32_t row = 0; row < region.height; row++)
        {
            const std::size_t offset = (static_cast<std::size_t>(region.y + row) *
                pbmodulation::kLocalDesktopCanvasWidth + region.x) * 4;
            std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(offset), static_cast<std::size_t>(region.width) * 4,
                destination.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
}

void RequireDecode(const localdesktoptest::GrayImage& image, const std::span<const std::byte> expected)
{
    auto created = pbmodulation::RemoteVisualLowFpsWorkspace::Create(
        pbmodulation::RemoteVisualLowFpsWorkspace::RequiredBytes());
    REQUIRE(created);
    auto workspace = std::move(created.Value());
    std::vector<std::byte> hard(pbmodulation::kRemoteVisualLowFpsDataBytes);
    std::vector<float> soft(pbmodulation::kRemoteVisualLowFpsCodedBits);
    const auto observation = pbmodulation::DecodeRemoteVisualLowFpsFrame(image.View(), workspace, hard, soft);
    INFO(pbmodulation::GetRemoteVisualLowFpsErasureName(observation.erasure));
    INFO(static_cast<int>(observation.bootstrap.erasure));
    INFO(observation.bootstrap.geometry.originX);
    INFO(observation.bootstrap.geometry.originY);
    INFO(observation.bootstrap.geometry.scaleX);
    INFO(observation.bootstrap.geometry.scaleY);
    INFO(observation.dataWorkUnits);
    INFO(observation.margin.minimum);
    INFO(observation.margin.p001);
    REQUIRE(observation.IsAccepted());
    REQUIRE(observation.dataBytes == pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(observation.unreliableSymbols == 0);
    REQUIRE(observation.staleRegions == 0);
    REQUIRE(std::ranges::equal(hard, expected));
    REQUIRE(std::ranges::all_of(soft, [](const float metric) { return std::isfinite(metric) && std::abs(metric) >= 0.2f; }));
}

} // namespace

TEST_CASE("RemoteVisual low-FPS profile freezes a balanced distance-eight codebook and four planes",
    "[remote-visual][low-fps][profile]")
{
    STATIC_REQUIRE(pbmodulation::kRemoteVisualLowFpsProfileId == 0x504252564C463431ULL);
    STATIC_REQUIRE(pbmodulation::kRemoteVisualLowFpsLayoutVersion == 7);
    STATIC_REQUIRE(pbmodulation::kRemoteVisualLowFpsBitsPerTile == 4);
    STATIC_REQUIRE(pbmodulation::kRemoteVisualLowFpsDataBytes == 8100);
    STATIC_REQUIRE(pbmodulation::kRemoteVisualLowFpsCodedBits == 64800);
    STATIC_REQUIRE(pbmodulation::kRemoteVisualLowFpsCodewords == 4);
    for (std::size_t first = 0; first < pbmodulation::kRemoteVisualLowFpsSymbolMasks.size(); first++)
    {
        REQUIRE(std::popcount(pbmodulation::kRemoteVisualLowFpsSymbolMasks[first]) == 8);
        REQUIRE(pbmodulation::kRemoteVisualLowFpsSymbolMasks[first + 8 < 16 ? first + 8 : first - 8] ==
            static_cast<std::uint16_t>(~pbmodulation::kRemoteVisualLowFpsSymbolMasks[first]));
        for (std::size_t second = first + 1; second < pbmodulation::kRemoteVisualLowFpsSymbolMasks.size(); second++)
        {
            REQUIRE(std::popcount(static_cast<std::uint16_t>(pbmodulation::kRemoteVisualLowFpsSymbolMasks[first] ^
                pbmodulation::kRemoteVisualLowFpsSymbolMasks[second])) >= 8);
        }
    }

    for (std::uint32_t plane = 0; plane < pbmodulation::kRemoteVisualLowFpsBitsPerTile; plane++)
    {
        std::vector<bool> visited(pbmodulation::kRemoteVisualLowFpsCodedBitsPerPlane);
        for (std::uint32_t dataOrdinal = 0; dataOrdinal < pbmodulation::kRemoteVisualDataTileCount; dataOrdinal++)
        {
            const std::uint32_t logical = pbmodulation::GetRemoteVisualLowFpsLogicalBit(dataOrdinal, plane, 17);
            if (logical < pbmodulation::kRemoteVisualLowFpsCodedBits)
            {
                const std::uint32_t planeLogical = logical - plane * pbmodulation::kRemoteVisualLowFpsCodedBitsPerPlane;
                REQUIRE_FALSE(visited[planeLogical]);
                visited[planeLogical] = true;
            }
        }
        REQUIRE(std::ranges::all_of(visited, [](const bool value) { return value; }));
    }
    REQUIRE(pbmodulation::GetRemoteVisualLowFpsLogicalBit(pbmodulation::kRemoteVisualDataTileCount, 0, 0) ==
        pbmodulation::kRemoteVisualLowFpsCodedBits);
    REQUIRE(pbmodulation::GetRemoteVisualLowFpsLogicalBit(0, pbmodulation::kRemoteVisualLowFpsBitsPerTile, 0) ==
        pbmodulation::kRemoteVisualLowFpsCodedBits);
}

TEST_CASE("RemoteVisual low-FPS CPU reference decodes exact and independently resampled canvases",
    "[remote-visual][low-fps][scale][integration]")
{
    const auto record = MakeLowFpsRecord(37);
    const auto data = MakeLowFpsData();
    std::vector<std::byte> bgra(static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight * 4);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(record, data, bgra));
    const auto original = localdesktoptest::GrayFromGolden(bgra);

    SECTION("one-to-one")
    {
        RequireDecode(original, data);
    }
    SECTION("half-scale area filter")
    {
        const auto scaled = localdesktoptest::Resample(original, 0.5, 0.5, 7.0, 9.0,
            localdesktoptest::FixtureFilter::Area);
        RequireDecode(scaled, data);
    }
    SECTION("uploaded-evidence scale area filter")
    {
        const auto scaled = localdesktoptest::Resample(original, 1.259375, 1.2592592592592593, 11.25, 13.5,
            localdesktoptest::FixtureFilter::Area);
        RequireDecode(scaled, data);
    }
    SECTION("anisotropic scale area filter")
    {
        const auto scaled = localdesktoptest::Resample(original, 0.75, 1.5, 5.5, 8.25,
            localdesktoptest::FixtureFilter::Area);
        RequireDecode(scaled, data);
    }
    SECTION("double-scale bilinear filter")
    {
        const auto scaled = localdesktoptest::Resample(original, 2.0, 2.0, 3.25, 6.75,
            localdesktoptest::FixtureFilter::Bilinear);
        RequireDecode(scaled, data);
    }
}

TEST_CASE("RemoteVisual low-FPS freshness mismatch erases one spatial region across all four planes",
    "[remote-visual][low-fps][freshness][erasure]")
{
    constexpr std::uint64_t sessionTag = 0xF00DBAAD12345678ULL;
    constexpr std::uint64_t sequence = 91;
    std::vector<float> physicalBits(pbmodulation::kRemoteVisualTileCount * pbmodulation::kRemoteVisualLowFpsBitsPerTile, 1.0f);
    std::vector<float> physicalFreshness(pbmodulation::kRemoteVisualTileCount, 0.0f);
    std::vector<float> logical(pbmodulation::kRemoteVisualLowFpsCodedBits, std::numeric_limits<float>::quiet_NaN());
    std::uint16_t staleRegion = pbmodulation::kRemoteVisualFreshnessRegionCount;
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        if (mapping.role == pbmodulation::RemoteVisualTileRole::FreshnessTag)
        {
            bool expectedOne = false;
            REQUIRE(pbmodulation::GetRemoteVisualFreshnessBit(sessionTag, sequence, physical, expectedOne));
            physicalFreshness[physical] = expectedOne ? -1.0f : 1.0f;
            if (staleRegion == pbmodulation::kRemoteVisualFreshnessRegionCount)
            {
                staleRegion = mapping.regionId;
            }
        }
    }
    std::uint32_t expectedErased = 0;
    bool flipped = false;
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        if (mapping.regionId == staleRegion && mapping.role == pbmodulation::RemoteVisualTileRole::FreshnessTag && !flipped)
        {
            physicalFreshness[physical] = -physicalFreshness[physical];
            flipped = true;
        }
        if (mapping.regionId == staleRegion && mapping.role == pbmodulation::RemoteVisualTileRole::Data)
        {
            for (std::uint32_t plane = 0; plane < pbmodulation::kRemoteVisualLowFpsBitsPerTile; plane++)
            {
                expectedErased += static_cast<std::uint32_t>(pbmodulation::GetRemoteVisualLowFpsLogicalBit(
                    mapping.dataOrdinal, plane, sequence) < pbmodulation::kRemoteVisualLowFpsCodedBits);
            }
        }
    }
    REQUIRE(flipped);
    const auto resolution = pbmodulation::ResolveRemoteVisualLowFpsPhysicalMetrics(physicalBits,
        physicalFreshness, sessionTag, sequence, logical);
    REQUIRE(resolution.valid);
    REQUIRE(resolution.staleRegions == 1);
    REQUIRE(resolution.freshnessTagMismatches == 1);
    REQUIRE(resolution.erasedDataMetrics == expectedErased);
    REQUIRE(static_cast<std::size_t>(std::ranges::count(logical, 0.0f)) == expectedErased);
    REQUIRE(static_cast<std::size_t>(std::ranges::count(logical, 1.0f)) + expectedErased == logical.size());
}

TEST_CASE("RemoteVisual low-FPS scaled carrier enters the shared four-codeword QC-LDPC and Transport truth boundary",
    "[remote-visual][low-fps][fec][transport][integration]")
{
    const auto record = MakeLowFpsRecord(117);
    const auto data = MakeLowFpsDiagnosticData(record);
    std::vector<std::byte> bgra(static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight * 4);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(record, data, bgra));
    const auto original = localdesktoptest::GrayFromGolden(bgra);
    const auto scaled = localdesktoptest::Resample(original, 1.259375, 1.2592592592592593, 11.25, 13.5,
        localdesktoptest::FixtureFilter::Area);
    auto created = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(created);
    auto channel = std::move(created.Value());
    const auto observation = channel.DecodeRemoteVisualLowFps(scaled.View());
    INFO(pbmodulation::GetRemoteVisualLowFpsErasureName(observation.modulation.erasure));
    REQUIRE(observation.modulation.IsAccepted());
    REQUIRE(observation.evaluation.evaluated);
    REQUIRE(observation.evaluation.codewords == 4);
    REQUIRE(observation.evaluation.acceptedTransportBlocks == 4);
    REQUIRE(observation.evaluation.IsVerified());
    REQUIRE(observation.evaluation.erroneousCodedBits == 0);
    REQUIRE(channel.GetAcceptedTransportBlocks().size() == 4);
    REQUIRE(channel.GetRemoteVisualLowFpsMarginHistogram().size() == pbmodulation::kRemoteVisualMarginBins);
}

TEST_CASE("RemoteVisual low-FPS freshness erasure recovers a temporally stale codec block through shared QC-LDPC",
    "[remote-visual][low-fps][stale][fec][integration]")
{
    constexpr std::uint64_t sessionTag = 0xDEADBEEF31415926ULL;
    const auto previousRecord = MakeLowFpsRecord(200, sessionTag);
    const auto currentRecord = MakeLowFpsRecord(201, sessionTag);
    const auto previousData = MakeLowFpsDiagnosticData(previousRecord);
    const auto currentData = MakeLowFpsDiagnosticData(currentRecord);
    std::vector<std::byte> previous(static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight * 4);
    std::vector<std::byte> current(previous.size());
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(previousRecord, previousData, previous));
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(currentRecord, currentData, current));
    std::uint16_t staleRegion = pbmodulation::kRemoteVisualFreshnessRegionCount;
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        if (mapping.role == pbmodulation::RemoteVisualTileRole::FreshnessTag)
        {
            bool previousBit = false;
            bool currentBit = false;
            REQUIRE(pbmodulation::GetRemoteVisualFreshnessBit(sessionTag, 200, physical, previousBit));
            REQUIRE(pbmodulation::GetRemoteVisualFreshnessBit(sessionTag, 201, physical, currentBit));
            if (previousBit != currentBit)
            {
                staleRegion = mapping.regionId;
                break;
            }
        }
    }
    REQUIRE(staleRegion < pbmodulation::kRemoteVisualFreshnessRegionCount);
    CopyLowFpsRegion(previous, current, staleRegion);
    const auto image = localdesktoptest::GrayFromGolden(current);
    auto created = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(created);
    auto channel = std::move(created.Value());
    const auto observation = channel.DecodeRemoteVisualLowFps(image.View());
    INFO(pbmodulation::GetRemoteVisualLowFpsErasureName(observation.modulation.erasure));
    INFO(observation.modulation.staleRegions);
    INFO(observation.evaluation.fecFailures);
    REQUIRE(observation.modulation.IsAccepted());
    REQUIRE(observation.modulation.staleRegions == 1);
    REQUIRE(observation.modulation.erasedDataMetrics > 0);
    REQUIRE(observation.evaluation.erroneousCodedBits > 0);
    REQUIRE(observation.evaluation.IsVerified());
    REQUIRE(observation.evaluation.acceptedTransportBlocks == 4);
}

TEST_CASE("RemoteVisual low-FPS geometry and resolver fail closed on invalid bounds and aliases",
    "[remote-visual][low-fps][negative]")
{
    pbmodulation::LocalDesktopGeometry geometry{0, 0, 0.49, 1, 0};
    REQUIRE(pbmodulation::ValidateRemoteVisualLowFpsGeometry(geometry) ==
        pbmodulation::RemoteVisualLowFpsErasure::ScaleOutOfRange);
    geometry.scaleX = 2.01;
    REQUIRE(pbmodulation::ValidateRemoteVisualLowFpsGeometry(geometry) ==
        pbmodulation::RemoteVisualLowFpsErasure::ScaleOutOfRange);
    geometry.scaleX = 1;
    geometry.originX = -0.01;
    REQUIRE(pbmodulation::ValidateRemoteVisualLowFpsGeometry(geometry) ==
        pbmodulation::RemoteVisualLowFpsErasure::InvalidInput);
    pbmodulation::RemoteVisualLowFpsDecodePolicy invalidPolicy;
    invalidPolicy.locator.maximumWorkUnits = 0;
    REQUIRE(pbmodulation::ValidateRemoteVisualLowFpsGeometry({}, invalidPolicy) ==
        pbmodulation::RemoteVisualLowFpsErasure::InvalidPolicy);

    std::vector<float> physical(pbmodulation::kRemoteVisualTileCount * pbmodulation::kRemoteVisualLowFpsBitsPerTile);
    std::span<float> aliased = physical;
    const auto invalid = pbmodulation::ResolveRemoteVisualLowFpsPhysicalMetrics(physical,
        aliased.first(pbmodulation::kRemoteVisualTileCount), 1, 2,
        aliased.first(pbmodulation::kRemoteVisualLowFpsCodedBits));
    REQUIRE_FALSE(invalid.valid);
}

TEST_CASE("RemoteVisual low-FPS decode never reports acceptance when an output buffer is null",
    "[remote-visual][low-fps][negative]")
{
    const auto record = MakeLowFpsRecord(11);
    const auto data = MakeLowFpsData();
    std::vector<std::byte> bgra(static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight * 4);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(record, data, bgra));
    const auto image = localdesktoptest::GrayFromGolden(bgra);

    auto created = pbmodulation::RemoteVisualLowFpsWorkspace::Create(
        pbmodulation::RemoteVisualLowFpsWorkspace::RequiredBytes());
    REQUIRE(created);
    auto workspace = std::move(created.Value());

    // A default span carries a null data pointer, so the caller gave the decoder nowhere to
    // write. IsAccepted() is the only acceptance signal the caller has, therefore every such
    // call must report an erasure instead of leaving the default None.
    std::vector<std::byte> hard(pbmodulation::kRemoteVisualLowFpsDataBytes);
    std::vector<float> soft(pbmodulation::kRemoteVisualLowFpsCodedBits);
    const std::span<const std::byte> constantHard;
    const std::span<const float> constantSoft;
    const auto nullHard = std::span<std::byte>(const_cast<std::byte*>(constantHard.data()), std::size_t{0});
    const auto nullSoft = std::span<float>(const_cast<float*>(constantSoft.data()), std::size_t{0});

    const auto missingHard = pbmodulation::DecodeRemoteVisualLowFpsFrame(image.View(), workspace, nullHard, soft);
    REQUIRE_FALSE(missingHard.IsAccepted());
    REQUIRE(missingHard.dataBytes == 0);
    const auto missingSoft = pbmodulation::DecodeRemoteVisualLowFpsFrame(image.View(), workspace, hard, nullSoft);
    REQUIRE_FALSE(missingSoft.IsAccepted());
    REQUIRE(missingSoft.dataBytes == 0);

    // Non-null but undersized buffers must keep failing closed with the named reason.
    std::vector<std::byte> shortHard(pbmodulation::kRemoteVisualLowFpsDataBytes - 1);
    std::vector<float> shortSoft(pbmodulation::kRemoteVisualLowFpsCodedBits - 1);
    REQUIRE(pbmodulation::DecodeRemoteVisualLowFpsFrame(image.View(), workspace, shortHard, soft).erasure ==
        pbmodulation::RemoteVisualLowFpsErasure::OutputBufferTooSmall);
    REQUIRE(pbmodulation::DecodeRemoteVisualLowFpsFrame(image.View(), workspace, hard, shortSoft).erasure ==
        pbmodulation::RemoteVisualLowFpsErasure::OutputBufferTooSmall);

    // A correctly sized call on the same workspace still succeeds, so the guard above is not
    // a permanent poisoning of the workspace.
    const auto accepted = pbmodulation::DecodeRemoteVisualLowFpsFrame(image.View(), workspace, hard, soft);
    REQUIRE(accepted.IsAccepted());
    REQUIRE(accepted.dataBytes == pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(std::ranges::equal(hard, data));
}
