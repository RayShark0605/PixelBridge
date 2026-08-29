#include "presentation_cli_arguments.h"
#include "../common/diagnostic_file.h"
#include "pbrenderd3d/data_window.h"
#include "pbmodulation/local_desktop_bootstrap.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/shape_chroma.h"
#include "pbdesktoplevels/reference_channel.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/session_random.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

void Usage()
{
    std::cout << "Usage: PixelBridgeEncoder --data-window [--visual local-desktop-bootstrap|desktop-levels-2x2|desktop-levels-4x4|shape-chroma] [--frames N] [--telemetry NEW_FILE.jsonl]\n"
              << "       PixelBridgeEncoder --visual local-desktop-bootstrap [--frames N] [--telemetry NEW_FILE.jsonl]\n"
              << "N bounds accepted CPU frame submissions (1..1000000), not displayed frames.\n"
              << "Without --frames, press Escape in the data window to stop.\n"
              << "Default --data-window remains PB-ReferenceRaster-1. The explicit visual selects experimental SDR Bootstrap only.\n"
              << "Physical-layer modes submit a new sequence every 500 ms by default (--sequence-interval-ms 1..60000 overrides), require 1:1 SDR and carry diagnostic Robust QC-LDPC data only.\n"
              << "Presentation diagnostics only; not a file sender or capture certification. Telemetry never overwrites an existing file.\n";
}

class DiagnosticFile
{
public:
    explicit DiagnosticFile(const wchar_t* path) : file_(path)
    {
    }
    void Write(const pbrenderd3d::DataWindowSnapshot& snapshot)
    {
        if (!file_.IsEnabled())
        {
            return;
        }
        std::ostringstream stream;
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        pbrenderd3d::WriteDataWindowSnapshotJson(stream, snapshot);
        stream << '\n';
        file_.Write(stream.str());
    }
    void Finish()
    {
        file_.Finish();
    }

private:
    pbdiagnostic::DiagnosticFile file_;
};

}

