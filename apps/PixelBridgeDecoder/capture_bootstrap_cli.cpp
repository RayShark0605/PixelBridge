#include "capture_bootstrap_arguments.h"
#include "capture_bootstrap_telemetry.h"
#include "../common/diagnostic_file.h"
#include "pbscreencapturedxgi/dxgi_capture.h"
#include "pbscreencapturewgc/wgc_capture.h"
#include "pbprotocol/checked_integer.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace
{
using namespace pbcapturenormalize;

void Usage()
{
    std::cout << "Usage: PixelBridgeDecoder --capture-bootstrap --backend wgc|dxgi [--roi LEFT TOP RIGHT BOTTOM]\n"
                 "                          [--seconds N] [--telemetry NEW_FILE.jsonl]\n"
                 "       PixelBridgeDecoder --capture-desktop-levels --backend wgc|dxgi [same ROI/duration/telemetry options]\n"
                 "       PixelBridgeDecoder --capture-shape-chroma --backend wgc|dxgi [same ROI/duration/telemetry options]\n"
                 "ROI is signed physical desktop pixels, fully inside one monitor. Omit it to select interactively.\n"
                 "N is a finite capture duration (1..600 seconds, default 10); initialization/shutdown are separate bounded phases.\n"
                 "Experimental LocalDesktop Bootstrap only; identities are recovered only from captured pixels.\n"
                 "Explicit DiagnosticCpuReadback; no file receiver, HDR tone mapping or certified throughput claim.\n"
                 "Physical-layer modes require strict 1:1 SDR; no scaling, fallback or cross-frame soft combining.\n"
                 "Exit: 0 = verified data (Bootstrap-only in old mode), 4 = none verified, 3 = selector cancelled, 2 = arguments, 1 = error.\n";
}

void Require(const CaptureStatus& status, const char* operation)
{
    if (!status)
    {
        throw std::runtime_error(std::string(operation) + ": " + GetCaptureErrorName(status.code) + "; stage=" +
                                 std::to_string(static_cast<unsigned int>(status.stage)) + "; native=" + std::to_string(status.nativeError));
    }
}

// The only variant is the native factory. Both variants use the same strict
// contract, readback, CPU processor and lifecycle code; no automatic fallback.
class CaptureSession
{
public:
    CaptureSession(const pbdecoder::BootstrapBackend backend, const CaptureNormalizeConfig& config, const std::shared_ptr<ScreenCaptureConsumer>& consumer)
    {
        if (backend == pbdecoder::BootstrapBackend::Wgc)
        {
            Require(pbscreencapturewgc::WgcCapture::CreateNormalized(config, consumer, wgc_), "WGC CreateNormalized");
        }
        else
        {
            Require(pbscreencapturedxgi::DxgiCapture::Create(config, consumer, dxgi_), "DXGI Create");
        }
    }
    [[nodiscard]] CaptureSnapshot GetSnapshot() const noexcept
    {
        return wgc_ ? wgc_->GetSnapshot() : dxgi_->GetSnapshot();
    }
    [[nodiscard]] CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept
    {
        return wgc_ ? wgc_->GetNormalizationSnapshot() : dxgi_->GetNormalizationSnapshot();
    }
    [[nodiscard]] CaptureStatus Stop() noexcept
    {
        return wgc_ ? wgc_->Stop() : dxgi_->Stop();
    }
private:
    std::unique_ptr<pbscreencapturewgc::WgcCapture> wgc_;
    std::unique_ptr<pbscreencapturedxgi::DxgiCapture> dxgi_;
};

struct ReadbackStopGuard
{
    std::shared_ptr<DiagnosticCpuReadback> readback;
    ~ReadbackStopGuard()
    {
        if (readback)
        {
            // Runs on the application thread after CaptureSession is destroyed.
            // In exception paths this still never joins on a GPU owner/deferred thread.
            static_cast<void>(readback->Stop(3000));
        }
    }
};
} // namespace

