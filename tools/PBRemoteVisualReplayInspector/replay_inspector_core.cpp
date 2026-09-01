#include "replay_inspector_core.h"

#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/remote_visual.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbmodulation/shape_chroma.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbremotevisualreplayinspector
{
namespace
{

struct ProfileBinding
{
    std::uint64_t profileId = 0;
    std::uint8_t layoutVersion = 0;
    const char* name = nullptr;
};

struct MarginEvidence
{
    std::uint64_t samples = 0;
    double minimum = 0;
    double p50 = 0;
    double p01 = 0;
    double p001 = 0;
};

struct AcceptedBlockEvidence
{
    std::uint32_t slot = 0;
    std::uint32_t byteCount = 0;
    std::string blake3;
};

struct FrameEvidence
{
    std::uint32_t ordinal = 0;
    std::uint64_t captureEpoch = 0;
    std::uint64_t captureObservation = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowPitch = 0;
    std::uint32_t pixelFormat = 0;
    std::string capturedRoiBlake3;
    bool bootstrapAccepted = false;
    std::string bootstrapErasure;
    std::string canonicalBootstrapBlake3;
    std::uint64_t sessionTag = 0;
    std::uint64_t frameSequence = 0;
    bool geometryAvailable = false;
    pbmodulation::LocalDesktopGeometry geometry;
    bool modulationAccepted = false;
    std::string modulationErasure;
    std::uint32_t dataBytes = 0;
    std::uint32_t unreliablePrimary = 0;
    std::optional<std::uint32_t> unreliableSecondary;
    std::optional<std::uint32_t> freshnessRegions;
    std::optional<std::uint32_t> staleRegions;
    std::optional<std::uint32_t> freshnessTagMismatches;
    std::optional<std::uint32_t> freshnessTagErasures;
    std::optional<std::uint32_t> erasedDataMetrics;
    std::uint64_t dataWorkUnits = 0;
    std::string pixelError;
    MarginEvidence primaryMargin;
    std::optional<MarginEvidence> secondaryMargin;
    pbdesktoplevels::FrameEvaluation evaluation;
    std::vector<AcceptedBlockEvidence> acceptedBlocks;
    std::optional<pbrealcapturereplay::ReplayV2DemodObservationView> recordedObservation;
};

[[nodiscard]] std::optional<ProfileBinding> ResolveProfile(const std::uint64_t profileId) noexcept
{
    if (profileId == pbmodulation::kDesktopLevels2ProfileId)
    {
        return ProfileBinding{profileId, pbmodulation::kDesktopLevelsLayoutVersion,
            "PB-Mod-DesktopLevels-2x2"};
    }
    if (profileId == pbmodulation::kShapeChromaProfileId)
    {
        return ProfileBinding{profileId, pbmodulation::kShapeChromaLayoutVersion,
            "PB-Mod-ShapeChroma-1"};
    }
    if (profileId == pbmodulation::kRemoteVisualProfileId)
    {
        return ProfileBinding{profileId, pbmodulation::kRemoteVisualLayoutVersion,
            "PB-RemoteVisual-Resilient-1"};
    }
    if (profileId == pbmodulation::kRemoteVisualLowFpsProfileId)
    {
        return ProfileBinding{profileId, pbmodulation::kRemoteVisualLowFpsLayoutVersion,
            "PB-RemoteVisual-LF4-X1"};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<pbmodulation::LumaPixelFormat> ResolvePixelFormat(
    const DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8_UNORM: return pbmodulation::LumaPixelFormat::Gray8;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return pbmodulation::LumaPixelFormat::Bgra8;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return pbmodulation::LumaPixelFormat::R10G10B10A2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return pbmodulation::LumaPixelFormat::Fp16LinearSdr;
    default: return std::nullopt;
    }
}

void AppendUnsigned(std::string& output, const std::uint64_t value)
{
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{})
    {
        throw std::length_error("unsigned integer JSON serialization failed");
    }
    output.append(buffer.data(), converted.ptr);
}

void AppendSigned(std::string& output, const std::int64_t value)
{
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{})
    {
        throw std::length_error("signed integer JSON serialization failed");
    }
    output.append(buffer.data(), converted.ptr);
}

void AppendDouble(std::string& output, const double value)
{
    if (!std::isfinite(value))
    {
        throw std::runtime_error("non-finite decoder result cannot enter canonical JSON");
    }
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (converted.ec != std::errc{})
    {
        throw std::length_error("floating-point JSON serialization failed");
    }
    output.append(buffer.data(), converted.ptr);
}

void AppendBoolean(std::string& output, const bool value)
{
    output.append(value ? "true" : "false");
}

void AppendJsonString(std::string& output, const std::string_view value)
{
    static constexpr char hexDigits[] = "0123456789abcdef";
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
            output.push_back(hexDigits[character >> 4]);
            output.push_back(hexDigits[character & 0x0F]);
        }
        else
        {
            output.push_back(static_cast<char>(character));
        }
    }
    output.push_back('"');
}

