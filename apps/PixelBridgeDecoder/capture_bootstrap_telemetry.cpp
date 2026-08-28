#include "capture_bootstrap_telemetry.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace pbdecoder
{
using namespace pbcapturenormalize;
namespace
{
void WriteDomain(std::ostream& stream, const ScreenCaptureDomain& domain)
{
    constexpr char digits[] = "0123456789abcdef";
    stream << "{\"sourceId\":\"";
    for (const auto value : domain.sourceId)
    {
        const auto byte = std::to_integer<unsigned int>(value);
        stream << digits[byte >> 4] << digits[byte & 15];
    }
    stream << "\",\"captureEpoch\":\"" << domain.captureEpoch << "\"}";
}

void WriteNumber(std::ostream& stream, const double number)
{
    if (std::isfinite(number))
    {
        stream << number;
    }
    else
    {
        stream << "null";
    }
}

const char* BackendName(const CaptureBackendKind backend) noexcept
{
    return backend == CaptureBackendKind::Wgc ? "wgc" : "dxgi";
}

const char* StateName(const CaptureState state) noexcept
{
    switch (state)
    {
    case CaptureState::Starting: return "Starting";
    case CaptureState::Running: return "Running";
    case CaptureState::Draining: return "Draining";
    case CaptureState::Recreating: return "Recreating";
    case CaptureState::Stopped: return "Stopped";
    case CaptureState::Failed: return "Failed";
    case CaptureState::WaitingForEnvironment: return "WaitingForEnvironment";
    }
    return "Unknown";
}

const char* RebuildName(const CaptureRebuildReason reason) noexcept
{
    switch (reason)
    {
    case CaptureRebuildReason::None: return "None";
    case CaptureRebuildReason::Requested: return "Requested";
    case CaptureRebuildReason::AccessLost: return "AccessLost";
    case CaptureRebuildReason::DisplayChanged: return "DisplayChanged";
    case CaptureRebuildReason::DesktopChanged: return "DesktopChanged";
    case CaptureRebuildReason::SourceDescriptionChanged: return "SourceDescriptionChanged";
    case CaptureRebuildReason::DeviceLost: return "DeviceLost";
    case CaptureRebuildReason::UnavailableEnvironment: return "UnavailableEnvironment";
    }
    return "Unknown";
}

void Configure(std::ostringstream& stream)
{
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.imbue(std::locale::classic());
    stream << std::boolalpha << std::setprecision(17);
}

void WriteStatus(std::ostream& stream, const CaptureStatus& status)
{
    stream << "{\"code\":\"" << GetCaptureErrorName(status.code) << "\",\"stage\":" << static_cast<unsigned int>(status.stage)
           << ",\"native\":" << status.nativeError << '}';
}
} // namespace

std::string SerializeBootstrapDiagnosticEvent(const BootstrapDiagnosticEvent& event)
{
    const auto& metadata = event.capture;
    const auto& visual = event.visual;
    std::ostringstream stream;
    Configure(stream);
    stream << "{\"event\":\"bootstrap-observation\",\"backend\":\"" << BackendName(metadata.backend) << "\",\"domain\":";
    WriteDomain(stream, metadata.domain);
    stream << ",\"observation\":\"" << metadata.captureObservation << "\",\"sourceGeneration\":\"" << metadata.sourceGeneration
           << "\",\"slotGeneration\":\"" << metadata.slotGeneration << "\",\"slot\":" << metadata.slotIndex
           << ",\"adapter\":{\"high\":" << metadata.adapterLuid.HighPart << ",\"low\":" << metadata.adapterLuid.LowPart
           << "},\"sourceFormat\":" << static_cast<unsigned int>(metadata.sourcePixelFormat) << ",\"format\":" << static_cast<unsigned int>(metadata.pixelFormat)
           << ",\"bitsPerColor\":" << metadata.bitsPerColor << ",\"colorSpace\":" << metadata.outputColorSpace << ",\"hdr\":" << metadata.hdr
           << ",\"signalEncoding\":" << static_cast<unsigned int>(metadata.signalEncoding)
           << ",\"physicalRoi\":[" << metadata.physicalRoi.left << ',' << metadata.physicalRoi.top << ',' << metadata.physicalRoi.right << ',' << metadata.physicalRoi.bottom
           << "],\"sourceContentSize\":[" << metadata.sourceContentSize.width << ',' << metadata.sourceContentSize.height
           << "],\"sourceExtent\":[" << metadata.sourceExtent.width << ',' << metadata.sourceExtent.height << "],\"roiSize\":[" << metadata.roiSize.width << ',' << metadata.roiSize.height
           << "],\"displayRotation\":" << static_cast<unsigned int>(metadata.displayRotation) << ",\"sourceTransform\":" << static_cast<unsigned int>(metadata.sourceTransform)
           << ",\"cursorExcluded\":" << metadata.isCursorExcluded << ",\"sourceCursorState\":" << static_cast<unsigned int>(metadata.sourceCursorState)
           << ",\"pointer\":{\"positionKnown\":" << metadata.pointer.positionKnown << ",\"shapeKnown\":" << metadata.pointer.shapeKnown
           << ",\"separateVisible\":" << metadata.pointer.separateVisible << ",\"physicalLeft\":" << metadata.pointer.physicalLeft << ",\"physicalTop\":" << metadata.pointer.physicalTop
           << "},\"timestamp\":{\"domain\":\"" << (metadata.timestamp.domain == CaptureTimestampDomain::WgcSystemRelative100ns ? "WgcSystemRelative100ns" : "DxgiQpcTicks")
           << "\",\"raw\":\"" << metadata.timestamp.rawValue << "\",\"frequency\":\"" << metadata.timestamp.rawFrequency
           << "\",\"monotonic100ns\":\"" << metadata.timestamp.monotonic100ns << "\"},\"disposition\":\"" << GetBootstrapDispositionName(event.disposition)
           << "\",\"visualErasure\":\"" << pbmodulation::GetLocalDesktopErasureName(visual.erasure) << "\",\"geometryGeneration\":\"" << event.geometryGeneration
           << "\",\"calibrationGeneration\":\"" << event.calibrationGeneration << "\",\"identity\":";
    const bool hasIdentity = visual.IsAccepted() && event.disposition != BootstrapDisposition::InvalidMetadata && event.disposition != BootstrapDisposition::UnsupportedSignal;
    if (hasIdentity)
    {
        std::uint32_t crc = 0;
        for (std::size_t index = 0; index < 4; index++)
        {
            crc |= std::to_integer<std::uint32_t>(visual.canonical44[40 + index]) << (index * 8);
        }
        stream << "{\"SessionTag\":\"" << event.bootstrap.sessionTag.value << "\",\"FrameSequence\":\"" << event.bootstrap.frameSequence
               << "\",\"VisualProfileId\":\"" << event.bootstrap.visualProfileId << "\",\"BootstrapCrc\":\"" << crc << "\"}";
    }
    else
    {
        stream << "null";
    }
    stream << ",\"copies\":[";
    for (std::size_t index = 0; index < visual.copies.size(); index++)
    {
        const auto& copy = visual.copies[index];
        if (index != 0)
        {
            stream << ',';
        }
        stream << "{\"fec\":" << copy.fecDecoded << ",\"crc\":" << copy.crcValid << ",\"record\":" << copy.recordValid
               << ",\"correctedSymbols\":" << copy.correctedSymbols << ",\"residual\":";
        WriteNumber(stream, copy.residual);
        stream << ",\"midGrayFraction\":";
        WriteNumber(stream, copy.midGrayFraction);
        stream << ",\"sampleMidGrayFraction\":";
        WriteNumber(stream, copy.sampleMidGrayFraction);
        stream << '}';
    }
    stream << "],\"geometry\":{\"originX\":";
    WriteNumber(stream, visual.geometry.originX);
    stream << ",\"originY\":";
    WriteNumber(stream, visual.geometry.originY);
    stream << ",\"scaleX\":";
    WriteNumber(stream, visual.geometry.scaleX);
    stream << ",\"scaleY\":";
    WriteNumber(stream, visual.geometry.scaleY);
    stream << ",\"residualPixels\":";
    WriteNumber(stream, visual.geometry.markerResidualPixels);
    stream << "},\"black\":";
    WriteNumber(stream, visual.blackLevel);
    stream << ",\"white\":";
    WriteNumber(stream, visual.whiteLevel);
    stream << ",\"quality\":";
    WriteNumber(stream, visual.quality);
    stream << ",\"timingResidual\":";
    WriteNumber(stream, visual.timingResidual);
    stream << ",\"timingBitErrorFraction\":";
    WriteNumber(stream, visual.timingBitErrorFraction);
    stream << ",\"midGrayFraction\":";
    WriteNumber(stream, visual.midGrayFraction);
    stream << ",\"sampleMidGrayFraction\":";
    WriteNumber(stream, visual.sampleMidGrayFraction);
    stream << ",\"markerCandidates\":" << visual.markerCandidates << ",\"geometryCandidates\":" << visual.geometryCandidates
           << ",\"workUnits\":" << visual.workUnits << "}\n";
    return stream.str();
}

std::string SerializeCaptureBootstrapSnapshot(const char* eventType, const CaptureSnapshot& capture, const CaptureNormalizeSnapshot& normalized,
    const DiagnosticReadbackSnapshot& readback, const BootstrapDiagnosticSnapshot& visual, const std::uint64_t staleDiagnosticEvents, const std::uint64_t elapsedMilliseconds)
{
    if (eventType == nullptr || (std::string_view(eventType) != "capture-snapshot" && std::string_view(eventType) != "capture-final"))
    {
        throw std::invalid_argument("invalid diagnostic snapshot event type");
    }
    const auto& environment = capture.environment;
    std::ostringstream stream;
    Configure(stream);
    stream << "{\"event\":\"" << eventType << "\",\"DiagnosticCpuReadback\":true,\"backend\":\"" << BackendName(environment.backendKind)
           << "\",\"elapsedMilliseconds\":" << elapsedMilliseconds << ",\"state\":\"" << StateName(capture.state) << "\",\"error\":";
    WriteStatus(stream, capture.error);
    stream << ",\"domain\":";
    WriteDomain(stream, normalized.domain);
    stream << ",\"domainActive\":" << normalized.active << ",\"epochStarts\":" << normalized.epochStarts << ",\"invalidations\":" << normalized.invalidations
           << ",\"processorResets\":" << readback.processorResets << ",\"adapter\":{\"high\":" << environment.adapterLuid.HighPart << ",\"low\":" << environment.adapterLuid.LowPart
           << "},\"format\":" << static_cast<unsigned int>(environment.pixelFormat) << ",\"bitsPerColor\":" << environment.bitsPerColor
           << ",\"colorSpace\":" << environment.outputColorSpace << ",\"hdr\":" << environment.hdr
           << ",\"arrivedFrames\":" << capture.arrivedFrames << ",\"copiedFrames\":" << capture.copiedFrames << ",\"deliveredFrames\":" << capture.deliveredFrames
           << ",\"droppedFrames\":" << capture.droppedFrames << ",\"expiredFrames\":" << capture.expiredFrames << ",\"staleFrames\":" << capture.staleFrames
           << ",\"acquireTimeouts\":" << capture.acquireTimeouts << ",\"pointerOnlyFrames\":" << capture.pointerOnlyFrames << ",\"accumulatedFrames\":" << capture.accumulatedFrames
           << ",\"recreates\":" << capture.recreates << ",\"accessLostEvents\":" << capture.accessLostEvents << ",\"rebuildReason\":\"" << RebuildName(capture.lastRebuildReason)
           << "\",\"waitingNativeError\":" << capture.waitingNativeError << ",\"environmentAttempts\":" << capture.environmentAttempts
           << ",\"frameLeaseHighWater\":" << capture.frameLeaseHighWater << ",\"frameAgeHighWater100ns\":" << capture.frameAgeHighWater100ns
           << ",\"normalizedFrames\":" << normalized.acceptedFrames << ",\"captureErasures\":" << normalized.erasedFrames
           << ",\"captureErasure\":\"" << GetCaptureErasureName(normalized.lastErasure) << "\",\"captureErasureCounts\":[";
    for (std::size_t index = 0; index < normalized.erasures.size(); index++)
    {
        if (index != 0)
        {
            stream << ',';
        }
        stream << normalized.erasures[index];
    }
    stream << "],\"readback\":{\"domain\":";
    WriteDomain(stream, readback.domain);
    stream << ",\"active\":" << readback.active << ",\"resetPending\":" << readback.resetPending << ",\"error\":";
    WriteStatus(stream, readback.error);
    stream << ",\"lastGraphicsFailure\":";
    WriteStatus(stream, readback.lastGraphicsFailure);
    stream << ",\"deviceLossEvents\":" << readback.deviceLossEvents;
    stream << ",\"reservedBytes\":" << readback.reservation.totalBytes << ",\"residentStagingBytes\":" << readback.residentStagingBytes
           << ",\"copies\":" << readback.submittedCopies << ",\"mappedFrames\":" << readback.mappedFrames << ",\"bytes\":" << readback.readbackBytes
           << ",\"mapCalls\":" << readback.mapCalls << ",\"mapBlockingRetries\":" << readback.mapBlockingRetries << ",\"lastRowPitch\":" << readback.lastMappedRowPitch << ",\"queueHighWater\":" << readback.queueHighWater
           << ",\"stagingHighWater\":" << readback.stagingHighWater << ",\"cpuBufferHighWater\":" << readback.cpuBufferHighWater
           << ",\"frameAgeHighWater100ns\":" << readback.frameAgeHighWater100ns << ",\"readbackLatencyHighWater100ns\":" << readback.readbackLatencyHighWater100ns
           << ",\"analysisLatencyHighWater100ns\":" << readback.analysisLatencyHighWater100ns << ",\"analyzed\":" << readback.analyzedFrames
           << ",\"committed\":" << readback.committedFrames << ",\"discarded\":" << readback.discardedCandidates << ",\"drops\":" << readback.dropEvents
           << ",\"lastDrop\":\"" << GetDiagnosticReadbackDropName(readback.lastDrop) << "\",\"dropCounts\":[";
    for (std::size_t index = 0; index < readback.drops.size(); index++)
    {
        if (index != 0)
        {
            stream << ',';
        }
        stream << readback.drops[index];
    }
    stream << "],\"workerStopped\":" << readback.workerStopped << "},\"visual\":{\"domain\":";
    if (visual.domain)
    {
        WriteDomain(stream, *visual.domain);
    }
    else
    {
        stream << "null";
    }
    const bool temporalStateCurrent = normalized.active && readback.active && !readback.resetPending &&
                                      readback.domain == normalized.domain && visual.domain && *visual.domain == normalized.domain;
    stream << ",\"temporalStateCurrent\":" << temporalStateCurrent << ",\"observations\":" << visual.observations
           << ",\"accepted\":" << visual.accepted << ",\"duplicates\":" << visual.duplicates << ",\"erasures\":" << visual.erasures
           << ",\"identityConflicts\":" << visual.identityConflicts << ",\"geometryGeneration\":\"" << visual.geometryGeneration
           << "\",\"calibrationGeneration\":\"" << visual.calibrationGeneration << "\",\"trackedSessions\":" << visual.trackedSessions
           << ",\"retainedSequences\":" << visual.retainedSequences << ",\"queuedEvents\":" << visual.queuedEvents << ",\"diagnosticQueueDrops\":" << visual.diagnosticQueueDrops
           << ",\"lastDisposition\":\"" << GetBootstrapDispositionName(visual.lastDisposition) << "\"},\"staleDiagnosticEvents\":" << staleDiagnosticEvents
           << ",\"shutdownComplete\":" << capture.shutdownComplete << ",\"deferredCleanup\":" << capture.deferredCleanup << "}\n";
    return stream.str();
}

} // namespace pbdecoder
