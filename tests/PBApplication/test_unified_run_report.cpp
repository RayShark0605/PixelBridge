#include "run_report.h"
#include "unified_decoder_test_support.h"
#include "pbstorage/output_file.h"
#include "pbprotocol/transport_block_codec.h"

#include <catch2/catch_test_macros.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <tuple>

namespace
{
class G17Scratch
{
public:
    G17Scratch() : root_(std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal()),
        path_(root_ / (L"g17-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())))
    {
        REQUIRE(path_.parent_path() == root_);
        REQUIRE(std::filesystem::create_directory(path_));
    }
    ~G17Scratch()
    {
        if (path_.parent_path() == root_ && path_.filename().wstring().starts_with(L"g17-"))
        {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    std::filesystem::path Directory(const wchar_t* name) const
    {
        const auto path = path_ / name;
        std::filesystem::create_directories(path);
        return path;
    }
private:
    std::filesystem::path root_;
    std::filesystem::path path_;
};

QJsonObject Json(const std::string& text)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    REQUIRE(error.error == QJsonParseError::NoError);
    REQUIRE(document.isObject());
    return document.object();
}

bool Terminal(const pbapp::DecoderRuntime& runtime)
{
    const auto state = runtime.GetSnapshot().state;
    return state == pbapp::DecoderState::Completed || state == pbapp::DecoderState::Failed;
}

auto DecodeParameters(const pbdemodd3d11::CaptureDemodulatorConfig& config)
{
    const auto& policy = config.unifiedVisualPolicy;
    const auto& locator = policy.locator;
    return std::tie(config.visualProfileId, config.slotCount, config.maximumFrameAgeMilliseconds,
        config.resultQueueCapacity, config.maximumResidentBytes, config.evaluationMode,
        config.maximumRoiWidth, config.maximumRoiHeight, policy.minimumLumaLevelGap,
        policy.maximumPilotDeviation, policy.minimumChromaSeparation, policy.maximumPhasePilotResidual,
        policy.maximumTimingBitErrorFraction, policy.minimumDecisionMetric, policy.maximumFecIterations,
        locator.maximumWorkUnits, locator.maximumMarkers, locator.maximumGeometries,
        locator.maximumRefinementIterations, locator.minimumScale, locator.maximumScale,
        locator.minimumContrast, locator.maximumMarkerResidual, locator.maximumBootstrapResidual,
        locator.maximumTimingResidual, locator.maximumTimingBitErrorFraction, locator.maximumMidGrayFraction,
        locator.midGrayBoundary, locator.maximumGeometryResidualPixels);
}

void ChangeValidTransportPayload(pbdemodd3d11::CaptureDemodulatorResult& frame)
{
    const auto blocks = std::span(frame.demodulation.acceptedUnifiedBlocks).first(frame.demodulation.acceptedUnifiedBlockCount);
    const auto transport = std::ranges::find_if(blocks, [](const auto& block)
    {
        return block.kind == pbmodulation::UnifiedSlotKind::Transport;
    });
    REQUIRE(transport != blocks.end());
    const auto parsed = pbprotocol::ParseTransportBlock(std::span(transport->bytes).first(transport->size));
    REQUIRE(parsed);
    auto payload = std::vector<std::byte>(parsed.Value().payload.begin(), parsed.Value().payload.end());
    REQUIRE_FALSE(payload.empty());
    payload[0] ^= std::byte{1};
    REQUIRE(pbprotocol::SerializeTransportBlock(parsed.Value().header, payload, std::span(transport->bytes).first(transport->size)));
}

const pbapp::RunReportContext context{"test", "g17", "fixture-commit", "2026-09-04T00:00:00Z"};
} // namespace

TEST_CASE("G17 report has a versioned Unified boundary and never upgrades legacy diagnostics", "[application][report][g17]")
{
    pbapp::EncoderSnapshot encoder;
    pbapp::DecoderSnapshot decoder;
    REQUIRE(Json(pbapp::BuildEncoderRunReportJson(context, encoder))["schema"] == "PixelBridge.RunReport.2");
    REQUIRE(Json(pbapp::BuildDecoderRunReportJson(context, decoder))["schema"] == "PixelBridge.RunReport.2");
    encoder.visualProfile = decoder.visualProfile = pbapp::VisualProfile::UnifiedLc4;
    const auto encoderJson = Json(pbapp::BuildEncoderRunReportJson(context, encoder));
    const auto decoderJson = Json(pbapp::BuildDecoderRunReportJson(context, decoder));
    REQUIRE(encoderJson["schema"] == "PixelBridge.RunReport.3");
    REQUIRE(decoderJson["schema"] == "PixelBridge.RunReport.3");
    REQUIRE(encoderJson["receiverProgress"].isNull());
    REQUIRE(encoderJson["verifiedGoodput"].isNull());
    REQUIRE(encoderJson["scheduler"].toObject()["controlSlotOccupancy"].isNull());
    REQUIRE(encoderJson["scheduler"].toObject()["frameSequenceLeaseEnd"].isNull());
    REQUIRE(encoderJson["scheduler"].toObject()["repairIdLeaseEnd"].isNull());
    REQUIRE(decoderJson["publish"].toObject()["finalReopenVerified"].isNull());
    REQUIRE(decoderJson["verifiedEncodedBytesPerUniqueFrame"].isNull());
    REQUIRE(decoderJson["unifiedTelemetry"].toObject()["lanes"].toArray()[0].toObject()["metrics"].isNull());
    REQUIRE(decoderJson.contains("NonDecodingOperatorMetadata"));
    REQUIRE_FALSE(decoderJson.contains("remoteMetadata"));
    encoder.preparationComplete = true;
    encoder.preparedSourceBytes = 100;
    encoder.preparedSegmentCount = 1;
    encoder.preparationMilliseconds = 7;
    encoder.resumeVerificationMilliseconds = 8;
    encoder.submittedLogicalFrames = 2;
    encoder.submittedControlSlots = 12;
    encoder.cycleCount = 3;
    encoder.durableRepairIdLeaseEnd = 123;
    const auto populated = Json(pbapp::BuildEncoderRunReportJson(context, encoder));
    REQUIRE(populated["scheduler"].toObject()["controlSlotOccupancy"].toDouble() == 12.0 / 62.0);
    REQUIRE(populated["scheduler"].toObject()["repairIdLeaseEnd"].toInt() == 123);
    REQUIRE(populated["preparation"].toObject()["resumeVerificationMilliseconds"].toInt() == 8);
    encoder.controlSlotCounterOverflow = true;
    REQUIRE(Json(pbapp::BuildEncoderRunReportJson(context, encoder))["scheduler"].toObject()["submittedControlSlots"].isNull());
}

TEST_CASE("G17 actual Unified pixels feed truthful final reports independently of provider metadata", "[application][report][g17]")
{
    G17Scratch scratch;
    std::vector<std::byte> bytes;
    SECTION("zero-byte")
    {
    }
    SECTION("RAW Wirehair")
    {
        bytes = g16test::RawBytes(20000);
    }
    pbapp::EncoderSnapshot encoderSnapshot;
    const auto frames = g16test::MakeFrames(scratch.Directory(L"tx"), bytes, 1, &encoderSnapshot);
    const std::uint64_t expectedControlSlots = bytes.empty() ? 8 : 12;
    REQUIRE(encoderSnapshot.state == pbapp::EncoderState::Stopped);
    REQUIRE(encoderSnapshot.errorDetail.empty());
    REQUIRE(encoderSnapshot.preparationComplete);
    REQUIRE(encoderSnapshot.submittedLogicalFrames == 1);
    REQUIRE(encoderSnapshot.submittedControlSlots == expectedControlSlots);
    REQUIRE(encoderSnapshot.submittedControlSlots == frames[0].demodulation.unifiedObservation.acceptedControlRecords);
    REQUIRE(encoderSnapshot.durableFrameSequenceLeaseEnd > 0);
    const auto encoderReport = pbapp::BuildEncoderRunReportJson(context, encoderSnapshot);
    REQUIRE(Json(encoderReport)["scheduler"].toObject()["submittedControlSlots"].toDouble() == static_cast<double>(expectedControlSlots));
    {
        std::ofstream evidence(std::filesystem::path(PB_TEST_SCRATCH_ROOT) / L"g17-encoder.json");
        evidence << encoderReport;
        REQUIRE(evidence.good());
    }
    QJsonObject firstTelemetry;
    pbdemodd3d11::CaptureDemodulatorConfig firstDemod;
    for (int provider = 0; provider < 2; provider++)
    {
        const auto state = std::make_shared<g16test::ReceiveState>();
        state->Push(frames[0]);
        auto config = pbapp::MakeUnifiedDecoderConfig(scratch.Directory(provider == 0 ? L"first" : L"second").wstring(), g16test::Region());
        config.remoteMetadata.remoteProvider = provider == 0 ? "Fixture A" : "Fixture B \"quoted\"\nUTF8 provider";
        config.remoteMetadata.remoteMode = provider == 0 ? "quality" : "speed";
        config.remoteMetadata.providerVersion = provider == 0 ? "1" : "999";
        pbapp::DecoderRuntime runtime(g16test::Services(state));
        REQUIRE(runtime.Start(config));
        REQUIRE(g16test::WaitFor([&]()
        {
            return Terminal(runtime);
        }));
        runtime.Stop();
        const auto snapshot = runtime.GetSnapshot();
        INFO(snapshot.errorDetail);
        REQUIRE(g16test::VerifyOutput(snapshot, bytes));
        REQUIRE(snapshot.wholeFileDigestCheck == true);
        REQUIRE(snapshot.finalRenameSucceeded == true);
        REQUIRE(snapshot.finalReopenVerified == true);
        REQUIRE(snapshot.verifiedEncodedSegmentBytes == bytes.size());
        REQUIRE(snapshot.unifiedTelemetry.uniqueFrames == 1);
        REQUIRE(snapshot.unifiedTelemetry.observations == 1);
        const auto report = Json(pbapp::BuildDecoderRunReportJson(context, snapshot));
        REQUIRE(report["verifiedEncodedBytesPerUniqueFrame"].toDouble(-1) == static_cast<double>(bytes.size()));
        REQUIRE(report["NonDecodingOperatorMetadata"].toObject()["remoteProvider"].toString().toStdString() == config.remoteMetadata.remoteProvider);
        const auto telemetry = report["unifiedTelemetry"].toObject();
        for (const auto& lane : telemetry["lanes"].toArray())
        {
            REQUIRE(lane.toObject()["metrics"].toObject()["samples"].toDouble() > 0);
        }
        if (provider == 0)
        {
            firstTelemetry = telemetry;
            firstDemod = state->requestedDemod;
        }
        else
        {
            REQUIRE(firstTelemetry == telemetry);
            REQUIRE(DecodeParameters(firstDemod) == DecodeParameters(state->requestedDemod));
        }
        REQUIRE(state->requestedDemod.visualProfileId == pbprotocol::kUnifiedVisualProfileId);
        REQUIRE(state->requestedDemod.maximumFrameAgeMilliseconds == 250);
        REQUIRE(state->requestedDemod.maximumResidentBytes == 256ULL * 1024 * 1024);
        REQUIRE(state->requestedDemod.maximumRoiWidth == 1920);
        REQUIRE(state->requestedDemod.maximumRoiHeight == 1080);
        std::ofstream evidence(std::filesystem::path(PB_TEST_SCRATCH_ROOT) / (provider == 0 ? L"g17-unified-published.json" : L"g17-provider-variant.json"));
        evidence << pbapp::BuildDecoderRunReportJson(context, snapshot);
        REQUIRE(evidence.good());
    }
}

TEST_CASE("G17 fallback duplicates and resumed completion retain honest frame coverage", "[application][report][g17]")
{
    G17Scratch scratch;
    const std::vector<std::byte> bytes(8ULL * 1024 * 1024 + 1, std::byte{0x31});
    const auto frames = g16test::MakeFrames(scratch.Directory(L"tx"), bytes, 2);
    const auto config = pbapp::MakeUnifiedDecoderConfig(scratch.Directory(L"out").wstring(), g16test::Region());
    bool publishedInSameRun = false;
    {
        const auto state = std::make_shared<g16test::ReceiveState>();
        state->Push(frames[0]);
        pbapp::DecoderRuntime runtime(g16test::Services(state));
        REQUIRE(runtime.Start(config));
        REQUIRE(g16test::WaitFor([&]()
        {
            return runtime.GetSnapshot().verifiedSegmentCount == 1 || Terminal(runtime);
        }));
        REQUIRE(runtime.GetSnapshot().verifiedSegmentCount == 1);
        {
            const std::scoped_lock lock(state->mutex);
            state->capture.state = pbcapturenormalize::CaptureState::Failed;
            state->capture.error = pbcapturenormalize::CaptureStatus::Failure(
                pbcapturenormalize::CaptureError::AccessLost, pbcapturenormalize::CaptureStage::Callback);
        }
        REQUIRE(g16test::WaitFor([&]()
        {
            return runtime.GetSnapshot().actualBackend == pbapp::CaptureBackend::Dxgi || Terminal(runtime);
        }));
        REQUIRE(runtime.GetSnapshot().actualBackend == pbapp::CaptureBackend::Dxgi);
        state->Push(frames[0]);
        REQUIRE(g16test::WaitFor([&]()
        {
            return runtime.GetSnapshot().unifiedTelemetry.duplicateObservations == 1 || Terminal(runtime);
        }));
        SECTION("same-run completion counts Bootstrap-only erasure without synthetic lane samples")
        {
            auto erased = frames[1];
            erased.kind = pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly;
            erased.demodulation = {};
            state->Push(erased);
            state->Push(frames[1]);
            const bool completed = g16test::WaitFor([&]()
            {
                return Terminal(runtime);
            });
            const auto observed = runtime.GetSnapshot();
            INFO("state=" << pbapp::GetDecoderStateName(observed.state) << ", verifiedSegments=" << observed.verifiedSegmentCount
                << ", observations=" << observed.unifiedTelemetry.observations << ", uniqueFrames=" << observed.unifiedTelemetry.uniqueFrames
                << ", duplicates=" << observed.unifiedTelemetry.duplicateObservations << ", error=" << observed.errorDetail);
            if (!completed)
            {
                std::ofstream evidence(std::filesystem::path(PB_TEST_SCRATCH_ROOT) / L"g17-bootstrap-only-stall.json");
                evidence << pbapp::BuildDecoderRunReportJson(context, observed);
                REQUIRE(evidence.good());
            }
            REQUIRE(completed);
            runtime.Stop();
            const auto snapshot = runtime.GetSnapshot();
            INFO(snapshot.errorDetail);
            REQUIRE(g16test::VerifyOutput(snapshot, bytes));
            REQUIRE(snapshot.unifiedTelemetry.observations == 4);
            REQUIRE(snapshot.unifiedTelemetry.uniqueFrames == 2);
            REQUIRE(snapshot.unifiedTelemetry.duplicateObservations == 2);
            REQUIRE(snapshot.unifiedTelemetry.frameErasedObservations == 1);
            REQUIRE(snapshot.unifiedTelemetry.lanes[0].metricObservations == 3);
            REQUIRE(snapshot.unifiedTelemetry.uniqueVisualFps.has_value());
            REQUIRE(snapshot.uniqueVisualFps == snapshot.unifiedTelemetry.uniqueVisualFps);
            REQUIRE(snapshot.verifiedEncodedSegmentBytes.has_value());
            const auto report = pbapp::BuildDecoderRunReportJson(context, snapshot);
            REQUIRE(Json(report)["verifiedEncodedBytesPerUniqueFrame"].toDouble(-1) ==
                static_cast<double>(*snapshot.verifiedEncodedSegmentBytes) / 2.0);
            std::ofstream evidence(std::filesystem::path(PB_TEST_SCRATCH_ROOT) / L"g17-fallback-published.json");
            evidence << report;
            REQUIRE(evidence.good());
            publishedInSameRun = true;
        }
        SECTION("stop and resume has no lifetime frame coverage")
        {
            runtime.Stop();
            REQUIRE(runtime.GetSnapshot().unifiedTelemetry.uniqueFrames == 1);
            REQUIRE(runtime.GetSnapshot().unifiedTelemetry.duplicateObservations == 1);
            REQUIRE(Json(pbapp::BuildDecoderRunReportJson(context, runtime.GetSnapshot()))["verifiedEncodedBytesPerUniqueFrame"].isNull());
        }
    }
    if (!publishedInSameRun)
    {
        const auto state = std::make_shared<g16test::ReceiveState>();
        state->Push(frames[1]);
        pbapp::DecoderRuntime runtime(g16test::Services(state));
        REQUIRE(runtime.Start(config));
        REQUIRE(g16test::WaitFor([&]()
        {
            return Terminal(runtime);
        }));
        runtime.Stop();
        const auto snapshot = runtime.GetSnapshot();
        INFO(snapshot.errorDetail);
        REQUIRE(g16test::VerifyOutput(snapshot, bytes));
        REQUIRE(snapshot.resumeVerificationMilliseconds.has_value());
        REQUIRE(snapshot.resumeVerificationSucceeded == true);
        const auto report = Json(pbapp::BuildDecoderRunReportJson(context, snapshot));
        REQUIRE(report["verifiedEncodedBytesPerUniqueFrame"].isNull());
        REQUIRE(report["publishedFrameMetric"].toObject()["unavailableReason"] == "ResumeHasNoLifetimeFrameCoverage");
        REQUIRE(report["publish"].toObject()["finalReopenVerified"] == true);
        std::ofstream evidence(std::filesystem::path(PB_TEST_SCRATCH_ROOT) / L"g17-resumed-published.json");
        evidence << pbapp::BuildDecoderRunReportJson(context, snapshot);
        REQUIRE(evidence.good());
    }
}

TEST_CASE("G17 invalid telemetry withdraws performance without changing actual payload admission", "[application][report][g17]")
{
    G17Scratch scratch;
    const auto bytes = g16test::RawBytes(128);
    auto frames = g16test::MakeFrames(scratch.Directory(L"tx"), bytes);
    frames[0].demodulation.unifiedObservation.laneMetrics[0].samples++;
    const auto state = std::make_shared<g16test::ReceiveState>();
    state->Push(frames[0]);
    pbapp::DecoderRuntime runtime(g16test::Services(state));
    REQUIRE(runtime.Start(pbapp::MakeUnifiedDecoderConfig(scratch.Directory(L"out").wstring(), g16test::Region())));
    REQUIRE(g16test::WaitFor([&]()
    {
        return Terminal(runtime);
    }));
    runtime.Stop();
    const auto snapshot = runtime.GetSnapshot();
    INFO(snapshot.errorDetail);
    REQUIRE(g16test::VerifyOutput(snapshot, bytes));
    REQUIRE(snapshot.finalPublishSucceeded);
    REQUIRE(snapshot.wholeFileDigestCheck == true);
    REQUIRE(snapshot.finalReopenVerified == true);
    REQUIRE_FALSE(snapshot.unifiedTelemetry.frameCoverageComplete);
    const auto report = Json(pbapp::BuildDecoderRunReportJson(context, snapshot));
    REQUIRE(report["verifiedEncodedBytesPerUniqueFrame"].isNull());
    REQUIRE(report["publishedFrameMetric"].toObject()["unavailableReason"] == "IncompleteObservedFrameCoverage");
    auto missingBytes = snapshot;
    missingBytes.verifiedEncodedSegmentBytes.reset();
    REQUIRE(Json(pbapp::BuildDecoderRunReportJson(context, missingBytes))["publishedFrameMetric"].toObject()["unavailableReason"] ==
        "EncodedByteCoverageUnavailable");
}

TEST_CASE("G17 first usable newer frame promotion retains old-frame rejection and same-frame conflicts", "[application][report][g17]")
{
    G17Scratch scratch;
    const auto frames = g16test::MakeFrames(scratch.Directory(L"tx"), g16test::RawBytes(100000), 2);
    const auto state = std::make_shared<g16test::ReceiveState>();
    state->Push(frames[0]);
    pbapp::DecoderRuntime runtime(g16test::Services(state));
    REQUIRE(runtime.Start(pbapp::MakeUnifiedDecoderConfig(scratch.Directory(L"out").wstring(), g16test::Region())));
    REQUIRE(g16test::WaitFor([&]()
    {
        return runtime.GetSnapshot().outerUniqueSymbols != 0 || Terminal(runtime);
    }));
    const auto firstSymbols = runtime.GetSnapshot().outerUniqueSymbols;
    REQUIRE(firstSymbols > 0);
    REQUIRE_FALSE(Terminal(runtime));
    auto bootstrapOnly = frames[1];
    bootstrapOnly.kind = pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly;
    bootstrapOnly.demodulation = {};
    state->Push(bootstrapOnly);
    state->Push(frames[1]);
    REQUIRE(g16test::WaitFor([&]()
    {
        return runtime.GetSnapshot().outerUniqueSymbols > firstSymbols || Terminal(runtime);
    }));
    const auto promotedSymbols = runtime.GetSnapshot().outerUniqueSymbols;
    REQUIRE(promotedSymbols > firstSymbols);
    REQUIRE_FALSE(Terminal(runtime));
    auto oldFrame = frames[0];
    ChangeValidTransportPayload(oldFrame);
    state->Push(oldFrame);
    REQUIRE(g16test::WaitFor([&]()
    {
        return runtime.GetSnapshot().reorderedFrameSequences == 1 || Terminal(runtime);
    }));
    REQUIRE(runtime.GetSnapshot().reorderedFrameSequences == 1);
    REQUIRE(runtime.GetSnapshot().outerUniqueSymbols == promotedSymbols);
    REQUIRE_FALSE(Terminal(runtime));
    auto conflicting = frames[1];
    ChangeValidTransportPayload(conflicting);
    state->Push(conflicting);
    REQUIRE(g16test::WaitFor([&]()
    {
        return Terminal(runtime);
    }));
    runtime.Stop();
    const auto snapshot = runtime.GetSnapshot();
    INFO(snapshot.errorDetail);
    REQUIRE(snapshot.state == pbapp::DecoderState::Failed);
    REQUIRE(snapshot.errorDetail.find("conflicting accepted bytes in the same Unified frame slot") != std::string::npos);
    REQUIRE_FALSE(snapshot.finalPublishSucceeded);
    REQUIRE(snapshot.verifiedRawBytes == 0);
    REQUIRE(Json(pbapp::BuildDecoderRunReportJson(context, snapshot))["verifiedEncodedBytesPerUniqueFrame"].isNull());
}

TEST_CASE("G17 Storage phase observations distinguish digest rejection rename conflict and final reopen", "[application][report][g17]")
{
    G17Scratch scratch;
    const auto bytes = g16test::RawBytes(128);
    pbstorage::OutputFileConfig config{scratch.Directory(L"storage").wstring(), pbprotocol::SessionTag{17}, bytes.size(), 4096, "phase.bin"};
    std::unique_ptr<pbstorage::OutputFile> storage;
    REQUIRE(pbstorage::OutputFile::Create(config, storage));
    REQUIRE_FALSE(storage->GetSnapshot().wholeFileDigestVerified.has_value());
    REQUIRE(storage->WriteVerifiedSegment(0, bytes));
    REQUIRE(storage->FlushVerifiedSegment());
    const pbprotocol::WholeFileDigest digest{pbprotocol::ComputeBlake3Digest(bytes)};
    SECTION("wrong digest leaves rename and reopen unobserved")
    {
        auto wrong = digest;
        wrong.bytes[0] ^= std::byte{1};
        REQUIRE_FALSE(storage->Publish(wrong));
        REQUIRE(storage->GetSnapshot().wholeFileDigestVerified == false);
        REQUIRE_FALSE(storage->GetSnapshot().finalRenameSucceeded.has_value());
        REQUIRE_FALSE(storage->GetSnapshot().finalReopenVerified.has_value());
    }
    SECTION("final name conflict does not fake final reopen")
    {
        std::ofstream existing(std::filesystem::path(storage->GetSnapshot().finalPath));
        existing << "do not overwrite";
        existing.close();
        REQUIRE_FALSE(storage->Publish(digest));
        REQUIRE(storage->GetSnapshot().wholeFileDigestVerified == true);
        REQUIRE(storage->GetSnapshot().finalRenameSucceeded == false);
        REQUIRE_FALSE(storage->GetSnapshot().finalReopenVerified.has_value());
    }
    SECTION("normal publish and post-rename recovery have different rename evidence")
    {
        const auto reservationName = std::filesystem::path(storage->GetSnapshot().finalPath).filename().u8string();
        const pbstorage::OutputFileReservation reservation{std::string(reservationName.begin(), reservationName.end())};
        REQUIRE(storage->Publish(digest));
        REQUIRE(storage->GetSnapshot().wholeFileDigestVerified == true);
        REQUIRE(storage->GetSnapshot().finalRenameSucceeded == true);
        REQUIRE(storage->GetSnapshot().finalReopenVerified == true);
        storage.reset();
        REQUIRE(pbstorage::OutputFile::CreateOrResume(config, reservation, digest, storage));
        REQUIRE(storage->GetSnapshot().recoveredPublished);
        REQUIRE(storage->GetSnapshot().wholeFileDigestVerified == true);
        REQUIRE_FALSE(storage->GetSnapshot().finalRenameSucceeded.has_value());
        REQUIRE(storage->GetSnapshot().finalReopenVerified == true);
    }
}
