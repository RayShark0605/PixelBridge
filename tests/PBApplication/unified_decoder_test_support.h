#pragma once

#include "local_desktop_runtime.h"
#include "pbmodulation/unified_visual.h"
#include "pbprotocol/blake3_digest.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <stdexcept>

// Shared by G16 controller tests and the offscreen widget smoke. Never linked
// as a product input mode: only capture/GPU edges are replaced, not Receiver.
namespace g16test
{

inline void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

inline bool WaitFor(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

inline pbscreenregion::ScreenCaptureRegion Region()
{
    return {reinterpret_cast<HMONITOR>(std::uintptr_t{1}), {0, 0, 1920, 1080}, {0, 0, 3840, 2160},
        96, 96, DXGI_MODE_ROTATION_IDENTITY};
}

inline std::vector<std::byte> RawBytes(const std::size_t count)
{
    std::vector<std::byte> bytes(count);
    std::uint32_t state = 0x16380904;
    for (auto& byte : bytes)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<std::byte>(state & 255);
    }
    return bytes;
}

struct PixelFrames
{
    std::mutex mutex;
    std::vector<std::vector<std::byte>> frames;
    std::uint32_t limit = 1;
    bool stopped = false;
};

class PixelCollector final : public pbapp::EncoderPresentation
{
public:
    explicit PixelCollector(std::shared_ptr<PixelFrames> frames) : frames_(std::move(frames))
    {
    }
    pbrenderd3d::DataWindowSnapshot GetSnapshot() const override
    {
        const std::scoped_lock lock(frames_->mutex);
        pbrenderd3d::DataWindowSnapshot snapshot;
        snapshot.state = frames_->stopped ? pbrenderd3d::WindowState::Stopped : pbrenderd3d::WindowState::Running;
        snapshot.environment.clientWidth = 1920;
        snapshot.environment.clientHeight = 1080;
        snapshot.contract = {1920, 1080, 2, 1, pbrenderd3d::FlipEffect::Discard,
            true, true, true, true, true, true, true, true, true, true, true, true};
        snapshot.candidateContractSatisfied = true;
        snapshot.viewport.disposition = pbrenderd3d::PresentationViewportDisposition::Active;
        snapshot.timing.presentationEpoch = 1;
        snapshot.pendingFrame = frames_->frames.size() >= frames_->limit;
        return snapshot;
    }
    pbrenderd3d::PresentationStatus SubmitFrame(const pbrenderd3d::CanonicalBgraFrameView& frame) override
    {
        const std::scoped_lock lock(frames_->mutex);
        Check(frames_->frames.size() < frames_->limit, "pixel fixture exceeded its bounded frame budget");
        frames_->frames.emplace_back(frame.pixels.begin(), frame.pixels.end());
        return {};
    }
    void RequestStop() noexcept override
    {
        const std::scoped_lock lock(frames_->mutex);
        frames_->stopped = true;
    }
    void Stop() noexcept override
    {
        RequestStop();
    }
private:
    std::shared_ptr<PixelFrames> frames_;
};

inline std::vector<pbdemodd3d11::CaptureDemodulatorResult> MakeFrames(const std::filesystem::path& root,
    const std::span<const std::byte> sourceBytes, const std::uint32_t frameCount = 1)
{
    Check(frameCount > 0 && frameCount <= 8, "fixture frame budget is outside 1..8");
    std::filesystem::create_directories(root);
    const auto source = root / L"g16-source.bin";
    {
        std::ofstream file(source, std::ios::binary);
        file.write(reinterpret_cast<const char*>(sourceBytes.data()), static_cast<std::streamsize>(sourceBytes.size()));
        Check(file.good(), "cannot write fixture source");
    }
    const auto pixels = std::make_shared<PixelFrames>();
    pixels->limit = frameCount;
    pbapp::EncoderRuntime encoder([pixels](const pbrenderd3d::DataWindowConfig&)
    {
        return std::make_unique<PixelCollector>(pixels);
    });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 60);
    config.sessionStateRoot = root / L"encoder-state";
    const auto start = encoder.Start(config);
    Check(static_cast<bool>(start), start.message.c_str());
    const bool ready = WaitFor([&]()
    {
        const std::scoped_lock lock(pixels->mutex);
        return pixels->frames.size() == frameCount || encoder.GetSnapshot().state == pbapp::EncoderState::Failed;
    });
    encoder.Stop();
    Check(ready && pixels->frames.size() == frameCount, encoder.GetSnapshot().errorDetail.c_str());
    auto oracleResult = pbmodulation::UnifiedVisualCpuOracle::Create(pbmodulation::UnifiedVisualCpuOracle::RequiredBytes());
    Check(static_cast<bool>(oracleResult), "cannot create CPU pixel oracle");
    auto oracle = std::move(oracleResult).Value();
    std::vector<pbdemodd3d11::CaptureDemodulatorResult> frames;
    for (const auto& raster : pixels->frames)
    {
        pbdemodd3d11::CaptureDemodulatorResult result;
        result.kind = pbdemodd3d11::CaptureDemodulatorResultKind::UnifiedFrame;
        result.geometryStatus = pbdemodd3d11::CaptureDemodulatorGeometryStatus::ExactCanvas;
        const pbmodulation::LumaView view{raster, 1920, 1080, 1920 * 4, pbmodulation::LumaPixelFormat::Bgra8};
        const auto observation = oracle.DecodeMixedFrame(view);
        Check(observation.IsFrameAvailable(), "runtime pixel fixture could not be decoded");
        result.bootstrap = observation.bootstrap;
        Check(static_cast<bool>(pbprotocol::SerializeBootstrapRecord(observation.bootstrapRecord, result.bootstrapRecord)),
            "cannot serialize observed Bootstrap");
        result.demodulation.visualProfileId = observation.bootstrapRecord.visualProfileId;
        result.demodulation.unifiedObservation = observation;
        const auto blocks = oracle.GetAcceptedBlocks();
        result.demodulation.acceptedUnifiedBlockCount = static_cast<std::uint32_t>(blocks.size());
        std::copy(blocks.begin(), blocks.end(), result.demodulation.acceptedUnifiedBlocks.begin());
        frames.push_back(std::move(result));
    }
    return frames;
}