void AppendUint64HexString(std::string& output, const std::uint64_t value)
{
    static constexpr char hexDigits[] = "0123456789abcdef";
    output.push_back('"');
    for (std::size_t nibble = 0; nibble < 16; nibble++)
    {
        const std::size_t shift = (15 - nibble) * 4;
        output.push_back(hexDigits[(value >> shift) & 0x0FU]);
    }
    output.push_back('"');
}

[[nodiscard]] std::string BytesToHex(const std::span<const std::byte> bytes)
{
    static constexpr char hexDigits[] = "0123456789abcdef";
    std::string output(bytes.size() * 2, '\0');
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        const unsigned int value = std::to_integer<unsigned int>(bytes[index]);
        output[index * 2] = hexDigits[value >> 4];
        output[index * 2 + 1] = hexDigits[value & 0x0FU];
    }
    return output;
}

[[nodiscard]] std::string HashToHex(const std::span<const std::byte> bytes)
{
    const auto digest = pbprotocol::ComputeBlake3Digest(bytes);
    return BytesToHex(digest);
}

[[nodiscard]] std::string HashStringToHex(const std::string_view value)
{
    const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
    return HashToHex(bytes);
}

template<typename Margin>
[[nodiscard]] MarginEvidence MakeMarginEvidence(const Margin& margin) noexcept
{
    return {margin.samples, margin.minimum, margin.p50, margin.p01, margin.p001};
}

void CopyAcceptedBlocks(const pbdesktoplevels::ReferenceChannel& channel, FrameEvidence& output)
{
    const auto accepted = channel.GetAcceptedTransportBlocks();
    output.acceptedBlocks.reserve(accepted.size());
    for (const auto& block : accepted)
    {
        const auto bytes = std::span(block.bytes).first(block.byteCount);
        output.acceptedBlocks.push_back({block.slot, block.byteCount, HashToHex(bytes)});
    }
}

