#include "codec_probe_core.h"

#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbremotevisualcodecprobe
{
namespace
{

constexpr std::uint64_t kSourceSessionTag = 0xA24C70E19B35D86FULL;
constexpr std::uint64_t kFirstFrameSequence = 900;

struct AcceptedBlockEvidence
{
    std::uint32_t slot = 0;
    std::uint32_t byteCount = 0;
    std::string blake3;
};

struct FrameEvidence
{
    CodecFrameSummary summary;
    std::string inputBlake3;
    std::string bootstrapErasure;
    std::uint32_t staleRegions = 0;
    std::uint32_t erasedDataMetrics = 0;
    std::uint32_t codewords = 0;
    std::uint32_t fecFailures = 0;
    std::uint32_t crcFailures = 0;
    std::uint32_t identityFailures = 0;
    std::uint64_t comparedCodedBits = 0;
    std::uint64_t erroneousCodedBits = 0;
    std::vector<AcceptedBlockEvidence> acceptedBlocks;
};

void AppendUnsigned(std::string& output, const std::uint64_t value)
{
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{})
    {
        throw std::length_error("integer serialization failed");
    }
    output.append(buffer.data(), converted.ptr);
}

void AppendBoolean(std::string& output, const bool value)
{
    output.append(value ? "true" : "false");
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

void AppendUint64HexString(std::string& output, const std::uint64_t value)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    output.push_back('"');
    for (std::size_t nibble = 0; nibble < 16; nibble++)
    {
        const std::size_t shift = (15 - nibble) * 4;
        output.push_back(kHexDigits[(value >> shift) & 0x0FU]);
    }
    output.push_back('"');
}

std::string DigestToHex(const std::array<std::byte, pbprotocol::kDigestBytes>& digest)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string output;
    output.resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); index++)
    {
        const unsigned int value = std::to_integer<unsigned int>(digest[index]);
        output[index * 2] = kHexDigits[value >> 4];
        output[index * 2 + 1] = kHexDigits[value & 0x0FU];
    }
    return output;
}

std::string HashToHex(const std::span<const std::byte> bytes)
{
    return DigestToHex(pbprotocol::ComputeBlake3Digest(bytes));
}

std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeBootstrapRecord(const std::uint64_t frameSequence)
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = pbmodulation::kRemoteVisualLowFpsLayoutVersion;
    record.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    record.sessionTag.value = kSourceSessionTag;
    record.frameSequence = frameSequence;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bytes{};
    if (!pbprotocol::SerializeBootstrapRecord(record, bytes))
    {
        throw std::runtime_error("Bootstrap serialization failed");
    }
    return bytes;
}

std::vector<std::byte> MakeDiagnosticData(const std::span<const std::byte> record)
{
    std::vector<std::byte> data(pbmodulation::kRemoteVisualLowFpsDataBytes);
    if (!pbdesktoplevels::GenerateDiagnosticData(record, data))
    {
        throw std::runtime_error("diagnostic data generation failed");
    }
    return data;
}

std::vector<std::byte> ConvertNeutralBgraToGray(const std::span<const std::byte> bgra)
{
    if (bgra.size() % 4 != 0)
    {
        throw std::runtime_error("BGRA input has an invalid byte count");
    }
    std::vector<std::byte> gray(bgra.size() / 4);
    for (std::size_t index = 0; index < gray.size(); index++)
    {
        const std::size_t sourceOffset = index * 4;
        if (bgra[sourceOffset] != bgra[sourceOffset + 1] || bgra[sourceOffset] != bgra[sourceOffset + 2] ||
            bgra[sourceOffset + 3] != std::byte{255})
        {
            throw std::runtime_error("LF4 source is not neutral opaque BGRA");
        }
        gray[index] = bgra[sourceOffset];
    }
    return gray;
}

