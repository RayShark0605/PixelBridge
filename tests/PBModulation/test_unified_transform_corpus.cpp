#include "pbmodulation/unified_visual.h"
#include "pbmodulation/visual_temporal.h"

#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/protocol_version.h"
#include "pbprotocol/transport_block_codec.h"
#include "pbremotevisualsimulator/channel_transform.h"

#include "unified_transform_corpus_cases.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace pbmodulation;
using namespace pbremotevisualsimulator;

#ifndef PB_UNIFIED_TRANSFORM_MANIFEST
#error PB_UNIFIED_TRANSFORM_MANIFEST must name the checked-in Unified transform corpus manifest
#endif

#ifndef PB_UNIFIED_TRANSFORM_REPORT
#error PB_UNIFIED_TRANSFORM_REPORT must name the generated Unified transform corpus report
#endif

using unifiedtransformtest::kCorpusCaptureEpoch;
using unifiedtransformtest::kCorpusSessionTag;
constexpr std::size_t kLaneCount = 3;
constexpr std::size_t kSlotRejectionCount = static_cast<std::size_t>(UnifiedSlotRejection::IdentityFailure) + 1;
constexpr std::size_t kCorpusPayloadBytes = kUnifiedInformationBytes - pbprotocol::kTransportMinimumBlockBytes;
constexpr std::uint64_t kMinimumBasePayloadBytes = 16ULL * 1024;

static_assert(kCorpusPayloadBytes == 1314);
static_assert(GetUnifiedLaneCapacity(UnifiedLane::BaseLuma).transportPayloadBytes == 22338);
static_assert(static_cast<std::size_t>(UnifiedLane::BaseLuma) == 0);
static_assert(static_cast<std::size_t>(UnifiedLane::FineLuma) == 1);
static_assert(static_cast<std::size_t>(UnifiedLane::Chroma) == 2);

struct FrameFixture
{
    std::uint64_t sequence = 0;
    std::array<std::byte, kLocalDesktopBootstrapRecordBytes> bootstrap{};
    std::array<std::byte, kUnifiedCodedFrameBytes> coded{};
    std::array<std::array<std::byte, kUnifiedInformationBytes>, kUnifiedCodewordCount> transportBlocks{};
    std::vector<std::byte> pixels;
};

struct CorpusRecord
{
    std::string name;
    std::uint64_t sequence = 0;
    std::string channelManifest;
    std::string manifestBlake3;
    std::string outputBlake3;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    bool inputValid = false;
    UnifiedErasureReason frameErasure = UnifiedErasureReason::CanvasClipped;
    LocalDesktopErasureReason bootstrapErasure = LocalDesktopErasureReason::InvalidView;
    std::array<std::string, 5> geometryBinary64{};
    std::array<UnifiedErasureReason, kLaneCount> laneErasure{};
    std::array<std::uint64_t, kLaneCount> acceptedWireBytes{};
    std::array<std::uint64_t, kLaneCount> acceptedPayloadBytes{};
    std::array<std::uint32_t, kLaneCount> erasedMetrics{};
    std::array<std::uint32_t, kLaneCount> hardBitErrors{};
    std::array<std::uint32_t, kLaneCount> fecValidSlots{};
    std::array<std::uint32_t, kLaneCount> crcValidSlots{};
    std::array<std::uint32_t, kLaneCount> acceptedSlots{};
    std::array<std::uint32_t, kSlotRejectionCount> slotRejectionCounts{};
    std::uint32_t acceptedBlocks = 0;
    std::uint32_t internalFalseAcceptedBlocks = 0;
    std::uint32_t truthMismatchedAcceptedBlocks = 0;
    std::uint32_t freshnessCurrentRegions = 0;
    bool conflictExpected = false;
    std::uint32_t conflictOutputBlocks = 0;
    std::string temporalDisposition;
};

