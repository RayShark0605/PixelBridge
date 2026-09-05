#include "application_build_identity.h"
#include "diagnostic_file.h"
#include "local_desktop_runtime.h"
#include "pbprotocol/checked_integer.h"
#include "run_report.h"
#include "sender_carousel_scheduler.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t minimumRunSeconds = 30;
constexpr std::uint32_t maximumHighResolutionRunSeconds = 600;
constexpr std::uint32_t maximumRunSeconds = 3600;
constexpr std::uint32_t maximumDiagnosticDimension = 4096;
constexpr std::size_t maximumSampleBytes = 24 * 1024;
constexpr std::size_t maximumSampleCount = maximumHighResolutionRunSeconds + 2;
constexpr std::size_t maximumEvidenceBytes = pbdiagnostic::DiagnosticFile::maximumBytes;
constexpr auto safetyInterval = std::chrono::milliseconds(200);
constexpr auto sampleInterval = std::chrono::seconds(1);
constexpr std::uint32_t extendedRunSampleIntervalSeconds = 6;
constexpr auto extendedRunSampleInterval = std::chrono::seconds(extendedRunSampleIntervalSeconds);
static_assert(maximumSampleBytes * maximumSampleCount <= maximumEvidenceBytes);
static_assert(static_cast<std::size_t>(maximumRunSeconds / extendedRunSampleIntervalSeconds) + 2 <= maximumSampleCount);

void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::uint32_t ParseTimeout(const std::wstring_view text)
{
    Require(!text.empty(), "empty timeout");
    std::uint32_t value = 0;
    for (const wchar_t character : text)
    {
        Require(character >= L'0' && character <= L'9', "invalid timeout");
        const auto digit = static_cast<std::uint32_t>(character - L'0');
        Require(value <= (maximumRunSeconds - digit) / 10, "timeout exceeds 3600 seconds");
        value = value * 10 + digit;
    }
    Require(value >= minimumRunSeconds, "timeout must be at least 30 seconds");
    return value;
}

std::chrono::seconds SelectSampleInterval(const std::uint32_t seconds) noexcept
{
    return seconds <= maximumHighResolutionRunSeconds ? sampleInterval : extendedRunSampleInterval;
}

std::uint32_t ParseDiagnosticDimension(const std::wstring_view text)
{
    Require(!text.empty(), "empty diagnostic dimension");
    std::uint32_t value = 0;
    for (const wchar_t character : text)
    {
        Require(character >= L'0' && character <= L'9', "invalid diagnostic dimension");
        const auto digit = static_cast<std::uint32_t>(character - L'0');
        Require(value <= (maximumDiagnosticDimension - digit) / 10, "diagnostic dimension exceeds 4096 pixels");
        value = value * 10 + digit;
    }
    Require(value != 0, "diagnostic dimension must be positive");
    return value;
}

bool CanAppendSample(const std::size_t samples, const std::size_t bytes, const std::size_t nextBytes) noexcept
{
    return samples < maximumSampleCount && nextBytes < maximumSampleBytes &&
        bytes <= maximumEvidenceBytes && nextBytes + 1 <= maximumEvidenceBytes - bytes;
}

void WriteNew(const std::filesystem::path& path, const std::string& text)
{
    pbdiagnostic::DiagnosticFile file(path.c_str());
    file.Write(text);
    file.Finish();
}

class Evidence
{
public:
    explicit Evidence(const std::filesystem::path& root) : root_(root)
    {
        Require(root_.is_absolute() && !root_.filename().empty(), "run directory must be an absolute new path");
        Require(std::filesystem::is_directory(root_.parent_path()), "run parent must already exist");
        Require(std::filesystem::create_directory(root_), "run directory must be new; resume is not a clean G21 run");
    }

    void Sample(const std::string& json)
    {
        Require(CanAppendSample(samples_, bytes_, json.size()), "bounded evidence budget exhausted");
        if (!samplesFile_)
        {
            samplesFile_ = std::make_unique<pbdiagnostic::DiagnosticFile>((root_ / "samples.jsonl").c_str());
        }
        samplesFile_->Write(json);
        samplesFile_->Write("\n");
        bytes_ += json.size() + 1;
        samples_++;
    }

    void Finish()
    {
        if (samplesFile_)
        {
            samplesFile_->Finish();
        }
    }

    void Record(const char* name, const std::string& json) const
    {
        WriteNew(root_ / name, json);
    }

private:
    const std::filesystem::path root_;
    std::unique_ptr<pbdiagnostic::DiagnosticFile> samplesFile_;
    std::size_t samples_ = 0;
    std::size_t bytes_ = 0;
};