int RunDataWindowCommand(const int argumentCount, wchar_t* arguments[])
{
    pbencoder::DataWindowArguments options;
    if (!pbencoder::ParseDataWindowArguments(argumentCount, arguments, options))
    {
        Usage();
        return 2;
    }
    if (options.showHelp)
    {
        Usage();
        return 0;
    }
    const bool localDesktopBootstrap = options.visual == pbencoder::PresentationVisual::LocalDesktopBootstrap;
    const bool desktopLevels = options.visual == pbencoder::PresentationVisual::DesktopLevels2 || options.visual == pbencoder::PresentationVisual::DesktopLevels4;
    const bool shapeChroma = options.visual == pbencoder::PresentationVisual::ShapeChroma;
    const bool physicalLayer = desktopLevels || shapeChroma;
    const auto* const levelsProfile = desktopLevels ? pbmodulation::GetDesktopLevelsProfile(options.visual == pbencoder::PresentationVisual::DesktopLevels2 ?
        pbmodulation::kDesktopLevels2ProfileId : pbmodulation::kDesktopLevels4ProfileId) : nullptr;
    const bool hasFrameCount = options.hasFrameCount;
    const std::uint64_t frameLimit = options.frameLimit;
    const std::uint32_t canvasWidth = localDesktopBootstrap || physicalLayer ? pbmodulation::kLocalDesktopCanvasWidth : pbmodulation::kReferenceCanvasWidth;
    const std::uint32_t canvasHeight = localDesktopBootstrap || physicalLayer ? pbmodulation::kLocalDesktopCanvasHeight : pbmodulation::kReferenceCanvasHeight;
    try
    {
        DiagnosticFile telemetry(options.telemetryPath);
        const auto session = pbprotocol::GenerateRandomSessionId();
        if (!session)
        {
            throw std::runtime_error("OS CSPRNG session creation failed");
        }
        const auto sessionTag = pbprotocol::DeriveSessionTag(session.Value());
        pbrenderd3d::DataWindowConfig windowConfig;
        windowConfig.width = canvasWidth;
        windowConfig.height = canvasHeight;
        auto created = pbrenderd3d::DataWindow::Create(windowConfig);
        if (!created)
        {
            const auto error = created.Error();
            throw std::runtime_error(std::string(pbrenderd3d::GetPresentationErrorName(error.code)) + ":" + pbrenderd3d::GetPresentationStageName(error.stage) +
                                     ":" + std::to_string(error.nativeError));
        }
        const auto window = std::move(created).Value();
        std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bootstrapBytes{};
        std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> control{};
        const std::size_t dataBytes = desktopLevels ? levelsProfile->dataBytes : shapeChroma ? pbmodulation::kShapeChromaDataBytes :
            localDesktopBootstrap ? 0 : pbmodulation::kReferenceDataRegionBytes;
        std::vector<std::byte> data(dataBytes);
        std::vector<std::byte> pixels(localDesktopBootstrap || physicalLayer ? pbmodulation::kLocalDesktopFrameBgraBytes : pbmodulation::kReferenceFrameBgraBytes);
        for (std::size_t index = 0; index < data.size(); index++)
        {
            data[index] = static_cast<std::byte>((index * 37 + 5) & 255);
        }
        std::uint64_t sequence = 0;
        std::uint64_t lastPresents = 0;
        auto lastProgress = std::chrono::steady_clock::now();
        auto lastLog = lastProgress;
        auto nextSequence = lastProgress;
        // The configured physical-layer dwell is expected silence between
        // presents, not a hang: the no-progress deadline spans two full
        // configured intervals.
        const auto noProgressLimit = std::chrono::milliseconds(std::max<std::uint64_t>(10000, physicalLayer ?
            2 * std::uint64_t(options.sequenceIntervalMilliseconds) + 1000 : 0ull));
        for (;;)
        {
            const auto snapshot = window->GetSnapshot();
            const auto now = std::chrono::steady_clock::now();
            if (snapshot.state == pbrenderd3d::WindowState::Failed)
            {
                telemetry.Write(snapshot);
                throw std::runtime_error(std::string(pbrenderd3d::GetPresentationErrorName(snapshot.error.code)) + ":" +
                                         pbrenderd3d::GetPresentationStageName(snapshot.error.stage) + ":" + std::to_string(snapshot.error.nativeError));
            }
            if (snapshot.state == pbrenderd3d::WindowState::Stopped)
            {
                if (hasFrameCount && sequence < frameLimit)
                {
                    throw std::runtime_error("data window closed before the submission budget completed");
                }
                break;
            }
            if (snapshot.totalPresentCalls != lastPresents)
            {
                lastPresents = snapshot.totalPresentCalls;
                lastProgress = now;
            }
            if (now - lastLog >= std::chrono::seconds(1))
            {
                telemetry.Write(snapshot);
                lastLog = now;
            }
            if (hasFrameCount && sequence >= frameLimit && !snapshot.pendingFrame && !snapshot.inFlightFrame)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                break;
            }
            if (hasFrameCount && now - lastProgress > noProgressLimit)
            {
                throw std::runtime_error("finite presentation run made no progress for " + std::to_string(noProgressLimit.count()) + " ms");
            }
            if ((!hasFrameCount || sequence < frameLimit) && snapshot.state == pbrenderd3d::WindowState::Running && !snapshot.pendingFrame &&
                (!physicalLayer || now >= nextSequence))
            {
                // Each accepted submission advances the same in-band sequence.
                // Only the explicit visual option changes the raster binding.
                const pbprotocol::BootstrapRecord bootstrap{
                    pbprotocol::kBootstrapVersion, pbprotocol::GetProtocolVersion(),
                    desktopLevels ? pbmodulation::kDesktopLevelsLayoutVersion : shapeChroma ? pbmodulation::kShapeChromaLayoutVersion :
                        localDesktopBootstrap ? pbmodulation::kLocalDesktopLayoutVersion : std::uint8_t{1},
                    desktopLevels ? levelsProfile->visualProfileId : shapeChroma ? pbmodulation::kShapeChromaProfileId :
                        localDesktopBootstrap ? pbmodulation::kLocalDesktopVisualProfileId : 0x5042524546524153ULL, sessionTag, sequence, 0, 0};
                if (!pbprotocol::SerializeBootstrapRecord(bootstrap, bootstrapBytes))
                {
                    throw std::runtime_error("canonical Bootstrap serialization failed");
                }
                if (physicalLayer && !pbdesktoplevels::GenerateDiagnosticData(bootstrapBytes, data))
                {
                    throw std::runtime_error("physical-layer diagnostic Transport/Robust QC-LDPC generation failed");
                }
                const auto encoded = desktopLevels ? pbmodulation::EncodeDesktopLevelsFrame(bootstrapBytes, data, pixels) :
                    shapeChroma ? pbmodulation::EncodeShapeChromaFrame(bootstrapBytes, data, pixels) :
                    localDesktopBootstrap ? pbmodulation::EncodeLocalDesktopBootstrapFrame(bootstrapBytes, pixels) :
                    pbmodulation::EncodeReferenceFrame({bootstrapBytes, control, data}, pixels);
                if (!encoded)
                {
                    throw std::runtime_error(localDesktopBootstrap ? "LocalDesktop Bootstrap frame generation failed" : "canonical reference frame generation failed");
                }
                const auto status =
                    window->SubmitFrame({pixels, canvasWidth, canvasHeight, static_cast<std::size_t>(canvasWidth) * 4,
                                         sequence, snapshot.timing.presentationEpoch});
                if (status)
                {
                    if (sequence == std::numeric_limits<std::uint64_t>::max())
                    {
                        throw std::runtime_error("FrameSequence exhausted; a new Session is required");
                    }
                    sequence++;
                    if (physicalLayer)
                    {
                        // Pace from the actual accepted submission, never rush
                        // delayed sequences to catch up and shorten their dwell.
                        nextSequence = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.sequenceIntervalMilliseconds);
                    }
                }
                else if (status.code != pbrenderd3d::PresentationErrorCode::EpochMismatch && status.code != pbrenderd3d::PresentationErrorCode::Paused &&
                         status.code != pbrenderd3d::PresentationErrorCode::NotRunning)
                {
                    throw std::runtime_error(localDesktopBootstrap ? "LocalDesktop Bootstrap frame submission failed" : "reference frame submission failed");
                }
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        const auto finalSample = window->GetSnapshot();
        telemetry.Write(finalSample);
        window->Stop();
        const auto stoppedSample = window->GetSnapshot();
        const bool stopFailed = stoppedSample.state == pbrenderd3d::WindowState::Failed;
        if (stopFailed)
        {
            telemetry.Write(stoppedSample);
        }
        telemetry.Finish();
        pbrenderd3d::WriteDataWindowSnapshotJson(std::cout, stopFailed ? stoppedSample : finalSample);
        std::cout << '\n';
        return stopFailed || finalSample.state == pbrenderd3d::WindowState::Failed ? 1 : 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "PixelBridgeEncoder presentation error: " << exception.what() << '\n';
        return 1;
    }
}