void SetBootstrapEvidence(const pbmodulation::LocalDesktopObservation& bootstrap,
    FrameEvidence& output)
{
    output.bootstrapAccepted = bootstrap.IsAccepted();
    output.bootstrapErasure = pbmodulation::GetLocalDesktopErasureName(bootstrap.erasure);
    if (!bootstrap.IsAccepted())
    {
        return;
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(bootstrap.canonical44);
    if (!parsed)
    {
        throw std::runtime_error("accepted Bootstrap failed canonical protocol parsing");
    }
    output.canonicalBootstrapBlake3 = HashToHex(bootstrap.canonical44);
    output.sessionTag = parsed.Value().sessionTag.value;
    output.frameSequence = parsed.Value().frameSequence;
    output.geometryAvailable = true;
    output.geometry = bootstrap.geometry;
}

void DecodeFrame(const ProfileBinding& profile, const pbmodulation::LumaView& view,
    pbdesktoplevels::ReferenceChannel& channel, FrameEvidence& output)
{
    if (profile.profileId == pbmodulation::kDesktopLevels2ProfileId)
    {
        pbmodulation::DesktopLevelsDecodePolicy policy;
        policy.locator.minimumScale = 0.5;
        policy.locator.maximumScale = 2.0;
        const auto observation = channel.Decode(view, policy, pbdesktoplevels::EvaluationMode::Transport);
        SetBootstrapEvidence(observation.modulation.bootstrap, output);
        output.modulationAccepted = observation.modulation.IsAccepted();
        output.modulationErasure = pbmodulation::GetDesktopLevelsErasureName(observation.modulation.erasure);
        output.dataBytes = observation.modulation.dataBytes;
        output.unreliablePrimary = observation.modulation.unreliableTiles;
        output.dataWorkUnits = observation.modulation.dataWorkUnits;
        output.pixelError = pbmodulation::GetLocalDesktopErasureName(observation.modulation.pixelError);
        output.primaryMargin = MakeMarginEvidence(observation.modulation.margin);
        output.evaluation = observation.evaluation;
    }
    else if (profile.profileId == pbmodulation::kShapeChromaProfileId)
    {
        const auto observation = channel.DecodeShapeChroma(view, {}, pbdesktoplevels::EvaluationMode::Transport);
        SetBootstrapEvidence(observation.modulation.bootstrap, output);
        output.modulationAccepted = observation.modulation.IsAccepted();
        output.modulationErasure = pbmodulation::GetShapeChromaErasureName(observation.modulation.erasure);
        output.dataBytes = observation.modulation.dataBytes;
        output.unreliablePrimary = observation.modulation.unreliableShapeTiles;
        output.unreliableSecondary = observation.modulation.unreliableChromaTiles;
        output.dataWorkUnits = observation.modulation.dataWorkUnits;
        output.pixelError = pbmodulation::GetLocalDesktopErasureName(observation.modulation.pixelError);
        output.primaryMargin = MakeMarginEvidence(observation.modulation.shapeMargin);
        output.secondaryMargin = MakeMarginEvidence(observation.modulation.chromaMargin);
        output.evaluation = observation.evaluation;
    }
    else if (profile.profileId == pbmodulation::kRemoteVisualProfileId)
    {
        const auto observation = channel.DecodeRemoteVisual(view, {}, pbdesktoplevels::EvaluationMode::Transport);
        SetBootstrapEvidence(observation.modulation.bootstrap, output);
        output.modulationAccepted = observation.modulation.IsAccepted();
        output.modulationErasure = pbmodulation::GetRemoteVisualErasureName(observation.modulation.erasure);
        output.dataBytes = observation.modulation.dataBytes;
        output.unreliablePrimary = observation.modulation.unreliableTiles;
        output.freshnessRegions = observation.modulation.freshnessRegions;
        output.staleRegions = observation.modulation.staleRegions;
        output.freshnessTagMismatches = observation.modulation.freshnessTagMismatches;
        output.freshnessTagErasures = observation.modulation.freshnessTagErasures;
        output.dataWorkUnits = observation.modulation.dataWorkUnits;
        output.pixelError = pbmodulation::GetLocalDesktopErasureName(observation.modulation.pixelError);
        output.primaryMargin = MakeMarginEvidence(observation.modulation.margin);
        output.evaluation = observation.evaluation;
    }
    else
    {
        const auto observation = channel.DecodeRemoteVisualLowFps(view, {},
            pbdesktoplevels::EvaluationMode::Transport);
        SetBootstrapEvidence(observation.modulation.bootstrap, output);
        output.modulationAccepted = observation.modulation.IsAccepted();
        output.modulationErasure = pbmodulation::GetRemoteVisualLowFpsErasureName(observation.modulation.erasure);
        output.dataBytes = observation.modulation.dataBytes;
        output.unreliablePrimary = observation.modulation.unreliableSymbols;
        output.freshnessRegions = observation.modulation.freshnessRegions;
        output.staleRegions = observation.modulation.staleRegions;
        output.freshnessTagMismatches = observation.modulation.freshnessTagMismatches;
        output.freshnessTagErasures = observation.modulation.freshnessTagErasures;
        output.erasedDataMetrics = observation.modulation.erasedDataMetrics;
        output.dataWorkUnits = observation.modulation.dataWorkUnits;
        output.pixelError = pbmodulation::GetLocalDesktopErasureName(observation.modulation.pixelError);
        output.primaryMargin = MakeMarginEvidence(observation.modulation.margin);
        output.evaluation = observation.evaluation;
    }
    if (output.evaluation.falseAcceptedCodewords != 0)
    {
        throw std::runtime_error("receiver-only Transport evaluation reported unavailable sender-truth mismatches");
    }
    CopyAcceptedBlocks(channel, output);
    if (output.acceptedBlocks.size() != output.evaluation.acceptedTransportBlocks)
    {
        throw std::runtime_error("accepted Transport block snapshot disagrees with its evaluation count");
    }
}

void AppendOptionalUnsigned(std::string& output, const std::optional<std::uint32_t> value)
{
    if (value)
    {
        AppendUnsigned(output, *value);
    }
    else
    {
        output.append("null");
    }
}

void AppendMargin(std::string& output, const MarginEvidence& margin)
{
    output.append("{\"samples\":");
    AppendUnsigned(output, margin.samples);
    output.append(",\"minimum\":");
    AppendDouble(output, margin.minimum);
    output.append(",\"p50\":");
    AppendDouble(output, margin.p50);
    output.append(",\"p01\":");
    AppendDouble(output, margin.p01);
    output.append(",\"p001\":");
    AppendDouble(output, margin.p001);
    output.push_back('}');
}

void AppendEvaluation(std::string& output, const pbdesktoplevels::FrameEvaluation& evaluation)
{
    output.append("{\"evaluated\":");
    AppendBoolean(output, evaluation.evaluated);
    output.append(",\"paddingValid\":");
    AppendBoolean(output, evaluation.paddingValid);
    output.append(",\"codewords\":");
    AppendUnsigned(output, evaluation.codewords);
    output.append(",\"fecFailures\":");
    AppendUnsigned(output, evaluation.fecFailures);
    output.append(",\"crcFailures\":");
    AppendUnsigned(output, evaluation.crcFailures);
    output.append(",\"identityFailures\":");
    AppendUnsigned(output, evaluation.identityFailures);
    output.append(",\"falseAcceptedCodewords\":null,\"falseAcceptedCodewordsAvailability\":"
        "\"UnavailableReceiverOnly\",\"acceptedTransportBlocks\":");
    AppendUnsigned(output, evaluation.acceptedTransportBlocks);
    output.append(",\"acceptedRemoteControlBlocks\":");
    AppendUnsigned(output, evaluation.acceptedRemoteControlBlocks);
    output.append(",\"iterationsTotal\":");
    AppendUnsigned(output, evaluation.iterationsTotal);
    output.append(",\"iterationsMaximum\":");
    AppendUnsigned(output, evaluation.iterationsMaximum);
    output.push_back('}');
}

void AppendRecordedObservation(std::string& output,
    const std::optional<pbrealcapturereplay::ReplayV2DemodObservationView>& observation)
{
    if (!observation)
    {
        output.append("null");
        return;
    }
    output.append("{\"disposition\":");
    AppendUnsigned(output, static_cast<std::uint8_t>(observation->disposition));
    output.append(",\"bootstrapAttempted\":");
    AppendBoolean(output, observation->bootstrapAttempted);
    output.append(",\"bootstrapSucceeded\":");
    AppendBoolean(output, observation->bootstrapSucceeded);
    output.append(",\"frameSequenceAvailable\":");
    AppendBoolean(output, observation->frameSequenceAvailable);
    output.append(",\"frameSequence\":");
    AppendUnsigned(output, observation->frameSequence);
    output.append(",\"transportProduced\":");
    AppendBoolean(output, observation->transportProduced);
    output.append(",\"receiverAdmitted\":");
    AppendBoolean(output, observation->receiverAdmitted);
    output.append(",\"diagnosticCode\":");
    AppendUnsigned(output, observation->diagnosticCode);
    output.push_back('}');
}

[[nodiscard]] std::string SerializePayload(const pbrealcapturereplay::ReplayV2FileSnapshot& snapshot,
    const ProfileBinding& profile, const std::vector<FrameEvidence>& frames,
    const ReplayInspection& summary)
{
    std::string output;
    output.reserve(4096 + frames.size() * 2048);
    output.append("{\"descriptor\":{\"datasetClass\":\"RemoteVisual\",\"datasetId\":");
    AppendJsonString(output, BytesToHex(snapshot.descriptor.datasetId));
    output.append(",\"runId\":");
    AppendJsonString(output, snapshot.descriptor.runId);
    output.append(",\"visualProfile\":");
    AppendJsonString(output, profile.name);
    output.append(",\"visualProfileId\":");
    AppendUint64HexString(output, profile.profileId);
    output.append(",\"visualLayoutVersion\":");
    AppendUnsigned(output, profile.layoutVersion);
    output.append(",\"createdUtc100ns\":");
    AppendSigned(output, snapshot.descriptor.createdUtc100ns);
    output.append(",\"remoteMetadataBytes\":");
    AppendUnsigned(output, snapshot.descriptor.remoteMetadataJsonUtf8.size());
    output.append(",\"remoteMetadataBlake3\":");
    AppendJsonString(output, HashStringToHex(snapshot.descriptor.remoteMetadataJsonUtf8));
    output.append("},\"reader\":{\"fileBytes\":");
    AppendUnsigned(output, snapshot.fileBytes);
    output.append(",\"totalRasterBytes\":");
    AppendUnsigned(output, snapshot.totalRasterBytes);
    output.append(",\"captureFrames\":");
    AppendUnsigned(output, snapshot.captureFrames);
    output.append(",\"demodObservations\":");
    AppendUnsigned(output, snapshot.demodObservations);
    output.append(",\"recordsProcessed\":");
    AppendUnsigned(output, snapshot.recordsProcessed);
    output.append(",\"complete\":");
    AppendBoolean(output, snapshot.complete);
    output.append("},\"summary\":{\"bootstrapAcceptedFrames\":");
    AppendUnsigned(output, summary.bootstrapAcceptedFrames);
    output.append(",\"modulationAcceptedFrames\":");
    AppendUnsigned(output, summary.modulationAcceptedFrames);
    output.append(",\"transportAcceptedFrames\":");
    AppendUnsigned(output, summary.transportAcceptedFrames);
    output.append(",\"acceptedTransportBlocks\":");
    AppendUnsigned(output, summary.acceptedTransportBlocks);
    output.append(",\"senderTruthAvailable\":false,\"acceptanceAuthority\":"
        "\"QC-LDPC+padding+TransportCRC+SessionIdentity\",\"finalFileDisposition\":\"NotEvaluated\"},\"frames\":[");
    for (std::size_t index = 0; index < frames.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        const auto& frame = frames[index];
        output.append("{\"ordinal\":");
        AppendUnsigned(output, frame.ordinal);
        output.append(",\"captureEpoch\":");
        AppendUnsigned(output, frame.captureEpoch);
        output.append(",\"captureObservation\":");
        AppendUnsigned(output, frame.captureObservation);
        output.append(",\"raster\":{\"width\":");
        AppendUnsigned(output, frame.width);
        output.append(",\"height\":");
        AppendUnsigned(output, frame.height);
        output.append(",\"rowPitch\":");
        AppendUnsigned(output, frame.rowPitch);
        output.append(",\"dxgiFormat\":");
        AppendUnsigned(output, frame.pixelFormat);
        output.append(",\"blake3\":");
        AppendJsonString(output, frame.capturedRoiBlake3);
        output.append("},\"bootstrap\":{\"accepted\":");
        AppendBoolean(output, frame.bootstrapAccepted);
        output.append(",\"erasure\":");
        AppendJsonString(output, frame.bootstrapErasure);
        output.append(",\"canonicalBlake3\":");
        if (frame.bootstrapAccepted)
        {
            AppendJsonString(output, frame.canonicalBootstrapBlake3);
        }
        else
        {
            output.append("null");
        }
        output.append(",\"sessionTag\":");
        if (frame.bootstrapAccepted)
        {
            AppendUint64HexString(output, frame.sessionTag);
        }
        else
        {
            output.append("null");
        }
        output.append(",\"frameSequence\":");
        if (frame.bootstrapAccepted)
        {
            AppendUnsigned(output, frame.frameSequence);
        }
        else
        {
            output.append("null");
        }
        output.append("},\"geometry\":");
        if (frame.geometryAvailable)
        {
            output.append("{\"originX\":");
            AppendDouble(output, frame.geometry.originX);
            output.append(",\"originY\":");
            AppendDouble(output, frame.geometry.originY);
            output.append(",\"scaleX\":");
            AppendDouble(output, frame.geometry.scaleX);
            output.append(",\"scaleY\":");
            AppendDouble(output, frame.geometry.scaleY);
            output.append(",\"markerResidualPixels\":");
            AppendDouble(output, frame.geometry.markerResidualPixels);
            output.push_back('}');
        }
        else
        {
            output.append("null");
        }
        output.append(",\"modulation\":{\"accepted\":");
        AppendBoolean(output, frame.modulationAccepted);
        output.append(",\"erasure\":");
        AppendJsonString(output, frame.modulationErasure);
        output.append(",\"dataBytes\":");
        AppendUnsigned(output, frame.dataBytes);
        output.append(",\"unreliablePrimary\":");
        AppendUnsigned(output, frame.unreliablePrimary);
        output.append(",\"unreliableSecondary\":");
        AppendOptionalUnsigned(output, frame.unreliableSecondary);
        output.append(",\"freshnessRegions\":");
        AppendOptionalUnsigned(output, frame.freshnessRegions);
        output.append(",\"staleRegions\":");
        AppendOptionalUnsigned(output, frame.staleRegions);
        output.append(",\"freshnessTagMismatches\":");
        AppendOptionalUnsigned(output, frame.freshnessTagMismatches);
        output.append(",\"freshnessTagErasures\":");
        AppendOptionalUnsigned(output, frame.freshnessTagErasures);
        output.append(",\"erasedDataMetrics\":");
        AppendOptionalUnsigned(output, frame.erasedDataMetrics);
        output.append(",\"dataWorkUnits\":");
        AppendUnsigned(output, frame.dataWorkUnits);
        output.append(",\"pixelError\":");
        AppendJsonString(output, frame.pixelError);
        output.append(",\"primaryMargin\":");
        AppendMargin(output, frame.primaryMargin);
        output.append(",\"secondaryMargin\":");
        if (frame.secondaryMargin)
        {
            AppendMargin(output, *frame.secondaryMargin);
        }
        else
        {
            output.append("null");
        }
        output.append("},\"transport\":");
        AppendEvaluation(output, frame.evaluation);
        output.append(",\"acceptedBlocks\":[");
        for (std::size_t blockIndex = 0; blockIndex < frame.acceptedBlocks.size(); blockIndex++)
        {
            if (blockIndex != 0)
            {
                output.push_back(',');
            }
            const auto& block = frame.acceptedBlocks[blockIndex];
            output.append("{\"slot\":");
            AppendUnsigned(output, block.slot);
            output.append(",\"byteCount\":");
            AppendUnsigned(output, block.byteCount);
            output.append(",\"blake3\":");
            AppendJsonString(output, block.blake3);
            output.push_back('}');
        }
        output.append("],\"recordedLiveObservation\":");
        AppendRecordedObservation(output, frame.recordedObservation);
        output.push_back('}');
    }
    output.append("]}");
    return output;
}

[[nodiscard]] std::string SealPayload(const std::string& payload)
{
    std::string output;
    output.reserve(payload.size() + 192);
    output.append("{\"schema\":");
    AppendJsonString(output, kReplayInspectionSchema);
    output.append(",\"version\":");
    AppendUnsigned(output, kReplayInspectionVersion);
    output.append(",\"payloadBlake3\":");
    AppendJsonString(output, HashStringToHex(payload));
    output.append(",\"payload\":");
    output.append(payload);
    output.append("}\n");
    return output;
}

} // namespace

