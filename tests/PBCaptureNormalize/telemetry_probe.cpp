#include "../../apps/PixelBridgeDecoder/capture_bootstrap_telemetry.h"

#include <iostream>
#include <limits>

int main()
{
    pbdecoder::BootstrapDiagnosticEvent event;
    event.capture.domain.captureEpoch = std::numeric_limits<std::uint64_t>::max();
    event.capture.captureObservation = std::numeric_limits<std::uint64_t>::max();
    event.capture.backend = pbcapturenormalize::CaptureBackendKind::Dxgi;
    event.capture.timestamp.domain = pbcapturenormalize::CaptureTimestampDomain::DxgiQpcTicks;
    event.capture.timestamp.rawValue = std::numeric_limits<std::int64_t>::max();
    event.capture.timestamp.rawFrequency = 10000000;
    event.capture.physicalRoi = {-1920, -1080, 0, 0};
    event.disposition = pbdecoder::BootstrapDisposition::Accepted;
    event.visual.erasure = pbmodulation::LocalDesktopErasureReason::None;
    event.visual.canonical44[40] = std::byte{0x01};
    event.visual.canonical44[41] = std::byte{0x23};
    event.visual.canonical44[42] = std::byte{0x45};
    event.visual.canonical44[43] = std::byte{0x67};
    event.visual.geometry.scaleX = 0.5;
    event.visual.geometry.scaleY = 2;
    event.visual.copies[0].sampleMidGrayFraction = 0.125;
    event.visual.copies[1].sampleMidGrayFraction = 0.25;
    event.visual.sampleMidGrayFraction = 0.25;
    event.bootstrap.sessionTag.value = std::numeric_limits<std::uint64_t>::max();
    event.bootstrap.frameSequence = std::numeric_limits<std::uint64_t>::max();
    event.bootstrap.visualProfileId = 0x50424C4442533031ULL;
    std::cout << pbdecoder::SerializeBootstrapDiagnosticEvent(event);
    event.disposition = pbdecoder::BootstrapDisposition::VisualErasure;
    event.visual.erasure = pbmodulation::LocalDesktopErasureReason::NonFinitePixel;
    event.visual.geometry.originX = std::numeric_limits<double>::quiet_NaN();
    event.visual.geometry.originY = std::numeric_limits<double>::infinity();
    event.visual.copies[0].sampleMidGrayFraction = std::numeric_limits<double>::quiet_NaN();
    event.visual.copies[1].sampleMidGrayFraction = std::numeric_limits<double>::infinity();
    event.visual.sampleMidGrayFraction = std::numeric_limits<double>::quiet_NaN();
    std::cout << pbdecoder::SerializeBootstrapDiagnosticEvent(event);
    pbcapturenormalize::CaptureSnapshot capture;
    capture.environment.backendKind = pbcapturenormalize::CaptureBackendKind::Dxgi;
    capture.state = pbcapturenormalize::CaptureState::WaitingForEnvironment;
    capture.lastRebuildReason = pbcapturenormalize::CaptureRebuildReason::DesktopChanged;
    pbcapturenormalize::CaptureNormalizeSnapshot normalized;
    normalized.domain = event.capture.domain;
    normalized.lastErasure = pbcapturenormalize::CaptureErasureReason::CursorPossiblyComposited;
    std::cout << pbdecoder::SerializeCaptureBootstrapSnapshot("capture-final", capture, normalized, {}, {}, 7, 1000);

    normalized.active = true;
    normalized.domain.sourceId[0] = std::byte{0x22};
    normalized.domain.sourceId[15] = std::byte{0xAA};
    normalized.domain.captureEpoch = 2;
    pbcapturenormalize::DiagnosticReadbackSnapshot readback;
    readback.domain = normalized.domain;
    readback.active = true;
    readback.resetPending = true;
    pbdecoder::BootstrapDiagnosticSnapshot visual;
    visual.domain = normalized.domain;
    visual.domain->captureEpoch = 1;
    visual.geometryGeneration = 7;
    visual.calibrationGeneration = 9;
    visual.trackedSessions = 1;
    visual.retainedSequences = 1;
    capture.state = pbcapturenormalize::CaptureState::Running;
    std::cout << pbdecoder::SerializeCaptureBootstrapSnapshot("capture-snapshot", capture, normalized, readback, visual, 0, 2000);

    readback.resetPending = false;
    visual.domain = normalized.domain;
    visual.geometryGeneration = 0;
    visual.calibrationGeneration = 0;
    visual.trackedSessions = 0;
    visual.retainedSequences = 0;
    std::cout << pbdecoder::SerializeCaptureBootstrapSnapshot("capture-snapshot", capture, normalized, readback, visual, 0, 2001);
    // Equal numeric epochs must not conceal a different full source identity.
    readback.domain.sourceId[15] ^= std::byte{1};
    std::cout << pbdecoder::SerializeCaptureBootstrapSnapshot("capture-snapshot", capture, normalized, readback, visual, 0, 2002);
    readback.domain = normalized.domain;
    visual.domain->sourceId[15] ^= std::byte{1};
    std::cout << pbdecoder::SerializeCaptureBootstrapSnapshot("capture-snapshot", capture, normalized, readback, visual, 0, 2003);
    visual.domain = normalized.domain;
    readback.resetPending = true;
    std::cout << pbdecoder::SerializeCaptureBootstrapSnapshot("capture-snapshot", capture, normalized, readback, visual, 0, 2004);
    readback.resetPending = false;
    readback.active = false;
    std::cout << pbdecoder::SerializeCaptureBootstrapSnapshot("capture-snapshot", capture, normalized, readback, visual, 0, 2005);
    readback.active = true;
    normalized.active = false;
    std::cout << pbdecoder::SerializeCaptureBootstrapSnapshot("capture-snapshot", capture, normalized, readback, visual, 0, 2006);
    visual.desktopLevels = true;
    readback.reservation.processingBytes = 16 * 1024 * 1024;
    std::cout << pbdecoder::SerializeCaptureBootstrapSnapshot("capture-snapshot", capture, normalized, readback, visual, 0, 3000);
    pbdesktoplevels::ReferenceStatistics statistics;
    pbdesktoplevels::FrameEvaluation evaluation;
    evaluation.evaluated = evaluation.paddingValid = true;
    evaluation.codewords = 10;
    evaluation.comparedCodedBits = 162000;
    std::array<std::uint64_t, 4096> histogram{};
    histogram.back() = 86688;
    if (!statistics.Add(evaluation, 0, histogram, 1))
    {
        return 1;
    }
    evaluation.erroneousCodedBits = 40500;
    evaluation.fecFailures = 1;
    if (!statistics.Add(evaluation, 15, histogram, 1))
    {
        return 1;
    }
    visual.candidates[1].statistics = statistics.GetSummary();
    visual.candidates[1].geometryErasures = 3;
    visual.candidates[1].pilotErasures = 4;
    visual.candidates[1].duplicates = 5;
    visual.unrecognizedBootstrap = 6;
    std::cout << pbdecoder::SerializeCaptureBootstrapSnapshot("capture-final", capture, normalized, readback, visual, 0, 3001);
    event.desktopLevels = true;
    event.levels.modulation.profileId = pbmodulation::kDesktopLevels4ProfileId;
    event.levels.modulation.calibration.phaseResidual = std::numeric_limits<double>::quiet_NaN();
    event.levels.evaluation = evaluation;
    std::cout << pbdecoder::SerializeBootstrapDiagnosticEvent(event);
    return std::cout ? 0 : 1;
}
