#include "support.h"
#include "local_desktop_runtime.h"
#include "run_report.h"
#include <array>
#include <atomic>
#include <chrono>
#include <sstream>
#include <thread>

namespace pbstep3b
{
namespace
{
struct ProducerState
{
    explicit ProducerState(const std::filesystem::path& path) : pixels(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
    }
    Handle pixels;
    std::array<std::string, kFrameCount> frameDigests;
    std::atomic<std::uint32_t> frames = 0;
    std::atomic<bool> stopped = false;
};

class FrozenPresentation final : public pbapp::EncoderPresentation
{
public:
    explicit FrozenPresentation(std::shared_ptr<ProducerState> state) : state_(std::move(state))
    {
    }
    pbrenderd3d::DataWindowSnapshot GetSnapshot() const override
    {
        pbrenderd3d::DataWindowSnapshot snapshot;
        snapshot.state = state_->stopped ? pbrenderd3d::WindowState::Stopped : pbrenderd3d::WindowState::Running;
        snapshot.environment.clientWidth = 1920;
        snapshot.environment.clientHeight = 1080;
        snapshot.contract = {1920, 1080, 2, 1, pbrenderd3d::FlipEffect::Discard, true, true, true, true, true, true, true, true, true, true, true, true};
        snapshot.candidateContractSatisfied = true;
        snapshot.viewport.disposition = pbrenderd3d::PresentationViewportDisposition::Active;
        snapshot.timing.presentationEpoch = 1;
        snapshot.pendingFrame = !state_->stopped && state_->frames >= kFrameCount;
        return snapshot;
    }
    pbrenderd3d::PresentationStatus SubmitFrame(const pbrenderd3d::CanonicalBgraFrameView& frame) override
    {
        const auto index = state_->frames.load();
        Require(index < kFrameCount && frame.pixels.size() == kFrameBytes, "Production fixture frame budget");
        WriteExact(state_->pixels.Get(), frame.pixels);
        state_->frameDigests[index] = Digest(frame.pixels);
        state_->frames++;
        return {};
    }
    void RequestStop() noexcept override
    {
        state_->stopped = true;
    }
    void Stop() noexcept override
    {
        RequestStop();
    }
private:
    std::shared_ptr<ProducerState> state_;
};
}

void MakeFixture(const std::filesystem::path& source, const std::filesystem::path& root)
{
    RequireLocal(source);
    RequireLocal(root);
    Require(std::filesystem::file_size(source) == kSourceBytes, "Only approved 64 KiB source supported");
    Require(std::filesystem::create_directory(root), "Fixture root must be new");
    const auto state = std::make_shared<ProducerState>(root / "source.bgra");
    pbapp::EncoderRuntime encoder([state](const pbrenderd3d::DataWindowConfig&)
        {
            return std::make_unique<FrozenPresentation>(state);
        });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 15);
    config.sessionStateRoot = root / "encoder-state";
    config.diagnostics = std::make_shared<pbcore::StageDiagnostics>();
    const auto status = encoder.Start(config);
    Require(static_cast<bool>(status), status.message);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (state->frames < kFrameCount && encoder.GetSnapshot().state != pbapp::EncoderState::Failed && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    encoder.Stop();
    const auto snapshot = encoder.GetSnapshot();
    Require(state->frames == kFrameCount && snapshot.state == pbapp::EncoderState::Stopped, "Production fixture incomplete: " + snapshot.errorDetail);
    Require(snapshot.sourceBytes == kSourceBytes && snapshot.segmentCount == 1 && snapshot.compressionCodec == pbprotocol::CompressionCodec::Raw &&
        snapshot.outerFecMode == pbprotocol::OuterFecMode::WirehairV2 && snapshot.outerBlockCount == 50, "Production multi-frame fixture dimensions or FEC mode differ from approved contract");
    Require(FlushFileBuffers(state->pixels.Get()), "Frozen pixels flush failed");
    std::ostringstream manifest;
    manifest << "{\"schema\":\"PixelBridge.Step3B.FrozenPixels.1\",\"producer\":\"ProductionEncoderRuntime\",\"sessionIdentity\":\"OS-CSPRNG-frozen-in-pixels\","
        << "\"sourceBytes\":65536,\"width\":1920,\"height\":1080,\"rowPitch\":7680,\"pixelFormat\":\"BGRA8\",\"colorContract\":\"CanonicalSDR_RGB_full_no_conversion\","
        << "\"file\":\"source.bgra\",\"frameCount\":30,\"outerFec\":\"WirehairV2\",\"outerBlockBytes\":1314,\"systematicBlockCount\":" << snapshot.outerBlockCount
        << ",\"timeBaseNumerator\":1,\"timeBaseDenominator\":15,\"timingAuthority\":\"SyntheticOrdinal_NotMeasuredSenderClock\",\"frames\":[";
    for (std::uint32_t index = 0; index < kFrameCount; index++)
    {
        manifest << (index == 0 ? "" : ",") << "{\"ordinal\":" << index << ",\"pts\":" << index << ",\"duration\":1,\"blake3\":\"" << state->frameDigests[index] << "\"}";
    }
    manifest << "]}\n";
    WriteNewText(root / "fixture.json", manifest.str());
    WriteNewText(root / "sender-report.json", pbapp::BuildEncoderRunReportJson({"Step3BProducer", "sealed-Step2", "tool-only", "unknown"}, snapshot));
}
}
