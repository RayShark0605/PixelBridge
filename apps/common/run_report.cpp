#include "run_report.h"

#include <iomanip>
#include <cmath>
#include <locale>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace pbapp
{
namespace
{

void WriteEscaped(std::ostream& stream, const std::string_view value)
{
    stream << '"';
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': stream << "\\\""; break;
        case '\\': stream << "\\\\"; break;
        case '\b': stream << "\\b"; break;
        case '\f': stream << "\\f"; break;
        case '\n': stream << "\\n"; break;
        case '\r': stream << "\\r"; break;
        case '\t': stream << "\\t"; break;
        default:
            if (character < 0x20U)
            {
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(character) << std::dec;
            }
            else
            {
                stream << character;
            }
            break;
        }
    }
    stream << '"';
}

template <typename ValueType>
void WriteNumber(std::ostream& stream, const ValueType value)
{
    if constexpr (std::is_floating_point_v<ValueType>)
    {
        if (!std::isfinite(value))
        {
            stream << "null";
            return;
        }
    }
    stream << value;
}

template <typename ValueType>
void WriteOptionalNumber(std::ostream& stream, const std::optional<ValueType>& value)
{
    if (value)
    {
        WriteNumber(stream, *value);
    }
    else
    {
        stream << "null";
    }
}

void WriteRemoteMetadata(std::ostream& stream, const RemoteRunMetadata& metadata)
{
    stream << "{\"channelType\":";
    WriteEscaped(stream, metadata.channelType == ChannelType::LocalDesktop ? "LocalDesktop" :
        metadata.channelType == ChannelType::SunloginRemoteVisual ? "Sunlogin RemoteVisual" : "Other");
    stream << ",\"remoteProviderVersion\":";
    WriteEscaped(stream, metadata.providerVersion);
    stream << ",\"remoteMode\":";
    WriteEscaped(stream, metadata.remoteMode);
    stream << ",\"targetFps\":";
    WriteOptionalNumber(stream, metadata.targetFps);
    stream << ",\"observedFps\":";
    WriteOptionalNumber(stream, metadata.observedFps);
    stream << ",\"chromaMode\":";
    WriteEscaped(stream, metadata.chromaMode == ChromaMode::Chroma444 ? "4:4:4" :
        metadata.chromaMode == ChromaMode::Chroma420 ? "4:2:0" : "Unknown");
    stream << ",\"remoteResolution\":";
    WriteEscaped(stream, metadata.remoteResolution);
    stream << ",\"remoteWindowScale\":";
    WriteEscaped(stream, metadata.remoteWindowScale);
    stream << ",\"networkNote\":";
    WriteEscaped(stream, metadata.networkNote);
    stream << ",\"observedBandwidthMbps\":";
    WriteOptionalNumber(stream, metadata.observedBandwidthMbps);
    stream << ",\"observedLatencyMilliseconds\":";
    WriteOptionalNumber(stream, metadata.observedLatencyMilliseconds);
    stream << '}';
}

void WriteContext(std::ostream& stream, const RunReportContext& context)
{
    stream << "\"schema\":\"PixelBridge.RunReport.1\",\"applicationName\":";
    WriteEscaped(stream, context.applicationName);
    stream << ",\"applicationVersion\":";
    WriteEscaped(stream, context.applicationVersion);
    stream << ",\"gitCommit\":";
    WriteEscaped(stream, context.gitCommit);
    stream << ",\"exportedAtUtc\":";
    WriteEscaped(stream, context.exportedAtUtc);
}

} // namespace

