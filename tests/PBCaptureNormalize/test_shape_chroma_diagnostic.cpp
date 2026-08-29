#include "bootstrap_diagnostic_test_support.h"
#include "../../apps/PixelBridgeDecoder/capture_bootstrap_telemetry.h"

#include <limits>

using namespace bootstrapdiagnostictest;

namespace
{
Raster ShapeChromaRaster(const std::uint64_t sequence = 0)
{
    const auto record = CanonicalShapeChromaRecord(sequence);
    std::vector<std::byte> data(pbmodulation::kShapeChromaDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, data));
    Raster result{{1920, 1080}, std::vector<std::byte>(pbmodulation::kLocalDesktopFrameBgraBytes)};
    REQUIRE(pbmodulation::EncodeShapeChromaFrame(record, data, result.pixels));
    return result;
}
} // namespace

TEST_CASE("ShapeChroma processor commits exact Transport/FEC metrics once and emits bounded telemetry", "[shape-chroma][processor]")
{
    std::shared_ptr<BootstrapDiagnosticProcessor> processor;
    REQUIRE(BootstrapDiagnosticProcessor::CreateShapeChroma(processor));
    REQUIRE(processor->ProcessingReservedBytes() == pbdesktoplevels::kProcessingReservationBytes);
    processor->Reset(Domain());

    const auto raster = ShapeChromaRaster();
    auto event = Observe(*processor, raster, Metadata(raster.size));
    REQUIRE(event.shapeChroma);
    REQUIRE_FALSE(event.desktopLevels);
    RequireAccepted(event, CanonicalShapeChromaRecord());
    REQUIRE(event.shape.modulation.IsAccepted());
    REQUIRE(event.shape.evaluation.IsVerified());
    REQUIRE(event.shape.evaluation.codewords == pbmodulation::kShapeChromaCodewords);
    REQUIRE(event.shape.evaluation.acceptedTransportBlocks == pbmodulation::kShapeChromaCodewords);
    REQUIRE(event.shape.evaluation.comparedCodedBits == pbmodulation::kShapeChromaCodewords * pbdesktoplevels::kCodewordBits);
    REQUIRE(event.shape.evaluation.falseAcceptedCodewords == 0);
    REQUIRE(event.shape.modulation.shapeMargin.samples == pbmodulation::kShapeChromaTileCount);
    REQUIRE(event.shape.modulation.chromaMargin.samples == pbmodulation::kShapeChromaTileCount);
    const auto eventJson = SerializeBootstrapDiagnosticEvent(event);
    REQUIRE(eventJson.find("\"event\":\"shape-chroma-observation\"") != std::string::npos);
    REQUIRE(eventJson.find("\"shapeChroma\":{") != std::string::npos);
    REQUIRE(eventJson.find("\"codewords\":32") != std::string::npos);

    auto snapshot = processor->GetSnapshot();
    REQUIRE(snapshot.shapeChroma);
    REQUIRE_FALSE(snapshot.desktopLevels);
    REQUIRE(snapshot.accepted == 1);
    REQUIRE(snapshot.shape.statistics.frames == 1);
    REQUIRE(snapshot.shape.statistics.verifiedFrames == 1);
    REQUIRE(snapshot.shape.statistics.falseAcceptedCodewords == 0);
    REQUIRE(snapshot.shape.statistics.margin.samples == pbmodulation::kShapeChromaTileCount);
    REQUIRE(snapshot.shape.chromaMargin.samples == pbmodulation::kShapeChromaTileCount);
    REQUIRE(snapshot.telemetry.fecEvaluatedFrames == 1);
    REQUIRE(snapshot.telemetry.postFecFailedFrames == 0);
    REQUIRE(snapshot.telemetry.fecFrameErrorRate == 0.0);
    REQUIRE(GetBootstrapDiagnosticSuccessExitCode(snapshot) == 0);
    const auto snapshotJson = SerializeCaptureBootstrapSnapshot("capture-final", {}, {}, {}, snapshot, 0, 1000);
    REQUIRE(snapshotJson.find("\"shapeChroma\":{") != std::string::npos);
    REQUIRE(snapshotJson.find("\"candidate\":\"shape-chroma\"") != std::string::npos);
    REQUIRE(snapshotJson.find("\"chromaMargin\":{") != std::string::npos);

    event = Observe(*processor, raster, Metadata(raster.size, 2));
    REQUIRE(event.disposition == BootstrapDisposition::DuplicatePixels);
    REQUIRE(event.shape.evaluation.IsVerified());
    snapshot = processor->GetSnapshot();
    REQUIRE(snapshot.shape.statistics.frames == 1);
    REQUIRE(snapshot.shape.duplicates == 1);
    REQUIRE(snapshot.telemetry.fecEvaluatedFrames == 1);

    const auto next = ShapeChromaRaster(1);
    event = Observe(*processor, next, Metadata(next.size, 3));
    REQUIRE(event.disposition == BootstrapDisposition::Accepted);
    snapshot = processor->GetSnapshot();
    REQUIRE(snapshot.accepted == 2);
    REQUIRE(snapshot.shape.statistics.frames == 2);
    REQUIRE(snapshot.shape.statistics.verifiedFrames == 2);
    REQUIRE(snapshot.shape.statistics.margin.samples == 2 * pbmodulation::kShapeChromaTileCount);
    REQUIRE(snapshot.shape.chromaMargin.samples == 2 * pbmodulation::kShapeChromaTileCount);
    REQUIRE(snapshot.telemetry.fecEvaluatedFrames == 2);
    REQUIRE(snapshot.telemetry.fecFrameErrorRate == 0.0);
}

