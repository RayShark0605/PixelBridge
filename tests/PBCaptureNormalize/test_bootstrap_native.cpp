#include "pbcapturenormalize/diagnostic_readback.h"
#include "pbrenderd3d/data_window.h"
#include "pbscreencapturedxgi/dxgi_capture.h"
#include "pbscreencapturewgc/wgc_capture.h"
#include "../../apps/PixelBridgeDecoder/bootstrap_diagnostic.h"
#include "../../libs/PBCaptureNormalize/src/native_support.h"
#include "../../libs/PBRenderD3D/src/presentation_backend.h"
#include "../../libs/PBScreenCaptureDxgi/src/capture_internal.h"
#include "../../libs/PBScreenCaptureWgc/src/capture_internal.h"
#include "../PBModulation/local_desktop_test_fixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

int RunCaptureBootstrapCommand(int argumentCount, wchar_t* arguments[]);

namespace
{

using namespace pbcapturenormalize;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

template<typename Predicate>
bool Await(const Clock::time_point deadline, Predicate predicate)
{
    do
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (Clock::now() < deadline);
    return predicate();
}

LONG Coordinate(const std::int64_t value)
{
    REQUIRE(value >= (std::numeric_limits<LONG>::min)());
    REQUIRE(value <= (std::numeric_limits<LONG>::max)());
    return static_cast<LONG>(value);
}

std::vector<std::byte> HalfScaleIndependentRaster(const std::string_view stem = "a")
{
    // The expected record is NEVER given to the capture/processor path. Only
    // this sender image is presented; the processor receives actual OS pixels.
    const auto source = localdesktoptest::MakeGoldenRaster(stem);
    std::vector<std::byte> scaled(std::size_t{960} * 540 * 4);
    for (std::uint32_t y = 0; y < 540; y++)
    {
        for (std::uint32_t x = 0; x < 960; x++)
        {
            for (std::size_t channel = 0; channel < 4; channel++)
            {
                std::uint32_t sum = 0;
                for (std::uint32_t row = 0; row < 2; row++)
                {
                    for (std::uint32_t column = 0; column < 2; column++)
                    {
                        const std::size_t offset = ((std::size_t{y} * 2 + row) * 1920 + x * 2 + column) * 4 + channel;
                        sum += std::to_integer<std::uint8_t>(source[offset]);
                    }
                }
                scaled[(std::size_t{y} * 960 + x) * 4 + channel] = static_cast<std::byte>((sum + 2u) / 4u);
            }
        }
    }
    return scaled;
}

struct NativeFixture
{
    void Initialize(const Clock::time_point deadline)
    {
        INFO("Native Bootstrap requires a PMv2 test executable; no DPI mode is changed by this test");
        REQUIRE(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
        const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        REQUIRE(primary != nullptr);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        REQUIRE(GetMonitorInfoW(primary, &monitor));
        const auto workWidth = static_cast<std::int64_t>(monitor.rcWork.right) - monitor.rcWork.left;
        const auto workHeight = static_cast<std::int64_t>(monitor.rcWork.bottom) - monitor.rcWork.top;
        INFO("Native Bootstrap needs at least 960x540 unobstructed physical pixels on the primary output");
        REQUIRE(workWidth >= 960);
        REQUIRE(workHeight >= 540);
        const auto left = static_cast<std::int64_t>(monitor.rcWork.left) + (workWidth - 960) / 2;
        const auto top = static_cast<std::int64_t>(monitor.rcWork.top) + (workHeight - 540) / 2;
        const RECT requested{Coordinate(left), Coordinate(top), Coordinate(left + 960), Coordinate(top + 540)};
        const auto resolved = pbscreenregion::ResolveScreenCaptureRegion(requested, normalize.capture.region);
        INFO("ResolveScreenCaptureRegion=" << pbscreenregion::GetScreenRegionErrorName(resolved.code) << " HRESULT=" << resolved.nativeError);
        REQUIRE(resolved);

        ComPtr<IDXGIFactory1> factory;
        REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput6> output;
        const auto found = pbcapturenormalize::detail::FindCaptureAdapter(factory.Get(), normalize.capture.region, adapter, output, environment);
        INFO("Capture output resolve=" << GetCaptureErrorName(found.code) << " HRESULT=" << found.nativeError);
        REQUIRE(found);
        std::cout << "bootstrap-native precondition: colorSpace=" << environment.outputColorSpace << " hdr=" << environment.hdr
                  << " bits=" << environment.bitsPerColor << " adapter=" << environment.adapterLuid.HighPart << ':' << environment.adapterLuid.LowPart << '\n';
        INFO("Native Bootstrap SDR baseline is unavailable: output must be RGB_FULL_G22_NONE_P709, not HDR/unknown; this test does not change display settings");
        REQUIRE_FALSE(environment.hdr);
        REQUIRE(environment.outputColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);

        pbrenderd3d::DataWindowConfig windowConfig;
        windowConfig.width = 960;
        windowConfig.height = 540;
        windowConfig.clientOrigin = pbrenderd3d::PhysicalPoint{requested.left, requested.top};
        windowConfig.waitTimeoutMilliseconds = 2000;
        auto created = pbrenderd3d::DataWindow::Create(windowConfig);
        INFO("DataWindow create=" << pbrenderd3d::GetPresentationErrorName(created.Error().code));
        REQUIRE(created);
        window = std::move(created).Value();
        windowHandle = reinterpret_cast<HWND>(pbrenderd3d::DataWindowTestAccess::GetWindowToken(*window));
        REQUIRE(windowHandle != nullptr);
        // Only our own transient fixture becomes topmost, without activation.
        // No external window, display mode, HDR setting or cursor is modified.
        REQUIRE(SetWindowPos(windowHandle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW));
        REQUIRE(Await((std::min)(deadline, Clock::now() + std::chrono::seconds(4)), [&]
        {
            return window->GetSnapshot().candidateContractSatisfied;
        }));
        const auto snapshot = window->GetSnapshot();
        REQUIRE(snapshot.environment.clientOrigin.x == requested.left);
        REQUIRE(snapshot.environment.clientOrigin.y == requested.top);
        REQUIRE(snapshot.environment.clientWidth == 960);
        REQUIRE(snapshot.environment.clientHeight == 540);
        REQUIRE(snapshot.contract.scalingNone);
        REQUIRE_FALSE(snapshot.softwareRasterizer);
        normalize.capture.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        normalize.capture.maximumFrameAgeMilliseconds = 1000;
        normalize.capture.gpuTimeoutMilliseconds = 1000;
        pixels = HalfScaleIndependentRaster();
        Present(deadline, 1);
        RequireVisible(deadline);
    }

    void Present(const Clock::time_point deadline, const std::uint64_t presentationSequence)
    {
        const auto before = window->GetSnapshot();
        const auto status = window->SubmitFrame({pixels, 960, 540, 960 * 4, presentationSequence, before.timing.presentationEpoch});
        INFO("Native fixture submit=" << pbrenderd3d::GetPresentationErrorName(status.code));
        REQUIRE(status);
        REQUIRE(Await((std::min)(deadline, Clock::now() + std::chrono::seconds(4)), [&]
        {
            return window->GetSnapshot().totalSuccessfulPresents > before.totalSuccessfulPresents;
        }));
    }

    bool Visible() const noexcept
    {
        const auto& rectangle = normalize.capture.region.physicalRect;
        const std::array<POINT, 5> points{{{rectangle.left + 1, rectangle.top + 1}, {rectangle.right - 2, rectangle.top + 1},
            {rectangle.left + 1, rectangle.bottom - 2}, {rectangle.right - 2, rectangle.bottom - 2}, {rectangle.left + 480, rectangle.top + 270}}};
        return std::all_of(points.begin(), points.end(), [this](const POINT point)
        {
            return GetAncestor(WindowFromPoint(point), GA_ROOT) == windowHandle;
        });
    }

    void RequireVisible(const Clock::time_point deadline) const
    {
        const bool visible = Await((std::min)(deadline, Clock::now() + std::chrono::seconds(2)), [this] { return Visible(); });
        if (!visible)
        {
            const auto& rectangle = normalize.capture.region.physicalRect;
            std::cout << "bootstrap-native visibility failed: fixture=" << windowHandle << " roi=" << rectangle.left << ',' << rectangle.top << ','
                      << rectangle.right << ',' << rectangle.bottom << " centre-window=" << WindowFromPoint(POINT{rectangle.left + 480, rectangle.top + 270}) << '\n';
        }
        INFO("Native Bootstrap fixture is occluded: WindowFromPoint must identify our DataWindow at all four corners and centre; no skip/fake capture is permitted");
        REQUIRE(visible);
    }

    std::unique_ptr<pbrenderd3d::DataWindow> window;
    HWND windowHandle = nullptr;
    CaptureNormalizeConfig normalize;
    CaptureEnvironment environment;
    std::vector<std::byte> pixels;
};

class NativeCapture
{
public:
    CaptureStatus Start(const CaptureBackendKind kind, const CaptureNormalizeConfig& config, const std::shared_ptr<DiagnosticCpuReadback>& readback)
    {
        return kind == CaptureBackendKind::Wgc ? pbscreencapturewgc::WgcCapture::CreateNormalized(config, readback, wgc_) :
                                               pbscreencapturedxgi::DxgiCapture::Create(config, readback, dxgi_);
    }
    CaptureSnapshot GetSnapshot() const noexcept
    {
        return wgc_ ? wgc_->GetSnapshot() : dxgi_->GetSnapshot();
    }
    CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept
    {
        return wgc_ ? wgc_->GetNormalizationSnapshot() : dxgi_->GetNormalizationSnapshot();
    }
    CaptureStatus Stop() noexcept
    {
        return wgc_ ? wgc_->Stop() : dxgi_->Stop();
    }
    void RequestRecreate() noexcept
    {
        if (wgc_)
        {
            pbscreencapturewgc::WgcCaptureTestAccess::RequestRecreate(*wgc_);
        }
        else
        {
            pbscreencapturedxgi::DxgiCaptureTestAccess::RequestRecreate(*dxgi_);
        }
    }
private:
    std::unique_ptr<pbscreencapturewgc::WgcCapture> wgc_;
    std::unique_ptr<pbscreencapturedxgi::DxgiCapture> dxgi_;
};

// Observe the real application's transient reset state on the existing CPU
// worker, without modifying pixels, metadata, admission, or callback ordering.
// One fixed snapshot avoids racing a polling thread against the next Analyze.
class ResetObservedProcessor final : public CpuFrameProcessor
{
public:
    explicit ResetObservedProcessor(std::shared_ptr<pbdecoder::BootstrapDiagnosticProcessor> processor) : processor_(std::move(processor))
    {
    }
    void Reset(std::optional<ScreenCaptureDomain> domain) noexcept override
    {
        processor_->Reset(std::move(domain));
        const auto snapshot = processor_->GetSnapshot();
        const std::lock_guard lock(mutex_);
        lastReset_ = snapshot;
    }
    CaptureStatus Analyze(const ScreenCaptureFrameMetadata& metadata, const std::span<const std::byte> pixels, const std::size_t rowPitch) override
    {
        return processor_->Analyze(metadata, pixels, rowPitch);
    }
    void Commit(const ScreenCaptureFrameMetadata& metadata) noexcept override
    {
        processor_->Commit(metadata);
    }
    void Discard() noexcept override
    {
        processor_->Discard();
    }
    pbdecoder::BootstrapDiagnosticSnapshot GetLastReset() const noexcept
    {
        const std::lock_guard lock(mutex_);
        return lastReset_;
    }
private:
    const std::shared_ptr<pbdecoder::BootstrapDiagnosticProcessor> processor_;
    mutable std::mutex mutex_;
    pbdecoder::BootstrapDiagnosticSnapshot lastReset_;
};

void RunNativeBootstrap(const CaptureBackendKind kind)
{
    const auto started = Clock::now();
    const auto deadline = started + std::chrono::seconds(24);
    NativeFixture fixture;
    fixture.Initialize(deadline);
    const auto expected = localdesktoptest::LoadGoldenRecord("a");
    const auto processor = std::make_shared<pbdecoder::BootstrapDiagnosticProcessor>();
    const auto resetObserver = std::make_shared<ResetObservedProcessor>(processor);
    DiagnosticReadbackConfig readbackConfig;
    readbackConfig.maximumRoiSize = {960, 540};
    readbackConfig.maximumFrameAgeMilliseconds = 1000;
    std::shared_ptr<DiagnosticCpuReadback> readback;
    REQUIRE(DiagnosticCpuReadback::Create(readbackConfig, resetObserver, readback));
    NativeCapture capture;
    const auto status = capture.Start(kind, fixture.normalize, readback);
    INFO("Native capture create=" << GetCaptureErrorName(status.code) << " stage=" << static_cast<unsigned>(status.stage) << " HRESULT=" << status.nativeError);
    REQUIRE(status);
    fixture.Present(deadline, 2);
    pbdecoder::BootstrapDiagnosticEvent lastEvent;
    std::optional<pbdecoder::BootstrapDiagnosticEvent> accepted;
    const bool completed = Await(deadline, [&]
    {
        for (std::size_t index = 0; index < pbdecoder::BootstrapDiagnosticProcessor::eventCapacity; index++)
        {
            pbdecoder::BootstrapDiagnosticEvent event;
            if (!processor->TakeEvent(event))
            {
                break;
            }
            lastEvent = event;
            if (event.disposition == pbdecoder::BootstrapDisposition::Accepted)
            {
                accepted = event;
                return true;
            }
        }
        return capture.GetSnapshot().state == CaptureState::Failed;
    });
    const auto captureSnapshot = capture.GetSnapshot();
    const auto normalized = capture.GetNormalizationSnapshot();
    const auto readbackSnapshot = readback->GetSnapshot();
    const auto processorSnapshot = processor->GetSnapshot();
    std::cout << "bootstrap-native backend=" << static_cast<unsigned>(kind) << " state=" << static_cast<unsigned>(captureSnapshot.state)
              << " captureError=" << GetCaptureErrorName(captureSnapshot.error.code) << " arrived=" << captureSnapshot.arrivedFrames
              << " copied=" << captureSnapshot.copiedFrames << " normalized=" << normalized.acceptedFrames << " captureErasures=" << normalized.erasedFrames
              << " lastCaptureErasure=" << GetCaptureErasureName(normalized.lastErasure) << " mapped=" << readbackSnapshot.mappedFrames
              << " commits=" << readbackSnapshot.committedFrames << " readbackDrop=" << GetDiagnosticReadbackDropName(readbackSnapshot.lastDrop)
              << " accepted=" << processorSnapshot.accepted << " disposition=" << pbdecoder::GetBootstrapDispositionName(lastEvent.disposition)
              << " visual=" << pbmodulation::GetLocalDesktopErasureName(lastEvent.visual.erasure) << '\n';
    INFO("Expected an actual SDR/cursor-excluded screen observation through runtime, normalization, readback and the application processor within 24 seconds");
    REQUIRE(completed);
    REQUIRE(accepted.has_value());
    CHECK(captureSnapshot.error);
    CHECK(readbackSnapshot.error);
    fixture.RequireVisible(deadline);
    CHECK(accepted->visual.IsAccepted());
    CHECK(accepted->visual.canonical44 == expected);
    for (const auto& copy : accepted->visual.copies)
    {
        CHECK(copy.fecDecoded);
        CHECK(copy.crcValid);
        CHECK(copy.recordValid);
        CHECK(copy.canonical44 == expected);
    }
    CHECK(accepted->bootstrap.visualProfileId == 0x50424C4442533031ULL);
    CHECK(accepted->bootstrap.visualLayoutVersion == 2);
    CHECK(accepted->bootstrap.sessionTag.value == 0x81DF204BD997BAD0ULL);
    CHECK(accepted->bootstrap.frameSequence == 0x1112131415161718ULL);
    CHECK(accepted->bootstrap.controlEpoch == 0x21222324u);
    CHECK(accepted->capture.backend == kind);
    CHECK(accepted->capture.domain == normalized.domain);
    CHECK(accepted->capture.domain.captureEpoch == captureSnapshot.captureEpoch);
    CHECK(std::any_of(accepted->capture.domain.sourceId.begin(), accepted->capture.domain.sourceId.end(), [](const auto value) { return value != std::byte{0}; }));
    CHECK(accepted->capture.captureObservation > 0);
    CHECK(accepted->capture.sourceGeneration > 0);
    CHECK(accepted->capture.slotGeneration > 0);
    CHECK(EqualRect(&accepted->capture.physicalRoi, &fixture.normalize.capture.region.physicalRect));
    CHECK(accepted->capture.roiSize == CaptureSize{960, 540});
    CHECK(accepted->capture.adapterLuid.LowPart == fixture.environment.adapterLuid.LowPart);
    CHECK(accepted->capture.adapterLuid.HighPart == fixture.environment.adapterLuid.HighPart);
    CHECK(accepted->capture.isCursorExcluded);
    CHECK_FALSE(accepted->capture.hdr);
    // The DXGI duplication backend converts the SDR scan-out into the FP16 ROI contract, so the
    // expected signal class follows the delivered pixel format (backend-independent normalized contract).
    CHECK(accepted->capture.signalEncoding == (accepted->capture.pixelFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
        ? CaptureSignalEncoding::LinearScRgb : CaptureSignalEncoding::SdrRgb));
    CHECK(accepted->capture.timestamp.monotonic100ns > 0);
    CHECK(accepted->capture.timestamp.domain == (kind == CaptureBackendKind::Wgc ? CaptureTimestampDomain::WgcSystemRelative100ns : CaptureTimestampDomain::DxgiQpcTicks));
    CHECK(std::abs(accepted->visual.geometry.scaleX - 0.5) < 0.005);
    CHECK(std::abs(accepted->visual.geometry.scaleY - 0.5) < 0.005);
    CHECK(std::abs(accepted->visual.geometry.originX) <= 1.25);
    CHECK(std::abs(accepted->visual.geometry.originY) <= 1.25);
    CHECK(processorSnapshot.accepted > 0);
    CHECK(readbackSnapshot.diagnosticCpuReadback);
    CHECK(readbackSnapshot.mappedFrames > 0);
    CHECK(readbackSnapshot.committedFrames > 0);

    const auto expectedNext = localdesktoptest::LoadGoldenRecord("b");
    auto nextPixels = HalfScaleIndependentRaster("b");
    REQUIRE(expectedNext != expected);
    const auto originalDomain = accepted->capture.domain;
    const auto beforeRecreate = capture.GetSnapshot();
    const auto beforeNormalized = capture.GetNormalizationSnapshot();
    const auto beforeReadback = readback->GetSnapshot();
    const auto beforeProcessor = processor->GetSnapshot();
    REQUIRE(beforeNormalized.active);
    REQUIRE(beforeNormalized.domain == originalDomain);
    REQUIRE(beforeProcessor.domain == originalDomain);
    REQUIRE(accepted->capture.slotIndex < fixture.normalize.capture.roiTextureCount);
    std::array<std::uint64_t, pbcapturenormalize::detail::maximumRoiTextures> slotGenerations{};
    REQUIRE(accepted->capture.slotIndex < slotGenerations.size());
    slotGenerations[accepted->capture.slotIndex] = accepted->capture.slotGeneration;

    // Request the actual pool/duplication recreation on this same capture
    // instance. There is no fake source, metadata injection, or second capture.
    capture.RequestRecreate();
    CaptureSnapshot recreatedCapture;
    CaptureNormalizeSnapshot recreatedNormalized;
    DiagnosticReadbackSnapshot recreatedReadback;
    pbdecoder::BootstrapDiagnosticSnapshot resetState;
    const bool restarted = Await(deadline, [&]
    {
        recreatedCapture = capture.GetSnapshot();
        recreatedNormalized = capture.GetNormalizationSnapshot();
        recreatedReadback = readback->GetSnapshot();
        resetState = resetObserver->GetLastReset();
        return recreatedCapture.state == CaptureState::Failed ||
            (recreatedCapture.state == CaptureState::Running && recreatedNormalized.active &&
             recreatedNormalized.domain.captureEpoch > originalDomain.captureEpoch &&
             recreatedCapture.captureEpoch == recreatedNormalized.domain.captureEpoch && recreatedReadback.active &&
             recreatedReadback.domain == recreatedNormalized.domain && !recreatedReadback.resetPending &&
             resetState.domain == recreatedNormalized.domain);
    });
    INFO("Real recreation must produce a new active capture/readback/processor domain before presenting the independent second pattern");
    INFO("recreate state=" << static_cast<unsigned>(recreatedCapture.state) << " error=" << GetCaptureErrorName(recreatedCapture.error.code)
         << " stage=" << static_cast<unsigned>(recreatedCapture.error.stage) << " HRESULT=" << recreatedCapture.error.nativeError
         << " epoch=" << recreatedCapture.captureEpoch << " processorResets=" << recreatedReadback.processorResets);
    REQUIRE(restarted);
    REQUIRE(recreatedCapture.state == CaptureState::Running);
    REQUIRE(recreatedNormalized.active);
    REQUIRE(recreatedReadback.active);
    REQUIRE(resetState.domain.has_value());
    const auto newDomain = recreatedNormalized.domain;
    REQUIRE(newDomain != originalDomain);
    CHECK(newDomain.sourceId == originalDomain.sourceId);
    CHECK(newDomain.captureEpoch > originalDomain.captureEpoch);
    CHECK(recreatedCapture.captureEpoch == newDomain.captureEpoch);
    CHECK(recreatedCapture.recreates > beforeRecreate.recreates);
    CHECK(recreatedCapture.error);
    CHECK(recreatedReadback.error);
    CHECK(recreatedNormalized.epochStarts > beforeNormalized.epochStarts);
    CHECK(recreatedNormalized.invalidations > beforeNormalized.invalidations);
    CHECK(recreatedReadback.domainStarts > beforeReadback.domainStarts);
    CHECK(recreatedReadback.invalidations > beforeReadback.invalidations);
    CHECK(recreatedReadback.processorResets > beforeReadback.processorResets);
    CHECK(resetState.domain == newDomain);
    CHECK(resetState.resets > beforeProcessor.resets);
    CHECK(resetState.geometryGeneration == 0);
    CHECK(resetState.calibrationGeneration == 0);
    CHECK(resetState.trackedSessions == 0);
    CHECK(resetState.retainedSequences == 0);
    CHECK(resetState.queuedEvents == 0);

    fixture.pixels = std::move(nextPixels);
    fixture.Present(deadline, 3);
    const auto stationaryPresentCount = fixture.window->GetSnapshot().totalSuccessfulPresents;
    std::optional<pbdecoder::BootstrapDiagnosticEvent> firstNewAccepted;
    std::optional<pbdecoder::BootstrapDiagnosticEvent> nextAccepted;
    std::uint64_t lastObservation = beforeRecreate.arrivedFrames;
    const bool recovered = Await(deadline, [&]
    {
        for (std::size_t index = 0; index < pbdecoder::BootstrapDiagnosticProcessor::eventCapacity; index++)
        {
            pbdecoder::BootstrapDiagnosticEvent event;
            if (!processor->TakeEvent(event))
            {
                break;
            }
            lastEvent = event;
            // Freshly captured 'a' pixels between restart and the sole 'b'
            // Present are valid. Old observations/domain relabeling are not.
            CHECK(event.capture.domain == newDomain);
            CHECK(event.capture.domain != originalDomain);
            CHECK(event.capture.domain.sourceId == originalDomain.sourceId);
            CHECK(event.capture.sourceGeneration > accepted->capture.sourceGeneration);
            CHECK(event.capture.captureObservation > lastObservation);
            lastObservation = event.capture.captureObservation;
            REQUIRE(event.capture.slotIndex < fixture.normalize.capture.roiTextureCount);
            REQUIRE(event.capture.slotIndex < slotGenerations.size());
            CHECK(event.capture.slotGeneration > slotGenerations[event.capture.slotIndex]);
            slotGenerations[event.capture.slotIndex] = event.capture.slotGeneration;
            if (event.disposition == pbdecoder::BootstrapDisposition::Accepted)
            {
                CHECK(event.visual.IsAccepted());
                CHECK((event.visual.canonical44 == expected || event.visual.canonical44 == expectedNext));
                if (!firstNewAccepted)
                {
                    firstNewAccepted = event;
                    CHECK(event.geometryGeneration == 1);
                    CHECK(event.calibrationGeneration == 1);
                }
                if (event.visual.canonical44 == expectedNext)
                {
                    nextAccepted = event;
                    return true;
                }
            }
        }
        return capture.GetSnapshot().state == CaptureState::Failed;
    });
    const auto recoveredCapture = capture.GetSnapshot();
    const auto recoveredNormalized = capture.GetNormalizationSnapshot();
    const auto recoveredReadback = readback->GetSnapshot();
    const auto recoveredProcessor = processor->GetSnapshot();
    std::cout << "bootstrap-native recreated: backend=" << static_cast<unsigned>(kind) << " oldEpoch=" << originalDomain.captureEpoch
              << " newEpoch=" << recoveredCapture.captureEpoch << " recreates=" << recoveredCapture.recreates
              << " state=" << static_cast<unsigned>(recoveredCapture.state) << " error=" << GetCaptureErrorName(recoveredCapture.error.code)
              << " resets=" << recoveredProcessor.resets << " mapped=" << recoveredReadback.mappedFrames
              << " captureErasures=" << recoveredNormalized.erasedFrames << " lastCaptureErasure=" << GetCaptureErasureName(recoveredNormalized.lastErasure)
              << " readbackDrop=" << GetDiagnosticReadbackDropName(recoveredReadback.lastDrop)
              << " accepted=" << recoveredProcessor.accepted << " disposition=" << pbdecoder::GetBootstrapDispositionName(lastEvent.disposition)
              << " visual=" << pbmodulation::GetLocalDesktopErasureName(lastEvent.visual.erasure) << '\n';
    INFO("The static second pattern must finish through owner Poll/readback without any further SubmitFrame or Present, within the original 24-second deadline");
    REQUIRE(recovered);
    REQUIRE(firstNewAccepted.has_value());
    REQUIRE(nextAccepted.has_value());
    fixture.RequireVisible(deadline);
    CHECK(fixture.window->GetSnapshot().totalSuccessfulPresents == stationaryPresentCount);
    CHECK(nextAccepted->visual.IsAccepted());
    CHECK(nextAccepted->visual.canonical44 == expectedNext);
    for (const auto& copy : nextAccepted->visual.copies)
    {
        CHECK(copy.fecDecoded);
        CHECK(copy.crcValid);
        CHECK(copy.recordValid);
        CHECK(copy.canonical44 == expectedNext);
    }
    CHECK(nextAccepted->bootstrap.visualProfileId == 0x50424C4442533031ULL);
    CHECK(nextAccepted->bootstrap.visualLayoutVersion == 2);
    CHECK(nextAccepted->bootstrap.sessionTag.value == 0x81DF204BD997BAD0ULL);
    CHECK(nextAccepted->bootstrap.frameSequence == 0x1112131415161719ULL);
    CHECK(nextAccepted->bootstrap.controlEpoch == 0x21222324u);
    CHECK(nextAccepted->pixelDigest != accepted->pixelDigest);
    CHECK(nextAccepted->capture.backend == kind);
    CHECK(nextAccepted->capture.domain == newDomain);
    CHECK(nextAccepted->capture.domain == recoveredNormalized.domain);
    CHECK(nextAccepted->capture.domain == recoveredReadback.domain);
    CHECK(nextAccepted->capture.domain == recoveredProcessor.domain);
    CHECK(nextAccepted->capture.domain.captureEpoch == recoveredCapture.captureEpoch);
    CHECK(nextAccepted->capture.sourceGeneration > accepted->capture.sourceGeneration);
    CHECK(nextAccepted->capture.captureObservation > beforeRecreate.arrivedFrames);
    CHECK(nextAccepted->capture.slotGeneration > 0);
    CHECK(EqualRect(&nextAccepted->capture.physicalRoi, &fixture.normalize.capture.region.physicalRect));
    CHECK(nextAccepted->capture.roiSize == CaptureSize{960, 540});
    CHECK(nextAccepted->capture.sourceContentSize == recoveredCapture.environment.contentSize);
    CHECK(nextAccepted->capture.sourceExtent == recoveredCapture.environment.sourceSize);
    CHECK(nextAccepted->capture.displayRotation == fixture.normalize.capture.region.rotation);
    CHECK(nextAccepted->capture.sourceTransform == recoveredCapture.environment.sourceRotation);
    CHECK(nextAccepted->capture.sourcePixelFormat == recoveredCapture.environment.pixelFormat);
    CHECK(nextAccepted->capture.pixelFormat == nextAccepted->capture.sourcePixelFormat);
    CHECK(nextAccepted->capture.adapterLuid.LowPart == fixture.environment.adapterLuid.LowPart);
    CHECK(nextAccepted->capture.adapterLuid.HighPart == fixture.environment.adapterLuid.HighPart);
    CHECK(nextAccepted->capture.isCursorExcluded);
    CHECK_FALSE(nextAccepted->capture.hdr);
    CHECK(nextAccepted->capture.signalEncoding == (nextAccepted->capture.pixelFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
        ? CaptureSignalEncoding::LinearScRgb : CaptureSignalEncoding::SdrRgb));
    CHECK(nextAccepted->capture.timestamp.monotonic100ns > accepted->capture.timestamp.monotonic100ns);
    CHECK(nextAccepted->capture.timestamp.domain == (kind == CaptureBackendKind::Wgc ? CaptureTimestampDomain::WgcSystemRelative100ns : CaptureTimestampDomain::DxgiQpcTicks));
    CHECK(std::abs(nextAccepted->visual.geometry.scaleX - 0.5) < 0.005);
    CHECK(std::abs(nextAccepted->visual.geometry.scaleY - 0.5) < 0.005);
    CHECK(std::abs(nextAccepted->visual.geometry.originX) <= 1.25);
    CHECK(std::abs(nextAccepted->visual.geometry.originY) <= 1.25);
    CHECK(nextAccepted->geometryGeneration > 0);
    CHECK(nextAccepted->calibrationGeneration > 0);
    CHECK(recoveredCapture.error);
    CHECK(recoveredReadback.error);
    CHECK(recoveredCapture.copiedFrames > beforeRecreate.copiedFrames);
    CHECK(recoveredReadback.mappedFrames > beforeReadback.mappedFrames);
    CHECK(recoveredReadback.committedFrames > beforeReadback.committedFrames);
    CHECK(recoveredProcessor.accepted > beforeProcessor.accepted);
    CHECK(recoveredProcessor.trackedSessions == 1);
    CHECK(recoveredProcessor.retainedSequences >= 1);
    CHECK(recoveredProcessor.retainedSequences <= 2);
    CHECK(recoveredProcessor.staleCommits == beforeProcessor.staleCommits);
    CHECK(recoveredProcessor.identityConflicts == beforeProcessor.identityConflicts);
    CHECK(accepted->capture.domain == originalDomain);

    REQUIRE(capture.Stop());
    REQUIRE(capture.Stop());
    const auto stopped = capture.GetSnapshot();
    CHECK(stopped.state == CaptureState::Stopped);
    CHECK(stopped.shutdownComplete);
    CHECK_FALSE(stopped.deferredCleanup);
    CHECK(stopped.queuedFrames == 0);
    CHECK(stopped.liveFrameLeases == 0);
    CHECK(stopped.busyRoiTextures == 0);
    const auto normalizedStopped = capture.GetNormalizationSnapshot();
    CHECK_FALSE(normalizedStopped.active);
    CHECK(normalizedStopped.domain == newDomain);
    CHECK(normalizedStopped.invalidations > recreatedNormalized.invalidations);
    REQUIRE(readback->Stop(2000));
    const auto readbackStopped = readback->GetSnapshot();
    CHECK(readbackStopped.workerStopped);
    CHECK_FALSE(readbackStopped.active);
    CHECK(readbackStopped.pendingStagingFrames == 0);
    CHECK(readbackStopped.queuedFrames == 0);
    CHECK(readbackStopped.cpuBuffersInUse == 0);
    const auto processorStopped = processor->GetSnapshot();
    CHECK_FALSE(processorStopped.domain.has_value());
    CHECK(processorStopped.geometryGeneration == 0);
    CHECK(processorStopped.calibrationGeneration == 0);
    CHECK(processorStopped.trackedSessions == 0);
    CHECK(processorStopped.retainedSequences == 0);
    CHECK(processorStopped.queuedEvents == 0);
    CHECK(processorStopped.staleCommits == beforeProcessor.staleCommits);
    CHECK(processorStopped.identityConflicts == beforeProcessor.identityConflicts);
    pbdecoder::BootstrapDiagnosticEvent afterStop;
    CHECK_FALSE(processor->TakeEvent(afterStop));
    CHECK(fixture.window->GetSnapshot().totalSuccessfulPresents == stationaryPresentCount);
    fixture.window->Stop();
    CHECK(fixture.window->GetSnapshot().state == pbrenderd3d::WindowState::Stopped);
    CHECK_FALSE(IsWindow(fixture.windowHandle));
    CHECK(Clock::now() - started < std::chrono::seconds(30));
}

void RunNativeBootstrapCli(const CaptureBackendKind kind)
{
    const auto started = Clock::now();
    NativeFixture fixture;
    fixture.Initialize(started + std::chrono::seconds(10));
    const auto& region = fixture.normalize.capture.region.physicalRect;
    // Only the selected physical ROI/backend/run duration enter the real CLI.
    // SessionTag/FrameSequence/Profile are exclusively in the displayed pixels.
    std::vector<std::wstring> arguments{L"PixelBridgeDecoder", L"--capture-bootstrap", L"--backend",
        kind == CaptureBackendKind::Wgc ? L"wgc" : L"dxgi", L"--seconds", L"3", L"--roi",
        std::to_wstring(region.left), std::to_wstring(region.top), std::to_wstring(region.right), std::to_wstring(region.bottom)};
    std::vector<wchar_t*> pointers;
    pointers.reserve(arguments.size());
    for (auto& argument : arguments)
    {
        pointers.push_back(argument.data());
    }
    REQUIRE(RunCaptureBootstrapCommand(static_cast<int>(pointers.size()), pointers.data()) == 0);
    fixture.RequireVisible(started + std::chrono::seconds(20));
    fixture.window->Stop();
    CHECK_FALSE(IsWindow(fixture.windowHandle));
    CHECK(Clock::now() - started < std::chrono::seconds(25));
}

} // namespace

TEST_CASE("Actual WGC pixels carry independently generated half-scale LocalDesktop Bootstrap", "[bootstrap-wgc-native]")
{
    RunNativeBootstrap(CaptureBackendKind::Wgc);
}

TEST_CASE("Actual Desktop Duplication pixels carry independently generated half-scale LocalDesktop Bootstrap", "[bootstrap-dxgi-native]")
{
    RunNativeBootstrap(CaptureBackendKind::Dxgi);
}

TEST_CASE("Decoder diagnostic CLI accepts Bootstrap only from actual WGC screen pixels", "[bootstrap-wgc-cli-native]")
{
    RunNativeBootstrapCli(CaptureBackendKind::Wgc);
}

TEST_CASE("Decoder diagnostic CLI accepts Bootstrap only from actual DXGI screen pixels", "[bootstrap-dxgi-cli-native]")
{
    RunNativeBootstrapCli(CaptureBackendKind::Dxgi);
}