std::string BuildEncoderRunReportJson(const RunReportContext& context,
    const EncoderSnapshot& snapshot)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::boolalpha << std::setprecision(17) << '{';
    WriteContext(stream, context);
    stream << ",\"role\":\"Encoder\",\"runId\":";
    WriteEscaped(stream, snapshot.runId);
    stream << ",\"runStartedUnixMilliseconds\":" << snapshot.runStartedUnixMilliseconds
           << ",\"runEndedUnixMilliseconds\":";
    WriteOptionalNumber(stream, snapshot.runEndedUnixMilliseconds);
    stream << ",\"state\":";
    WriteEscaped(stream, GetEncoderStateName(snapshot.state));
    stream << ",\"sessionId\":";
    WriteEscaped(stream, snapshot.sessionIdHex);
    stream << ",\"sessionTag\":" << snapshot.sessionTag
           << ",\"sourcePath\":";
    WriteEscaped(stream, snapshot.sourcePath);
    stream << ",\"fileBytes\":" << snapshot.sourceBytes << ",\"profile\":";
    WriteEscaped(stream, GetVisualProfileName(snapshot.visualProfile));
    stream << ",\"compressionCodec\":";
    WriteEscaped(stream, GetCompressionCodecName(snapshot.compressionCodec));
    stream << ",\"outerFec\":";
    WriteEscaped(stream, GetOuterFecModeName(snapshot.outerFecMode));
    stream << ",\"innerFec\":\"Robust DVB-S2 Short QC-LDPC\""
           << ",\"outerBlockCount\":" << snapshot.outerBlockCount
           << ",\"dataWindow\":{\"left\":" << snapshot.dataWindowLeft
           << ",\"top\":" << snapshot.dataWindowTop
           << ",\"width\":" << snapshot.dataWindowWidth
           << ",\"height\":" << snapshot.dataWindowHeight << '}';
    stream << ",\"broadcastRuntimeMilliseconds\":" << snapshot.broadcastRuntimeMilliseconds
           << ",\"cycleCount\":" << snapshot.cycleCount
           << ",\"cyclePosition\":" << snapshot.cyclePosition
           << ",\"cycleFrameCount\":" << snapshot.cycleFrameCount
           << ",\"currentSegmentOrdinal\":" << snapshot.currentSegmentOrdinal
           << ",\"segmentCount\":" << snapshot.segmentCount
           << ",\"currentOuterBlockId\":" << snapshot.currentOuterBlockId
           << ",\"frameSequence\":" << snapshot.frameSequence
           << ",\"presentationEpoch\":" << snapshot.presentationEpoch
           << ",\"presentedVisualFps\":";
    WriteOptionalNumber(stream, snapshot.presentedVisualFps);
    stream << ",\"presentCallFps\":";
    WriteOptionalNumber(stream, snapshot.presentCallFps);
    stream << ",\"generatedVisualFramesPerSecond\":";
    WriteNumber(stream, snapshot.generatedVisualFramesPerSecond);
    stream << ",\"generatedPayloadBytesPerSecond\":";
    WriteNumber(stream, snapshot.generatedPayloadBytesPerSecond);
    stream << ",\"pendingFrames\":" << snapshot.pendingFrames
           << ",\"pendingHighWater\":" << snapshot.pendingHighWater
           << ",\"submittedFrames\":" << snapshot.submittedFrames
           << ",\"replacedPendingFrames\":" << snapshot.replacedPendingFrames
           << ",\"candidateContractSatisfied\":" << snapshot.candidateContractSatisfied
           << ",\"sourceStable\":" << snapshot.sourceStable
           << ",\"wholeFileDigest\":";
    WriteEscaped(stream, snapshot.wholeFileDigestHex);
    stream << ",\"receiverProgress\":null,\"receiverEta\":null,\"verifiedGoodput\":null"
           << ",\"remoteMetadata\":";
    WriteRemoteMetadata(stream, snapshot.remoteMetadata);
    stream << ",\"statusMessage\":";
    WriteEscaped(stream, snapshot.statusMessage);
    stream << ",\"errorDetail\":";
    WriteEscaped(stream, snapshot.errorDetail);
    stream << '}';
    return stream.str();
}

