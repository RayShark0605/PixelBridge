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

void WriteOptionalRect(std::ostream& stream, const std::optional<MetadataPhysicalRect>& rectangle)
{
    if (!rectangle)
    {
        stream << "null";
        return;
    }
    stream << "{\"left\":" << rectangle->left << ",\"top\":" << rectangle->top
           << ",\"right\":" << rectangle->right << ",\"bottom\":" << rectangle->bottom << '}';
}

void WriteObservedLocatorGeometry(std::ostream& stream, const ObservedLocatorGeometrySnapshot& geometry)
{
    stream << "{\"authority\":\"AcceptedBootstrapLocatorPixels\",\"samples\":" << geometry.samples
           << ",\"lastOriginX\":";
    WriteOptionalNumber(stream, geometry.lastOriginX);
    stream << ",\"lastOriginY\":";
    WriteOptionalNumber(stream, geometry.lastOriginY);
    stream << ",\"lastScaleX\":";
    WriteOptionalNumber(stream, geometry.lastScaleX);
    stream << ",\"lastScaleY\":";
    WriteOptionalNumber(stream, geometry.lastScaleY);
    stream << ",\"lastMarkerResidualPixels\":";
    WriteOptionalNumber(stream, geometry.lastMarkerResidualPixels);
    stream << ",\"minimumOriginX\":";
    WriteOptionalNumber(stream, geometry.minimumOriginX);
    stream << ",\"maximumOriginX\":";
    WriteOptionalNumber(stream, geometry.maximumOriginX);
    stream << ",\"minimumOriginY\":";
    WriteOptionalNumber(stream, geometry.minimumOriginY);
    stream << ",\"maximumOriginY\":";
    WriteOptionalNumber(stream, geometry.maximumOriginY);
    stream << ",\"minimumScaleX\":";
    WriteOptionalNumber(stream, geometry.minimumScaleX);
    stream << ",\"maximumScaleX\":";
    WriteOptionalNumber(stream, geometry.maximumScaleX);
    stream << ",\"minimumScaleY\":";
    WriteOptionalNumber(stream, geometry.minimumScaleY);
    stream << ",\"maximumScaleY\":";
    WriteOptionalNumber(stream, geometry.maximumScaleY);
    stream << ",\"minimumMarkerResidualPixels\":";
    WriteOptionalNumber(stream, geometry.minimumMarkerResidualPixels);
    stream << ",\"maximumMarkerResidualPixels\":";
    WriteOptionalNumber(stream, geometry.maximumMarkerResidualPixels);
    stream << ",\"maximumScaleAnisotropy\":";
    WriteOptionalNumber(stream, geometry.maximumScaleAnisotropy);
    stream << '}';
}