std::array<std::byte, kLocalDesktopBootstrapRecordBytes> MakeBootstrap(const std::uint64_t sequence)
{
    std::array<std::byte, kLocalDesktopBootstrapRecordBytes> bytes{};
    const pbprotocol::BootstrapRecord record{pbprotocol::kBootstrapVersion, pbprotocol::GetProtocolVersion(),
        kUnifiedVisualProfile.productProfile.visualLayoutVersion,
        kUnifiedVisualProfile.productProfile.visualProfileId, kCorpusSessionTag, sequence,
        0x47100000U | static_cast<std::uint32_t>(sequence & 0xFFFFU), 0};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
    return bytes;
}

std::array<std::byte, kUnifiedInformationBytes> MakeTransportBlock(const std::uint64_t sequence,
    const std::uint32_t slot)
{
    std::array<std::byte, kCorpusPayloadBytes> payload{};
    for (std::size_t index = 0; index < payload.size(); index++)
    {
        payload[index] = static_cast<std::byte>((sequence * 29 + slot * 71 + index * 17 + index / 7) & 0xFFU);
    }
    const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData,
        pbprotocol::kTransportProtocolMinor, 0, kCorpusSessionTag, 10000 + sequence,
        static_cast<std::uint32_t>(sequence * kUnifiedCodewordCount + slot),
        static_cast<std::uint16_t>(payload.size())};
    std::array<std::byte, kUnifiedInformationBytes> block{};
    REQUIRE(pbprotocol::GetTransportSerializedSize(header) == block.size());
    REQUIRE(pbprotocol::SerializeTransportBlock(header, payload, block));
    return block;
}

FrameFixture BuildFrame(const std::uint64_t sequence)
{
    FrameFixture fixture;
    fixture.sequence = sequence;
    fixture.bootstrap = MakeBootstrap(sequence);
    for (std::uint32_t slot = 0; slot < kUnifiedCodewordCount; slot++)
    {
        fixture.transportBlocks[slot] = MakeTransportBlock(sequence, slot);
        REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust,
            fixture.transportBlocks[slot], std::span<std::byte>(fixture.coded).subspan(
                static_cast<std::size_t>(slot) * kUnifiedCodewordBytes, kUnifiedCodewordBytes)));
    }
    fixture.pixels.resize(kUnifiedFrameBgraBytes);
    REQUIRE(EncodeUnifiedVisualFrame(fixture.bootstrap, fixture.coded, fixture.pixels));
    return fixture;
}

BgraImageView MakeBgraView(const FrameFixture& fixture)
{
    return BgraImageView{fixture.pixels, kUnifiedVisualProfile.canvasWidth, kUnifiedVisualProfile.canvasHeight,
        static_cast<std::size_t>(kUnifiedVisualProfile.canvasWidth) * 4};
}

LumaView MakeLumaView(const BgraImage& image)
{
    return LumaView{image.pixels, image.width, image.height, image.rowPitch, LumaPixelFormat::Bgra8};
}

std::size_t LaneIndex(const UnifiedLane lane) noexcept
{
    return static_cast<std::size_t>(lane);
}

std::string HexBinary64(const double value)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    std::string output(16, '0');
    for (std::size_t index = 0; index < output.size(); index++)
    {
        const std::size_t shift = (output.size() - index - 1) * 4;
        output[index] = kHexDigits[(bits >> shift) & 0xFU];
    }
    return output;
}

const char* GetUnifiedErasureName(const UnifiedErasureReason reason) noexcept
{
    switch (reason)
    {
    case UnifiedErasureReason::None: return "none";
    case UnifiedErasureReason::LocatorFailure: return "locator-failure";
    case UnifiedErasureReason::BootstrapFailure: return "bootstrap-failure";
    case UnifiedErasureReason::CanvasClipped: return "canvas-clipped";
    case UnifiedErasureReason::IdentityConflict: return "identity-conflict";
    case UnifiedErasureReason::BaseLumaPilotFailure: return "base-luma-pilot-failure";
    case UnifiedErasureReason::FineLumaPilotFailure: return "fine-luma-pilot-failure";
    case UnifiedErasureReason::ChromaPilotFailure: return "chroma-pilot-failure";
    case UnifiedErasureReason::LocalStaleRegion: return "local-stale-region";
    case UnifiedErasureReason::LocalLowDecisionMargin: return "local-low-decision-margin";
    case UnifiedErasureReason::LocalSamplingFailure: return "local-sampling-failure";
    }
    return "unknown";
}