void CheckApprovedTopology(const pbapp::MonitorSafetySelection& safety)
{
    const auto& right = safety.experimentMonitor;
    const auto& left = safety.protectedMonitor;
    Require(left.deviceName == L"\\\\.\\DISPLAY1" && right.deviceName == L"\\\\.\\DISPLAY2" &&
        right.physicalRect.right - static_cast<std::int64_t>(right.physicalRect.left) == 2560 &&
        right.physicalRect.bottom - static_cast<std::int64_t>(right.physicalRect.top) == 1440 &&
        right.physicalRect.left >= left.physicalRect.right && right.rotation == DXGI_MODE_ROTATION_IDENTITY,
        "approved DISPLAY1/protected and DISPLAY2/right 2560x1440 topology is unavailable");
    const auto target = pbapp::ValidateMonitorSafetyTarget(safety, right.physicalRect, right.monitor);
    Require(static_cast<bool>(target), std::string("protected-screen containment: ") + pbapp::GetMonitorSafetyErrorName(target.code));
}

pbapp::MonitorSafetySelection ResolveSafety()
{
    pbapp::MonitorSafetySelection safety;
    const auto status = pbapp::ResolveMonitorSafetySelection(L"\\\\.\\DISPLAY1", L"\\\\.\\DISPLAY2", safety);
    Require(static_cast<bool>(status), std::string("monitor safety: ") + pbapp::GetMonitorSafetyErrorName(status.code));
    CheckApprovedTopology(safety);
    return safety;
}

void CheckTarget(const pbapp::MonitorSafetySelection& safety)
{
    Require(static_cast<bool>(pbapp::RevalidateMonitorSafetySelection(safety)), "monitor topology changed; stopping capture");
    const RECT& rectangle = safety.experimentMonitor.physicalRect;
    const auto status = pbapp::ValidateMonitorSafetyTarget(safety, rectangle, MonitorFromRect(&rectangle, MONITOR_DEFAULTTONULL));
    Require(static_cast<bool>(status), "right-screen ROI no longer belongs to the approved monitor");
}

pbscreenregion::ScreenCaptureRegion ResolveRegion(const pbapp::MonitorSafetySelection& safety)
{
    CheckTarget(safety);
    pbscreenregion::ScreenCaptureRegion region;
    const auto status = pbscreenregion::ResolveScreenCaptureRegion(safety.experimentMonitor.physicalRect, region);
    Require(static_cast<bool>(status) && region.monitor == safety.experimentMonitor.monitor &&
        EqualRect(&region.monitorPhysicalRect, &safety.experimentMonitor.physicalRect) &&
        region.dpiX == safety.experimentMonitor.dpiX && region.dpiY == safety.experimentMonitor.dpiY &&
        region.rotation == safety.experimentMonitor.rotation, "cannot resolve the exact approved physical right-monitor ROI");
    return region;
}

void WriteMonitor(std::ostream& stream, const pbapp::MonitorInfo& monitor)
{
    // Device names are fixed by CheckApprovedTopology, not arbitrary JSON text.
    stream << "{\"deviceName\":\"" << (monitor.deviceName == L"\\\\.\\DISPLAY1" ? "\\\\\\\\.\\\\DISPLAY1" : "\\\\\\\\.\\\\DISPLAY2")
        << "\",\"hmonitor\":" << reinterpret_cast<std::uintptr_t>(monitor.monitor)
        << ",\"left\":" << monitor.physicalRect.left << ",\"top\":" << monitor.physicalRect.top
        << ",\"right\":" << monitor.physicalRect.right << ",\"bottom\":" << monitor.physicalRect.bottom
        << ",\"dpiX\":" << monitor.dpiX << ",\"dpiY\":" << monitor.dpiY << ",\"refreshRate\":" << monitor.refreshRate
        << ",\"rotation\":" << static_cast<unsigned int>(monitor.rotation)
        << ",\"adapterLuidLow\":" << monitor.adapterLuid.LowPart << ",\"adapterLuidHigh\":" << monitor.adapterLuid.HighPart << '}';
}

std::string PreflightReport(const pbapp::MonitorSafetySelection& safety)
{
    std::ostringstream stream;
    stream << "{\"schema\":\"PixelBridge.G21.Preflight.1\",\"gitCommit\":\"" << PB_GIT_COMMIT
        << "\",\"testOnly\":true,\"captureStarted\":false,\"protectedMonitor\":";
    WriteMonitor(stream, safety.protectedMonitor);
    stream << ",\"experimentMonitor\":";
    WriteMonitor(stream, safety.experimentMonitor);
    stream << ",\"roi\":\"Entire physical experiment monitor; no historical coordinates\",\"safetyPollMilliseconds\":200"
        << ",\"transportEvidence\":\"User-operated opaque remote pixels; provider unknown\""
        << ",\"transportMetadataAuthority\":\"NonDecodingOperatorMetadata\""
        << ",\"productionPolicy\":\"MakeUnifiedDecoderConfig; default DecoderRuntime services; Auto WGC/DXGI\""
        << ",\"productRemoteMonitorReplayConfigurationChanged\":false,\"inputAutomation\":false,\"windowCreated\":false}";
    return stream.str();
}

