#include "bootstrap_diagnostic_test_support.h"
#include "../../apps/PixelBridgeDecoder/capture_bootstrap_telemetry.h"
#include "../PBModulation/desktop_levels_test_support.h"
#include <limits>

using namespace bootstrapdiagnostictest;

namespace
{
Raster LevelsRaster(const unsigned tile, const unsigned sequence = 0, const bool badPadding = false)
{
    const auto record = desktoptest::Record(tile, sequence);
    const std::size_t count = tile == 2 ? 86688 : 21672;
    std::vector<std::byte> data(count);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, data));
    Raster result{{1920, 1080}, std::vector<std::byte>(1920 * 1080 * 4)};
    REQUIRE(pbmodulation::EncodeDesktopLevelsFrame(record, data, result.pixels));
    if (badPadding)
    {
        // Independently map the last padding tile; one wrong bit with a valid
        // Bootstrap is a post-FEC failure and must still consume the identity.
        const auto positions = desktoptest::Coordinates(tile);
        const auto physical = ((positions.size() - 1) * 65537 + (sequence % 16) * (1728 / tile)) % positions.size();
        const auto position = positions[physical];
        desktoptest::Paint(result.pixels, position[0], position[1], tile, tile, 96);
    }
    return result;
}
} // namespace

TEST_CASE("DesktopLevels processor commits metrics once including FEC failure and rejects same-session candidate switch", "[desktop-levels][processor]")
{
    std::shared_ptr<BootstrapDiagnosticProcessor> processor;
    REQUIRE(BootstrapDiagnosticProcessor::CreateDesktopLevels(processor));
    REQUIRE(processor->ProcessingReservedBytes() == 16 * 1024 * 1024);
    processor->Reset(Domain());
    const auto damaged = LevelsRaster(4, 0, true);
    auto metadata = Metadata(damaged.size);
    REQUIRE(processor->Analyze(metadata, damaged.pixels, damaged.RowPitch()));
    REQUIRE(processor->GetSnapshot().candidates[1].statistics.frames == 0);
    REQUIRE(GetBootstrapDiagnosticSuccessExitCode(processor->GetSnapshot()) == 4);
    processor->Commit(metadata);
    auto snapshot = processor->GetSnapshot();
    REQUIRE(snapshot.accepted == 0);
    REQUIRE(snapshot.candidates[1].statistics.frames == 1);
    REQUIRE(snapshot.candidates[1].statistics.postFecFailedFrames == 1);
    REQUIRE(snapshot.candidates[1].statistics.erroneousCodedBits == 0);
    REQUIRE(snapshot.lastDisposition == BootstrapDisposition::PostFecFailure);
    REQUIRE(snapshot.telemetry.fecEvaluatedFrames == 1);
    REQUIRE(snapshot.telemetry.postFecFailedFrames == 1);
    REQUIRE(snapshot.telemetry.fecFrameErrorRate == 1.0);
    REQUIRE(snapshot.telemetryFailures == 0);
    BootstrapDiagnosticEvent event;
    REQUIRE(processor->TakeEvent(event));
    REQUIRE(event.desktopLevels);
    REQUIRE(event.levels.evaluation.evaluated);
    REQUIRE_FALSE(event.levels.evaluation.IsVerified());
    const auto eventJson = SerializeBootstrapDiagnosticEvent(event);
    REQUIRE(eventJson.find("desktop-levels-observation") != std::string::npos);
    REQUIRE(eventJson.find("\"comparedCodedBits\":162000") != std::string::npos);

    const auto repairedPixels = LevelsRaster(4);
    event = Observe(*processor, repairedPixels, Metadata(repairedPixels.size, 2));
    REQUIRE(event.disposition == BootstrapDisposition::DuplicateObservation);
    REQUIRE(event.levels.evaluation.IsVerified()); // recomputation is diagnostic only, never re-admitted
    snapshot = processor->GetSnapshot();
    REQUIRE(snapshot.candidates[1].statistics.frames == 1);
    REQUIRE(snapshot.candidates[1].statistics.verifiedFrames == 0);
    REQUIRE(snapshot.candidates[1].duplicates == 1);
    REQUIRE(snapshot.telemetry.fecEvaluatedFrames == 1); // duplicate FrameSequence is not a second FER denominator
    REQUIRE(GetBootstrapDiagnosticSuccessExitCode(snapshot) == 4);

    const auto next = LevelsRaster(4, 1);
    event = Observe(*processor, next, Metadata(next.size, 3));
    REQUIRE(event.disposition == BootstrapDisposition::Accepted);
    snapshot = processor->GetSnapshot();
    REQUIRE(snapshot.candidates[1].statistics.frames == 2);
    REQUIRE(snapshot.candidates[1].statistics.verifiedFrames == 1);
    REQUIRE(snapshot.telemetry.fecEvaluatedFrames == 2);
    REQUIRE(snapshot.telemetry.postFecFailedFrames == 1);
    REQUIRE(snapshot.telemetry.fecFrameErrorRate == 0.5);
    REQUIRE(GetBootstrapDiagnosticSuccessExitCode(snapshot) == 0);
    const auto switched = LevelsRaster(2, 2);
    event = Observe(*processor, switched, Metadata(switched.size, 4));
    REQUIRE(event.disposition == BootstrapDisposition::IdentityConflict);
    REQUIRE(processor->GetSnapshot().candidates[0].statistics.frames == 0);
    REQUIRE(GetBootstrapDiagnosticSuccessExitCode(processor->GetSnapshot()) == 1);
}