std::string Classify(const pbdesktoplevels::RemoteVisualLowFpsReferenceObservation& observation)
{
    if (observation.evaluation.falseAcceptedCodewords != 0)
    {
        return "FalseAcceptance";
    }
    if (observation.evaluation.IsVerified())
    {
        return "Verified";
    }
    if (observation.modulation.IsAccepted())
    {
        return "RejectedNoFalseAccept";
    }
    return "ErasureNoFalseAccept";
}

std::string SerializeSourcePayload(const CodecSourceSequence& source)
{
    std::string output;
    output.reserve(1024);
    output.append("{\"format\":\"bgra8\",\"width\":");
    AppendUnsigned(output, kSourceWidth);
    output.append(",\"height\":");
    AppendUnsigned(output, kSourceHeight);
    output.append(",\"frameBytes\":");
    AppendUnsigned(output, kBgraFrameBytes);
    output.append(",\"frameCount\":");
    AppendUnsigned(output, kSourceFrameCount);
    output.append(",\"profile\":\"PB-RemoteVisual-LF4-X1\",\"profileId\":");
    AppendUint64HexString(output, pbmodulation::kRemoteVisualLowFpsProfileId);
    output.append(",\"layoutVersion\":");
    AppendUnsigned(output, pbmodulation::kRemoteVisualLowFpsLayoutVersion);
    output.append(",\"sessionTag\":");
    AppendUint64HexString(output, kSourceSessionTag);
    output.append(",\"sequenceBlake3\":");
    AppendJsonString(output, HashToHex(source.bgraFrames));
    output.append(",\"frames\":[");
    for (std::uint32_t index = 0; index < kSourceFrameCount; index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        const auto frame = std::span(source.bgraFrames).subspan(static_cast<std::size_t>(index) * kBgraFrameBytes,
            kBgraFrameBytes);
        output.append("{\"index\":");
        AppendUnsigned(output, index);
        output.append(",\"frameSequence\":");
        AppendUnsigned(output, kFirstFrameSequence + index);
        output.append(",\"blake3\":");
        AppendJsonString(output, HashToHex(frame));
        output.push_back('}');
    }
    output.append("]}");
    return output;
}

std::string SealPayload(const std::string_view schema, const std::string& payload)
{
    const auto* payloadBytes = reinterpret_cast<const std::byte*>(payload.data());
    const std::string payloadBlake3 = HashToHex({payloadBytes, payload.size()});
    std::string output;
    output.reserve(payload.size() + 160);
    output.append("{\"schema\":");
    AppendJsonString(output, schema);
    output.append(",\"version\":");
    AppendUnsigned(output, kCodecProbeVersion);
    output.append(",\"payloadBlake3\":");
    AppendJsonString(output, payloadBlake3);
    output.append(",\"payload\":");
    output.append(payload);
    output.append("}\n");
    return output;
}