const char* GetTemporalDispositionName(const VisualIdentityDisposition disposition) noexcept
{
    switch (disposition)
    {
    case VisualIdentityDisposition::Invalid: return "invalid";
    case VisualIdentityDisposition::Unique: return "unique";
    case VisualIdentityDisposition::Duplicate: return "duplicate";
    case VisualIdentityDisposition::Reordered: return "reordered";
    }
    return "unknown";
}

CorpusRecord RunCase(UnifiedVisualCpuOracle& oracle, const std::string& name, const FrameFixture& source,
    const std::span<const ChannelTransform> transforms, const FrameFixture* const reference = nullptr,
    const UnifiedExpectedFrameIdentity& expectedIdentity = {}, const bool conflictExpected = false,
    const std::optional<VisualIdentityDisposition> temporalDisposition = std::nullopt)
{
    const std::optional<BgraImageView> referenceView = reference == nullptr ? std::nullopt :
        std::optional<BgraImageView>{MakeBgraView(*reference)};
    const auto executionResult = ExecuteChannelTransformPlan(MakeBgraView(source), referenceView,
        {unifiedtransformtest::MakeCorpusSeed(name, source.sequence), transforms});
    INFO(name);
    INFO(GetChannelTransformErrorName(executionResult.Error().code));
    REQUIRE(executionResult);
    const ChannelTransformExecution& execution = executionResult.Value();
    const LumaView transformedView = MakeLumaView(execution.output);
    const LocalDesktopBootstrapBinding binding{kUnifiedVisualProfile.productProfile.visualProfileId,
        kUnifiedVisualProfile.productProfile.visualLayoutVersion};
    const LocalDesktopObservation publicLocator = DecodeLocalDesktopBootstrap(transformedView, binding);
    const UnifiedVisualObservation observation = oracle.DecodeMixedFrame(transformedView, expectedIdentity);
    REQUIRE(publicLocator.erasure == observation.bootstrap.erasure);
    if (publicLocator.IsAccepted())
    {
        REQUIRE(publicLocator.canonical44 == observation.bootstrap.canonical44);
    }

    CorpusRecord record;
    record.name = name;
    record.sequence = source.sequence;
    record.channelManifest = execution.canonicalManifestJson;
    record.manifestBlake3 = ChannelDigestToHex(execution.manifestBlake3);
    record.outputBlake3 = ChannelDigestToHex(execution.outputBlake3);
    record.outputWidth = execution.output.width;
    record.outputHeight = execution.output.height;
    record.inputValid = observation.inputValid;
    record.frameErasure = observation.frameErasure;
    record.bootstrapErasure = observation.bootstrap.erasure;
    record.geometryBinary64 = {HexBinary64(observation.bootstrap.geometry.originX),
        HexBinary64(observation.bootstrap.geometry.originY), HexBinary64(observation.bootstrap.geometry.scaleX),
        HexBinary64(observation.bootstrap.geometry.scaleY),
        HexBinary64(observation.bootstrap.geometry.markerResidualPixels)};
    record.laneErasure = {observation.baseLuma.erasureReason, observation.fineLuma.erasureReason,
        observation.chroma.erasureReason};
    record.acceptedBlocks = observation.acceptedBlocks;
    record.internalFalseAcceptedBlocks = observation.falseAcceptedBlocks;
    record.freshnessCurrentRegions = static_cast<std::uint32_t>(std::ranges::count_if(observation.freshness,
        [](const UnifiedFreshnessObservation& freshness)
        {
            return freshness.current;
        }));
    record.conflictExpected = conflictExpected;
    record.conflictOutputBlocks = conflictExpected ? observation.acceptedBlocks : 0;
    record.temporalDisposition = temporalDisposition ? GetTemporalDispositionName(*temporalDisposition) : "not-observed";

    const std::span<const UnifiedSoftMetric> metrics = oracle.GetSoftMetrics();
    REQUIRE(metrics.size() == kUnifiedSoftMetricCount);
    for (std::size_t bit = 0; bit < metrics.size(); bit++)
    {
        const UnifiedSoftMetric metric = metrics[bit];
        const std::size_t lane = LaneIndex(metric.lane);
        REQUIRE(lane < kLaneCount);
        if (metric.value == 0)
        {
            record.erasedMetrics[lane]++;
            continue;
        }
        const bool expectedBit = ((std::to_integer<std::uint8_t>(source.coded[bit / 8]) >> (bit % 8)) & 1U) != 0;
        record.hardBitErrors[lane] += static_cast<std::uint32_t>((metric.value < 0) != expectedBit);
    }

    for (const UnifiedSlotObservation& slot : observation.slots)
    {
        const std::size_t lane = LaneIndex(slot.lane);
        const std::size_t rejection = static_cast<std::size_t>(slot.rejection);
        REQUIRE(lane < kLaneCount);
        REQUIRE(rejection < record.slotRejectionCounts.size());
        record.slotRejectionCounts[rejection]++;
        record.fecValidSlots[lane] += static_cast<std::uint32_t>(slot.fecValid);
        record.crcValidSlots[lane] += static_cast<std::uint32_t>(slot.crcValid);
        record.acceptedSlots[lane] += static_cast<std::uint32_t>(slot.accepted);
    }

    std::array<bool, kUnifiedCodewordCount> acceptedSlots{};
    for (const UnifiedAcceptedBlock& block : oracle.GetAcceptedBlocks())
    {
        if (block.codewordSlot >= kUnifiedCodewordCount || block.kind != UnifiedSlotKind::Transport ||
            block.size > block.bytes.size() || acceptedSlots[block.codewordSlot])
        {
            record.truthMismatchedAcceptedBlocks++;
            continue;
        }
        acceptedSlots[block.codewordSlot] = true;
        const std::span<const std::byte> acceptedBytes(block.bytes.data(), block.size);
        const UnifiedLaneContract* const laneContract = FindUnifiedLaneForCodewordSlot(block.codewordSlot);
        REQUIRE(laneContract != nullptr);
        const std::size_t lane = LaneIndex(laneContract->lane);
        record.acceptedWireBytes[lane] += block.size;
        const auto parsed = pbprotocol::ParseTransportBlock(acceptedBytes);
        if (!parsed)
        {
            record.truthMismatchedAcceptedBlocks++;
            continue;
        }
        record.acceptedPayloadBytes[lane] += parsed.Value().payload.size();
        const auto& expected = source.transportBlocks[block.codewordSlot];
        if (acceptedBytes.size() != expected.size() || !std::ranges::equal(acceptedBytes, expected))
        {
            record.truthMismatchedAcceptedBlocks++;
        }
    }

    CAPTURE(record.name, record.frameErasure, record.bootstrapErasure, record.laneErasure,
        record.acceptedBlocks, record.acceptedPayloadBytes, record.erasedMetrics, record.hardBitErrors,
        record.fecValidSlots, record.crcValidSlots, record.internalFalseAcceptedBlocks,
        record.truthMismatchedAcceptedBlocks, record.freshnessCurrentRegions);
    return record;
}