bool InspectReplay(const std::filesystem::path& replayPath,
    const pbrealcapturereplay::ReplayV2Limits& limits, ReplayInspection& output,
    std::string& error) noexcept
{
    try
    {
        std::unique_ptr<pbrealcapturereplay::ReplayV2Reader> reader;
        const auto openStatus = pbrealcapturereplay::ReplayV2Reader::Open(replayPath, limits, reader);
        if (!openStatus)
        {
            error = "ReplayV2Reader rejected the sealed input at stage " +
                std::to_string(static_cast<std::uint8_t>(openStatus.stage)) + ", error " +
                std::to_string(static_cast<std::uint8_t>(openStatus.code));
            return false;
        }
        const auto openedSnapshot = reader->GetSnapshot();
        if (openedSnapshot.descriptor.datasetClass != pbrealcapturereplay::ReplayDatasetClass::RemoteVisual)
        {
            error = "Replay dataset class is not RemoteVisual";
            return false;
        }
        const auto profile = ResolveProfile(openedSnapshot.descriptor.visualProfileId);
        if (!profile)
        {
            error = "Replay VisualProfileId is not a supported Step-02 evidence profile";
            return false;
        }

        auto channelResult = pbdesktoplevels::ReferenceChannel::Create(
            pbdesktoplevels::kProcessingReservationBytes);
        if (!channelResult)
        {
            error = "bounded ReferenceChannel workspace creation failed";
            return false;
        }
        auto channel = std::move(channelResult).Value();
        std::vector<FrameEvidence> frames;
        std::vector<pbrealcapturereplay::ReplayV2DemodObservationView> recordedObservations;
        frames.reserve(limits.maximumCaptureFrames);
        recordedObservations.reserve(limits.maximumCaptureFrames);
        for (;;)
        {
            pbrealcapturereplay::ReplayV2Record record;
            const auto readStatus = reader->ReadNext(record);
            if (!readStatus)
            {
                if (readStatus.code != pbrealcapturereplay::ReplayError::EndOfFile)
                {
                    error = "ReplayV2Reader record validation failed at stage " +
                        std::to_string(static_cast<std::uint8_t>(readStatus.stage)) + ", error " +
                        std::to_string(static_cast<std::uint8_t>(readStatus.code));
                    return false;
                }
                break;
            }
            if (record.type == pbrealcapturereplay::ReplayV2RecordType::DemodObservation)
            {
                if (record.demodObservation.visualProfileId != profile->profileId)
                {
                    error = "recorded live observation VisualProfileId conflicts with the sealed descriptor";
                    return false;
                }
                const auto duplicate = std::ranges::find_if(recordedObservations,
                    [&](const auto& value)
                    {
                        return value.captureEpoch == record.demodObservation.captureEpoch &&
                            value.captureObservation == record.demodObservation.captureObservation;
                    });
                if (duplicate != recordedObservations.end())
                {
                    error = "Replay contains duplicate live observations for one capture identity";
                    return false;
                }
                recordedObservations.push_back(record.demodObservation);
                continue;
            }

            const auto pixelFormat = ResolvePixelFormat(record.capture.capturedRoi.pixelFormat);
            if (!pixelFormat)
            {
                error = "Replay captured ROI uses an unsupported bounded luma input format";
                return false;
            }
            FrameEvidence frame;
            frame.ordinal = record.ordinal;
            frame.captureEpoch = record.capture.capture.domain.captureEpoch;
            frame.captureObservation = record.capture.capture.captureObservation;
            frame.width = record.capture.capturedRoi.width;
            frame.height = record.capture.capturedRoi.height;
            frame.rowPitch = record.capture.capturedRoi.rowPitch;
            frame.pixelFormat = static_cast<std::uint32_t>(record.capture.capturedRoi.pixelFormat);
            frame.capturedRoiBlake3 = HashToHex(record.capture.capturedRoi.pixels);
            const pbmodulation::LumaView view{record.capture.capturedRoi.pixels,
                record.capture.capturedRoi.width, record.capture.capturedRoi.height,
                record.capture.capturedRoi.rowPitch, *pixelFormat};
            DecodeFrame(*profile, view, channel, frame);
            frames.push_back(std::move(frame));
        }

        for (const auto& observation : recordedObservations)
        {
            const auto frame = std::ranges::find_if(frames,
                [&](const FrameEvidence& value)
                {
                    return value.captureEpoch == observation.captureEpoch &&
                        value.captureObservation == observation.captureObservation;
                });
            if (frame == frames.end())
            {
                error = "recorded live observation has no matching receiver capture";
                return false;
            }
            frame->recordedObservation = observation;
        }

        const auto finalSnapshot = reader->GetSnapshot();
        if (!finalSnapshot.complete || finalSnapshot.captureFrames != frames.size() ||
            finalSnapshot.demodObservations != recordedObservations.size())
        {
            error = "Replay reader did not finish at its validated footer boundary";
            return false;
        }
        ReplayInspection candidate;
        candidate.captureFrames = static_cast<std::uint32_t>(frames.size());
        candidate.recordedDemodObservations = static_cast<std::uint32_t>(recordedObservations.size());
        for (const auto& frame : frames)
        {
            candidate.bootstrapAcceptedFrames += frame.bootstrapAccepted ? 1U : 0U;
            candidate.modulationAcceptedFrames += frame.modulationAccepted ? 1U : 0U;
            const bool allTransportBlocksAccepted = frame.evaluation.evaluated && frame.evaluation.paddingValid &&
                frame.evaluation.codewords != 0 && frame.evaluation.acceptedTransportBlocks == frame.evaluation.codewords &&
                frame.evaluation.fecFailures == 0 && frame.evaluation.crcFailures == 0 && frame.evaluation.identityFailures == 0;
            candidate.transportAcceptedFrames += allTransportBlocksAccepted ? 1U : 0U;
            candidate.acceptedTransportBlocks += frame.evaluation.acceptedTransportBlocks;
        }
        candidate.canonicalJson = SealPayload(SerializePayload(finalSnapshot, *profile, frames, candidate));
        output = std::move(candidate);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }
    catch (...)
    {
        error = "unknown Replay inspection failure";
    }
    return false;
}

} // namespace pbremotevisualreplayinspector