std::string SerializeEvaluationPayload(const std::span<const std::byte> input,
    const std::vector<FrameEvidence>& frames, const bool truthBoundaryValid, const bool allFramesVerified)
{
    std::uint64_t verifiedFrames = 0;
    std::uint64_t erasureFrames = 0;
    std::uint64_t rejectedFrames = 0;
    std::uint64_t falseAcceptedCodewords = 0;
    std::uint64_t acceptedTransportBlocks = 0;
    for (const FrameEvidence& frame : frames)
    {
        if (frame.summary.classification == "Verified")
        {
            verifiedFrames++;
        }
        else if (frame.summary.classification == "ErasureNoFalseAccept")
        {
            erasureFrames++;
        }
        else if (frame.summary.classification == "RejectedNoFalseAccept")
        {
            rejectedFrames++;
        }
        falseAcceptedCodewords += frame.summary.falseAcceptedCodewords;
        acceptedTransportBlocks += frame.summary.acceptedTransportBlocks;
    }
    std::string output;
    output.reserve(16 * 1024);
    output.append("{\"input\":{\"format\":\"gray8\",\"width\":");
    AppendUnsigned(output, kSourceWidth);
    output.append(",\"height\":");
    AppendUnsigned(output, kSourceHeight);
    output.append(",\"frameBytes\":");
    AppendUnsigned(output, kGrayFrameBytes);
    output.append(",\"frameCount\":");
    AppendUnsigned(output, frames.size());
    output.append(",\"byteCount\":");
    AppendUnsigned(output, input.size());
    output.append(",\"blake3\":");
    AppendJsonString(output, HashToHex(input));
    output.append("},\"frames\":[");
    for (std::size_t index = 0; index < frames.size(); index++)
    {
        if (index != 0)
        {
            output.push_back(',');
        }
        const FrameEvidence& frame = frames[index];
        output.append("{\"index\":");
        AppendUnsigned(output, frame.summary.index);
        output.append(",\"inputBlake3\":");
        AppendJsonString(output, frame.inputBlake3);
        output.append(",\"classification\":");
        AppendJsonString(output, frame.summary.classification);
        output.append(",\"erasure\":");
        AppendJsonString(output, frame.summary.erasure);
        output.append(",\"bootstrapAccepted\":");
        AppendBoolean(output, frame.summary.bootstrapAccepted);
        output.append(",\"bootstrapErasure\":");
        AppendJsonString(output, frame.bootstrapErasure);
        output.append(",\"sessionTag\":");
        if (frame.summary.bootstrapAccepted)
        {
            AppendUint64HexString(output, frame.summary.sessionTag);
        }
        else
        {
            output.append("null");
        }
        output.append(",\"frameSequence\":");
        if (frame.summary.bootstrapAccepted)
        {
            AppendUnsigned(output, frame.summary.frameSequence);
        }
        else
        {
            output.append("null");
        }
        output.append(",\"staleRegions\":");
        AppendUnsigned(output, frame.staleRegions);
        output.append(",\"erasedDataMetrics\":");
        AppendUnsigned(output, frame.erasedDataMetrics);
        output.append(",\"evaluation\":{\"codewords\":");
        AppendUnsigned(output, frame.codewords);
        output.append(",\"fecFailures\":");
        AppendUnsigned(output, frame.fecFailures);
        output.append(",\"crcFailures\":");
        AppendUnsigned(output, frame.crcFailures);
        output.append(",\"identityFailures\":");
        AppendUnsigned(output, frame.identityFailures);
        output.append(",\"falseAcceptedCodewords\":");
        AppendUnsigned(output, frame.summary.falseAcceptedCodewords);
        output.append(",\"acceptedTransportBlocks\":");
        AppendUnsigned(output, frame.summary.acceptedTransportBlocks);
        output.append(",\"comparedCodedBits\":");
        AppendUnsigned(output, frame.comparedCodedBits);
        output.append(",\"erroneousCodedBits\":");
        AppendUnsigned(output, frame.erroneousCodedBits);
        output.append("},\"acceptedBlocks\":[");
        for (std::size_t blockIndex = 0; blockIndex < frame.acceptedBlocks.size(); blockIndex++)
        {
            if (blockIndex != 0)
            {
                output.push_back(',');
            }
            const AcceptedBlockEvidence& block = frame.acceptedBlocks[blockIndex];
            output.append("{\"slot\":");
            AppendUnsigned(output, block.slot);
            output.append(",\"byteCount\":");
            AppendUnsigned(output, block.byteCount);
            output.append(",\"blake3\":");
            AppendJsonString(output, block.blake3);
            output.push_back('}');
        }
        output.append("]}");
    }
    output.append("],\"summary\":{\"frameCount\":");
    AppendUnsigned(output, frames.size());
    output.append(",\"verifiedFrames\":");
    AppendUnsigned(output, verifiedFrames);
    output.append(",\"erasureFrames\":");
    AppendUnsigned(output, erasureFrames);
    output.append(",\"rejectedFrames\":");
    AppendUnsigned(output, rejectedFrames);
    output.append(",\"acceptedTransportBlocks\":");
    AppendUnsigned(output, acceptedTransportBlocks);
    output.append(",\"falseAcceptedCodewords\":");
    AppendUnsigned(output, falseAcceptedCodewords);
    output.append(",\"truthBoundaryValid\":");
    AppendBoolean(output, truthBoundaryValid);
    output.append(",\"allFramesVerified\":");
    AppendBoolean(output, allFramesVerified);
    output.append("}}");
    return output;
}

} // namespace