TEST_CASE("DesktopLevels processor Discard stale Commit and Reset do not publish candidate statistics", "[desktop-levels][processor][epoch]")
{
    std::shared_ptr<BootstrapDiagnosticProcessor> processor;
    REQUIRE(BootstrapDiagnosticProcessor::CreateDesktopLevels(processor));
    const auto raster = LevelsRaster(2);
    const auto metadata = Metadata(raster.size);
    processor->Reset(metadata.domain);
    REQUIRE(processor->Analyze(metadata, raster.pixels, raster.RowPitch()));
    processor->Discard();
    processor->Commit(metadata);
    REQUIRE(processor->GetSnapshot().candidates[0].statistics.frames == 0);
    REQUIRE(processor->Analyze(metadata, raster.pixels, raster.RowPitch()));
    auto forged = metadata;
    forged.slotGeneration++;
    processor->Commit(forged);
    REQUIRE(processor->GetSnapshot().candidates[0].statistics.frames == 0);
    REQUIRE(processor->Analyze(metadata, raster.pixels, raster.RowPitch()));
    processor->Reset(Domain(2));
    processor->Commit(metadata);
    REQUIRE(processor->GetSnapshot().candidates[0].statistics.frames == 0);
    REQUIRE(processor->GetSnapshot().trackedSessions == 0);
    REQUIRE(processor->GetSnapshot().calibrationGeneration == 0);
    const auto event = Observe(*processor, raster, Metadata(raster.size, 1, Domain(2)));
    REQUIRE(event.disposition == BootstrapDisposition::Accepted);
    processor->Reset(std::nullopt);
    const auto snapshot = processor->GetSnapshot();
    REQUIRE_FALSE(snapshot.domain);
    REQUIRE_FALSE(snapshot.telemetry.active);
    REQUIRE(snapshot.retainedSequences == 0);
    REQUIRE(snapshot.candidates[0].statistics.frames == 1); // cumulative measurement, no live old-domain calibration
}

TEST_CASE("DesktopLevels resource reservation and actual signal encoding fail closed", "[desktop-levels][processor][budget]")
{
    std::shared_ptr<BootstrapDiagnosticProcessor> processor;
    REQUIRE(BootstrapDiagnosticProcessor::CreateDesktopLevels(processor));
    DiagnosticReadbackConfig config;
    config.maximumRoiSize = {1920, 1080};
    std::shared_ptr<DiagnosticCpuReadback> readback;
    REQUIRE(DiagnosticCpuReadback::Create(config, processor, readback).code == CaptureError::ResourceLimit);
    REQUIRE_FALSE(readback);
    config.processingReservedBytes = processor->ProcessingReservedBytes();
    DiagnosticReadbackBudget budget;
    REQUIRE(CalculateDiagnosticReadbackBudget(config, budget));
    REQUIRE(budget.totalBytes == 1920ULL * 1080 * 8 * 6 + 16 * 1024 * 1024);
    REQUIRE(budget.processingBytes == 16 * 1024 * 1024);
    const auto saved = budget;
    config.maximumReadbackBytes = budget.totalBytes - 1;
    REQUIRE(CalculateDiagnosticReadbackBudget(config, budget).code == CaptureError::ResourceLimit);
    REQUIRE(budget == saved);
    config.processingReservedBytes = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(CalculateDiagnosticReadbackBudget(config, budget).code == CaptureError::ResourceLimit);
    REQUIRE(budget == saved);

    const auto raster = LevelsRaster(4);
    processor->Reset(Domain());
    auto metadata = Metadata(raster.size);
    metadata.bitsPerColor = 10; // output bit depth alone is NOT HDR
    REQUIRE(Observe(*processor, raster, metadata).disposition == BootstrapDisposition::Accepted);
    metadata.captureObservation = metadata.slotGeneration = 2;
    metadata.hdr = true;
    REQUIRE(Observe(*processor, raster, metadata).disposition == BootstrapDisposition::UnsupportedSignal);
    metadata.captureObservation = metadata.slotGeneration = 3;
    metadata.hdr = false;
    metadata.signalEncoding = CaptureSignalEncoding::Unknown;
    REQUIRE(Observe(*processor, raster, metadata).disposition == BootstrapDisposition::UnsupportedSignal);
    REQUIRE(processor->GetSnapshot().candidates[1].statistics.frames == 1);
}
