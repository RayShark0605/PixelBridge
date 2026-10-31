#include "presentation_backend.h"

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

namespace
{
using namespace pbrenderd3d;

struct HiddenBackend
{
    std::unique_ptr<PresentationBackend> backend;
    explicit HiddenBackend(const bool warp = true, const PresentationStage failAfterStage = PresentationStage::None)
    {
        NativeBackendTestOptions options;
        options.warp = warp;
        options.verifyUploads = true;
        options.hiddenWindow = true;
        options.failAfterStage = failAfterStage;
        backend = MakeNativeBackend(options);
    }
    ~HiddenBackend()
    {
        backend->Shutdown();
    }
};

[[nodiscard]] DataWindowConfig MakeConfig()
{
    DataWindowConfig config;
    config.width = 64;
    config.height = 32;
    config.topmost = true;
    config.clientOrigin = PhysicalPoint{0, 0};
    config.presentationModel = PresentationModel::ComposedBlt;
    return config;
}
}

TEST_CASE("Hidden real blt swap chains expose the actual descriptor without flip statistics", "[presentation][composed-blt][hidden]")
{
    const auto config = MakeConfig();
    for (const bool warp : {true, false})
    {
        HiddenBackend fixture(warp);
        const auto status = fixture.backend->Initialize(config);
        INFO(static_cast<int>(status.code) << ":" << static_cast<int>(status.stage) << ":" << status.nativeError);
        REQUIRE(static_cast<bool>(status));
        const HWND window = reinterpret_cast<HWND>(fixture.backend->GetWindowToken());
        REQUIRE(window != nullptr);
        REQUIRE_FALSE(IsWindowVisible(window));
        const auto contract = fixture.backend->GetContract();
        CHECK(contract.presentationModel == PresentationModel::ComposedBlt);
        CHECK(contract.swapEffect == PresentationSwapEffect::BltDiscard);
        CHECK(contract.pacing == PresentationPacing::GpuCompletionAndVsync);
        CHECK_FALSE(contract.latencyWaitable);
        CHECK_FALSE(contract.scalingNone);
        CHECK(contract.scalingStretch);
        CHECK(contract.bufferWidth == config.width);
        CHECK(contract.bufferHeight == config.height);
        CHECK(contract.bufferCount == 2);
        CHECK(contract.maximumFrameLatency == 1);
        CHECK(contract.bgraUnorm);
        CHECK(contract.noMsaa);
        CHECK(contract.alphaIgnored);
        CHECK(contract.tearingDisabled);
        CHECK(contract.perMonitorV2);
        CHECK(fixture.backend->GetDiagnostics().liveOwnedHandles == 1);
        CHECK(fixture.backend->GetStatistics().status == pbpresenttiming::StatisticsStatus::Unavailable);
    }
}

TEST_CASE("Hidden blt permits are single use, reset after drain, and cancellable", "[presentation][composed-blt][hidden]")
{
    const auto config = MakeConfig();
    for (int repetition = 0; repetition < 3; repetition++)
    {
        HiddenBackend fixture;
        REQUIRE(static_cast<bool>(fixture.backend->Initialize(config)));
        std::vector<std::byte> pixels(static_cast<std::size_t>(config.width) * config.height * 4, std::byte{0x80});
        REQUIRE(fixture.backend->Wait(true, 20).wake == BackendWake::FramePermit);
        CHECK(fixture.backend->Wait(true, 5).wake != BackendWake::FramePermit);
        REQUIRE(static_cast<bool>(fixture.backend->Upload(pixels)));
        const auto present = fixture.backend->Present();
        REQUIRE(static_cast<bool>(present.error));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto wake = BackendWake::Timeout;
        while (wake != BackendWake::FramePermit && std::chrono::steady_clock::now() < deadline)
        {
            wake = fixture.backend->Wait(true, 20).wake;
            REQUIRE(wake != BackendWake::Failed);
        }
        REQUIRE(wake == BackendWake::FramePermit);
        const auto environment = fixture.backend->PollEnvironment();
        REQUIRE(environment);
        REQUIRE(static_cast<bool>(fixture.backend->Reconfigure(environment.Value())));
        REQUIRE(fixture.backend->Wait(true, 20).wake == BackendWake::FramePermit);
        fixture.backend->Cancel();
        CHECK(fixture.backend->Wait(true, 1000).wake != BackendWake::FramePermit);
        const HWND window = reinterpret_cast<HWND>(fixture.backend->GetWindowToken());
        fixture.backend->Shutdown();
        CHECK(fixture.backend->GetDiagnostics().liveOwnedHandles == 0);
        CHECK(fixture.backend->GetDiagnostics().liveGraphicsObjects == 0);
        CHECK_FALSE(IsWindow(window));
    }
}

TEST_CASE("Partial hidden blt initialization releases every graphics object and handle", "[presentation][composed-blt][hidden]")
{
    for (const auto stage : {PresentationStage::WakeEvent, PresentationStage::WindowClass, PresentationStage::Window,
        PresentationStage::Adapter, PresentationStage::Device, PresentationStage::SwapChain,
        PresentationStage::FrameLatency, PresentationStage::BackBuffer})
    {
        HiddenBackend fixture(true, stage);
        const auto status = fixture.backend->Initialize(MakeConfig());
        REQUIRE_FALSE(static_cast<bool>(status));
        CHECK(status.stage == stage);
        fixture.backend->Shutdown();
        CHECK(fixture.backend->GetDiagnostics().liveOwnedHandles == 0);
        CHECK(fixture.backend->GetDiagnostics().liveGraphicsObjects == 0);
    }
}

TEST_CASE("Hidden default flip retains its real waitable discard contract", "[presentation][hidden]")
{
    auto config = MakeConfig();
    config.presentationModel = PresentationModel::Flip;
    HiddenBackend fixture;
    REQUIRE(static_cast<bool>(fixture.backend->Initialize(config)));
    const auto contract = fixture.backend->GetContract();
    CHECK(contract.presentationModel == PresentationModel::Flip);
    CHECK(contract.swapEffect == PresentationSwapEffect::FlipDiscard);
    CHECK(contract.pacing == PresentationPacing::FrameLatencyWaitable);
    CHECK(contract.latencyWaitable);
    CHECK(contract.scalingNone);
    CHECK_FALSE(contract.scalingStretch);
    CHECK(contract.bufferCount == 2);
    CHECK(contract.maximumFrameLatency == 1);
    CHECK(fixture.backend->GetDiagnostics().liveOwnedHandles == 2);
    CHECK_FALSE(IsWindowVisible(reinterpret_cast<HWND>(fixture.backend->GetWindowToken())));
}

TEST_CASE("Cancelling hidden blt before its first permit is immediate and never grants capacity", "[presentation][composed-blt][hidden]")
{
    HiddenBackend fixture;
    REQUIRE(static_cast<bool>(fixture.backend->Initialize(MakeConfig())));
    fixture.backend->Cancel();
    const auto start = std::chrono::steady_clock::now();
    CHECK(fixture.backend->Wait(true, 1000).wake == BackendWake::Message);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250));
    CHECK(fixture.backend->GetDiagnostics().framePermits == 0);
}