bool BuildCanonicalSourceSequence(CodecSourceSequence& output, std::string& error)
{
    error.clear();
    try
    {
        CodecSourceSequence source;
        source.bgraFrames.resize(kBgraFrameBytes * kSourceFrameCount);
        for (std::uint32_t index = 0; index < kSourceFrameCount; index++)
        {
            const auto record = MakeBootstrapRecord(kFirstFrameSequence + index);
            const auto data = MakeDiagnosticData(record);
            const auto frame = std::span(source.bgraFrames).subspan(static_cast<std::size_t>(index) * kBgraFrameBytes,
                kBgraFrameBytes);
            if (!pbmodulation::EncodeRemoteVisualLowFpsFrame(record, data, frame))
            {
                throw std::runtime_error("LF4 source raster encoding failed");
            }
        }
        source.canonicalManifestJson = SealPayload(kCodecSourceSchema, SerializeSourcePayload(source));
        output = std::move(source);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }
    catch (...)
    {
        error = "unknown source sequence generation failure";
    }
    return false;
}

bool BuildAdversarialGrayFrame(const CodecAdversarialFrameKind kind,
    std::vector<std::byte>& output, std::string& error)
{
    error.clear();
    if (kind != CodecAdversarialFrameKind::TransportErasure &&
        kind != CodecAdversarialFrameKind::ValidCrcWrongIdentity)
    {
        error = "unsupported adversarial frame kind";
        return false;
    }
    try
    {
        const auto bootstrap = MakeBootstrapRecord(kFirstFrameSequence);
        std::vector<std::byte> data;
        if (kind == CodecAdversarialFrameKind::TransportErasure)
        {
            data.resize(pbmodulation::kRemoteVisualLowFpsDataBytes);
            std::uint32_t state = 0x6D2B79F5U;
            for (std::byte& value : data)
            {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                value = static_cast<std::byte>(state & 0xFFU);
            }
        }
        else
        {
            const auto conflictingIdentity = MakeBootstrapRecord(kFirstFrameSequence + 1);
            data = MakeDiagnosticData(conflictingIdentity);
        }
        std::vector<std::byte> bgra(kBgraFrameBytes);
        if (!pbmodulation::EncodeRemoteVisualLowFpsFrame(bootstrap, data, bgra))
        {
            throw std::runtime_error("adversarial LF4 raster encoding failed");
        }
        std::vector<std::byte> gray = ConvertNeutralBgraToGray(bgra);
        output = std::move(gray);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }
    catch (...)
    {
        error = "unknown adversarial frame generation failure";
    }
    return false;
}