struct ReceiveState
{
    std::mutex mutex;
    std::deque<pbdemodd3d11::CaptureDemodulatorResult> frames;
    pbcapturenormalize::CaptureSnapshot capture;
    pbcapturenormalize::CaptureNormalizeSnapshot normalized;
    pbdemodd3d11::CaptureDemodulatorSnapshot demod;
    pbdemodd3d11::CaptureDemodulatorConfig requestedDemod;
    pbapp::CaptureBackend backend = pbapp::CaptureBackend::Wgc;
    std::uint32_t sessionStarts = 0;
    bool failWgc = false;
    bool paused = false;

    void Push(const pbdemodd3d11::CaptureDemodulatorResult& frame)
    {
        const std::scoped_lock lock(mutex);
        Check(frames.size() < 16, "fixture queue exceeds its bound");
        frames.push_back(frame);
    }
    std::uint64_t Delivered()
    {
        const std::scoped_lock lock(mutex);
        return capture.deliveredFrames;
    }
};

class NullConsumer final : public pbcapturenormalize::ScreenCaptureConsumer
{
public:
    pbcapturenormalize::CaptureStatus DomainStarted(const pbcapturenormalize::ScreenCaptureDomain&,
        const pbcapturenormalize::CaptureEnvironment&, ID3D11Device*) override
    {
        return {};
    }
    void DomainInvalidated(const pbcapturenormalize::ScreenCaptureDomain&) noexcept override
    {
    }
    pbcapturenormalize::CaptureStatus Submit(const pbcapturenormalize::ScreenCaptureFrame&, ID3D11DeviceContext*) override
    {
        return {};
    }
};

class CapturedResults final : public pbapp::DecoderDemodulator
{
public:
    explicit CapturedResults(std::shared_ptr<ReceiveState> state) : state_(std::move(state)), consumer_(std::make_shared<NullConsumer>())
    {
    }
    std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> GetConsumer() const override
    {
        return consumer_;
    }
    bool TakeResult(pbdemodd3d11::CaptureDemodulatorResult& result) override
    {
        const std::scoped_lock lock(state_->mutex);
        if (state_->frames.empty() || state_->paused || !state_->normalized.active)
        {
            return false;
        }
        result = std::move(state_->frames.front());
        state_->frames.pop_front();
        LARGE_INTEGER counter{};
        LARGE_INTEGER frequency{};
        Check(QueryPerformanceCounter(&counter) && QueryPerformanceFrequency(&frequency), "QPC unavailable");
        const std::int64_t now = (counter.QuadPart / frequency.QuadPart) * 10000000 +
            (counter.QuadPart % frequency.QuadPart) * 10000000 / frequency.QuadPart;
        result.metadata.domain = state_->normalized.domain;
        result.metadata.backend = state_->backend == pbapp::CaptureBackend::Wgc ?
            pbcapturenormalize::CaptureBackendKind::Wgc : pbcapturenormalize::CaptureBackendKind::Dxgi;
        state_->capture.deliveredFrames++;
        result.metadata.captureObservation = state_->capture.deliveredFrames;
        result.metadata.timestamp.monotonic100ns = now;
        result.metadata.timestamp.arrivalQpc100ns = now;
        result.demodulation.metadata = result.metadata;
        state_->capture.arrivedFrames = state_->capture.deliveredFrames;
        state_->capture.copiedFrames = state_->capture.deliveredFrames;
        state_->demod.completedFrames++;
        if (result.bootstrap.IsAccepted())
        {
            state_->demod.bootstrapAcceptedFrames++;
        }
        return true;
    }
    pbdemodd3d11::CaptureDemodulatorSnapshot GetSnapshot() const override
    {
        const std::scoped_lock lock(state_->mutex);
        return state_->demod;
    }
private:
    std::shared_ptr<ReceiveState> state_;
    std::shared_ptr<NullConsumer> consumer_;
};