pbapp::RunReportContext ReportContext()
{
    return {"PBUnifiedRemoteGate.Decoder", pbcore::GetBuildInfo().version, PB_GIT_COMMIT, ""};
}

std::string DecoderReport(const pbapp::DecoderSnapshot& snapshot, const std::uint64_t safetyChecks)
{
    auto json = pbapp::BuildDecoderRunReportJson(ReportContext(), snapshot);
    Require(!json.empty() && json.back() == '}', "unexpected production report format");
    json.pop_back();
    std::ostringstream stream;
    stream << json << ",\"remoteGate\":{\"steadyMilliseconds\":"
        << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()
        << ",\"safetyRevalidations\":" << safetyChecks << ",\"defaultProductionFactories\":true"
        << ",\"pixelReadbackDiagnostic\":false,\"sourceOrOracleProvided\":false"
        << ",\"captureAdmissionDrops\":" << snapshot.captureAdmissionDrops << ",\"captureArrived\":" << snapshot.captureArrivedFrames
        << ",\"captureCopied\":" << snapshot.captureCopiedFrames << ",\"captureDelivered\":" << snapshot.captureDeliveredFrames
        << ",\"captureDropped\":" << snapshot.captureDroppedFrames << ",\"captureExpired\":" << snapshot.captureExpiredFrames
        << ",\"captureAgeHighWater100ns\":" << snapshot.captureFrameAgeHighWater100ns
        << ",\"bootstrapAccepted\":" << snapshot.bootstrapAcceptedFrames << ",\"bootstrapRejected\":" << snapshot.bootstrapRejectedFrames
        << ",\"bootstrapCpuTotal100ns\":" << snapshot.bootstrapCpuTimeTotal100ns
        << ",\"postGpuFecCpuTotal100ns\":" << snapshot.postGpuFecCpuTimeTotal100ns
        << ",\"demodGpuTotal100ns\":" << snapshot.demodGpuTimeTotal100ns
        << ",\"demodPendingHighWater\":" << snapshot.demodPendingHighWater
        << ",\"resultQueueHighWater\":" << snapshot.resultQueueHighWater << ",\"staleResultDrops\":" << snapshot.staleResultDrops
        << ",\"acceptedTransportBlocks\":" << snapshot.acceptedTransportBlocks
        << ",\"outerResourceRejections\":" << snapshot.outerResourceRejections
        << ",\"outerProtocolResourceLimitExceededRejections\":"
        << snapshot.outerProtocolResourceLimitExceededRejections
        << ",\"outerProtocolResourceExhaustedRejections\":" << snapshot.outerProtocolResourceExhaustedRejections
        << ",\"outerControlReassemblyQuotaExceededRejections\":"
        << snapshot.outerControlReassemblyQuotaExceededRejections
        << ",\"outerFecOutOfMemoryRejections\":" << snapshot.outerFecOutOfMemoryRejections
        << ",\"outerFecDecoderQuotaExceededRejections\":" << snapshot.outerFecDecoderQuotaExceededRejections
        << ",\"outerFecExtraInsufficientRejections\":" << snapshot.outerFecExtraInsufficientRejections
        << ",\"outerConflictRejections\":" << snapshot.outerConflictRejections
        << ",\"receiverResourcePolicyRejectedCount\":" << snapshot.receiverResourcePolicyRejectedCount
        << ",\"receiverControlRejectedByResourcePolicyCount\":"
        << snapshot.receiverControlRejectedByResourcePolicyCount
        << ",\"outerOrphanAdmittedBlockCount\":" << snapshot.outerOrphanAdmittedBlockCount
        << ",\"outerOrphanDroppedByQuotaCount\":" << snapshot.outerOrphanDroppedByQuotaCount
        << ",\"outerOrphanResourceExhaustedCount\":" << snapshot.outerOrphanResourceExhaustedCount
        << ",\"outerOrphanConflictRejectionCount\":" << snapshot.outerOrphanConflictRejectionCount
        << ",\"outerOrphanCachedBlockCount\":" << snapshot.outerOrphanCachedBlockCount
        << ",\"outerOrphanCachedBytes\":" << snapshot.outerOrphanCachedBytes
        << ",\"outerPeakOrphanCachedBytes\":" << snapshot.outerPeakOrphanCachedBytes
        << ",\"outerDeferredResourceBusyCount\":" << snapshot.outerDeferredResourceBusyCount
        << ",\"outerFecQuotaExceededCount\":" << snapshot.outerFecQuotaExceededCount
        << ",\"outerActiveDecoderLimit\":" << snapshot.outerActiveDecoderLimit
        << ",\"outerTotalDecoderByteLimit\":" << snapshot.outerTotalDecoderByteLimit
        << ",\"outerActiveDecoderCount\":" << snapshot.outerActiveDecoderCount
        << ",\"outerPeakActiveDecoderCount\":" << snapshot.outerPeakActiveDecoderCount
        << ",\"outerReservedDecoderBytes\":" << snapshot.outerReservedDecoderBytes
        << ",\"outerPeakReservedDecoderBytes\":" << snapshot.outerPeakReservedDecoderBytes
        << ",\"independentFalseAcceptedCodewordOracle\":null,\"oracleUnavailableReason\":\"No independent sender truth supplied to receiver\"}}";
    return stream.str();
}