void RequireNoFalseAcceptance(const CorpusRecord& record)
{
    CAPTURE(record.name);
    REQUIRE(record.internalFalseAcceptedBlocks == 0);
    REQUIRE(record.truthMismatchedAcceptedBlocks == 0);
}

void RequireFrameAvailable(const CorpusRecord& record)
{
    CAPTURE(record.name, record.frameErasure, record.bootstrapErasure);
    REQUIRE(record.inputValid);
    REQUIRE(record.frameErasure == UnifiedErasureReason::None);
}

void RequireFullRecovery(const CorpusRecord& record)
{
    CAPTURE(record.name, record.frameErasure, record.bootstrapErasure, record.laneErasure,
        record.acceptedBlocks, record.acceptedPayloadBytes, record.erasedMetrics, record.hardBitErrors,
        record.fecValidSlots, record.crcValidSlots, record.acceptedSlots);
    RequireNoFalseAcceptance(record);
    RequireFrameAvailable(record);
    REQUIRE(record.acceptedBlocks == kUnifiedCodewordCount);
    REQUIRE(record.acceptedSlots == std::array<std::uint32_t, kLaneCount>{17, 4, 10});
    REQUIRE(record.fecValidSlots == record.acceptedSlots);
    REQUIRE(record.crcValidSlots == record.acceptedSlots);
    REQUIRE(record.acceptedPayloadBytes == std::array<std::uint64_t, kLaneCount>{22338, 5256, 13140});
}