class CaptureSession final : public pbapp::DecoderCaptureSession
{
public:
    CaptureSession(std::shared_ptr<ReceiveState> state, const pbapp::CaptureBackend backend) : state_(std::move(state)), backend_(backend)
    {
    }
    pbcapturenormalize::CaptureStatus Start(const pbcapturenormalize::CaptureNormalizeConfig& config,
        std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer>) noexcept override
    {
        const std::scoped_lock lock(state_->mutex);
        state_->sessionStarts++;
        state_->backend = backend_;
        state_->capture = {};
        state_->normalized = {};
        state_->demod = {};
        state_->capture.captureEpoch = config.capture.initialCaptureEpoch;
        state_->capture.state = pbcapturenormalize::CaptureState::Running;
        state_->normalized.enabled = true;
        state_->normalized.active = true;
        state_->normalized.domain.captureEpoch = config.capture.initialCaptureEpoch;
        state_->normalized.domain.sourceId[0] = static_cast<std::byte>(state_->sessionStarts);
        if (state_->failWgc && backend_ == pbapp::CaptureBackend::Wgc)
        {
            state_->capture.state = pbcapturenormalize::CaptureState::Failed;
            state_->capture.error = pbcapturenormalize::CaptureStatus::Failure(
                pbcapturenormalize::CaptureError::NativeFailure, pbcapturenormalize::CaptureStage::Configuration);
        }
        return state_->capture.error;
    }
    pbcapturenormalize::CaptureSnapshot GetSnapshot() const noexcept override
    {
        const std::scoped_lock lock(state_->mutex);
        return state_->capture;
    }
    pbcapturenormalize::CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept override
    {
        const std::scoped_lock lock(state_->mutex);
        return state_->normalized;
    }
    void RequestStop() noexcept override
    {
    }
    pbcapturenormalize::CaptureStatus Stop() noexcept override
    {
        const std::scoped_lock lock(state_->mutex);
        state_->normalized.active = false;
        state_->capture.shutdownComplete = true;
        state_->demod.demodulator.shutdown = true;
        return state_->capture.error;
    }
private:
    std::shared_ptr<ReceiveState> state_;
    pbapp::CaptureBackend backend_;
};

inline pbapp::DecoderRuntimeServices Services(const std::shared_ptr<ReceiveState>& state)
{
    pbapp::DecoderRuntimeServices services;
    services.captureFactory = [state](const pbapp::CaptureBackend backend)
    {
        return std::make_unique<CaptureSession>(state, backend);
    };
    services.demodulatorFactory = [state](const pbdemodd3d11::CaptureDemodulatorConfig& config,
        std::shared_ptr<pbapp::DecoderDemodulator>& output)
    {
        const std::scoped_lock lock(state->mutex);
        pbdemodd3d11::CaptureDemodulatorBudget budget;
        const auto status = pbdemodd3d11::CalculateCaptureDemodulatorBudget(config, budget);
        if (status)
        {
            state->requestedDemod = config;
            output = std::make_shared<CapturedResults>(state);
        }
        return status;
    };
    return services;
}

inline bool VerifyOutput(const pbapp::DecoderSnapshot& snapshot, const std::span<const std::byte> expected)
{
    if (snapshot.state != pbapp::DecoderState::Completed || !snapshot.wholeFileDigestVerified ||
        !snapshot.finalPublishSucceeded || snapshot.verifiedRawBytes != expected.size())
    {
        return false;
    }
    const std::filesystem::path path(std::u8string(snapshot.outputPath.begin(), snapshot.outputPath.end()));
    if (std::filesystem::file_size(path) != expected.size())
    {
        return false;
    }
    std::vector<std::byte> bytes(expected.size());
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good() && std::equal(bytes.begin(), bytes.end(), expected.begin(), expected.end()) &&
        pbprotocol::ComputeBlake3Digest(bytes) == pbprotocol::ComputeBlake3Digest(expected);
}

} // namespace g16test