bool ReceiverChecksPassed(const pbapp::DecoderSnapshot& snapshot) noexcept
{
    std::uint64_t detailedResourceRejections = 0;
    for (const std::uint64_t count : {snapshot.outerProtocolResourceLimitExceededRejections,
        snapshot.outerProtocolResourceExhaustedRejections,
        snapshot.outerControlReassemblyQuotaExceededRejections, snapshot.outerFecOutOfMemoryRejections,
        snapshot.outerFecDecoderQuotaExceededRejections, snapshot.outerFecExtraInsufficientRejections})
    {
        detailedResourceRejections = pbprotocol::SaturatingAddUnsigned(detailedResourceRejections, count);
    }
    return snapshot.state == pbapp::DecoderState::Completed && snapshot.wholeFileDigestCheck.value_or(false) &&
        snapshot.finalPublishSucceeded && snapshot.finalReopenVerified.value_or(false) &&
        !snapshot.resumeStateLoaded && !snapshot.outputRecoveredAfterPublish && snapshot.outerResourceRejections == 0 &&
        detailedResourceRejections == snapshot.outerResourceRejections &&
        snapshot.receiverResourcePolicyRejectedCount == 0 && snapshot.outerOrphanDroppedByQuotaCount == 0 &&
        snapshot.outerOrphanResourceExhaustedCount == 0 && snapshot.outerOrphanConflictRejectionCount == 0 &&
        snapshot.outerOrphanCachedBlockCount == 0 && snapshot.outerOrphanCachedBytes == 0 &&
        snapshot.outerConflictRejections == 0 && snapshot.outerDeferredResourceBusyCount == 0 &&
        snapshot.outerFecQuotaExceededCount == 0 && snapshot.errorDetail.empty();
}

pbtelemetry::PublishedFrameMetric PublishedMetric(const pbapp::DecoderSnapshot& snapshot) noexcept
{
    if (!snapshot.verifiedEncodedSegmentBytes)
    {
        return {std::nullopt, "EncodedByteCoverageUnavailable"};
    }
    return pbtelemetry::EvaluatePublishedFrameMetric(snapshot.unifiedTelemetry, *snapshot.verifiedEncodedSegmentBytes,
        snapshot.wholeFileDigestCheck.value_or(false), snapshot.finalPublishSucceeded, snapshot.finalReopenVerified.value_or(false),
        snapshot.resumeStateLoaded || snapshot.outputRecoveredAfterPublish);
}

bool ReachesThreshold(const pbtelemetry::PublishedFrameMetric& metric, const double threshold) noexcept
{
    return metric.bytesPerUniqueFrame && std::isfinite(*metric.bytesPerUniqueFrame) && *metric.bytesPerUniqueFrame >= threshold;
}

std::string GateReport(const pbapp::DecoderSnapshot& snapshot)
{
    const auto metric = PublishedMetric(snapshot);
    std::ostringstream stream;
    stream << std::boolalpha << "{\"schema\":\"PixelBridge.G21.ReceiverChecks.1\",\"receiverLocalChecksPassed\":"
        << ReceiverChecksPassed(snapshot) << ",\"hard16KiBFrameMetricPassed\":" << ReachesThreshold(metric, 16384.0)
        << ",\"engineering32KiBTargetReached\":" << ReachesThreshold(metric, 32768.0)
        << ",\"externalSourceSha256AndBlake3Verification\":\"Pending independent post-receive verification\""
        << ",\"chromaAndBaseOnlyEvidence\":\"Pending separate audit\",\"sourceOrOracleProvided\":false,\"G21Completed\":false}";
    return stream.str();
}

void WriteLaneMetricReport(std::ostream& stream, const pbmodulation::UnifiedLaneMetricObservation& metrics)
{
    stream << std::boolalpha << "{\"available\":" << metrics.available << ",\"samples\":" << metrics.samples
        << ",\"zeroMetrics\":" << metrics.zeroMetrics << ",\"erasedMetrics\":" << metrics.erasedMetrics
        << ",\"minimumAbsoluteMetric\":" << metrics.minimumAbsoluteMetric << ",\"meanAbsoluteMetric\":";
    if (metrics.samples == 0)
    {
        stream << "null";
    }
    else
    {
        stream << static_cast<double>(metrics.absoluteMetricSum) / metrics.samples;
    }
    stream << '}';
}