void RequireBaseCapacity(const CorpusRecord& record)
{
    RequireNoFalseAcceptance(record);
    RequireFrameAvailable(record);
    CAPTURE(record.name, record.acceptedPayloadBytes);
    REQUIRE(record.acceptedPayloadBytes[LaneIndex(UnifiedLane::BaseLuma)] >= kMinimumBasePayloadBytes);
}

void RequireNoOutput(const CorpusRecord& record)
{
    RequireNoFalseAcceptance(record);
    CAPTURE(record.name, record.acceptedBlocks, record.acceptedWireBytes, record.conflictOutputBlocks);
    REQUIRE(record.acceptedBlocks == 0);
    REQUIRE(record.acceptedWireBytes == std::array<std::uint64_t, kLaneCount>{});
    REQUIRE(record.conflictOutputBlocks == 0);
}

void AppendJsonString(std::string& output, const std::string_view value)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character : value)
    {
        if (character == '"' || character == '\\')
        {
            output.push_back('\\');
            output.push_back(static_cast<char>(character));
        }
        else if (character < 0x20)
        {
            output.append("\\u00");
            output.push_back(kHexDigits[character >> 4]);
            output.push_back(kHexDigits[character & 0x0F]);
        }
        else
        {
            output.push_back(static_cast<char>(character));
        }
    }
    output.push_back('"');
}

template <typename ValueType, std::size_t Size>
void AppendNumberArray(std::string& output, const std::array<ValueType, Size>& values)
{
    output.push_back('[');
    for (std::size_t index = 0; index < values.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        output.append(std::to_string(static_cast<std::uint64_t>(values[index])));
    }
    output.push_back(']');
}

template <std::size_t Size>
void AppendStringArray(std::string& output, const std::array<std::string, Size>& values)
{
    output.push_back('[');
    for (std::size_t index = 0; index < values.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        AppendJsonString(output, values[index]);
    }
    output.push_back(']');
}