std::string BuildDecoderRunReportJson(const RunReportContext& context,
    const DecoderSnapshot& snapshot)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::boolalpha << std::setprecision(17) << '{';
    WriteContext(stream, context);
    stream << ",\"role\":\"Decoder\",\"runId\":";
    WriteEscaped(stream, snapshot.runId);
    stream << ",\"runStartedUnixMilliseconds\":" << snapshot.runStartedUnixMilliseconds
           << ",\"runEndedUnixMilliseconds\":";
    WriteOptionalNumber(stream, snapshot.runEndedUnixMilliseconds);
    stream << ",\"state\":";
    WriteEscaped(stream, GetDecoderStateName(snapshot.state));
    stream << ",\"requestedBackend\":";
    WriteEscaped(stream, GetCaptureBackendName(snapshot.requestedBackend));
    stream << ",\"actualBackend\":";
    if (snapshot.actualBackend)
    {
        WriteEscaped(stream, GetCaptureBackendName(*snapshot.actualBackend));
    }
    else
    {
        stream << "null";
    }
    stream << ",\"backendReason\":";
    WriteEscaped(stream, snapshot.backendReason);
    stream << ",\"profile\":";
    WriteEscaped(stream, GetVisualProfileName(snapshot.visualProfile));
    stream << ",\"sessionId\":";
    WriteEscaped(stream, snapshot.sessionIdHex);
    stream << ",\"sessionTag\":" << snapshot.sessionTag
           << ",\"descriptorKnown\":" << snapshot.descriptorKnown
           << ",\"originalFileBytes\":" << snapshot.originalFileBytes
           << ",\"verifiedRawBytes\":" << snapshot.verifiedRawBytes
           << ",\"remainingRawBytes\":" << snapshot.remainingRawBytes
           << ",\"recoveryProgress\":";
    WriteOptionalNumber(stream, snapshot.recoveryProgress);
    stream << ",\"instantVerifiedRawGoodputBytesPerSecond\":";
    WriteNumber(stream, snapshot.instantVerifiedRawGoodputBytesPerSecond);
    stream << ",\"smoothedVerifiedRawGoodputBytesPerSecond\":";
    WriteNumber(stream, snapshot.smoothedVerifiedRawGoodputBytesPerSecond);
    stream << ",\"averageVerifiedRawGoodputBytesPerSecond\":";
    WriteNumber(stream, snapshot.averageVerifiedRawGoodputBytesPerSecond);
    stream << ",\"verifiedRawGoodputBasis\":\"verifiedRawBytes\""
           << ",\"verifiedEncodedBytes\":" << snapshot.verifiedEncodedBytes
           << ",\"verifiedEncodedGoodputBitsPerSecond\":";
    WriteOptionalNumber(stream, snapshot.verifiedEncodedGoodputBitsPerSecond);
    stream << ",\"verifiedEncodedGoodputGate\":\"WholeFileDigest+finalPublish\"";
    stream << ",\"etaMilliseconds\":";
    WriteOptionalNumber(stream, snapshot.etaMilliseconds);
    stream << ",\"compressionCodec\":";
    WriteEscaped(stream, GetCompressionCodecName(snapshot.compressionCodec));
    stream << ",\"outerFec\":";
    WriteEscaped(stream, GetOuterFecModeName(snapshot.outerFecMode));
    stream << ",\"innerFec\":\"Robust DVB-S2 Short QC-LDPC\""
           << ",\"segmentCount\":" << snapshot.segmentCount
           << ",\"currentSegmentOrdinal\":" << snapshot.currentSegmentOrdinal
           << ",\"roi\":{\"left\":" << snapshot.roiLeft
           << ",\"top\":" << snapshot.roiTop
           << ",\"width\":" << snapshot.roiWidth
           << ",\"height\":" << snapshot.roiHeight
           << ",\"dpiX\":" << snapshot.dpiX
           << ",\"dpiY\":" << snapshot.dpiY
           << ",\"rotation\":" << snapshot.rotation
           << ",\"monitorLeft\":" << snapshot.monitorLeft
           << ",\"monitorTop\":" << snapshot.monitorTop
           << ",\"monitorWidth\":" << snapshot.monitorWidth
           << ",\"monitorHeight\":" << snapshot.monitorHeight << '}';
    stream << ",\"captureEpoch\":" << snapshot.captureEpoch
           << ",\"captureEpochResets\":" << snapshot.captureEpochResets
           << ",\"captureArrivedFrames\":" << snapshot.captureArrivedFrames
           << ",\"captureDeliveredFrames\":" << snapshot.captureDeliveredFrames
           << ",\"captureDroppedFrames\":" << snapshot.captureDroppedFrames
           << ",\"captureRecreates\":" << snapshot.captureRecreates
           << ",\"captureDeviceRecoveries\":" << snapshot.captureDeviceRecoveries
           << ",\"telemetryCapturedFrames\":" << snapshot.telemetryCapturedFrames
           << ",\"telemetryDroppedFrames\":" << snapshot.telemetryDroppedFrames
           << ",\"fingerprintedFrames\":" << snapshot.fingerprintedFrames
           << ",\"captureFps\":";
    WriteOptionalNumber(stream, snapshot.captureFps);
    stream << ",\"uniqueVisualFps\":";
    WriteOptionalNumber(stream, snapshot.uniqueVisualFps);
    stream << ",\"uniqueVisualFpsBasis\":\"collision-resistant admitted ROI pixel digest\""
           << ",\"frameSequenceGapEvents\":" << snapshot.frameSequenceGapEvents
           << ",\"skippedFrameSequences\":" << snapshot.skippedFrameSequences
           << ",\"duplicateFrameSequences\":" << snapshot.duplicateFrameSequences
           << ",\"reorderedFrameSequences\":" << snapshot.reorderedFrameSequences
           << ",\"admittedFrameSequenceFps\":";
    WriteOptionalNumber(stream, snapshot.admittedFrameSequenceFps);
    stream << ",\"telemetryBootstrapAttempts\":" << snapshot.telemetryBootstrapAttempts
           << ",\"telemetryBootstrapSuccesses\":" << snapshot.telemetryBootstrapSuccesses
           << ",\"bootstrapSuccessRate\":";
    WriteOptionalNumber(stream, snapshot.bootstrapSuccessRate);
    stream << ",\"bootstrapAcceptedFrames\":" << snapshot.bootstrapAcceptedFrames
           << ",\"bootstrapRejectedFrames\":" << snapshot.bootstrapRejectedFrames
           << ",\"bootstrapMismatchFrames\":" << snapshot.bootstrapMismatchFrames
           << ",\"bootstrapControlFrameFailures\":" << snapshot.bootstrapControlFrameFailures
           << ",\"evaluatedDataFrames\":" << snapshot.evaluatedDataFrames
           << ",\"postFecFailedFrames\":" << snapshot.postFecFailedFrames
           << ",\"acceptedTransportBlocks\":" << snapshot.acceptedTransportBlocks
           << ",\"comparedCodedBits\":" << snapshot.comparedCodedBits
           << ",\"erroneousCodedBits\":" << snapshot.erroneousCodedBits
           << ",\"fecFailures\":" << snapshot.fecFailures
           << ",\"crcFailures\":" << snapshot.crcFailures
           << ",\"identityFailures\":" << snapshot.identityFailures
           << ",\"falseAcceptedCodewords\":" << snapshot.falseAcceptedCodewords
           << ",\"preFecBerEstimate\":";
    WriteOptionalNumber(stream, snapshot.preFecBerEstimate);
    stream << ",\"fecFrameErrorRate\":";
    WriteOptionalNumber(stream, snapshot.fecFrameErrorRate);
    stream << ",\"frameLeaseHighWater\":" << snapshot.frameLeaseHighWater
           << ",\"demodPendingHighWater\":" << snapshot.demodPendingHighWater
           << ",\"resultQueueHighWater\":" << snapshot.resultQueueHighWater
           << ",\"staleResultDrops\":" << snapshot.staleResultDrops
           << ",\"roiGpuTimeTotal100ns\":" << snapshot.roiGpuTimeTotal100ns
           << ",\"demodGpuTimeTotal100ns\":" << snapshot.demodGpuTimeTotal100ns
           << ",\"bootstrapCpuTimeTotal100ns\":" << snapshot.bootstrapCpuTimeTotal100ns
           << ",\"postGpuFecCpuTimeTotal100ns\":" << snapshot.postGpuFecCpuTimeTotal100ns
           << ",\"wholeFileDigestVerified\":" << snapshot.wholeFileDigestVerified
           << ",\"finalPublishSucceeded\":" << snapshot.finalPublishSucceeded
           << ",\"wholeFileDigest\":";
    WriteEscaped(stream, snapshot.wholeFileDigestHex);
    stream << ",\"outputPath\":";
    WriteEscaped(stream, snapshot.outputPath);
    stream << ",\"recoveryRuntimeMilliseconds\":" << snapshot.recoveryRuntimeMilliseconds
           << ",\"remoteMetadata\":";
    WriteRemoteMetadata(stream, snapshot.remoteMetadata);
    stream << ",\"statusMessage\":";
    WriteEscaped(stream, snapshot.statusMessage);
    stream << ",\"errorDetail\":";
    WriteEscaped(stream, snapshot.errorDetail);
    stream << '}';
    return stream.str();
}