void WriteSlotReport(std::ostream& stream, const pbmodulation::UnifiedSlotObservation& slot)
{
    stream << std::boolalpha << "{\"lane\":" << static_cast<unsigned int>(slot.lane)
        << ",\"kind\":" << static_cast<unsigned int>(slot.kind)
        << ",\"rejection\":" << static_cast<unsigned int>(slot.rejection)
        << ",\"iterationsUsed\":" << slot.iterationsUsed << ",\"acceptedBytes\":" << slot.acceptedBytes
        << ",\"fecValid\":" << slot.fecValid << ",\"paddingValid\":" << slot.paddingValid
        << ",\"crcValid\":" << slot.crcValid << ",\"identityValid\":" << slot.identityValid
        << ",\"accepted\":" << slot.accepted << '}';
}

void InspectRemotePixels(const std::filesystem::path& inputPath, const std::filesystem::path& reportPath,
    const std::uint32_t width, const std::uint32_t height)
{
    Require(inputPath.is_absolute() && reportPath.is_absolute(), "diagnostic paths must be absolute");
    Require(std::filesystem::is_regular_file(inputPath), "diagnostic BGRA input must be a regular file");
    Require(std::filesystem::is_directory(reportPath.parent_path()) && !std::filesystem::exists(reportPath),
        "diagnostic report parent must exist and report must be new");
    const auto pixels = pbprotocol::CheckedMultiplyUint64(width, height);
    Require(static_cast<bool>(pixels), "diagnostic BGRA dimensions overflow");
    const auto expectedBytes = pbprotocol::CheckedMultiplyUint64(pixels.Value(), 4);
    Require(expectedBytes && expectedBytes.Value() <= 64ULL * 1024ULL * 1024ULL &&
        std::filesystem::file_size(inputPath) == expectedBytes.Value(), "diagnostic BGRA size mismatch");
    std::vector<std::byte> bgra(static_cast<std::size_t>(expectedBytes.Value()));
    std::ifstream input(inputPath, std::ios::binary);
    Require(static_cast<bool>(input.read(reinterpret_cast<char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()))) &&
        input.peek() == std::ifstream::traits_type::eof(), "diagnostic BGRA read failed");

    auto oracleResult = pbmodulation::UnifiedVisualCpuOracle::Create(pbmodulation::UnifiedVisualCpuOracle::RequiredBytes());
    Require(static_cast<bool>(oracleResult), "diagnostic CPU oracle creation failed");
    auto oracle = std::move(oracleResult).Value();
    const auto decodeStarted = Clock::now();
    const auto observation = oracle.DecodeMixedFrame({bgra, width, height, static_cast<std::size_t>(width) * 4,
        pbmodulation::LumaPixelFormat::Bgra8});
    const auto decodeNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - decodeStarted).count();
    const auto metrics = oracle.GetSoftMetrics();
    std::array<std::array<std::uint32_t, 11>, 3> erasuresByLane{};
    for (const auto& metric : metrics)
    {
        const std::size_t laneIndex = metric.lane == pbmodulation::UnifiedLane::BaseLuma ? 0 :
            metric.lane == pbmodulation::UnifiedLane::FineLuma ? 1 : 2;
        const auto reason = static_cast<std::size_t>(metric.erasureReason);
        Require(reason < erasuresByLane[laneIndex].size(), "unknown diagnostic metric erasure");
        erasuresByLane[laneIndex][reason]++;
    }
    std::uint64_t fecIterations = 0;
    for (const auto& slot : observation.slots)
    {
        fecIterations = pbprotocol::SaturatingAddUnsigned(fecIterations, static_cast<std::uint64_t>(slot.iterationsUsed));
    }

    std::ostringstream stream;
    stream.precision(17);
    stream << std::boolalpha << "{\"schema\":\"PixelBridge.G21.RemotePixelOfflineDiagnostic.1\""
        << ",\"authority\":\"Stored diagnostic BGRA pixels; not live capture and not sender truth\""
        << ",\"width\":" << width << ",\"height\":" << height << ",\"inputBytes\":" << bgra.size()
        << ",\"decodeNanoseconds\":" << decodeNanoseconds << ",\"inputValid\":" << observation.inputValid
        << ",\"frameErasure\":" << static_cast<unsigned int>(observation.frameErasure)
        << ",\"bootstrapErasure\":" << static_cast<unsigned int>(observation.bootstrap.erasure)
        << ",\"frameSequence\":" << observation.bootstrapRecord.frameSequence
        << ",\"sessionTag\":" << observation.bootstrapRecord.sessionTag.value
        << ",\"geometry\":{\"originX\":" << observation.bootstrap.geometry.originX
        << ",\"originY\":" << observation.bootstrap.geometry.originY << ",\"scaleX\":" << observation.bootstrap.geometry.scaleX
        << ",\"scaleY\":" << observation.bootstrap.geometry.scaleY
        << ",\"markerResidualPixels\":" << observation.bootstrap.geometry.markerResidualPixels << "}"
        << ",\"laneErasures\":{\"base\":" << static_cast<unsigned int>(observation.baseLuma.erasureReason)
        << ",\"fine\":" << static_cast<unsigned int>(observation.fineLuma.erasureReason)
        << ",\"chroma\":" << static_cast<unsigned int>(observation.chroma.erasureReason) << "}"
        << ",\"laneMetrics\":[";
    for (std::size_t lane = 0; lane < observation.laneMetrics.size(); lane++)
    {
        stream << (lane == 0 ? "" : ",");
        WriteLaneMetricReport(stream, observation.laneMetrics[lane]);
    }
    stream << "],\"metricErasureHistograms\":[";
    for (std::size_t lane = 0; lane < erasuresByLane.size(); lane++)
    {
        stream << (lane == 0 ? "[" : ",[");
        for (std::size_t reason = 0; reason < erasuresByLane[lane].size(); reason++)
        {
            stream << (reason == 0 ? "" : ",") << erasuresByLane[lane][reason];
        }
        stream << ']';
    }
    stream << "],\"fecIterations\":" << fecIterations << ",\"acceptedBlocks\":" << observation.acceptedBlocks
        << ",\"acceptedTransportBlocks\":" << observation.acceptedTransportBlocks
        << ",\"acceptedControlRecords\":" << observation.acceptedControlRecords << ",\"slots\":[";
    for (std::size_t slot = 0; slot < observation.slots.size(); slot++)
    {
        stream << (slot == 0 ? "" : ",");
        WriteSlotReport(stream, observation.slots[slot]);
    }
    stream << "]}";
    WriteNew(reportPath, stream.str());
    std::cout << stream.str() << '\n';
}