void WriteRemoteMetadata(std::ostream& stream, const RemoteRunMetadata& metadata)
{
    stream << "{\"schema\":\"PixelBridge.RemoteVisualRunMetadata.1\",\"runId\":";
    WriteEscaped(stream, metadata.runId);
    stream << ",\"channelType\":";
    WriteEscaped(stream, metadata.channelType == ChannelType::LocalDesktop ? "LocalDesktop" :
        metadata.channelType == ChannelType::RemoteVisual ? "RemoteVisual" : "Other");
    stream << ",\"remoteProvider\":";
    WriteEscaped(stream, metadata.remoteProvider);
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
    stream << ",\"computerBDisplayResolution\":";
    WriteEscaped(stream, metadata.computerBDisplayResolution);
    stream << ",\"computerBRefreshRate\":";
    WriteOptionalNumber(stream, metadata.computerBRefreshRate);
    stream << ",\"computerADisplayResolution\":";
    WriteEscaped(stream, metadata.computerADisplayResolution);
    stream << ",\"computerARefreshRate\":";
    WriteOptionalNumber(stream, metadata.computerARefreshRate);
    stream << ",\"remoteResolution\":";
    WriteEscaped(stream, metadata.remoteResolution);
    stream << ",\"remoteWindowPhysicalRect\":";
    WriteOptionalRect(stream, metadata.remoteWindowPhysicalRect);
    stream << ",\"selectedRoiPhysicalRect\":";
    WriteOptionalRect(stream, metadata.selectedRoiPhysicalRect);
    stream << ",\"estimatedScaleX\":";
    WriteOptionalNumber(stream, metadata.estimatedScaleX);
    stream << ",\"estimatedScaleY\":";
    WriteOptionalNumber(stream, metadata.estimatedScaleY);
    stream << ",\"letterboxStatus\":";
    WriteEscaped(stream, metadata.letterboxStatus);
    stream << ",\"cropStatus\":";
    WriteEscaped(stream, metadata.cropStatus);
    stream << ",\"geometryStatus\":";
    WriteEscaped(stream, metadata.geometryStatus);
    stream << ",\"networkType\":";
    WriteEscaped(stream, metadata.networkType);
    stream << ",\"observedBandwidthMbps\":";
    WriteOptionalNumber(stream, metadata.observedBandwidthMbps);
    stream << ",\"observedLatencyMilliseconds\":";
    WriteOptionalNumber(stream, metadata.observedLatencyMilliseconds);
    stream << ",\"protectedMonitorIdentity\":";
    WriteEscaped(stream, metadata.protectedMonitorIdentity);
    stream << ",\"experimentMonitorIdentity\":";
    WriteEscaped(stream, metadata.experimentMonitorIdentity);
    stream << ",\"remoteUiProvenance\":";
    WriteEscaped(stream, GetMetadataProvenanceName(metadata.remoteUiProvenance));
    stream << ",\"geometryProvenance\":";
    WriteEscaped(stream, GetMetadataProvenanceName(metadata.geometryProvenance));
    stream << ",\"networkProvenance\":";
    WriteEscaped(stream, GetMetadataProvenanceName(metadata.networkProvenance));
    stream << ",\"notes\":";
    WriteEscaped(stream, metadata.notes);
    stream << '}';
}

void WriteContext(std::ostream& stream, const RunReportContext& context)
{
    stream << "\"schema\":\"PixelBridge.RunReport.2\",\"applicationName\":";
    WriteEscaped(stream, context.applicationName);
    stream << ",\"applicationVersion\":";
    WriteEscaped(stream, context.applicationVersion);
    stream << ",\"gitCommit\":";
    WriteEscaped(stream, context.gitCommit);
    stream << ",\"exportedAtUtc\":";
    WriteEscaped(stream, context.exportedAtUtc);
}

} // namespace

