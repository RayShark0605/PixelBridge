#include "pbrenderd3d/data_window.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>

namespace pbrenderd3d
{
namespace
{

void WriteMetric(std::ostream& output, const std::optional<double> value)
{
    if (value && std::isfinite(*value) && *value >= 0)
    {
        output << *value;
    }
    else
    {
        output << "null";
    }
}

template <std::size_t Size> void WriteName(std::ostream& output, const std::array<wchar_t, Size>& name)
{
    output << '"';
    for (const wchar_t character : name)
    {
        if (character == 0)
        {
            break;
        }
        const auto code = static_cast<std::uint32_t>(character);
        if (code >= 32 && code < 127 && code != '\\' && code != '"')
        {
            output << static_cast<char>(code);
        }
        else
        {
            constexpr char digits[] = "0123456789abcdef";
            output << "\\u" << digits[(code >> 12) & 15] << digits[(code >> 8) & 15] << digits[(code >> 4) & 15] << digits[code & 15];
        }
    }
    output << '"';
}

}

void WriteDataWindowSnapshotJson(std::ostream& destination, const DataWindowSnapshot& snapshot)
{
    // Independent formatting also handles callers using hex, showbase,
    // scientific notation, digit grouping or a non-dot decimal separator.
    // A formatting/allocation failure cannot publish a partial JSON object.
    std::ostringstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.imbue(std::locale::classic());
    output << std::boolalpha << std::setprecision(17);
    const auto& timing = snapshot.timing;
    output << "{\"schema_version\":1,\"scope\":\"presentation-candidate-not-capture-certification\",\"windowState\":"
           << static_cast<unsigned int>(snapshot.state) << ",\"error\":\"" << GetPresentationErrorName(snapshot.error.code) << "\",\"errorStage\":\""
           << GetPresentationStageName(snapshot.error.stage) << "\",\"nativeError\":" << snapshot.error.nativeError
           << ",\"PresentationEpoch\":" << timing.presentationEpoch << ",\"epochReason\":\"" << pbpresenttiming::GetEpochReasonName(timing.epochReason)
           << "\",\"timingState\":\"" << pbpresenttiming::GetTimingStateName(timing.state) << "\",\"timingIssue\":\""
           << pbpresenttiming::GetTimingIssueName(timing.issue) << "\",\"qpcFrequency\":" << timing.qpcFrequency
           << ",\"epochStartQpc\":" << timing.epochStartQpc << ",\"sampleQpc\":" << timing.sampleQpc << ",\"DataWindowPhysicalSize\":["
           << snapshot.environment.clientWidth << ',' << snapshot.environment.clientHeight << "],\"clientOrigin\":[" << snapshot.environment.clientOrigin.x
           << ',' << snapshot.environment.clientOrigin.y << "],\"dpi\":" << snapshot.environment.dpi << ",\"display\":";
    WriteName(output, snapshot.environment.displayName);
    output << ",\"adapter\":";
    WriteName(output, snapshot.environment.adapterDescription);
    output << ",\"adapterLuid\":[" << snapshot.environment.adapterLuidHigh << ',' << snapshot.environment.adapterLuidLow << "],\"displayMode\":["
           << snapshot.environment.modeWidth << ',' << snapshot.environment.modeHeight << ',' << snapshot.environment.modeFrequency
           << "],\"singleMonitor\":" << snapshot.environment.singleMonitor << ",\"monitorIdentity\":" << snapshot.environment.monitorIdentity
           << ",\"modeChangeSerial\":" << snapshot.environment.modeChangeSerial << ",\"dpiChangeSerial\":" << snapshot.environment.dpiChangeSerial
           << ",\"viewportDisposition\":" << static_cast<unsigned int>(snapshot.viewport.disposition)
           << ",\"viewport\":[" << snapshot.viewport.originX << ',' << snapshot.viewport.originY << ',' << snapshot.viewport.width << ','
           << snapshot.viewport.height << "],\"viewportScale\":" << snapshot.viewport.scale
           << ",\"candidateContractSatisfied\":" << snapshot.candidateContractSatisfied << ",\"softwareRasterizer\":" << snapshot.softwareRasterizer
           << ",\"swapChainGeneration\":" << snapshot.swapChainGeneration << ",\"bufferGeneration\":" << snapshot.bufferGeneration
           << ",\"bufferCount\":" << snapshot.contract.bufferCount << ",\"maximumFrameLatency\":" << snapshot.contract.maximumFrameLatency
           << ",\"flipEffect\":\"" << (snapshot.contract.flipEffect == FlipEffect::Discard ? "flip-discard" : "flip-sequential")
           << "\",\"bgraUnorm\":" << snapshot.contract.bgraUnorm << ",\"noMsaa\":" << snapshot.contract.noMsaa
           << ",\"alphaIgnored\":" << snapshot.contract.alphaIgnored << ",\"scalingNone\":" << snapshot.contract.scalingNone
           << ",\"tearingDisabled\":" << snapshot.contract.tearingDisabled << ",\"latencyWaitable\":" << snapshot.contract.latencyWaitable
           << ",\"perMonitorV2\":" << snapshot.contract.perMonitorV2 << ",\"resizableChrome\":" << snapshot.contract.resizableChrome
           << ",\"immutableCanonicalSource\":" << snapshot.contract.immutableCanonicalSource
           << ",\"pointSampled\":" << snapshot.contract.pointSampled << ",\"centeredLetterbox\":" << snapshot.contract.centeredLetterbox
           << ",\"neutralMatteBelowMinimum\":" << snapshot.contract.neutralMatteBelowMinimum
           << ",\"submittedFrames\":" << snapshot.submittedFrames
           << ",\"replacedPendingFrames\":" << snapshot.replacedPendingFrames << ",\"discardedEpochFrames\":" << snapshot.discardedEpochFrames
           << ",\"sourceTextureReplacements\":" << snapshot.sourceTextureReplacements << ",\"repeatedPresentCalls\":" << snapshot.repeatedPresentCalls
           << ",\"invalidatedActiveFrames\":" << snapshot.invalidatedActiveFrames << ",\"activeFrame\":" << snapshot.activeFrame
           << ",\"activeFrameSequence\":" << snapshot.activeFrameSequence
           << ",\"activeFramePresentationEpoch\":" << snapshot.activeFramePresentationEpoch
           << ",\"totalPresentCalls\":" << snapshot.totalPresentCalls << ",\"totalSuccessfulPresents\":" << snapshot.totalSuccessfulPresents
           << ",\"neutralMattePresentCalls\":" << snapshot.neutralMattePresentCalls
           << ",\"pendingFrame\":" << snapshot.pendingFrame << ",\"inFlightFrame\":" << snapshot.inFlightFrame
           << ",\"neutralMattePending\":" << snapshot.neutralMattePending
           << ",\"epochPresentCalls\":" << timing.presentCalls << ",\"epochSuccessfulPresents\":" << timing.successfulPresents
           << ",\"epochFailedPresents\":" << timing.failedPresents << ",\"epochOccludedPresents\":" << timing.occludedPresents
           << ",\"observedPresents\":" << timing.observedPresents << ",\"observedUniqueVisuals\":" << timing.observedUniqueVisuals
           << ",\"observationCoverageComplete\":" << timing.observationCoverageComplete << ",\"unmatchedStatistics\":" << timing.unmatchedStatistics
           << ",\"historyOverflows\":" << timing.historyOverflows << ",\"PresentGlitchCount\":" << timing.presentGlitchCount
           << ",\"glitchScope\":\"observed-present-ids-in-continuous-producer-cadence\",\"PresentRefreshCountExtended\":" << timing.presentRefreshCountExtended
           << ",\"pendingHistory\":" << timing.pendingHistory << ",\"counterSaturated\":" << timing.counterSaturated << ",\"PresentCallFPS\":";
    WriteMetric(output, timing.presentCallFps);
    output << ",\"PresentedVisualFPS\":";
    WriteMetric(output, timing.presentedVisualFps);
    output << ",\"ObservedVisualFPSLowerBound\":";
    WriteMetric(output, timing.observedVisualFpsLowerBound);
    output << ",\"PresentQueueLatency\":";
    WriteMetric(output, timing.presentQueueLatencyMs);
    output << ",\"latencyUnit\":\"ms\",\"latencySource\":\"estimated-from-dxgi-not-Present-duration\",\"statisticsNativeStatus\":"
           << timing.lastStatisticsNativeStatus << ",\"rawStatistics\":";
    if (timing.lastRawStatistics)
    {
        const auto& statistics = *timing.lastRawStatistics;
        output << "{\"PresentCount\":" << statistics.presentId << ",\"PresentRefreshCount\":" << statistics.presentRefreshCount
               << ",\"SyncRefreshCount\":" << statistics.syncRefreshCount << ",\"SyncQPCTime\":" << statistics.syncQpcTime
               << ",\"SyncGPUTime\":" << statistics.syncGpuTime << '}';
    }
    else
    {
        output << "null";
    }
    output << ",\"presentIdNativeStatus\":" << snapshot.lastPresentIdNativeStatus << ",\"lastPresent\":";
    if (timing.lastPresent)
    {
        const auto& sample = *timing.lastPresent;
        output << "{\"FrameSequence\":" << sample.frameSequence << ",\"beginQpc\":" << sample.beginQpc << ",\"endQpc\":" << sample.endQpc
               << ",\"outcome\":" << static_cast<unsigned int>(sample.outcome) << ",\"presentId\":";
        if (sample.presentId)
        {
            output << *sample.presentId;
        }
        else
        {
            output << "null";
        }
        output << '}';
    }
    else
    {
        output << "null";
    }
    output << '}';
    const std::string text = output.str();
    destination.write(text.data(), static_cast<std::streamsize>(text.size()));
}

}