void RunReceive(const std::filesystem::path& root, const std::uint32_t seconds)
{
    const auto safety = ResolveSafety();
    const auto region = ResolveRegion(safety);
    Evidence evidence(root);
    evidence.Record("preflight.json", PreflightReport(safety));
    const auto output = root / "recovered";
    Require(std::filesystem::create_directory(output), "output directory must be new");
    pbapp::DecoderRuntime runtime; // No injected demodulator, capture edge, FEC, source or acceptance oracle.
    std::uint64_t safetyChecks = 1;
    try
    {
        CheckTarget(safety);
        safetyChecks++;
        const auto started = runtime.Start(pbapp::MakeUnifiedDecoderConfig(output.wstring(), region));
        Require(static_cast<bool>(started), started.message);
        const auto began = Clock::now();
        const auto deadline = began + std::chrono::seconds(seconds);
        const auto runSampleInterval = SelectSampleInterval(seconds);
        auto nextSample = began;
        while (Clock::now() < deadline)
        {
            CheckTarget(safety);
            safetyChecks++;
            const auto snapshot = runtime.GetSnapshot();
            if (Clock::now() >= nextSample)
            {
                evidence.Sample(DecoderReport(snapshot, safetyChecks));
                nextSample = Clock::now() + runSampleInterval;
            }
            if (snapshot.state == pbapp::DecoderState::Completed || snapshot.state == pbapp::DecoderState::Failed)
            {
                break;
            }
            Require(snapshot.largeOutputConfirmationState != pbapp::LargeOutputConfirmationState::AwaitingDecision,
                "unexpected large-output confirmation; no automatic approval in the remote gate");
            std::this_thread::sleep_for(safetyInterval);
        }
        // Stop drains the unchanged production owner. Its cleanup errors must
        // remain authoritative; never accept an earlier Completed snapshot.
        runtime.Stop();
        CheckTarget(safety);
        safetyChecks++;
        evidence.Finish();
        const auto snapshot = runtime.GetSnapshot();
        evidence.Record("final.json", DecoderReport(snapshot, safetyChecks));
        evidence.Record("receiver-checks.json", GateReport(snapshot));
        Require(ReceiverChecksPassed(snapshot), "receiver did not complete clean whole-digest, publish, reopen, cleanup and conflict checks");
        Require(ReachesThreshold(PublishedMetric(snapshot), 16384.0), "published file does not meet the G21 16 KiB/unique hard gate");
        std::cout << "PASS: receiver-local publication and frame-metric checks only; external source and G21 lane checks remain pending\n";
    }
    catch (const std::exception& exception)
    {
        runtime.Stop();
        evidence.Record("failure-final.json", DecoderReport(runtime.GetSnapshot(), safetyChecks));
        evidence.Record("failure.txt", exception.what());
        throw;
    }
}