int RunCaptureBootstrapCommand(const int argumentCount, wchar_t* arguments[])
{
    pbdecoder::CaptureBootstrapArguments options;
    if (!pbdecoder::ParseCaptureBootstrapArguments(argumentCount, arguments, options))
    {
        Usage();
        return 2;
    }
    if (options.showHelp)
    {
        Usage();
        return 0;
    }
    try
    {
        const bool physicalLayer = options.desktopLevels || options.shapeChroma;
        pbdiagnostic::DiagnosticFile telemetry(options.telemetryPath);
        pbscreenregion::ScreenCaptureRegion region;
        const RECT physicalRoi{options.physicalRoi[0], options.physicalRoi[1], options.physicalRoi[2], options.physicalRoi[3]};
        const auto regionStatus = options.hasRoi ? pbscreenregion::ResolveScreenCaptureRegion(physicalRoi, region) : pbscreenregion::SelectScreenCaptureRegion(region);
        if (!regionStatus)
        {
            std::cerr << "Screen region: " << pbscreenregion::GetScreenRegionErrorName(regionStatus.code) << "; stage=" << static_cast<unsigned int>(regionStatus.stage)
                      << "; native=" << regionStatus.nativeError << '\n';
            telemetry.Finish();
            return regionStatus.code == pbscreenregion::ScreenRegionErrorCode::Cancelled ? 3 : 1;
        }
        const auto roiWidth = static_cast<std::int64_t>(region.physicalRect.right) - region.physicalRect.left;
        const auto roiHeight = static_cast<std::int64_t>(region.physicalRect.bottom) - region.physicalRect.top;
        if (roiWidth <= 0 || roiHeight <= 0 || roiWidth > 16384 || roiHeight > 16384)
        {
            throw std::runtime_error("ROI exceeds the bounded diagnostic dimensions");
        }
        CaptureNormalizeConfig config;
        config.capture.region = region;
        // LocalDesktop senders emit exact SDR BGRA8 code values. Keep the owned
        // ROI in that production format for every diagnostic mode: native R10
        // or FP16 SDR sources use the existing bounded GPU down-conversion,
        // while HDR remains unsupported and fails before any tone-map/fallback.
        config.capture.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        // A diagnostic reference decoder has a finite, explicit CPU-age budget.
        // Capture/crop/rotation remain GPU-only; this reservation includes every
        // ring/scratch/staging/CPU buffer rather than counting only one texture.
        config.capture.maximumFrameAgeMilliseconds = 1000;
        config.capture.maximumRoiBytes = 512ull * 1024 * 1024;
        config.capture.maximumCaptureBytes = 1024ull * 1024 * 1024;
        DiagnosticReadbackConfig readbackConfig;
        readbackConfig.maximumRoiSize = {static_cast<std::int32_t>(roiWidth), static_cast<std::int32_t>(roiHeight)};
        readbackConfig.stagingTextureCount = config.capture.roiTextureCount;
        readbackConfig.maximumFrameAgeMilliseconds = config.capture.maximumFrameAgeMilliseconds;
        readbackConfig.maximumReadbackBytes = 512ull * 1024 * 1024;
        std::shared_ptr<pbdecoder::BootstrapDiagnosticProcessor> processor;
        if (physicalLayer)
        {
            readbackConfig.processingReservedBytes = pbdesktoplevels::kProcessingReservationBytes;
            DiagnosticReadbackBudget budget;
            Require(CalculateDiagnosticReadbackBudget(readbackConfig, budget), "physical-layer processing reservation");
            if (options.shapeChroma)
            {
                Require(pbdecoder::BootstrapDiagnosticProcessor::CreateShapeChroma(processor), "ShapeChroma processor Create");
            }
            else
            {
                Require(pbdecoder::BootstrapDiagnosticProcessor::CreateDesktopLevels(processor), "DesktopLevels processor Create");
            }
        }
        else
        {
            processor = std::make_shared<pbdecoder::BootstrapDiagnosticProcessor>();
        }
        std::shared_ptr<DiagnosticCpuReadback> readback;
        Require(DiagnosticCpuReadback::Create(readbackConfig, processor, readback), "Diagnostic readback Create");
        const ReadbackStopGuard readbackStop{readback};
        CaptureSession capture(options.backend, config, readback);
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds(options.seconds);
        auto nextSnapshot = start;
        std::uint64_t staleDiagnosticEvents = 0;
        bool runtimeFailed = false;
        const auto emitLine = [&telemetry](const std::string& line)
        {
            telemetry.Write(line);
            std::cout << line;
            if (!std::cout)
            {
                throw std::runtime_error("diagnostic stdout write failed");
            }
        };
        for (;;)
        {
            const auto now = std::chrono::steady_clock::now();
            const auto normalized = capture.GetNormalizationSnapshot();
            pbdecoder::BootstrapDiagnosticEvent event;
            // Fixed queue capacity bounds each drain; callbacks never perform I/O.
            for (std::size_t index = 0; index < pbdecoder::BootstrapDiagnosticProcessor::eventCapacity && processor->TakeEvent(event); index++)
            {
                if (!physicalLayer && (!normalized.active || normalized.domain != event.capture.domain))
                {
                    pbprotocol::SaturatingIncrementUnsigned(staleDiagnosticEvents);
                    continue;
                }
                emitLine(pbdecoder::SerializeBootstrapDiagnosticEvent(event));
            }
            const auto snapshot = capture.GetSnapshot();
            const auto readbackSnapshot = readback->GetSnapshot();
            const bool failed = pbdecoder::IsTerminalDiagnosticFailure(snapshot, readbackSnapshot);
            if (now >= nextSnapshot || now >= deadline || failed)
            {
                emitLine(pbdecoder::SerializeCaptureBootstrapSnapshot("capture-snapshot", snapshot, normalized, readbackSnapshot, processor->GetSnapshot(),
                    staleDiagnosticEvents, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count())));
                nextSnapshot = now + std::chrono::seconds(1);
            }
            if (failed || now >= deadline)
            {
                runtimeFailed = failed;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Invalidates the old domain before source/GPU retirement. Late CPU
        // candidates are discarded, never relabelled with a current epoch.
        const auto captureStop = capture.Stop();
        const auto workerStop = readback->Stop(3000);
        const auto stopped = capture.GetSnapshot();
        if (physicalLayer)
        {
            pbdecoder::BootstrapDiagnosticEvent event;
            for (std::size_t index = 0; index < pbdecoder::BootstrapDiagnosticProcessor::eventCapacity && processor->TakeEvent(event); index++)
            {
                emitLine(pbdecoder::SerializeBootstrapDiagnosticEvent(event));
            }
        }
        const auto finalVisual = processor->GetSnapshot();
        emitLine(pbdecoder::SerializeCaptureBootstrapSnapshot("capture-final", stopped, capture.GetNormalizationSnapshot(), readback->GetSnapshot(), finalVisual,
            staleDiagnosticEvents, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count())));
        telemetry.Finish();
        pbdiagnostic::FlushDiagnosticOutput(std::cout);
        Require(captureStop, "Capture Stop");
        Require(workerStop, "Readback Stop");
        if (runtimeFailed || stopped.state == CaptureState::Failed || stopped.deferredCleanup)
        {
            return 1;
        }
        return pbdecoder::GetBootstrapDiagnosticSuccessExitCode(finalVisual);
    }
    catch (const std::exception& error)
    {
        std::cerr << "PixelBridgeDecoder capture error: " << error.what() << '\n';
        return 1;
    }
}
