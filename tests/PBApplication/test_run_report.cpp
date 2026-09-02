#include "run_report.h"
#include "evidence_journal.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>

namespace
{

class JournalScratch
{
public:
    JournalScratch()
    {
        path_ = std::filesystem::path(PB_TEST_SCRATCH_ROOT) / L"run-evidence-journal";
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        error.clear();
        REQUIRE(std::filesystem::create_directories(path_, error));
        REQUIRE_FALSE(error);
    }

    ~JournalScratch()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("Encoder report explicitly omits receiver progress goodput and ETA", "[application][report][encoder]")
{
    pbapp::EncoderSnapshot snapshot;
    snapshot.state = pbapp::EncoderState::Broadcasting;
    snapshot.runId = "run-1";
    snapshot.sessionTag = 7;
    snapshot.cycleCount = 3;
    snapshot.cyclePosition = 5;
    snapshot.cycleFrameCount = 10;
    snapshot.dataWindowLeft = 2560;
    snapshot.runStartedUnixMilliseconds = 1000;
    snapshot.runEndedUnixMilliseconds = 2000;
    snapshot.visualProfile = pbapp::VisualProfile::RemoteVisualLowFps;
    snapshot.generatedVisualFramesPerSecond = (std::numeric_limits<double>::infinity)();
    snapshot.configuredLogicalVisualFps = 5;
    snapshot.configuredLogicalDwellMilliseconds = 200.0;
    snapshot.minimumObservedLogicalDwellMilliseconds = 200.25;
    snapshot.visualProfileId = 0x504252564C463431ULL;
    snapshot.visualLayoutVersion = 7;
    snapshot.codedDataBytesPerFrame = 8100;
    snapshot.codewordsPerFrame = 4;
    snapshot.rawVisualBitsPerLogicalFrame = 64800;
    snapshot.innerFecInformationBytesPerLogicalFrame = 5400;
    snapshot.transportPayloadCeilingBytesPerLogicalFrame = 5256;
    snapshot.configuredTransportPayloadCeilingBytesPerSecond = 26280;
    snapshot.sourceTextureReplacements = 19;
    snapshot.repeatedPresentCalls = 211;
    snapshot.invalidatedActiveFrames = 3;
    snapshot.activeFrame = true;
    snapshot.activeFrameSequence = 18;
    snapshot.configuredControlRepetitions = 12;
    snapshot.monitorSafetyPreflightPassed = true;
    snapshot.monitorSafetyRevalidationCount = 17;
    snapshot.monitorSafetyStatus = "PASS";
    snapshot.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    snapshot.remoteMetadata.remoteProvider = "TestRemote";
    const pbapp::RunReportContext context{"PixelBridgeEncoder", "0.1.0", "deadbeef", "2026-08-30T00:00:00Z"};
    const std::string json = pbapp::BuildEncoderRunReportJson(context, snapshot);
    REQUIRE(json.find("\"state\":\"Broadcasting\"") != std::string::npos);
    REQUIRE(json.find("\"schema\":\"PixelBridge.RunReport.2\"") != std::string::npos);
    REQUIRE(json.find("\"cycleCount\":3") != std::string::npos);
    REQUIRE(json.find("\"receiverProgress\":null") != std::string::npos);
    REQUIRE(json.find("\"receiverEta\":null") != std::string::npos);
    REQUIRE(json.find("\"verifiedGoodput\":null") != std::string::npos);
    REQUIRE(json.find("\"runStartedUnixMilliseconds\":1000") != std::string::npos);
    REQUIRE(json.find("\"runEndedUnixMilliseconds\":2000") != std::string::npos);
    REQUIRE(json.find("\"dataWindow\":{\"left\":2560") != std::string::npos);
    REQUIRE(json.find("\"generatedVisualFramesPerSecond\":null") != std::string::npos);
    REQUIRE(json.find("\"generatedVisualFramesPerSecondBasis\":\"logical source replacements") != std::string::npos);
    REQUIRE(json.find("\"configuredLogicalVisualFps\":5") != std::string::npos);
    REQUIRE(json.find("\"configuredLogicalDwellMilliseconds\":200") != std::string::npos);
    REQUIRE(json.find("\"minimumObservedLogicalDwellMilliseconds\":200.25") != std::string::npos);
    REQUIRE(json.find("\"logicalDwellViolationCount\":0") != std::string::npos);
    REQUIRE(json.find("\"visualProfileId\":5783275402097472561") != std::string::npos);
    REQUIRE(json.find("\"visualLayoutVersion\":7") != std::string::npos);
    REQUIRE(json.find("\"codedDataBytesPerFrame\":8100") != std::string::npos);
    REQUIRE(json.find("\"codewordsPerFrame\":4") != std::string::npos);
    REQUIRE(json.find("\"capacityModel\":{\"rawVisualBitsPerLogicalFrame\":64800,\"innerFecInformationBytesPerLogicalFrame\":5400,\"transportPayloadCeilingBytesPerLogicalFrame\":5256,\"configuredTransportPayloadCeilingBytesPerSecond\":26280") != std::string::npos);
    REQUIRE(json.find("excludes Control dilution, retransmission, capture loss, FEC rejection, Receiver verification and publish") != std::string::npos);
    REQUIRE(json.find("\"generatedPayloadBytesPerSecond\":null") != std::string::npos);
    REQUIRE(json.find("\"generatedPayloadBytesPerSecondBasis\":\"actual Outer-FEC payload bytes") != std::string::npos);
    REQUIRE(json.find("\"sourceTextureReplacements\":19") != std::string::npos);
    REQUIRE(json.find("\"repeatedPresentCalls\":211") != std::string::npos);
    REQUIRE(json.find("\"invalidatedActiveFrames\":3") != std::string::npos);
    REQUIRE(json.find("\"activeFrame\":true") != std::string::npos);
    REQUIRE(json.find("\"activeFrameSequence\":18") != std::string::npos);
    REQUIRE(json.find("\"configuredControlRepetitions\":12") != std::string::npos);
    REQUIRE(json.find("\"schema\":\"PixelBridge.RemoteVisualRunMetadata.1\"") != std::string::npos);
    REQUIRE(json.find("\"remoteProvider\":\"TestRemote\"") != std::string::npos);
    REQUIRE(json.find("\"evidence\":{\"journalEnabled\":false,\"valid\":true") != std::string::npos);
    REQUIRE(json.find("\"monitorSafety\":{\"preflightPassed\":true,\"revalidationCount\":17,\"status\":\"PASS\"}") != std::string::npos);
    REQUIRE(json.find("transferProgress") == std::string::npos);

    const std::string journalRecord = pbapp::BuildEncoderJournalRecord(1234, snapshot);
    REQUIRE(journalRecord.find("\"configuredLogicalDwellMs\":200") != std::string::npos);
    REQUIRE(journalRecord.find("\"minimumObservedLogicalDwellMs\":200.25") != std::string::npos);
    REQUIRE(journalRecord.find("\"sourceTextureReplacements\":19") != std::string::npos);
    REQUIRE(journalRecord.find("\"repeatedPresentCalls\":211") != std::string::npos);
    REQUIRE(journalRecord.find("\"invalidatedActiveFrames\":3") != std::string::npos);
    REQUIRE(journalRecord.find("\"activeFrameSequence\":18") != std::string::npos);
    REQUIRE(journalRecord.find("\"generatedVisualFps\":null") != std::string::npos);
    REQUIRE(journalRecord.find("\"generatedPayloadBytesPerSecond\":null") != std::string::npos);
    REQUIRE(journalRecord.find("\"configuredTransportPayloadCeilingBytesPerSecond\":26280") != std::string::npos);
    REQUIRE(journalRecord.find("\"monitorSafetyPreflightPassed\":true") != std::string::npos);
    REQUIRE(journalRecord.find("\"monitorSafetyRevalidationCount\":17") != std::string::npos);
}

TEST_CASE("Decoder report preserves verified progress and final acceptance independently", "[application][report][decoder]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::Failed;
    snapshot.visualProfile = pbapp::VisualProfile::RemoteVisualLowFps;
    snapshot.visualProfileId = 0x504252564C463431ULL;
    snapshot.visualLayoutVersion = 7;
    snapshot.codedDataBytesPerFrame = 8100;
    snapshot.codewordsPerFrame = 4;
    snapshot.descriptorKnown = true;
    snapshot.originalFileBytes = 100;
    snapshot.verifiedRawBytes = 100;
    snapshot.remainingRawBytes = 0;
    snapshot.recoveryProgress = 1.0;
    snapshot.smoothedVerifiedRawGoodputBytesPerSecond = 4096.0;
    snapshot.verifiedEncodedBytes = 80;
    snapshot.verifiedEncodedGoodputBitsPerSecond = 512.0;
    snapshot.captureFps = 60.0;
    snapshot.captureArrivedFrames = 120;
    snapshot.captureCopiedFrames = 119;
    snapshot.captureDeliveredFrames = 118;
    snapshot.captureDroppedFrames = 2;
    snapshot.captureAcquireTimeouts = 3;
    snapshot.capturePointerOnlyFrames = 4;
    snapshot.captureAccumulatedFrames = 121;
    snapshot.captureAccessLostEvents = 1;
    snapshot.captureExpiredFrames = 5;
    snapshot.captureStaleFrames = 6;
    snapshot.captureCursorErasures = 7;
    snapshot.captureFrameAgeHighWater100ns = 80000;
    snapshot.captureReadbackDropEvents = 9;
    snapshot.fingerprintedFrames = 0;
    snapshot.uniqueVisualFps.reset();
    snapshot.admittedFrameSequenceFps = 59.5;
    snapshot.wholeFileDigestVerified = false;
    snapshot.finalPublishSucceeded = false;
    snapshot.backendReason = "WGC unavailable; no fallback was attempted";
    snapshot.roiLeft = 10;
    snapshot.roiTop = 20;
    snapshot.roiWidth = 1920;
    snapshot.roiHeight = 1080;
    snapshot.evaluatedDataFrames = 3;
    snapshot.evaluatedCodewords = 12;
    snapshot.postFecFailedFrames = 2;
    snapshot.fecFailures = 1;
    snapshot.crcFailures = 1;
    snapshot.fecFrameErrorRate = 2.0 / 3.0;
    snapshot.fecCodewordFailureRate = 1.0 / 12.0;
    snapshot.fecAcceptedTransportBlocks = 10;
    snapshot.fecAcceptedTransportBlockRate = 10.0 / 12.0;
    snapshot.acceptedTransportBlocks = 8;
    snapshot.temporallyAdmittedTransportBlocks = 8;
    snapshot.comparedCodedBits = 194400;
    snapshot.erroneousCodedBits = 7;
    snapshot.falseAcceptedCodewords = 0;
    snapshot.falseAcceptedCodewordsAvailable = false;
    snapshot.remoteDuplicateRefinementAttempts = 8;
    snapshot.remoteDuplicateRefinementRecoveries = 1;
    snapshot.remoteMetricFrames = 9;
    snapshot.remoteMetricSamples = 583200;
    snapshot.remoteZeroMagnitudeMetrics = 29160;
    snapshot.remoteZeroMagnitudeMetricRate = 0.05;
    snapshot.remoteMinimumAbsoluteMetric = 0.0;
    snapshot.remoteMeanAbsoluteMetric = 0.8125;
    snapshot.remoteVerifiedMetricFrames = 3;
    snapshot.remoteRejectedMetricFrames = 6;
    snapshot.remoteVerifiedMeanAbsoluteMetric = 0.9375;
    snapshot.remoteRejectedMeanAbsoluteMetric = 0.75;
    snapshot.remoteRejectedZeroMagnitudeMetricRate = 0.0625;
    snapshot.remoteSymbolSamples = 150507;
    snapshot.remoteUnreliableSymbols = 1505;
    snapshot.remoteUnreliableSymbolRate = 1505.0 / 150507.0;
    snapshot.remoteFreshnessRegions = 693;
    snapshot.remoteFreshRegions = 676;
    snapshot.remoteStaleRegions = 17;
    snapshot.remoteStaleRegionRate = 17.0 / 693.0;
    snapshot.remoteFramesWithStaleRegions = 4;
    snapshot.remoteFreshnessTagMismatches = 51;
    snapshot.remoteFreshnessTagErasures = 12;
    snapshot.remoteFreshnessErasedDataMetrics = 2048;
    snapshot.remoteFreshnessErasedDataMetricRate = 2048.0 / 583200.0;
    snapshot.outerUniqueSymbols = 99;
    snapshot.outerIdenticalDuplicateSymbols = 7;
    snapshot.outerRecoveryAlreadyReadySymbols = 3;
    snapshot.outerAlreadyCompletedSymbols = 2;
    snapshot.outerRecoveryReadyEvents = 1;
    snapshot.outerResourceRejections = 4;
    snapshot.outerConflictRejections = 5;
    snapshot.captureStallCount = 2;
    snapshot.captureStallTotalMilliseconds = 3000;
    snapshot.visualStallCount = 4;
    snapshot.replayEnabled = true;
    snapshot.replayDiagnosticOnly = true;
    snapshot.replayCaptureOnly = true;
    snapshot.replayOfflineMode = true;
    snapshot.replaySampledOutFrames = 37;
    snapshot.replayMaximumCaptureFramesPerSecond = 10;
    snapshot.replaySamplingInterval100ns = 1000000;
    snapshot.replayOfflineCaptureFrames = 9;
    snapshot.replayOfflineDemodResults = 9;
    snapshot.replayOfflineObservationComparisons = 8;
    snapshot.replayOfflineObservationMismatches = 2;
    snapshot.monitorSafetyPreflightPassed = true;
    snapshot.monitorSafetyRevalidationCount = 23;
    snapshot.monitorSafetyStatus = "PASS";
    const pbapp::RunReportContext context{"PixelBridgeDecoder", "0.1.0", "deadbeef", "2026-08-30T00:00:00Z"};
    const std::string json = pbapp::BuildDecoderRunReportJson(context, snapshot);
    REQUIRE(json.find("\"state\":\"Failed\"") != std::string::npos);
    REQUIRE(json.find("\"recoveryProgress\":1") != std::string::npos);
    REQUIRE(json.find("\"wholeFileDigestVerified\":false") != std::string::npos);
    REQUIRE(json.find("\"finalPublishSucceeded\":false") != std::string::npos);
    REQUIRE(json.find("\"actualBackend\":null") != std::string::npos);
    REQUIRE(json.find("\"monitorSafety\":{\"preflightPassed\":true,\"revalidationCount\":23,\"status\":\"PASS\"}") != std::string::npos);
    REQUIRE(json.find("no fallback was attempted") != std::string::npos);
    REQUIRE(json.find("\"roi\":{\"left\":10,\"top\":20,\"width\":1920,\"height\":1080") != std::string::npos);
    REQUIRE(json.find("\"visualProfileId\":5783275402097472561") != std::string::npos);
    REQUIRE(json.find("\"visualLayoutVersion\":7") != std::string::npos);
    REQUIRE(json.find("\"codedDataBytesPerFrame\":8100") != std::string::npos);
    REQUIRE(json.find("\"codewordsPerFrame\":4") != std::string::npos);
    REQUIRE(json.find("\"evaluatedDataFrames\":3") != std::string::npos);
    REQUIRE(json.find("\"evaluatedCodewords\":12") != std::string::npos);
    REQUIRE(json.find("\"fecAcceptedTransportBlocks\":10") != std::string::npos);
    REQUIRE(json.find("\"fecAcceptedTransportBlockRate\":0.833") != std::string::npos);
    REQUIRE(json.find("\"acceptedTransportBlocks\":8") != std::string::npos);
    REQUIRE(json.find("\"temporallyAdmittedTransportBlocks\":8") != std::string::npos);
    REQUIRE(json.find("\"comparedCodedBits\":194400") != std::string::npos);
    REQUIRE(json.find("\"erroneousCodedBits\":7") != std::string::npos);
    REQUIRE(json.find("\"falseAcceptedCodewords\":null") != std::string::npos);
    REQUIRE(json.find("\"falseAcceptedCodewordsUnavailableReason\":\"Production receive has no independent truth oracle\"") != std::string::npos);
    REQUIRE(json.find("\"outerAdmission\":{\"uniqueSymbols\":99,\"identicalDuplicateSymbols\":7,\"recoveryAlreadyReadySymbols\":3,\"alreadyCompletedSymbols\":2,\"recoveryReadyEvents\":1,\"resourceRejections\":4,\"conflictRejections\":5}") != std::string::npos);
    REQUIRE(json.find("\"remoteDuplicateRefinementAttempts\":8") != std::string::npos);
    REQUIRE(json.find("\"remoteDuplicateRefinementRecoveries\":1") != std::string::npos);
    REQUIRE(json.find("\"remoteMetricTelemetry\":{\"frames\":9,\"samples\":583200,\"zeroMagnitudeMetrics\":29160") != std::string::npos);
    REQUIRE(json.find("\"zeroMagnitudeRate\":0.05") != std::string::npos);
    REQUIRE(json.find("\"minimumAbsoluteMetric\":0") != std::string::npos);
    REQUIRE(json.find("\"meanAbsoluteMetric\":0.8125") != std::string::npos);
    REQUIRE(json.find("\"verifiedFrames\":3") != std::string::npos);
    REQUIRE(json.find("\"rejectedFrames\":6") != std::string::npos);
    REQUIRE(json.find("\"transportEvaluatedFrames\":9") != std::string::npos);
    REQUIRE(json.find("\"nonTransportFrames\":0") != std::string::npos);
    REQUIRE(json.find("\"symbolSamples\":150507") != std::string::npos);
    REQUIRE(json.find("\"unreliableSymbols\":1505") != std::string::npos);
    REQUIRE(json.find("\"unreliableSymbolRate\":0.009") != std::string::npos);
    REQUIRE(json.find("\"verifiedMeanAbsoluteMetric\":0.9375") != std::string::npos);
    REQUIRE(json.find("\"rejectedMeanAbsoluteMetric\":0.75") != std::string::npos);
    REQUIRE(json.find("\"rejectedZeroMagnitudeRate\":0.0625") != std::string::npos);
    REQUIRE(json.find("\"freshnessRegions\":693") != std::string::npos);
    REQUIRE(json.find("\"freshRegions\":676") != std::string::npos);
    REQUIRE(json.find("\"staleRegions\":17") != std::string::npos);
    REQUIRE(json.find("\"framesWithStaleRegions\":4") != std::string::npos);
    REQUIRE(json.find("\"freshnessTagMismatches\":51") != std::string::npos);
    REQUIRE(json.find("\"freshnessTagErasures\":12") != std::string::npos);
    REQUIRE(json.find("\"freshnessErasedDataMetrics\":2048") != std::string::npos);
    REQUIRE(json.find("\"freshnessErasedDataMetricRate\":0.003") != std::string::npos);
    REQUIRE(json.find("\"highConfidenceWrongCodewords\":null") != std::string::npos);
    REQUIRE(json.find("no independent per-codeword truth oracle") != std::string::npos);
    REQUIRE(json.find("\"remoteMetricTelemetry\"") != std::string::npos);
    REQUIRE(json.find("\"unavailableReason\":\"\"") != std::string::npos);
    REQUIRE(json.find("\"captureStall\":{\"count\":2,\"totalMilliseconds\":3000") != std::string::npos);
    REQUIRE(json.find("\"visualStall\":{\"count\":4") != std::string::npos);
    REQUIRE(json.find("\"smoothedVerifiedRawGoodputBytesPerSecond\":4096") != std::string::npos);
    REQUIRE(json.find("\"verifiedRawGoodputBasis\":\"verifiedRawBytes\"") != std::string::npos);
    REQUIRE(json.find("\"verifiedEncodedBytes\":80") != std::string::npos);
    REQUIRE(json.find("\"verifiedEncodedGoodputBitsPerSecond\":512") != std::string::npos);
    REQUIRE(json.find("\"verifiedEncodedGoodputGate\":\"WholeFileDigest+finalPublish\"") != std::string::npos);
    REQUIRE(json.find("\"fecCodewordFailureRate\":0.083") != std::string::npos);
    REQUIRE(json.find("\"captureFps\":60") != std::string::npos);
    REQUIRE(json.find("\"captureArrivedFrames\":120") != std::string::npos);
    REQUIRE(json.find("\"captureCopiedFrames\":119") != std::string::npos);
    REQUIRE(json.find("\"captureDeliveredFrames\":118") != std::string::npos);
    REQUIRE(json.find("\"captureDroppedFrames\":2") != std::string::npos);
    REQUIRE(json.find("\"captureAcquireTimeouts\":3") != std::string::npos);
    REQUIRE(json.find("\"capturePointerOnlyFrames\":4") != std::string::npos);
    REQUIRE(json.find("\"captureAccumulatedFrames\":121") != std::string::npos);
    REQUIRE(json.find("\"captureAccessLostEvents\":1") != std::string::npos);
    REQUIRE(json.find("\"captureExpiredFrames\":5") != std::string::npos);
    REQUIRE(json.find("\"captureStaleFrames\":6") != std::string::npos);
    REQUIRE(json.find("\"captureCursorErasures\":7") != std::string::npos);
    REQUIRE(json.find("\"captureFrameAgeHighWater100ns\":80000") != std::string::npos);
    REQUIRE(json.find("\"captureReadbackDropEvents\":9") != std::string::npos);
    REQUIRE(json.find("\"fingerprintedFrames\":0") != std::string::npos);
    REQUIRE(json.find("\"uniqueVisualFps\":null") != std::string::npos);
    REQUIRE(json.find("\"uniqueVisualFpsBasis\":\"distinct legal (CaptureEpoch,SessionTag,FrameSequence)\"") != std::string::npos);
    REQUIRE(json.find("\"roiPixelDigestUniqueVisualFps\":null") != std::string::npos);
    REQUIRE(json.find("production D3D11 fast path has no full-ROI CPU pixel digest") != std::string::npos);
    REQUIRE(json.find("\"admittedFrameSequenceFps\":59.5") != std::string::npos);
    REQUIRE(json.find("\"offlineMode\":true") != std::string::npos);
    REQUIRE(json.find("\"captureOnly\":true") != std::string::npos);
    REQUIRE(json.find("\"sampledOutFrames\":37") != std::string::npos);
    REQUIRE(json.find("\"maximumCaptureFramesPerSecond\":10") != std::string::npos);
    REQUIRE(json.find("\"samplingInterval100ns\":1000000") != std::string::npos);
    REQUIRE(json.find("\"offlineCaptureFrames\":9") != std::string::npos);
    REQUIRE(json.find("\"offlineDemodResults\":9") != std::string::npos);
    REQUIRE(json.find("\"offlineObservationComparisons\":8") != std::string::npos);
    REQUIRE(json.find("\"offlineObservationMismatches\":2") != std::string::npos);
    REQUIRE(json.find("smoothedVerifiedGoodputBytesPerSecond") == std::string::npos);
    REQUIRE(json.find("duplicateVisualFrames") == std::string::npos);

    const std::string journalRecord = pbapp::BuildDecoderJournalRecord(1234, snapshot);
    REQUIRE(journalRecord.find("\"captureExpiredFrames\":5") != std::string::npos);
    REQUIRE(journalRecord.find("\"captureStaleFrames\":6") != std::string::npos);
    REQUIRE(journalRecord.find("\"captureReadbackDropEvents\":9") != std::string::npos);
    REQUIRE(journalRecord.find("\"outerUniqueSymbols\":99") != std::string::npos);
    REQUIRE(journalRecord.find("\"outerConflictRejections\":5") != std::string::npos);
    REQUIRE(journalRecord.find("\"evaluatedCodewords\":12") != std::string::npos);
    REQUIRE(journalRecord.find("\"fecAcceptedTransportBlocks\":10") != std::string::npos);
    REQUIRE(journalRecord.find("\"temporallyAdmittedTransportBlocks\":8") != std::string::npos);
    REQUIRE(journalRecord.find("\"remoteMetricSamples\":583200") != std::string::npos);
    REQUIRE(journalRecord.find("\"remoteSymbolSamples\":150507") != std::string::npos);
    REQUIRE(journalRecord.find("\"monitorSafetyPreflightPassed\":true") != std::string::npos);
    REQUIRE(journalRecord.find("\"monitorSafetyRevalidationCount\":23") != std::string::npos);
    REQUIRE(journalRecord.find("\"replayCaptureOnly\":true") != std::string::npos);
    REQUIRE(journalRecord.find("\"replayWrittenFrames\":0") != std::string::npos);
    REQUIRE(journalRecord.find("\"replaySampledOutFrames\":37") != std::string::npos);
}

TEST_CASE("Run evidence journal is exclusive cadence-bounded and records terminal evidence without a queue",
    "[application][report][journal]")
{
    JournalScratch scratch;
    const auto path = scratch.Path() / L"encoder-run.ndjson";
    pbapp::RunJournalLimits limits;
    limits.samplingIntervalMilliseconds = 1000;
    limits.maximumDurationMilliseconds = 5000;
    limits.maximumBytes = 4096;
    limits.maximumRecordBytes = 1024;
    std::unique_ptr<pbapp::RunEvidenceJournal> journal;
    auto status = pbapp::RunEvidenceJournal::Create(path, limits, journal);
    REQUIRE(status.enabled);
    REQUIRE(status.valid);
    REQUIRE(journal != nullptr);

    pbapp::EncoderSnapshot snapshot;
    snapshot.runId = "0123456789abcdef0123456789abcdef";
    snapshot.state = pbapp::EncoderState::Broadcasting;
    const std::string record = pbapp::BuildEncoderJournalRecord(1000, snapshot);
    status = journal->AppendSample(0, record);
    REQUIRE(status.samples == 1);
    status = journal->AppendSample(999, record);
    REQUIRE(status.samples == 1);
    status = journal->AppendSample(1000, record);
    REQUIRE(status.samples == 2);
    snapshot.state = pbapp::EncoderState::Stopped;
    status = journal->AppendTerminal(1001, pbapp::BuildEncoderJournalRecord(2001, snapshot));
    REQUIRE(status.samples == 3);
    status = journal->Finish();
    REQUIRE(status.valid);
    REQUIRE(status.finished);
    REQUIRE_FALSE(status.truncated);

    std::ifstream input(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE(static_cast<std::uint64_t>(bytes.size()) == status.bytesWritten);
    REQUIRE(std::count(bytes.begin(), bytes.end(), '\n') == 3);
    REQUIRE(bytes.find("\"state\":\"Stopped\"") != std::string::npos);

    std::unique_ptr<pbapp::RunEvidenceJournal> duplicate;
    const auto duplicateStatus = pbapp::RunEvidenceJournal::Create(path, limits, duplicate);
    REQUIRE(duplicateStatus.enabled);
    REQUIRE_FALSE(duplicateStatus.valid);
    REQUIRE(duplicate == nullptr);
}

TEST_CASE("Run evidence journal stops at duration or byte caps and malformed records invalidate only evidence",
    "[application][report][journal][limits]")
{
    JournalScratch scratch;
    pbapp::RunJournalLimits limits;
    limits.samplingIntervalMilliseconds = 1;
    limits.maximumDurationMilliseconds = 2;
    limits.maximumBytes = 128;
    limits.maximumRecordBytes = 128;
    std::unique_ptr<pbapp::RunEvidenceJournal> durationJournal;
    REQUIRE(pbapp::RunEvidenceJournal::Create(scratch.Path() / L"duration.ndjson", limits, durationJournal).valid);
    auto status = durationJournal->AppendSample(2, "{\"event\":1}");
    REQUIRE(status.valid);
    REQUIRE(status.truncated);
    REQUIRE(status.samples == 0);
    REQUIRE(durationJournal->Finish().finished);

    limits.maximumDurationMilliseconds = 100;
    limits.maximumBytes = 15;
    std::unique_ptr<pbapp::RunEvidenceJournal> byteJournal;
    REQUIRE(pbapp::RunEvidenceJournal::Create(scratch.Path() / L"bytes.ndjson", limits, byteJournal).valid);
    status = byteJournal->AppendSample(0, "{\"event\":12345}");
    REQUIRE(status.valid);
    REQUIRE(status.truncated);
    REQUIRE(status.samples == 0);

    limits.maximumBytes = 128;
    std::unique_ptr<pbapp::RunEvidenceJournal> malformedJournal;
    REQUIRE(pbapp::RunEvidenceJournal::Create(scratch.Path() / L"malformed.ndjson", limits, malformedJournal).valid);
    status = malformedJournal->AppendSample(0, "not-json");
    REQUIRE_FALSE(status.valid);
    REQUIRE(status.invalidReason.find("single-line JSON") != std::string::npos);
}

TEST_CASE("Run evidence journal invalidates evidence when elapsed time moves backwards",
    "[application][report][journal][time]")
{
    JournalScratch scratch;
    pbapp::RunJournalLimits limits;
    limits.samplingIntervalMilliseconds = 1000;
    limits.maximumDurationMilliseconds = 10000;
    limits.maximumBytes = 4096;
    limits.maximumRecordBytes = 1024;

    std::unique_ptr<pbapp::RunEvidenceJournal> sampleJournal;
    REQUIRE(pbapp::RunEvidenceJournal::Create(scratch.Path() / L"sample-regression.ndjson", limits,
        sampleJournal).valid);
    auto status = sampleJournal->AppendSample(2000, "{\"event\":1}");
    REQUIRE(status.valid);
    REQUIRE(status.samples == 1);
    status = sampleJournal->AppendSample(1999, "{\"event\":2}");
    REQUIRE_FALSE(status.valid);
    REQUIRE(status.samples == 1);
    REQUIRE(status.invalidReason.find("moved backwards") != std::string::npos);

    std::unique_ptr<pbapp::RunEvidenceJournal> terminalJournal;
    REQUIRE(pbapp::RunEvidenceJournal::Create(scratch.Path() / L"terminal-regression.ndjson", limits,
        terminalJournal).valid);
    status = terminalJournal->AppendSample(3000, "{\"event\":1}");
    REQUIRE(status.valid);
    status = terminalJournal->AppendTerminal(2999, "{\"event\":2}");
    REQUIRE_FALSE(status.valid);
    REQUIRE(status.samples == 1);
    REQUIRE(status.invalidReason.find("moved backwards") != std::string::npos);
}