void RunPolicyChecks()
{
    Require(ParseTimeout(L"30") == minimumRunSeconds && ParseTimeout(L"3600") == maximumRunSeconds, "timeout boundary mismatch");
    Require(SelectSampleInterval(600) == sampleInterval && SelectSampleInterval(601) == extendedRunSampleInterval &&
        SelectSampleInterval(maximumRunSeconds) == extendedRunSampleInterval, "timeout sample interval boundary mismatch");
    for (const std::wstring_view invalid : {L"", L"0", L"29", L"3601", L"-1", L"+30", L"30x", L"4294967296"})
    {
        bool rejected = false;
        try
        {
            static_cast<void>(ParseTimeout(invalid));
        }
        catch (const std::exception&)
        {
            rejected = true;
        }
        Require(rejected, "invalid timeout accepted");
    }
    Require(ParseDiagnosticDimension(L"1") == 1 && ParseDiagnosticDimension(L"4096") == maximumDiagnosticDimension,
        "diagnostic dimension boundary mismatch");
    for (const std::wstring_view invalid : {L"", L"0", L"4097", L"-1", L"1x", L"4294967296"})
    {
        bool rejected = false;
        try
        {
            static_cast<void>(ParseDiagnosticDimension(invalid));
        }
        catch (const std::exception&)
        {
            rejected = true;
        }
        Require(rejected, "invalid diagnostic dimension accepted");
    }
    Require(CanAppendSample(0, 0, maximumSampleBytes - 1) && !CanAppendSample(0, 0, maximumSampleBytes) &&
        !CanAppendSample(maximumSampleCount, 0, 1) && !CanAppendSample(0, maximumEvidenceBytes, 1) &&
        !CanAppendSample(0, maximumEvidenceBytes + 1, 0), "evidence bounds mismatch");
    pbapp::MonitorSafetySelection safety;
    safety.protectedMonitor.monitor = reinterpret_cast<HMONITOR>(1);
    safety.protectedMonitor.deviceName = L"\\\\.\\DISPLAY1";
    safety.protectedMonitor.physicalRect = {0, 0, 2560, 1440};
    safety.experimentMonitor.monitor = reinterpret_cast<HMONITOR>(2);
    safety.experimentMonitor.deviceName = L"\\\\.\\DISPLAY2";
    safety.experimentMonitor.physicalRect = {2560, 0, 5120, 1440};
    safety.experimentMonitor.rotation = DXGI_MODE_ROTATION_IDENTITY;
    CheckApprovedTopology(safety);
    auto invalidSafety = safety;
    invalidSafety.experimentMonitor.physicalRect = {2559, 0, 5119, 1440};
    bool overlapRejected = false;
    try
    {
        CheckApprovedTopology(invalidSafety);
    }
    catch (const std::exception&)
    {
        overlapRejected = true;
    }
    Require(overlapRejected, "one-pixel left-screen overlap accepted");
    Require(!pbapp::ValidateMonitorSafetyTarget(safety, safety.protectedMonitor.physicalRect, safety.protectedMonitor.monitor),
        "protected monitor accepted as target");
    Require(PreflightReport(safety).find("\"captureStarted\":false") != std::string::npos, "preflight mislabeled as capture");
    pbapp::DecoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::UnifiedLc4;
    snapshot.state = pbapp::DecoderState::Completed;
    snapshot.wholeFileDigestCheck = true;
    snapshot.finalPublishSucceeded = true;
    snapshot.finalReopenVerified = true;
    snapshot.verifiedEncodedSegmentBytes = 1024 * 1024;
    snapshot.unifiedTelemetry.sessionBound = true;
    snapshot.unifiedTelemetry.observationAvailable = true;
    snapshot.unifiedTelemetry.uniqueFrames = 64;
    Require(ReceiverChecksPassed(snapshot) && ReachesThreshold(PublishedMetric(snapshot), 16384.0), "exact hard threshold rejected");
    snapshot.unifiedTelemetry.uniqueFrames = 65;
    Require(ReceiverChecksPassed(snapshot) && !ReachesThreshold(PublishedMetric(snapshot), 16384.0), "below hard threshold accepted");
    snapshot.wholeFileDigestCheck = false;
    Require(!ReceiverChecksPassed(snapshot) && !PublishedMetric(snapshot).bytesPerUniqueFrame, "bad digest accepted");
    snapshot.wholeFileDigestCheck = true;
    snapshot.finalReopenVerified = false;
    Require(!ReceiverChecksPassed(snapshot), "missing final reopen accepted");
    snapshot.finalReopenVerified = true;
    snapshot.resumeStateLoaded = true;
    Require(!ReceiverChecksPassed(snapshot) && !PublishedMetric(snapshot).bytesPerUniqueFrame, "resume treated as clean frame coverage");
    snapshot.resumeStateLoaded = false;
    snapshot.outerConflictRejections = 1;
    Require(!ReceiverChecksPassed(snapshot), "conflict accepted by remote gate");
    snapshot.outerConflictRejections = 0;
    snapshot.outerResourceRejections = 1;
    snapshot.outerProtocolResourceLimitExceededRejections = 1;
    Require(!ReceiverChecksPassed(snapshot), "Outer FEC resource rejection accepted by remote gate");
    snapshot.outerResourceRejections = 0;
    snapshot.outerProtocolResourceLimitExceededRejections = 0;
    snapshot.receiverResourcePolicyRejectedCount = 1;
    Require(!ReceiverChecksPassed(snapshot), "Receiver resource-policy rejection accepted by remote gate");
    snapshot.receiverResourcePolicyRejectedCount = 0;
    snapshot.outerOrphanDroppedByQuotaCount = 1;
    Require(!ReceiverChecksPassed(snapshot), "orphan-cache quota drop accepted by remote gate");
    snapshot.outerOrphanDroppedByQuotaCount = 0;
    snapshot.outerDeferredResourceBusyCount = 1;
    Require(!ReceiverChecksPassed(snapshot), "deferred Outer FEC resource pressure accepted by remote gate");
    snapshot.outerDeferredResourceBusyCount = 0;
    snapshot.outerFecQuotaExceededCount = 1;
    Require(!ReceiverChecksPassed(snapshot), "Outer FEC decoder quota event accepted by remote gate");
    snapshot.outerFecQuotaExceededCount = 0;
    snapshot.errorDetail = "Post-publish cleanup warning";
    Require(!ReceiverChecksPassed(snapshot), "published file with cleanup warning accepted as a clean gate");
    snapshot.errorDetail.clear();
    snapshot.unifiedTelemetry.frameCoverageComplete = false;
    Require(!PublishedMetric(snapshot).bytesPerUniqueFrame, "incomplete frame coverage accepted");
    snapshot.outerActiveDecoderLimit = pbapp::senderUnifiedActiveSegmentWindowSize;
    snapshot.outerTotalDecoderByteLimit = 1073741824ULL;
    snapshot.outerPeakActiveDecoderCount = pbapp::senderUnifiedActiveSegmentWindowSize;
    const std::string decoderReport = DecoderReport(snapshot, 7);
    Require(decoderReport.find("\"safetyRevalidations\":7") != std::string::npos &&
        decoderReport.find("\"outerDeferredResourceBusyCount\":0") != std::string::npos &&
        decoderReport.find("\"outerFecQuotaExceededCount\":0") != std::string::npos &&
        decoderReport.find("\"outerProtocolResourceLimitExceededRejections\":0") != std::string::npos &&
        decoderReport.find("\"outerOrphanDroppedByQuotaCount\":0") != std::string::npos &&
        decoderReport.find("\"outerActiveDecoderLimit\":8") != std::string::npos &&
        decoderReport.find("\"outerTotalDecoderByteLimit\":1073741824") != std::string::npos &&
        decoderReport.find("\"outerPeakActiveDecoderCount\":8") != std::string::npos &&
        GateReport(snapshot).find("\"G21Completed\":false") != std::string::npos, "report boundary mismatch");
    std::cout << "PASS: remote gate policy; no monitor enumeration, capture, windows, files or input\n";
}

} // namespace