std::string BuildReport(const std::span<const CorpusRecord> records, const VisualIdentitySnapshot& temporalSnapshot)
{
    std::uint64_t internalFalseAcceptedBlocks = 0;
    std::uint64_t truthMismatchedAcceptedBlocks = 0;
    std::uint64_t conflictOutputBlocks = 0;
    std::uint64_t neutralChromaBasePayloadBytes = 0;
    for (const CorpusRecord& record : records)
    {
        internalFalseAcceptedBlocks += record.internalFalseAcceptedBlocks;
        truthMismatchedAcceptedBlocks += record.truthMismatchedAcceptedBlocks;
        conflictOutputBlocks += record.conflictOutputBlocks;
        if (record.name == "neutral-chroma-scale-075")
        {
            neutralChromaBasePayloadBytes = record.acceptedPayloadBytes[LaneIndex(UnifiedLane::BaseLuma)];
        }
    }

    std::string output;
    output.reserve(65536);
    output.append("{\"schema\":\"PixelBridge.UnifiedTransformCorpus.1\",\"scope\":");
    AppendJsonString(output, "provider-generic-synthetic-cpu-oracle");
    output.append(",\"sessionTagHex\":\"4754313053455353\",\"recordCount\":");
    output.append(std::to_string(records.size()));
    output.append(",\"slotRejectionOrder\":[\"none\",\"frame-erasure\",\"lane-erasure\",\"inner-fec-failure\","
        "\"invalid-information\",\"non-canonical-padding\",\"transport-crc-failure\",\"control-crc-failure\","
        "\"identity-failure\"],\"aggregate\":{\"internalFalseAcceptedBlocks\":");
    output.append(std::to_string(internalFalseAcceptedBlocks));
    output.append(",\"truthMismatchedAcceptedBlocks\":");
    output.append(std::to_string(truthMismatchedAcceptedBlocks));
    output.append(",\"conflictOutputBlocks\":");
    output.append(std::to_string(conflictOutputBlocks));
    output.append(",\"falseAcceptedBlocks\":");
    output.append(std::to_string(internalFalseAcceptedBlocks + truthMismatchedAcceptedBlocks));
    output.append(",\"minimumBasePayloadBytes\":");
    output.append(std::to_string(kMinimumBasePayloadBytes));
    output.append(",\"neutralChromaBasePayloadBytes\":");
    output.append(std::to_string(neutralChromaBasePayloadBytes));
    output.append("},\"temporal\":{\"uniqueFrames\":");
    output.append(std::to_string(temporalSnapshot.uniqueFrames));
    output.append(",\"duplicateFrames\":");
    output.append(std::to_string(temporalSnapshot.duplicateFrames));
    output.append(",\"reorderedFrames\":");
    output.append(std::to_string(temporalSnapshot.reorderedFrames));
    output.append(",\"gapEvents\":");
    output.append(std::to_string(temporalSnapshot.gapEvents));
    output.append(",\"skippedSequences\":");
    output.append(std::to_string(temporalSnapshot.skippedSequences));
    output.append("},\"records\":[");
    for (std::size_t recordIndex = 0; recordIndex < records.size(); recordIndex++)
    {
        if (recordIndex != 0)
        {
            output.push_back(',');
        }
        const CorpusRecord& record = records[recordIndex];
        output.append("{\"name\":");
        AppendJsonString(output, record.name);
        output.append(",\"sequence\":");
        output.append(std::to_string(record.sequence));
        output.append(",\"channelManifestBlake3\":");
        AppendJsonString(output, record.manifestBlake3);
        output.append(",\"outputBlake3\":");
        AppendJsonString(output, record.outputBlake3);
        output.append(",\"outputSize\":[");
        output.append(std::to_string(record.outputWidth));
        output.push_back(',');
        output.append(std::to_string(record.outputHeight));
        output.append("],\"inputValid\":");
        output.append(record.inputValid ? "true" : "false");
        output.append(",\"frameErasure\":");
        AppendJsonString(output, GetUnifiedErasureName(record.frameErasure));
        output.append(",\"bootstrapErasure\":");
        AppendJsonString(output, GetLocalDesktopErasureName(record.bootstrapErasure));
        output.append(",\"geometryBinary64\":");
        AppendStringArray(output, record.geometryBinary64);
        output.append(",\"laneErasure\":[");
        for (std::size_t lane = 0; lane < record.laneErasure.size(); lane++)
        {
            if (lane != 0)
            {
                output.push_back(',');
            }
            AppendJsonString(output, GetUnifiedErasureName(record.laneErasure[lane]));
        }
        output.append("],\"acceptedWireBytesByLane\":");
        AppendNumberArray(output, record.acceptedWireBytes);
        output.append(",\"acceptedPayloadBytesByLane\":");
        AppendNumberArray(output, record.acceptedPayloadBytes);
        output.append(",\"erasedMetricsByLane\":");
        AppendNumberArray(output, record.erasedMetrics);
        output.append(",\"hardBitErrorsByLane\":");
        AppendNumberArray(output, record.hardBitErrors);
        output.append(",\"fecValidSlotsByLane\":");
        AppendNumberArray(output, record.fecValidSlots);
        output.append(",\"crcValidSlotsByLane\":");
        AppendNumberArray(output, record.crcValidSlots);
        output.append(",\"acceptedSlotsByLane\":");
        AppendNumberArray(output, record.acceptedSlots);
        output.append(",\"slotRejectionCounts\":");
        AppendNumberArray(output, record.slotRejectionCounts);
        output.append(",\"acceptedBlocks\":");
        output.append(std::to_string(record.acceptedBlocks));
        output.append(",\"falseAcceptance\":{\"internal\":");
        output.append(std::to_string(record.internalFalseAcceptedBlocks));
        output.append(",\"truthMismatch\":");
        output.append(std::to_string(record.truthMismatchedAcceptedBlocks));
        output.append("},\"conflict\":{\"expected\":");
        output.append(record.conflictExpected ? "true" : "false");
        output.append(",\"outputBlocks\":");
        output.append(std::to_string(record.conflictOutputBlocks));
        output.append("},\"freshnessCurrentRegions\":");
        output.append(std::to_string(record.freshnessCurrentRegions));
        output.append(",\"temporalDisposition\":");
        AppendJsonString(output, record.temporalDisposition);
        output.append(",\"channelManifest\":");
        output.append(record.channelManifest);
        output.push_back('}');
    }
    output.append("]}\n");
    return output;
}