std::string BuildEncoderDiagnostics(const EncoderSnapshot& snapshot)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "PixelBridge Encoder\nState: " << GetEncoderStateName(snapshot.state)
           << "\nRunId: " << snapshot.runId << "\nSessionTag: " << snapshot.sessionTag
           << "\nProfile: " << GetVisualProfileName(snapshot.visualProfile)
           << "\nCompression: " << GetCompressionCodecName(snapshot.compressionCodec)
           << "\nOuter FEC: " << GetOuterFecModeName(snapshot.outerFecMode)
           << "\nBroadcast runtime ms: " << snapshot.broadcastRuntimeMilliseconds
           << "\nCarousel cycle/position: " << snapshot.cycleCount << '/' << snapshot.cyclePosition
           << " of " << snapshot.cycleFrameCount << "\nFrameSequence: " << snapshot.frameSequence
           << "\nStatus: " << snapshot.statusMessage << "\nError: " << snapshot.errorDetail
           << "\nReceiver progress/ETA: unavailable by architecture\n";
    return stream.str();
}

std::string BuildDecoderDiagnostics(const DecoderSnapshot& snapshot)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "PixelBridge Decoder\nState: " << GetDecoderStateName(snapshot.state)
           << "\nRunId: " << snapshot.runId << "\nSessionTag: " << snapshot.sessionTag
           << "\nBackend: " << GetCaptureBackendName(snapshot.requestedBackend) << " requested; ";
    if (snapshot.actualBackend)
    {
        stream << GetCaptureBackendName(*snapshot.actualBackend);
    }
    else
    {
        stream << "not established";
    }
    stream << " actual\nVerified raw bytes: " << snapshot.verifiedRawBytes << '/' << snapshot.originalFileBytes
           << "\nSmoothed verified raw goodput B/s (ETA basis): " << snapshot.smoothedVerifiedRawGoodputBytesPerSecond
           << "\nVerified encoded bytes: " << snapshot.verifiedEncodedBytes << " goodput bit/s=";
    if (snapshot.verifiedEncodedGoodputBitsPerSecond)
    {
        stream << *snapshot.verifiedEncodedGoodputBitsPerSecond;
    }
    else
    {
        stream << "unavailable until WholeFileDigest/final publish";
    }
    stream
           << "\nCaptureEpoch: " << snapshot.captureEpoch << " resets=" << snapshot.captureEpochResets
           << "\nWholeFileDigest: " << (snapshot.wholeFileDigestVerified ? "PASS" : "not accepted")
           << "\nFinal publish: " << (snapshot.finalPublishSucceeded ? "success" : "not published")
           << "\nOutput: " << snapshot.outputPath << "\nStatus: " << snapshot.statusMessage
           << "\nError: " << snapshot.errorDetail << '\n';
    return stream.str();
}

} // namespace pbapp