int wmain(const int count, wchar_t* arguments[])
{
    try
    {
        Require(count >= 2, "expected --build-identity, --self-test, --inspect-bgra INPUT NEW_REPORT WIDTH HEIGHT, --preflight NEW_RUN or --receive NEW_RUN SECONDS");
        const std::wstring_view role(arguments[1]);
        if (role == L"--build-identity" && count == 2)
        {
            pbapp::WriteApplicationBuildIdentity(std::cout, "PBUnifiedRemoteGate.Decoder", pbcore::GetBuildInfo(),
                pbprotocol::GetProtocolVersion(), PB_GIT_COMMIT);
        }
        else if (role == L"--self-test" && count == 2)
        {
            RunPolicyChecks();
        }
        else if (role == L"--preflight" && count == 3)
        {
            const auto safety = ResolveSafety();
            static_cast<void>(ResolveRegion(safety));
            Evidence evidence(arguments[2]);
            evidence.Record("preflight.json", PreflightReport(safety));
            std::cout << "PASS: current right-monitor preflight only; capture not started\n";
        }
        else if (role == L"--inspect-bgra" && count == 6)
        {
            InspectRemotePixels(arguments[2], arguments[3], ParseDiagnosticDimension(arguments[4]),
                ParseDiagnosticDimension(arguments[5]));
        }
        else if (role == L"--receive" && count == 4)
        {
            const auto seconds = ParseTimeout(arguments[3]);
            RunReceive(arguments[2], seconds);
        }
        else
        {
            throw std::runtime_error("invalid remote gate arguments; no implicit live mode");
        }
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Unified remote gate failed: " << exception.what() << '\n';
        return 1;
    }
}