bool EvaluateGray8Sequence(const std::span<const std::byte> frames,
    CodecSequenceEvaluation& output, std::string& error)
{
    error.clear();
    if (frames.empty() || frames.size() % kGrayFrameBytes != 0)
    {
        error = "Gray8 input size is not an exact nonzero frame sequence";
        return false;
    }
    const std::size_t frameCount = frames.size() / kGrayFrameBytes;
    if (frameCount > kMaximumEvaluationFrames)
    {
        error = "Gray8 input exceeds the frame-count limit";
        return false;
    }
    try
    {
        std::vector<FrameEvidence> evidence;
        evidence.reserve(frameCount);
        CodecSequenceEvaluation evaluation;
        evaluation.frames.reserve(frameCount);
        evaluation.truthBoundaryValid = true;
        evaluation.allFramesVerified = true;
        for (std::size_t index = 0; index < frameCount; index++)
        {
            const auto frame = frames.subspan(index * kGrayFrameBytes, kGrayFrameBytes);
            auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
            if (!channelResult)
            {
                throw std::runtime_error("reference channel allocation failed");
            }
            auto channel = std::move(channelResult).Value();
            const pbmodulation::LumaView view{frame, kSourceWidth, kSourceHeight, kSourceWidth,
                pbmodulation::LumaPixelFormat::Gray8};
            const auto observation = channel.DecodeRemoteVisualLowFps(view);
            FrameEvidence frameEvidence;
            frameEvidence.summary.index = static_cast<std::uint32_t>(index);
            frameEvidence.summary.classification = Classify(observation);
            frameEvidence.summary.erasure = pbmodulation::GetRemoteVisualLowFpsErasureName(observation.modulation.erasure);
            frameEvidence.summary.bootstrapAccepted = observation.modulation.bootstrap.IsAccepted();
            frameEvidence.summary.acceptedTransportBlocks = observation.evaluation.acceptedTransportBlocks;
            frameEvidence.summary.fecFailures = observation.evaluation.fecFailures;
            frameEvidence.summary.crcFailures = observation.evaluation.crcFailures;
            frameEvidence.summary.identityFailures = observation.evaluation.identityFailures;
            frameEvidence.summary.falseAcceptedCodewords = observation.evaluation.falseAcceptedCodewords;
            frameEvidence.inputBlake3 = HashToHex(frame);
            frameEvidence.bootstrapErasure = pbmodulation::GetLocalDesktopErasureName(observation.modulation.bootstrap.erasure);
            frameEvidence.staleRegions = observation.modulation.staleRegions;
            frameEvidence.erasedDataMetrics = observation.modulation.erasedDataMetrics;
            frameEvidence.codewords = observation.evaluation.codewords;
            frameEvidence.fecFailures = observation.evaluation.fecFailures;
            frameEvidence.crcFailures = observation.evaluation.crcFailures;
            frameEvidence.identityFailures = observation.evaluation.identityFailures;
            frameEvidence.comparedCodedBits = observation.evaluation.comparedCodedBits;
            frameEvidence.erroneousCodedBits = observation.evaluation.erroneousCodedBits;
            if (frameEvidence.summary.bootstrapAccepted)
            {
                const auto parsed = pbprotocol::ParseBootstrapRecord(observation.modulation.bootstrap.canonical44);
                if (!parsed)
                {
                    throw std::runtime_error("accepted Bootstrap could not be parsed");
                }
                frameEvidence.summary.sessionTag = parsed.Value().sessionTag.value;
                frameEvidence.summary.frameSequence = parsed.Value().frameSequence;
            }
            for (const auto& block : channel.GetAcceptedTransportBlocks())
            {
                AcceptedBlockEvidence blockEvidence;
                blockEvidence.slot = block.slot;
                blockEvidence.byteCount = block.byteCount;
                blockEvidence.blake3 = HashToHex(std::span(block.bytes).first(block.byteCount));
                frameEvidence.acceptedBlocks.push_back(std::move(blockEvidence));
            }
            evaluation.truthBoundaryValid = evaluation.truthBoundaryValid &&
                frameEvidence.summary.falseAcceptedCodewords == 0;
            evaluation.allFramesVerified = evaluation.allFramesVerified &&
                frameEvidence.summary.classification == "Verified";
            evaluation.frames.push_back(frameEvidence.summary);
            evidence.push_back(std::move(frameEvidence));
        }
        const std::string payload = SerializeEvaluationPayload(frames, evidence,
            evaluation.truthBoundaryValid, evaluation.allFramesVerified);
        evaluation.canonicalJson = SealPayload(kCodecEvaluationSchema, payload);
        output = std::move(evaluation);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }
    catch (...)
    {
        error = "unknown codec frame evaluation failure";
    }
    return false;
}

} // namespace pbremotevisualcodecprobe