std::string BuildRemoteVisualRunMetadataJson(const RemoteRunMetadata& metadata)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::boolalpha << std::setprecision(17);
    WriteRemoteMetadata(stream, metadata);
    return stream.str();
}

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
    stream << ",\"visualProfileId\":" << snapshot.visualProfileId
           << ",\"visualLayoutVersion\":" << static_cast<unsigned int>(snapshot.visualLayoutVersion)
           << ",\"codedDataBytesPerFrame\":" << snapshot.codedDataBytesPerFrame
           << ",\"codewordsPerFrame\":" << snapshot.codewordsPerFrame
           << ",\"capacityModel\":{\"rawVisualBitsPerLogicalFrame\":" << snapshot.rawVisualBitsPerLogicalFrame
           << ",\"innerFecInformationBytesPerLogicalFrame\":" << snapshot.innerFecInformationBytesPerLogicalFrame
           << ",\"transportPayloadCeilingBytesPerLogicalFrame\":" << snapshot.transportPayloadCeilingBytesPerLogicalFrame
           << ",\"configuredTransportPayloadCeilingBytesPerSecond\":";
    WriteOptionalNumber(stream, snapshot.configuredTransportPayloadCeilingBytesPerSecond);
    stream << ",\"basis\":\"profile constants multiplied by configured logical visual FPS; excludes Control dilution, retransmission, capture loss, FEC rejection, Receiver verification and publish\"}";
    stream << ",\"compressionCodec\":";
    WriteEscaped(stream, GetCompressionCodecName(snapshot.compressionCodec));
    stream << ",\"outerFec\":";
    WriteEscaped(stream, GetOuterFecModeName(snapshot.outerFecMode));
    stream << ",\"innerFec\":\"Robust DVB-S2 Short QC-LDPC\""
           << ",\"outerBlockCount\":" << snapshot.outerBlockCount
           << ",\"dataWindow\":{\"left\":" << snapshot.dataWindowLeft
           << ",\"top\":" << snapshot.dataWindowTop
           << ",\"width\":" << snapshot.dataWindowWidth
           << ",\"height\":" << snapshot.dataWindowHeight
           << ",\"singleMonitorFullscreen\":" << snapshot.singleMonitorFullscreen << '}';
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
    WriteOptionalNumber(stream, snapshot.generatedVisualFramesPerSecond);
    stream << ",\"generatedVisualFramesPerSecondBasis\":\"logical source replacements; (N-1)/(last-first), with the initial frame excluded as an elapsed interval\""
           << ",\"generatedPayloadBytesPerSecond\":";
    WriteOptionalNumber(stream, snapshot.generatedPayloadBytesPerSecond);
    stream << ",\"generatedPayloadBytesPerSecondBasis\":\"actual Outer-FEC payload bytes encoded into Data frames divided by broadcast runtime; Control frames contribute zero; not Receiver-verified goodput\""
           << ",\"configuredLogicalVisualFps\":" << snapshot.configuredLogicalVisualFps
           << ",\"configuredLogicalDwellMilliseconds\":";
    WriteOptionalNumber(stream, snapshot.configuredLogicalDwellMilliseconds);
    stream << ",\"minimumObservedLogicalDwellMilliseconds\":";
    WriteOptionalNumber(stream, snapshot.minimumObservedLogicalDwellMilliseconds);
    stream << ",\"logicalDwellViolationCount\":" << snapshot.logicalDwellViolationCount
           << ",\"configuredControlRepetitions\":" << snapshot.configuredControlRepetitions
           << ",\"pendingFrames\":" << snapshot.pendingFrames
           << ",\"pendingHighWater\":" << snapshot.pendingHighWater
           << ",\"submittedFrames\":" << snapshot.submittedFrames
           << ",\"replacedPendingFrames\":" << snapshot.replacedPendingFrames
           << ",\"sourceTextureReplacements\":" << snapshot.sourceTextureReplacements
           << ",\"repeatedPresentCalls\":" << snapshot.repeatedPresentCalls
           << ",\"invalidatedActiveFrames\":" << snapshot.invalidatedActiveFrames
           << ",\"activeFrame\":" << snapshot.activeFrame
           << ",\"activeFrameSequence\":" << snapshot.activeFrameSequence
           << ",\"candidateContractSatisfied\":" << snapshot.candidateContractSatisfied
           << ",\"sourceStable\":" << snapshot.sourceStable
           << ",\"wholeFileDigest\":";
    WriteEscaped(stream, snapshot.wholeFileDigestHex);
    stream << ",\"processCpuAveragePercent\":";
    WriteOptionalNumber(stream, snapshot.processCpuAveragePercent);
    stream << ",\"processCpuPeakPercent\":";
    WriteOptionalNumber(stream, snapshot.processCpuPeakPercent);
    stream << ",\"processCpuEquivalentCores\":";
    WriteOptionalNumber(stream, snapshot.processCpuEquivalentCores);
    stream << ",\"processCpuUnavailableReason\":";
    WriteEscaped(stream, snapshot.processCpuAveragePercent ? "" : snapshot.processCpuUnavailableReason);
    stream << ",\"processGpuEngineAveragePercent\":";
    WriteOptionalNumber(stream, snapshot.processGpuEngineAveragePercent);
    stream << ",\"processGpuEnginePeakPercent\":";
    WriteOptionalNumber(stream, snapshot.processGpuEnginePeakPercent);
    stream << ",\"processGpuUnavailableReason\":";
    WriteEscaped(stream, snapshot.processGpuEngineAveragePercent || snapshot.processGpuEnginePeakPercent ?
        "" : snapshot.processGpuUnavailableReason);
    stream << ",\"evidence\":{\"journalEnabled\":" << snapshot.journalEnabled
           << ",\"valid\":" << snapshot.evidenceValid
           << ",\"journalTruncated\":" << snapshot.journalTruncated
           << ",\"journalFinished\":" << snapshot.journalFinished
           << ",\"journalSamples\":" << snapshot.journalSamples
           << ",\"journalBytes\":" << snapshot.journalBytes
           << ",\"invalidReason\":";
    WriteEscaped(stream, snapshot.evidenceInvalidReason);
    stream << '}';
    stream << ",\"monitorSafety\":{\"preflightPassed\":" << snapshot.monitorSafetyPreflightPassed
           << ",\"revalidationCount\":" << snapshot.monitorSafetyRevalidationCount
           << ",\"status\":";
    WriteEscaped(stream, snapshot.monitorSafetyStatus);
    stream << '}';
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
    stream << ",\"visualProfileId\":" << snapshot.visualProfileId
           << ",\"visualLayoutVersion\":" << static_cast<unsigned int>(snapshot.visualLayoutVersion)
           << ",\"codedDataBytesPerFrame\":" << snapshot.codedDataBytesPerFrame
           << ",\"codewordsPerFrame\":" << snapshot.codewordsPerFrame
           << ",\"sessionId\":";
    WriteEscaped(stream, snapshot.sessionIdHex);
    stream << ",\"sessionTag\":" << snapshot.sessionTag
           << ",\"descriptorKnown\":" << snapshot.descriptorKnown
           << ",\"originalFileBytes\":" << snapshot.originalFileBytes
           << ",\"largeOutputConfirmationState\":";
    WriteEscaped(stream, GetLargeOutputConfirmationStateName(snapshot.largeOutputConfirmationState));
    stream << ",\"largeOutputConfirmationRequestId\":" << snapshot.largeOutputConfirmationRequestId
           << ",\"largeOutputConfirmationFileNameUtf8\":";
    WriteEscaped(stream, snapshot.largeOutputConfirmationFileNameUtf8);
    stream << ",\"outputAvailableBytesBeforeReservation\":" << snapshot.outputAvailableBytesBeforeReservation
           << ",\"outputRequestedAllocationBytes\":" << snapshot.outputRequestedAllocationBytes
           << ",\"outputActualAllocationBytes\":" << snapshot.outputActualAllocationBytes
           << ",\"outputPreallocationAttempted\":" << snapshot.outputPreallocationAttempted
           << ",\"outputPreallocationFullyAllocated\":" << snapshot.outputPreallocationFullyAllocated
           << ",\"outputFileSparse\":" << snapshot.outputFileSparse
           << ",\"outputFileCompressed\":" << snapshot.outputFileCompressed
           << ",\"outputVolumeSupportsSparseFiles\":" << snapshot.outputVolumeSupportsSparseFiles
           << ",\"outputVolumeSupportsCompression\":" << snapshot.outputVolumeSupportsCompression
           << ",\"outputVolumeCompressed\":" << snapshot.outputVolumeCompressed
           << ",\"outputRecoveredAfterPublish\":" << snapshot.outputRecoveredAfterPublish
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
           << ",\"captureCopiedFrames\":" << snapshot.captureCopiedFrames
           << ",\"captureDeliveredFrames\":" << snapshot.captureDeliveredFrames
           << ",\"captureDroppedFrames\":" << snapshot.captureDroppedFrames
           << ",\"captureAcquireTimeouts\":" << snapshot.captureAcquireTimeouts
           << ",\"capturePointerOnlyFrames\":" << snapshot.capturePointerOnlyFrames
           << ",\"captureAccumulatedFrames\":" << snapshot.captureAccumulatedFrames
           << ",\"captureAccessLostEvents\":" << snapshot.captureAccessLostEvents
           << ",\"captureExpiredFrames\":" << snapshot.captureExpiredFrames
           << ",\"captureStaleFrames\":" << snapshot.captureStaleFrames
           << ",\"captureCursorErasures\":" << snapshot.captureCursorErasures
           << ",\"captureFrameAgeHighWater100ns\":" << snapshot.captureFrameAgeHighWater100ns
           << ",\"captureReadbackDropEvents\":" << snapshot.captureReadbackDropEvents
           << ",\"captureRecreates\":" << snapshot.captureRecreates
           << ",\"captureDeviceRecoveries\":" << snapshot.captureDeviceRecoveries
           << ",\"telemetryCapturedFrames\":" << snapshot.telemetryCapturedFrames
           << ",\"telemetryDroppedFrames\":" << snapshot.telemetryDroppedFrames
           << ",\"fingerprintedFrames\":" << snapshot.fingerprintedFrames
           << ",\"captureFps\":";
    WriteOptionalNumber(stream, snapshot.captureFps);
    stream << ",\"uniqueVisualFps\":";
    WriteOptionalNumber(stream, snapshot.uniqueVisualFps);
    stream << ",\"uniqueVisualFpsBasis\":\"distinct legal (CaptureEpoch,SessionTag,FrameSequence)\""
           << ",\"roiPixelDigestUniqueVisualFps\":";
    WriteOptionalNumber(stream, snapshot.roiPixelDigestUniqueVisualFps);
    stream << ",\"roiPixelDigestAvailability\":";
    WriteEscaped(stream, snapshot.fingerprintedFrames == 0 ?
        "Unavailable: production D3D11 fast path has no full-ROI CPU pixel digest" : "Available");
    stream << ",\"frameSequenceGapEvents\":" << snapshot.frameSequenceGapEvents
           << ",\"skippedFrameSequences\":" << snapshot.skippedFrameSequences
           << ",\"duplicateFrameSequences\":" << snapshot.duplicateFrameSequences
           << ",\"reorderedFrameSequences\":" << snapshot.reorderedFrameSequences
           << ",\"admittedFrameSequenceFps\":";
    WriteOptionalNumber(stream, snapshot.admittedFrameSequenceFps);
    stream << ",\"telemetryBootstrapAttempts\":" << snapshot.telemetryBootstrapAttempts
           << ",\"telemetryBootstrapSuccesses\":" << snapshot.telemetryBootstrapSuccesses
           << ",\"bootstrapSuccessRate\":";
    WriteOptionalNumber(stream, snapshot.bootstrapSuccessRate);
    stream << ",\"observedLocatorGeometry\":";
    WriteObservedLocatorGeometry(stream, snapshot.observedLocatorGeometry);
    stream << ",\"bootstrapAcceptedFrames\":" << snapshot.bootstrapAcceptedFrames
           << ",\"bootstrapRejectedFrames\":" << snapshot.bootstrapRejectedFrames
           << ",\"bootstrapMismatchFrames\":" << snapshot.bootstrapMismatchFrames
           << ",\"bootstrapControlFrameFailures\":" << snapshot.bootstrapControlFrameFailures
           << ",\"evaluatedDataFrames\":" << snapshot.evaluatedDataFrames
           << ",\"evaluatedCodewords\":" << snapshot.evaluatedCodewords
           << ",\"postFecFailedFrames\":" << snapshot.postFecFailedFrames
           << ",\"fecAcceptedTransportBlocks\":" << snapshot.fecAcceptedTransportBlocks
           << ",\"fecAcceptedTransportBlockRate\":";
    WriteOptionalNumber(stream, snapshot.fecAcceptedTransportBlockRate);
    stream << ",\"acceptedTransportBlocks\":" << snapshot.acceptedTransportBlocks
           << ",\"temporallyAdmittedTransportBlocks\":" << snapshot.temporallyAdmittedTransportBlocks
           << ",\"acceptedTransportBlocksBasis\":\"Receiver-bound unique temporal admission; raw repeated FEC acceptance is fecAcceptedTransportBlocks\""
           << ",\"outerAdmission\":{\"uniqueSymbols\":" << snapshot.outerUniqueSymbols
           << ",\"identicalDuplicateSymbols\":" << snapshot.outerIdenticalDuplicateSymbols
           << ",\"recoveryAlreadyReadySymbols\":" << snapshot.outerRecoveryAlreadyReadySymbols
           << ",\"alreadyCompletedSymbols\":" << snapshot.outerAlreadyCompletedSymbols
           << ",\"recoveryReadyEvents\":" << snapshot.outerRecoveryReadyEvents
           << ",\"resourceRejections\":" << snapshot.outerResourceRejections
           << ",\"conflictRejections\":" << snapshot.outerConflictRejections << '}'
           << ",\"comparedCodedBits\":" << snapshot.comparedCodedBits
           << ",\"erroneousCodedBits\":" << snapshot.erroneousCodedBits
           << ",\"fecFailures\":" << snapshot.fecFailures
           << ",\"crcFailures\":" << snapshot.crcFailures
           << ",\"identityFailures\":" << snapshot.identityFailures
           << ",\"falseAcceptedCodewords\":";
    if (snapshot.falseAcceptedCodewordsAvailable)
    {
        stream << snapshot.falseAcceptedCodewords;
    }
    else
    {
        stream << "null";
    }
    stream << ",\"falseAcceptedCodewordsUnavailableReason\":";
    WriteEscaped(stream, snapshot.falseAcceptedCodewordsAvailable ? "" : snapshot.falseAcceptedCodewordsUnavailableReason);
    stream << ",\"endToEndUniqueFrameSequences\":" << snapshot.endToEndUniqueFrameSequences
           << ",\"endToEndUniqueVisualFps\":";
    WriteOptionalNumber(stream, snapshot.endToEndUniqueVisualFps);
    stream << ",\"remoteDuplicateRefinementAttempts\":" << snapshot.remoteDuplicateRefinementAttempts
           << ",\"remoteDuplicateRefinementRecoveries\":" << snapshot.remoteDuplicateRefinementRecoveries
           << ",\"remoteMetricTelemetry\":{\"frames\":" << snapshot.remoteMetricFrames
           << ",\"samples\":" << snapshot.remoteMetricSamples
           << ",\"zeroMagnitudeMetrics\":" << snapshot.remoteZeroMagnitudeMetrics
           << ",\"zeroMagnitudeRate\":";
    WriteOptionalNumber(stream, snapshot.remoteZeroMagnitudeMetricRate);
    stream << ",\"minimumAbsoluteMetric\":";
    WriteOptionalNumber(stream, snapshot.remoteMinimumAbsoluteMetric);
    stream << ",\"meanAbsoluteMetric\":";
    WriteOptionalNumber(stream, snapshot.remoteMeanAbsoluteMetric);
    stream << ",\"verifiedFrames\":" << snapshot.remoteVerifiedMetricFrames
           << ",\"rejectedFrames\":" << snapshot.remoteRejectedMetricFrames
           << ",\"transportEvaluatedFrames\":";
    if (snapshot.remoteVerifiedMetricFrames <= snapshot.remoteMetricFrames &&
        snapshot.remoteRejectedMetricFrames <= snapshot.remoteMetricFrames - snapshot.remoteVerifiedMetricFrames)
    {
        stream << snapshot.remoteVerifiedMetricFrames + snapshot.remoteRejectedMetricFrames;
    }
    else
    {
        stream << "null";
    }
    stream << ",\"nonTransportFrames\":";
    if (snapshot.remoteVerifiedMetricFrames <= snapshot.remoteMetricFrames &&
        snapshot.remoteRejectedMetricFrames <= snapshot.remoteMetricFrames - snapshot.remoteVerifiedMetricFrames)
    {
        stream << snapshot.remoteMetricFrames - snapshot.remoteVerifiedMetricFrames - snapshot.remoteRejectedMetricFrames;
    }
    else
    {
        stream << "null";
    }
    stream << ",\"symbolSamples\":" << snapshot.remoteSymbolSamples
           << ",\"unreliableSymbols\":" << snapshot.remoteUnreliableSymbols
           << ",\"unreliableSymbolRate\":";
    WriteOptionalNumber(stream, snapshot.remoteUnreliableSymbolRate);
    stream << ",\"verifiedMeanAbsoluteMetric\":";
    WriteOptionalNumber(stream, snapshot.remoteVerifiedMeanAbsoluteMetric);
    stream << ",\"rejectedMeanAbsoluteMetric\":";
    WriteOptionalNumber(stream, snapshot.remoteRejectedMeanAbsoluteMetric);
    stream << ",\"rejectedZeroMagnitudeRate\":";
    WriteOptionalNumber(stream, snapshot.remoteRejectedZeroMagnitudeMetricRate);
    stream << ",\"freshnessRegions\":" << snapshot.remoteFreshnessRegions
           << ",\"freshRegions\":" << snapshot.remoteFreshRegions
           << ",\"staleRegions\":" << snapshot.remoteStaleRegions
           << ",\"staleRegionRate\":";
    WriteOptionalNumber(stream, snapshot.remoteStaleRegionRate);
    stream << ",\"framesWithStaleRegions\":" << snapshot.remoteFramesWithStaleRegions
           << ",\"freshnessTagMismatches\":" << snapshot.remoteFreshnessTagMismatches
           << ",\"freshnessTagErasures\":" << snapshot.remoteFreshnessTagErasures
           << ",\"freshnessErasedDataMetrics\":" << snapshot.remoteFreshnessErasedDataMetrics
           << ",\"freshnessErasedDataMetricRate\":";
    WriteOptionalNumber(stream, snapshot.remoteFreshnessErasedDataMetricRate);
    stream << ",\"observationBasis\":\"profile-validated metric-bearing demodulation observations; bounded duplicate refinements may add signal samples but not FEC or Receiver admission samples\"";
    stream << ",\"highConfidenceWrongCodewords\":null"
           << ",\"highConfidenceWrongUnavailableReason\":";
    WriteEscaped(stream, "Production receive has no independent per-codeword truth oracle; use failed-frame confidence jointly with sealed Replay evidence");
    stream << ",\"unavailableReason\":";
    WriteEscaped(stream, snapshot.remoteMetricFrames == 0 ?
        "No RemoteVisual metric-bearing frame was demodulated" : "");
    stream << '}'
           << ",\"captureStall\":{\"count\":" << snapshot.captureStallCount
           << ",\"totalMilliseconds\":" << snapshot.captureStallTotalMilliseconds
           << ",\"maximumMilliseconds\":" << snapshot.captureStallMaximumMilliseconds
           << ",\"active\":" << snapshot.captureStallActive << '}'
           << ",\"visualStall\":{\"count\":" << snapshot.visualStallCount
           << ",\"totalMilliseconds\":" << snapshot.visualStallTotalMilliseconds
           << ",\"maximumMilliseconds\":" << snapshot.visualStallMaximumMilliseconds
           << ",\"active\":" << snapshot.visualStallActive << '}'
           << ",\"processCpuAveragePercent\":";
    WriteOptionalNumber(stream, snapshot.processCpuAveragePercent);
    stream << ",\"processCpuPeakPercent\":";
    WriteOptionalNumber(stream, snapshot.processCpuPeakPercent);
    stream << ",\"processCpuEquivalentCores\":";
    WriteOptionalNumber(stream, snapshot.processCpuEquivalentCores);
    stream << ",\"processCpuUnavailableReason\":";
    WriteEscaped(stream, snapshot.processCpuAveragePercent ? "" : snapshot.processCpuUnavailableReason);
    stream << ",\"processGpuEngineAveragePercent\":";
    WriteOptionalNumber(stream, snapshot.processGpuEngineAveragePercent);
    stream << ",\"processGpuEnginePeakPercent\":";
    WriteOptionalNumber(stream, snapshot.processGpuEnginePeakPercent);
    stream << ",\"processGpuUnavailableReason\":";
    WriteEscaped(stream, snapshot.processGpuEngineAveragePercent || snapshot.processGpuEnginePeakPercent ?
        "" : snapshot.processGpuUnavailableReason);
    stream << ",\"evidence\":{\"journalEnabled\":" << snapshot.journalEnabled
           << ",\"valid\":" << snapshot.evidenceValid
           << ",\"journalTruncated\":" << snapshot.journalTruncated
           << ",\"journalFinished\":" << snapshot.journalFinished
           << ",\"journalSamples\":" << snapshot.journalSamples
           << ",\"journalBytes\":" << snapshot.journalBytes
           << ",\"invalidReason\":";
    WriteEscaped(stream, snapshot.evidenceInvalidReason);
    stream << '}';
    stream << ",\"monitorSafety\":{\"preflightPassed\":" << snapshot.monitorSafetyPreflightPassed
           << ",\"revalidationCount\":" << snapshot.monitorSafetyRevalidationCount
           << ",\"status\":";
    WriteEscaped(stream, snapshot.monitorSafetyStatus);
    stream << '}';
    stream << ",\"replay\":{\"enabled\":" << snapshot.replayEnabled
           << ",\"diagnosticOnly\":" << snapshot.replayDiagnosticOnly
           << ",\"captureOnly\":" << snapshot.replayCaptureOnly
           << ",\"offlineMode\":" << snapshot.replayOfflineMode
           << ",\"evidenceValid\":" << snapshot.replayEvidenceValid
           << ",\"finalized\":" << snapshot.replayFinalized
           << ",\"writtenFrames\":" << snapshot.replayWrittenFrames
           << ",\"droppedFrames\":" << snapshot.replayDroppedFrames
           << ",\"sampledOutFrames\":" << snapshot.replaySampledOutFrames
           << ",\"maximumCaptureFramesPerSecond\":" << snapshot.replayMaximumCaptureFramesPerSecond
           << ",\"samplingInterval100ns\":" << snapshot.replaySamplingInterval100ns
           << ",\"writtenDemodObservations\":" << snapshot.replayWrittenDemodObservations
           << ",\"droppedDemodObservations\":" << snapshot.replayDroppedDemodObservations
           << ",\"queueHighWater\":" << snapshot.replayQueueHighWater
           << ",\"fileBytes\":" << snapshot.replayFileBytes
           << ",\"offlineCaptureFrames\":" << snapshot.replayOfflineCaptureFrames
           << ",\"offlineDemodResults\":" << snapshot.replayOfflineDemodResults
           << ",\"offlineObservationComparisons\":" << snapshot.replayOfflineObservationComparisons
           << ",\"offlineObservationMismatches\":" << snapshot.replayOfflineObservationMismatches
           << ",\"path\":";
    WriteEscaped(stream, snapshot.replayPath);
    stream << ",\"error\":";
    WriteEscaped(stream, snapshot.replayError);
    stream << '}';
    stream << ",\"preFecBerEstimate\":";
    WriteOptionalNumber(stream, snapshot.preFecBerEstimate);
    stream << ",\"fecFrameErrorRate\":";
    WriteOptionalNumber(stream, snapshot.fecFrameErrorRate);
    stream << ",\"fecCodewordFailureRate\":";
    WriteOptionalNumber(stream, snapshot.fecCodewordFailureRate);
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
           << "\nLarge output confirmation: " << GetLargeOutputConfirmationStateName(
               snapshot.largeOutputConfirmationState)
           << " requestId=" << snapshot.largeOutputConfirmationRequestId
           << "\nOutput allocation: requested=" << snapshot.outputRequestedAllocationBytes
           << " actual=" << snapshot.outputActualAllocationBytes
           << " sparse=" << (snapshot.outputFileSparse ? "true" : "false")
           << " compressed=" << (snapshot.outputFileCompressed ? "true" : "false")
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