TEST_CASE("ShapeChroma processor fails closed across mode, signal, resource and CaptureEpoch boundaries", "[shape-chroma][processor][epoch][budget]")
{
    std::shared_ptr<BootstrapDiagnosticProcessor> processor;
    REQUIRE(BootstrapDiagnosticProcessor::CreateShapeChroma(processor));
    DiagnosticReadbackConfig config;
    config.maximumRoiSize = {1920, 1080};
    std::shared_ptr<DiagnosticCpuReadback> readback;
    REQUIRE(DiagnosticCpuReadback::Create(config, processor, readback).code == CaptureError::ResourceLimit);
    REQUIRE_FALSE(readback);
    config.processingReservedBytes = processor->ProcessingReservedBytes();
    DiagnosticReadbackBudget budget;
    REQUIRE(CalculateDiagnosticReadbackBudget(config, budget));
    // The shared readback reservation deliberately charges the worst supported
    // eight-byte format, even though this ShapeChroma run requests BGRA8.
    REQUIRE(budget.totalBytes == 1920ULL * 1080 * 8 * 6 + pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(budget.processingBytes == pbdesktoplevels::kProcessingReservationBytes);
    const auto saved = budget;
    config.maximumReadbackBytes = budget.totalBytes - 1;
    REQUIRE(CalculateDiagnosticReadbackBudget(config, budget).code == CaptureError::ResourceLimit);
    REQUIRE(budget == saved);
    config.processingReservedBytes = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(CalculateDiagnosticReadbackBudget(config, budget).code == CaptureError::ResourceLimit);
    REQUIRE(budget == saved);

    const auto shape = ShapeChromaRaster();
    auto metadata = Metadata(shape.size);
    processor->Reset(metadata.domain);
    REQUIRE(processor->Analyze(metadata, shape.pixels, shape.RowPitch()));
    processor->Discard();
    processor->Commit(metadata);
    REQUIRE(processor->GetSnapshot().shape.statistics.frames == 0);
    REQUIRE(processor->Analyze(metadata, shape.pixels, shape.RowPitch()));
    processor->Reset(Domain(2));
    processor->Commit(metadata);
    REQUIRE(processor->GetSnapshot().shape.statistics.frames == 0);
    REQUIRE(processor->GetSnapshot().trackedSessions == 0);
    REQUIRE(processor->GetSnapshot().calibrationGeneration == 0);

    metadata = Metadata(shape.size, 1, Domain(2));
    metadata.hdr = true;
    REQUIRE(Observe(*processor, shape, metadata).disposition == BootstrapDisposition::UnsupportedSignal);
    metadata.captureObservation = metadata.slotGeneration = 2;
    metadata.hdr = false;
    metadata.signalEncoding = CaptureSignalEncoding::Unknown;
    REQUIRE(Observe(*processor, shape, metadata).disposition == BootstrapDisposition::UnsupportedSignal);
    REQUIRE(processor->GetSnapshot().shape.statistics.frames == 0);

    const auto direct = Render(CanonicalRecord(), false);
    metadata = Metadata(direct.size, 3, Domain(2));
    const auto modeMismatch = Observe(*processor, direct, metadata);
    REQUIRE(modeMismatch.disposition == BootstrapDisposition::VisualErasure);
    REQUIRE_FALSE(modeMismatch.shape.modulation.IsAccepted());
    REQUIRE(processor->GetSnapshot().shape.statistics.frames == 0);

    metadata = Metadata(shape.size, 4, Domain(2));
    const auto accepted = Observe(*processor, shape, metadata);
    REQUIRE(accepted.disposition == BootstrapDisposition::Accepted);
    REQUIRE(processor->GetSnapshot().shape.statistics.frames == 1);
    processor->Reset(std::nullopt);
    const auto finalSnapshot = processor->GetSnapshot();
    REQUIRE_FALSE(finalSnapshot.domain);
    REQUIRE_FALSE(finalSnapshot.telemetry.active);
    REQUIRE(finalSnapshot.retainedSequences == 0);
    REQUIRE(finalSnapshot.shape.statistics.frames == 1);
}