void WriteFile(const std::filesystem::path& path, const std::string_view contents)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    REQUIRE(stream);
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("Unified provider-generic transform corpus closes CPU admission and capacity gates",
    "[unified][transform][corpus][golden]")
{
    auto createdOracle = UnifiedVisualCpuOracle::Create(UnifiedVisualCpuOracle::RequiredBytes());
    REQUIRE(createdOracle);
    UnifiedVisualCpuOracle oracle = std::move(createdOracle).Value();
    const FrameFixture frame40 = BuildFrame(40);
    const FrameFixture frame41 = BuildFrame(41);
    const std::vector<unifiedtransformtest::MandatoryTransformCase> transformCases =
        unifiedtransformtest::MakeMandatoryTransformCases();
    std::vector<CorpusRecord> records;
    records.reserve(transformCases.size());
    VisualIdentityTracker temporalTracker;
    std::size_t temporalObservationIndex = 0;
    for (const unifiedtransformtest::MandatoryTransformCase& transformCase : transformCases)
    {
        std::optional<FrameFixture> dynamicFrame;
        const FrameFixture* source = nullptr;
        if (transformCase.sequence == frame40.sequence)
        {
            source = &frame40;
        }
        else if (transformCase.sequence == frame41.sequence)
        {
            source = &frame41;
        }
        else
        {
            dynamicFrame.emplace(BuildFrame(transformCase.sequence));
            source = &*dynamicFrame;
        }
        const FrameFixture* reference = transformCase.referenceSequence ? &frame40 : nullptr;
        const bool invalidReference = transformCase.referenceSequence.has_value() &&
            *transformCase.referenceSequence != frame40.sequence;
        REQUIRE_FALSE(invalidReference);

        std::optional<VisualIdentityDisposition> temporalDisposition;
        if (transformCase.expectedTemporalDisposition)
        {
            temporalDisposition = temporalTracker.Observe(source->sequence, kCorpusCaptureEpoch,
                static_cast<std::int64_t>(temporalObservationIndex + 1) * 10000000, kCorpusSessionTag.value);
            REQUIRE(temporalDisposition == transformCase.expectedTemporalDisposition);
            temporalObservationIndex++;
        }
        CorpusRecord record = RunCase(oracle, std::string(transformCase.name), *source, transformCase.transforms,
            reference, transformCase.expectedIdentity, transformCase.conflictExpected, temporalDisposition);
        switch (transformCase.expectation)
        {
        case unifiedtransformtest::MandatoryTransformExpectation::FullRecovery:
            RequireFullRecovery(record);
            break;
        case unifiedtransformtest::MandatoryTransformExpectation::BlurQuantized:
            RequireNoFalseAcceptance(record);
            RequireFrameAvailable(record);
            REQUIRE(record.acceptedSlots == std::array<std::uint32_t, kLaneCount>{0, 0, 10});
            REQUIRE(record.laneErasure[LaneIndex(UnifiedLane::BaseLuma)] ==
                UnifiedErasureReason::BaseLumaPilotFailure);
            REQUIRE(record.laneErasure[LaneIndex(UnifiedLane::FineLuma)] ==
                UnifiedErasureReason::FineLumaPilotFailure);
            break;
        case unifiedtransformtest::MandatoryTransformExpectation::BaseCapacity:
            RequireBaseCapacity(record);
            break;
        case unifiedtransformtest::MandatoryTransformExpectation::NeutralChromaBaseCapacity:
            RequireBaseCapacity(record);
            REQUIRE(record.laneErasure[LaneIndex(UnifiedLane::Chroma)] ==
                UnifiedErasureReason::ChromaPilotFailure);
            break;
        case unifiedtransformtest::MandatoryTransformExpectation::LocalizedStale:
            RequireNoFalseAcceptance(record);
            RequireFrameAvailable(record);
            REQUIRE(record.acceptedBlocks > 0);
            REQUIRE(record.acceptedBlocks < kUnifiedCodewordCount);
            REQUIRE(record.freshnessCurrentRegions < kUnifiedFreshnessRegionCount);
            break;
        case unifiedtransformtest::MandatoryTransformExpectation::CropNoOutput:
            RequireNoOutput(record);
            break;
        case unifiedtransformtest::MandatoryTransformExpectation::BootstrapConflictNoOutput:
            RequireNoOutput(record);
            REQUIRE((record.frameErasure == UnifiedErasureReason::BootstrapFailure ||
                record.frameErasure == UnifiedErasureReason::IdentityConflict));
            break;
        case unifiedtransformtest::MandatoryTransformExpectation::CallerIdentityNoOutput:
            RequireNoOutput(record);
            REQUIRE(record.frameErasure == UnifiedErasureReason::IdentityConflict);
            break;
        }
        records.push_back(std::move(record));
    }

    REQUIRE(records.size() == unifiedtransformtest::kMandatoryTransformCaseCount);
    const VisualIdentitySnapshot temporalSnapshot = temporalTracker.GetSnapshot();
    REQUIRE(temporalSnapshot.uniqueFrames == 3);
    REQUIRE(temporalSnapshot.duplicateFrames == 1);
    REQUIRE(temporalSnapshot.reorderedFrames == 1);
    REQUIRE(temporalSnapshot.gapEvents == 2);
    REQUIRE(temporalSnapshot.skippedSequences == 2);
    REQUIRE_FALSE(temporalSnapshot.framesPerSecond.has_value());

    std::uint64_t falseAcceptedBlocks = 0;
    std::uint64_t conflictOutputBlocks = 0;
    for (const CorpusRecord& record : records)
    {
        falseAcceptedBlocks += record.internalFalseAcceptedBlocks + record.truthMismatchedAcceptedBlocks;
        conflictOutputBlocks += record.conflictOutputBlocks;
    }
    REQUIRE(falseAcceptedBlocks == 0);
    REQUIRE(conflictOutputBlocks == 0);

    const std::string report = BuildReport(records, temporalSnapshot);
    WriteFile(PB_UNIFIED_TRANSFORM_REPORT, report);
    const std::string expectedManifest = ReadFile(PB_UNIFIED_TRANSFORM_MANIFEST);
    REQUIRE(report == expectedManifest);
}
