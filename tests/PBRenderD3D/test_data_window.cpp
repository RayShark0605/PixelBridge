#include "pbrenderd3d/data_window.h"
#include "presentation_backend.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
using namespace pbrenderd3d;
using namespace std::chrono_literals;

struct FakeControl
{
    std::mutex mutex;
    std::condition_variable changed;
    bool awake = false;
    bool cancelled = false;
    bool holdUpload = false;
    bool uploadEntered = false;
    bool holdReconfigure = false;
    bool reconfigureEntered = false;
    bool failWait = false;
    bool failQpcOnWake = false;
    std::atomic<bool> failQpc = false;
    bool corruptContract = false;
    bool mismatchedBuffer = false;
    unsigned int permits = 0;
    unsigned int shutdowns = 0;
    unsigned int uploads = 0;
    unsigned int presents = 0;
    unsigned int consumedPermits = 0;
    unsigned int reconfigurations = 0;
    unsigned int wrongOwnerCalls = 0;
    std::uint32_t presentId = 0;
    std::uint32_t deviceAdapterLow = 0;
    std::int32_t deviceAdapterHigh = 0;
    std::thread::id owner;
    PresentationStatus initializeStatus;
    PresentationStatus uploadStatus;
    PresentationStatus presentStatus;
    PresentationStatus reconfigureStatus;
    pbpresenttiming::PresentOutcome presentOutcome = pbpresenttiming::PresentOutcome::Success;
    WindowEnvironment environment;
    PresentationContract contract;
    BackendDiagnostics diagnostics;
    std::vector<std::byte> uploadedPixels;
    std::deque<BackendStatistics> statistics;
};

class FakeBackend final : public PresentationBackend
{
public:
    explicit FakeBackend(std::shared_ptr<FakeControl> control) : control_(std::move(control))
    {
    }
    std::int64_t GetQpcFrequency() const noexcept override
    {
        return 1000000;
    }
    std::int64_t NowQpc() const noexcept override
    {
        if (control_->failQpc.load())
        {
            return -1;
        }
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    PresentationStatus Initialize(const DataWindowConfig& config) noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        control_->owner = std::this_thread::get_id();
        control_->environment.clientWidth = config.width;
        control_->environment.clientHeight = config.height;
        control_->environment.modeWidth = 1920;
        control_->environment.modeHeight = 1080;
        control_->environment.modeFrequency = 60;
        control_->environment.dpi = 96;
        control_->environment.singleMonitor = true;
        control_->environment.adapterAvailable = true;
        control_->environment.monitorIdentity = 1;
        control_->contract = {config.width, config.height, config.bufferCount,        config.maximumFrameLatency, config.flipEffect, true, true, true, true,
                              true,         true,          !control_->corruptContract};
        if (control_->mismatchedBuffer)
        {
            control_->contract.bufferWidth++;
        }
        control_->diagnostics.swapChainGeneration = 1;
        control_->diagnostics.bufferGeneration = 1;
        control_->uploadedPixels.resize(static_cast<std::size_t>(config.width) * config.height * 4);
        return control_->initializeStatus;
    }
    PresentationResult<WindowEnvironment> PollEnvironment() noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        CheckOwner();
        return PresentationResult<WindowEnvironment>::Success(control_->environment);
    }
    PresentationStatus Reconfigure(const WindowEnvironment& environment) noexcept override
    {
        std::unique_lock lock(control_->mutex);
        CheckOwner();
        control_->reconfigureEntered = true;
        control_->changed.notify_all();
        control_->changed.wait(lock,
                               [&]
                               {
                                   return !control_->holdReconfigure || control_->cancelled;
                               });
        if (control_->cancelled)
        {
            return PresentationStatus::Failure(PresentationErrorCode::NotRunning, PresentationStage::GpuDrain);
        }
        control_->reconfigurations++;
        if (environment.minimized || environment.clientWidth == 0 || environment.clientHeight == 0 || !environment.adapterAvailable ||
            !environment.singleMonitor)
        {
            return PresentationStatus::Success();
        }
        if (control_->reconfigureStatus)
        {
            if (control_->deviceAdapterLow != environment.adapterLuidLow || control_->deviceAdapterHigh != environment.adapterLuidHigh)
            {
                control_->diagnostics.swapChainGeneration++;
                control_->deviceAdapterLow = environment.adapterLuidLow;
                control_->deviceAdapterHigh = environment.adapterLuidHigh;
            }
            control_->contract.bufferWidth = environment.clientWidth;
            control_->contract.bufferHeight = environment.clientHeight;
            control_->diagnostics.bufferGeneration++;
        }
        return control_->reconfigureStatus;
    }
    PresentationContract GetContract() const noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        return control_->contract;
    }
    BackendDiagnostics GetDiagnostics() const noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        return control_->diagnostics;
    }
    BackendWaitResult Wait(const bool requestPermit, const std::uint32_t milliseconds) noexcept override
    {
        std::unique_lock lock(control_->mutex);
        CheckOwner();
        control_->changed.wait_for(lock, std::chrono::milliseconds(milliseconds),
                                   [&]
                                   {
                                       return control_->cancelled || control_->awake || control_->failWait || (requestPermit && control_->permits != 0);
                                   });
        if (control_->failWait)
        {
            return {BackendWake::Failed, PresentationStatus::Failure(PresentationErrorCode::WaitFailed, PresentationStage::Wait, 123)};
        }
        if (control_->awake || control_->cancelled)
        {
            control_->awake = false;
            return {};
        }
        if (requestPermit && control_->permits != 0)
        {
            control_->permits--;
            control_->consumedPermits++;
            control_->diagnostics.framePermits++;
            return {BackendWake::FramePermit, {}};
        }
        return {BackendWake::Timeout, {}};
    }
    PresentationStatus Upload(const std::span<const std::byte> pixels) noexcept override
    {
        std::unique_lock lock(control_->mutex);
        CheckOwner();
        control_->uploadEntered = true;
        control_->changed.notify_all();
        control_->changed.wait(lock,
                               [&]
                               {
                                   return !control_->holdUpload || control_->cancelled;
                               });
        if (control_->cancelled)
        {
            return PresentationStatus::Failure(PresentationErrorCode::NotRunning, PresentationStage::Upload);
        }
        control_->uploads++;
        if (pixels.size() != control_->uploadedPixels.size())
        {
            return PresentationStatus::Failure(PresentationErrorCode::InvalidFrame, PresentationStage::Upload);
        }
        std::copy(pixels.begin(), pixels.end(), control_->uploadedPixels.begin());
        return control_->uploadStatus;
    }
    BackendPresentResult Present() noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        CheckOwner();
        control_->presents++;
        control_->diagnostics.presentCalls++;
        control_->presentId++;
        return {control_->presentStatus, control_->presentOutcome, control_->presentId};
    }
    BackendStatistics GetStatistics() noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        CheckOwner();
        if (control_->statistics.empty())
        {
            return {};
        }
        const auto result = control_->statistics.front();
        control_->statistics.pop_front();
        return result;
    }
    void Wake() noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        if (control_->failQpcOnWake)
        {
            control_->failQpc.store(true);
        }
        control_->awake = true;
        control_->changed.notify_all();
    }
    void Cancel() noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        control_->cancelled = true;
        control_->changed.notify_all();
    }
    void Shutdown() noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        CheckOwner();
        control_->shutdowns++;
    }
    std::uintptr_t GetWindowToken() const noexcept override
    {
        return 0;
    }

private:
    void CheckOwner() const noexcept
    {
        if (control_->owner != std::this_thread::get_id())
        {
            control_->wrongOwnerCalls++;
        }
    }
    std::shared_ptr<FakeControl> control_;
};

bool WaitUntil(const std::function<bool()>& predicate, const std::chrono::milliseconds timeout = 2000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

struct Fixture
{
    DataWindowConfig config;
    std::shared_ptr<FakeControl> control = std::make_shared<FakeControl>();
    std::unique_ptr<DataWindow> window;
    std::vector<std::byte> pixels;

    Fixture()
    {
        config.width = 8;
        config.height = 4;
        pixels.resize(128, std::byte{0x71});
    }
    void Start()
    {
        auto result = DataWindowTestAccess::Create(config, std::make_unique<FakeBackend>(control));
        REQUIRE(static_cast<bool>(result));
        window = std::move(result).Value();
    }
    PresentationStatus Submit(const std::uint64_t sequence = 1)
    {
        const auto snapshot = window->GetSnapshot();
        return window->SubmitFrame(
            {pixels, config.width, config.height, static_cast<std::size_t>(config.width) * 4, sequence, snapshot.timing.presentationEpoch});
    }
    void Change(const std::function<void(FakeControl&)>& operation)
    {
        const std::lock_guard lock(control->mutex);
        operation(*control);
        control->awake = true;
        control->changed.notify_all();
    }
    void Permit()
    {
        Change(
            [](FakeControl& state)
            {
                state.permits++;
            });
    }
};

}

TEST_CASE("Presentation configuration and frame spans enforce independent exact bounds")
{
    DataWindowConfig config;
    REQUIRE(static_cast<bool>(ValidateDataWindowConfig(config)));
    for (const auto dimension : {0u, 16385u, std::numeric_limits<std::uint32_t>::max()})
    {
        auto invalid = config;
        invalid.width = dimension;
        REQUIRE(ValidateDataWindowConfig(invalid).code == PresentationErrorCode::InvalidConfiguration);
    }
    for (const auto buffers : {0u, 1u, 17u})
    {
        auto invalid = config;
        invalid.bufferCount = buffers;
        REQUIRE_FALSE(static_cast<bool>(ValidateDataWindowConfig(invalid)));
    }
    for (const auto latency : {0u, 3u, std::numeric_limits<std::uint32_t>::max()})
    {
        auto invalid = config;
        invalid.maximumFrameLatency = latency;
        REQUIRE_FALSE(static_cast<bool>(ValidateDataWindowConfig(invalid)));
    }
    config.width = 8;
    config.height = 4;
    config.maximumFrameBytes = 127;
    REQUIRE(ValidateDataWindowConfig(config).code == PresentationErrorCode::ResourceLimit);
    config.maximumFrameBytes = 128;
    REQUIRE(static_cast<bool>(ValidateDataWindowConfig(config)));
    std::vector<std::byte> pixels(128);
    CanonicalBgraFrameView frame{pixels, 8, 4, 32, 1, 1};
    REQUIRE(static_cast<bool>(ValidateCanonicalBgraFrame(config, frame)));
    frame.pixels = std::span<const std::byte>(pixels).first(127);
    REQUIRE(ValidateCanonicalBgraFrame(config, frame).code == PresentationErrorCode::InvalidFrame);
    frame.pixels = pixels;
    frame.rowPitch = std::numeric_limits<std::size_t>::max();
    REQUIRE(ValidateCanonicalBgraFrame(config, frame).code == PresentationErrorCode::InvalidFrame);
    config.clientOrigin = PhysicalPoint{std::numeric_limits<std::int32_t>::max(), 0};
    REQUIRE_FALSE(static_cast<bool>(ValidateDataWindowConfig(config)));
    config.clientOrigin = PhysicalPoint{0, std::numeric_limits<std::int32_t>::max()};
    REQUIRE(ValidateDataWindowConfig(config).code == PresentationErrorCode::InvalidConfiguration);
    config.clientOrigin = PhysicalPoint{std::numeric_limits<std::int32_t>::min(), 0};
    REQUIRE(ValidateDataWindowConfig(config).code == PresentationErrorCode::InvalidConfiguration);
    config.clientOrigin = PhysicalPoint{0, std::numeric_limits<std::int32_t>::min()};
    REQUIRE(ValidateDataWindowConfig(config).code == PresentationErrorCode::InvalidConfiguration);
    config.clientOrigin = PhysicalPoint{std::numeric_limits<std::int32_t>::min() + 1, std::numeric_limits<std::int32_t>::min() + 1};
    REQUIRE(static_cast<bool>(ValidateDataWindowConfig(config)));
    config.clientOrigin = PhysicalPoint{std::numeric_limits<std::int32_t>::max() - 8, std::numeric_limits<std::int32_t>::max() - 4};
    REQUIRE(static_cast<bool>(ValidateDataWindowConfig(config)));
    config.clientOrigin = PhysicalPoint{-4000, -1000};
    REQUIRE(static_cast<bool>(ValidateDataWindowConfig(config)));
}

TEST_CASE("The first render needs a frame permit and ordinary wakeups cannot forge one")
{
    Fixture fixture;
    fixture.Start();
    REQUIRE(static_cast<bool>(fixture.Submit()));
    for (int index = 0; index < 20; index++)
    {
        fixture.Change(
            [](FakeControl&)
            {
            });
    }
    std::this_thread::sleep_for(20ms);
    REQUIRE(fixture.window->GetSnapshot().totalPresentCalls == 0);
    fixture.Permit();
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().totalPresentCalls == 1;
        }));
    fixture.Permit();
    std::this_thread::sleep_for(20ms);
    REQUIRE(fixture.window->GetSnapshot().totalPresentCalls == 1);
    fixture.window->Stop();
    REQUIRE(fixture.control->consumedPermits == 1);
    REQUIRE(fixture.control->uploads == 1);
    REQUIRE(fixture.control->wrongOwnerCalls == 0);
    REQUIRE(fixture.control->owner != std::this_thread::get_id());
    REQUIRE(fixture.control->shutdowns == 1);
}

TEST_CASE("Repeat mode presents one complete active source without advancing its identity")
{
    Fixture fixture;
    fixture.config.repeatActiveFrame = true;
    fixture.Start();
    REQUIRE(static_cast<bool>(fixture.Submit(17)));
    fixture.Permit();
    fixture.Permit();
    fixture.Permit();
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().totalPresentCalls == 3;
        }));
    const auto repeated = fixture.window->GetSnapshot();
    REQUIRE(repeated.sourceTextureReplacements == 1);
    REQUIRE(repeated.repeatedPresentCalls == 2);
    REQUIRE(repeated.activeFrame);
    REQUIRE(repeated.activeFrameSequence == 17);
    REQUIRE(repeated.activeFramePresentationEpoch == repeated.timing.presentationEpoch);
    REQUIRE(repeated.timing.lastPresent.has_value());
    REQUIRE(repeated.timing.lastPresent->frameSequence == 17);
    REQUIRE(fixture.control->uploads == 1);
    REQUIRE(fixture.control->presents == 3);

    std::fill(fixture.pixels.begin(), fixture.pixels.end(), std::byte{0x39});
    REQUIRE(static_cast<bool>(fixture.Submit(18)));
    fixture.Permit();
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().totalPresentCalls == 4;
        }));
    const auto replaced = fixture.window->GetSnapshot();
    REQUIRE(replaced.sourceTextureReplacements == 2);
    REQUIRE(replaced.repeatedPresentCalls == 2);
    REQUIRE(replaced.activeFrameSequence == 18);
    REQUIRE(fixture.control->uploads == 2);
    REQUIRE(std::ranges::all_of(fixture.control->uploadedPixels, [](const std::byte value)
    {
        return value == std::byte{0x39};
    }));
    fixture.window->Stop();
    REQUIRE_FALSE(fixture.window->GetSnapshot().activeFrame);
}

TEST_CASE("Repeat mode invalidates the active source at an epoch boundary")
{
    Fixture fixture;
    fixture.config.repeatActiveFrame = true;
    fixture.Start();
    REQUIRE(static_cast<bool>(fixture.Submit(41)));
    fixture.Permit();
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().totalPresentCalls == 1;
        }));
    const auto before = fixture.window->GetSnapshot();
    fixture.Change(
        [](FakeControl& state)
        {
            state.environment.modeChangeSerial++;
        });
    REQUIRE(WaitUntil(
        [&]
        {
            const auto snapshot = fixture.window->GetSnapshot();
            return snapshot.timing.presentationEpoch > before.timing.presentationEpoch && !snapshot.activeFrame;
        }));
    fixture.Permit();
    std::this_thread::sleep_for(20ms);
    const auto invalidated = fixture.window->GetSnapshot();
    REQUIRE(invalidated.totalPresentCalls == 1);
    REQUIRE(invalidated.invalidatedActiveFrames == 1);
    REQUIRE(invalidated.sourceTextureReplacements == 1);

    REQUIRE(static_cast<bool>(fixture.Submit(42)));
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().totalPresentCalls == 2;
        }));
    const auto restored = fixture.window->GetSnapshot();
    REQUIRE(restored.activeFrame);
    REQUIRE(restored.activeFrameSequence == 42);
    REQUIRE(restored.sourceTextureReplacements == 2);
    REQUIRE(fixture.control->uploads == 2);
    fixture.window->Stop();
}

TEST_CASE("The latest pending frame is owned, bounded, row-pitch aware and failure immutable")
{
    Fixture fixture;
    fixture.Start();
    REQUIRE(static_cast<bool>(fixture.Submit(1)));
    const auto epoch = fixture.window->GetSnapshot().timing.presentationEpoch;
    std::vector<std::byte> padded(4 * 40, std::byte{0xEE});
    for (std::size_t row = 0; row < 4; row++)
    {
        std::fill_n(padded.data() + row * 40, 32, static_cast<std::byte>(row + 1));
    }
    REQUIRE(static_cast<bool>(fixture.window->SubmitFrame({padded, 8, 4, 40, 2, epoch})));
    std::fill(padded.begin(), padded.end(), std::byte{0});
    REQUIRE(fixture.window->SubmitFrame({padded, 8, 4, 31, 3, epoch}).code == PresentationErrorCode::InvalidFrame);
    REQUIRE(fixture.window->SubmitFrame({padded, 8, 4, 40, 3, epoch + 1}).code == PresentationErrorCode::EpochMismatch);
    fixture.Permit();
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().totalPresentCalls == 1;
        }));
    fixture.window->Stop();
    const auto snapshot = fixture.window->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == 2);
    REQUIRE(snapshot.replacedPendingFrames == 1);
    for (std::size_t row = 0; row < 4; row++)
    {
        for (std::size_t column = 0; column < 32; column++)
        {
            REQUIRE(fixture.control->uploadedPixels[row * 32 + column] == static_cast<std::byte>(row + 1));
        }
    }
}

TEST_CASE("Only the last row's actual pixels are required, never its trailing padding")
{
    Fixture fixture;
    fixture.Start();
    const std::vector<std::byte> minimal(152, std::byte{0x48});
    const auto epoch = fixture.window->GetSnapshot().timing.presentationEpoch;
    REQUIRE(fixture.window->SubmitFrame({std::span(minimal).first(151), 8, 4, 40, 1, epoch}).code == PresentationErrorCode::InvalidFrame);
    REQUIRE(fixture.window->GetSnapshot().submittedFrames == 0);
    REQUIRE(static_cast<bool>(fixture.window->SubmitFrame({minimal, 8, 4, 40, 2, epoch})));
    fixture.Permit();
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().totalPresentCalls == 1;
        }));
    fixture.window->Stop();
    REQUIRE(std::all_of(fixture.control->uploadedPixels.begin(), fixture.control->uploadedPixels.end(),
                        [](const auto pixel)
                        {
                            return pixel == std::byte{0x48};
                        }));
}

TEST_CASE("Resize pauses rather than scaling and old epoch frames cannot re-enter")
{
    Fixture fixture;
    fixture.Start();
    const auto oldEpoch = fixture.window->GetSnapshot().timing.presentationEpoch;
    REQUIRE(static_cast<bool>(fixture.Submit()));
    fixture.Change(
        [](FakeControl& state)
        {
            state.environment.clientWidth = 9;
        });
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().state == WindowState::Paused;
        }));
    REQUIRE(fixture.window->GetSnapshot().discardedEpochFrames == 1);
    REQUIRE(fixture.window->GetSnapshot().totalPresentCalls == 0);
    REQUIRE(fixture.window->SubmitFrame({fixture.pixels, 8, 4, 32, 2, oldEpoch}).code == PresentationErrorCode::EpochMismatch);
    REQUIRE(fixture.Submit(3).code == PresentationErrorCode::Paused);
    fixture.Change(
        [](FakeControl& state)
        {
            state.environment.clientWidth = 8;
        });
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().state == WindowState::Running;
        }));
    REQUIRE(fixture.window->GetSnapshot().timing.presentationEpoch == oldEpoch + 2);
    REQUIRE(static_cast<bool>(fixture.Submit(4)));
    fixture.Permit();
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().totalPresentCalls == 1;
        }));
    fixture.window->Stop();
    REQUIRE(fixture.control->reconfigurations == 2);
}

TEST_CASE("Monitor, DPI and mode serial transitions start distinct epochs")
{
    Fixture fixture;
    fixture.Start();
    auto epoch = fixture.window->GetSnapshot().timing.presentationEpoch;
    fixture.Change(
        [](FakeControl& state)
        {
            state.environment.monitorIdentity = 2;
        });
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().timing.presentationEpoch > epoch;
        }));
    REQUIRE(fixture.window->GetSnapshot().timing.epochReason == pbpresenttiming::EpochReason::MonitorChanged);
    epoch = fixture.window->GetSnapshot().timing.presentationEpoch;
    fixture.Change(
        [](FakeControl& state)
        {
            state.environment.dpi = 144;
            state.environment.dpiChangeSerial++;
        });
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().timing.presentationEpoch > epoch;
        }));
    REQUIRE(fixture.window->GetSnapshot().timing.epochReason == pbpresenttiming::EpochReason::DpiChanged);
    epoch = fixture.window->GetSnapshot().timing.presentationEpoch;
    fixture.Change(
        [](FakeControl& state)
        {
            state.environment.modeChangeSerial++;
        });
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().timing.presentationEpoch > epoch;
        }));
    REQUIRE(fixture.window->GetSnapshot().timing.epochReason == pbpresenttiming::EpochReason::DisplayModeChanged);
    fixture.window->Stop();
}

TEST_CASE("Environment changes during upload discard the old raster without wasting a same-chain permit")
{
    Fixture fixture;
    fixture.control->holdUpload = true;
    fixture.Start();
    REQUIRE(static_cast<bool>(fixture.Submit()));
    fixture.Permit();
    REQUIRE(WaitUntil(
        [&]
        {
            const std::lock_guard lock(fixture.control->mutex);
            return fixture.control->uploadEntered;
        }));
    fixture.Change(
        [](FakeControl& state)
        {
            state.environment.modeChangeSerial++;
            state.holdUpload = false;
        });
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().discardedEpochFrames == 1;
        }));
    REQUIRE(fixture.window->GetSnapshot().totalPresentCalls == 0);
    REQUIRE(static_cast<bool>(fixture.Submit(2)));
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().totalPresentCalls == 1;
        }));
    fixture.window->Stop();
    REQUIRE(fixture.control->consumedPermits == 1);
}

TEST_CASE("Statistics disjoint resets timing but never recreates the graphics backend")
{
    Fixture fixture;
    fixture.Start();
    fixture.Change(
        [](FakeControl& state)
        {
            state.statistics.push_back({pbpresenttiming::StatisticsStatus::Disjoint, {}, -1});
        });
    REQUIRE(static_cast<bool>(fixture.Submit()));
    fixture.Permit();
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().timing.presentationEpoch == 2;
        }));
    REQUIRE(static_cast<bool>(fixture.Submit(2)));
    fixture.Permit();
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().totalPresentCalls == 2;
        }));
    fixture.window->Stop();
    REQUIRE(fixture.control->reconfigurations == 0);
    REQUIRE(fixture.control->diagnostics.swapChainGeneration == 1);
}

TEST_CASE("An acquired permit belongs to one swap chain and cannot cross an adapter rebuild")
{
    Fixture fixture;
    fixture.control->holdUpload = true;
    fixture.Start();
    REQUIRE(static_cast<bool>(fixture.Submit()));
    fixture.Permit();
    REQUIRE(WaitUntil(
        [&]
        {
            const std::lock_guard lock(fixture.control->mutex);
            return fixture.control->uploadEntered;
        }));
    fixture.Change(
        [](FakeControl& state)
        {
            state.environment.adapterLuidLow = 42;
            state.holdUpload = false;
        });
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().discardedEpochFrames == 1;
        }));
    REQUIRE(fixture.window->GetSnapshot().swapChainGeneration == 2);
    REQUIRE(static_cast<bool>(fixture.Submit(2)));
    std::this_thread::sleep_for(20ms);
    REQUIRE(fixture.window->GetSnapshot().totalPresentCalls == 0);
    fixture.Permit();
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().totalPresentCalls == 1;
        }));
    fixture.window->Stop();
    REQUIRE(fixture.control->consumedPermits == 2);
    REQUIRE(fixture.control->wrongOwnerCalls == 0);
}

TEST_CASE("Monitor straddling and temporarily missing adapters pause admission without speculative GPU allocation")
{
    Fixture fixture;
    fixture.Start();
    const auto before = fixture.window->GetSnapshot();
    REQUIRE(static_cast<bool>(fixture.Submit()));
    fixture.Change(
        [](FakeControl& state)
        {
            state.environment.singleMonitor = false;
            state.environment.adapterLuidLow = 42;
        });
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().state == WindowState::Paused;
        }));
    REQUIRE(fixture.Submit().code == PresentationErrorCode::Paused);
    REQUIRE(fixture.window->GetSnapshot().swapChainGeneration == before.swapChainGeneration);
    fixture.Change(
        [](FakeControl& state)
        {
            state.environment.singleMonitor = true;
            state.environment.adapterAvailable = false;
        });
    REQUIRE(WaitUntil(
        [&]
        {
            return !fixture.window->GetSnapshot().environment.adapterAvailable;
        }));
    REQUIRE(fixture.Submit().code == PresentationErrorCode::Paused);
    REQUIRE(fixture.window->GetSnapshot().totalPresentCalls == 0);
    fixture.Change(
        [](FakeControl& state)
        {
            state.environment.adapterAvailable = true;
        });
    REQUIRE(WaitUntil(
        [&]
        {
            return fixture.window->GetSnapshot().candidateContractSatisfied;
        }));
    REQUIRE(fixture.window->GetSnapshot().swapChainGeneration == 2);
    REQUIRE(fixture.window->GetSnapshot().discardedEpochFrames == 1);
    fixture.window->Stop();
}

TEST_CASE("Create and reconfigure failures clean up through the owner without publishing partial success")
{
    SECTION("Initialize failure")
    {
        Fixture fixture;
        fixture.control->initializeStatus = PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Device, -55);
        auto result = DataWindowTestAccess::Create(fixture.config, std::make_unique<FakeBackend>(fixture.control));
        REQUIRE_FALSE(static_cast<bool>(result));
        REQUIRE(result.Error() == fixture.control->initializeStatus);
        REQUIRE(fixture.control->shutdowns == 1);
        REQUIRE(fixture.control->wrongOwnerCalls == 0);
    }
    SECTION("Contract readback failure")
    {
        Fixture fixture;
        fixture.control->corruptContract = true;
        auto result = DataWindowTestAccess::Create(fixture.config, std::make_unique<FakeBackend>(fixture.control));
        REQUIRE_FALSE(static_cast<bool>(result));
        REQUIRE(result.Error().code == PresentationErrorCode::ContractViolation);
        REQUIRE(fixture.control->shutdowns == 1);
    }
    SECTION("Canonical client with a mismatched back buffer cannot publish a successful Create")
    {
        Fixture fixture;
        fixture.control->mismatchedBuffer = true;
        auto result = DataWindowTestAccess::Create(fixture.config, std::make_unique<FakeBackend>(fixture.control));
        REQUIRE_FALSE(static_cast<bool>(result));
        REQUIRE(result.Error().code == PresentationErrorCode::ContractViolation);
        REQUIRE(fixture.control->shutdowns == 1);
        REQUIRE(fixture.control->presents == 0);
    }
    SECTION("Resize failure")
    {
        Fixture fixture;
        fixture.Start();
        fixture.Change(
            [](FakeControl& state)
            {
                state.reconfigureStatus = PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Resize, -75);
                state.environment.clientHeight++;
            });
        REQUIRE(WaitUntil(
            [&]
            {
                return fixture.window->GetSnapshot().state == WindowState::Failed;
            }));
        REQUIRE(fixture.window->GetSnapshot().error.nativeError == -75);
        fixture.window->Stop();
        REQUIRE(fixture.control->shutdowns == 1);
    }
}

TEST_CASE("WAIT_FAILED, timeout, device loss and upload failures are terminal and retain exact diagnostics")
{
    SECTION("Wait failure")
    {
        Fixture fixture;
        fixture.Start();
        REQUIRE(static_cast<bool>(fixture.Submit()));
        fixture.Change(
            [](FakeControl& state)
            {
                state.failWait = true;
            });
        REQUIRE(WaitUntil(
            [&]
            {
                return fixture.window->GetSnapshot().state == WindowState::Failed;
            }));
        REQUIRE(fixture.window->GetSnapshot().error.nativeError == 123);
        fixture.window->Stop();
        REQUIRE(fixture.control->presents == 0);
    }
    SECTION("Timeout")
    {
        Fixture fixture;
        fixture.config.waitTimeoutMilliseconds = 20;
        fixture.Start();
        REQUIRE(static_cast<bool>(fixture.Submit()));
        REQUIRE(WaitUntil(
            [&]
            {
                return fixture.window->GetSnapshot().state == WindowState::Failed;
            }));
        REQUIRE(fixture.window->GetSnapshot().error.code == PresentationErrorCode::Timeout);
    }
    SECTION("A failed QPC cannot turn the frame-wait deadline into an infinite loop")
    {
        Fixture fixture;
        fixture.config.waitTimeoutMilliseconds = 20;
        fixture.control->failQpcOnWake = true;
        fixture.Start();
        REQUIRE(static_cast<bool>(fixture.Submit()));
        REQUIRE(WaitUntil(
            [&]
            {
                return fixture.window->GetSnapshot().state == WindowState::Failed;
            }));
        const auto snapshot = fixture.window->GetSnapshot();
        REQUIRE(snapshot.error == PresentationStatus::Failure(PresentationErrorCode::Timeout, PresentationStage::Wait));
        REQUIRE_FALSE(snapshot.timing.presentCallFps);
        REQUIRE_FALSE(snapshot.timing.presentedVisualFps);
        fixture.window->Stop();
        REQUIRE(fixture.control->presents == 0);
        REQUIRE(fixture.control->shutdowns == 1);
    }
    SECTION("Device removed")
    {
        Fixture fixture;
        fixture.control->presentStatus = PresentationStatus::Failure(PresentationErrorCode::DeviceLost, PresentationStage::Present, -99);
        fixture.control->presentOutcome = pbpresenttiming::PresentOutcome::Failure;
        fixture.Start();
        REQUIRE(static_cast<bool>(fixture.Submit()));
        fixture.Permit();
        REQUIRE(WaitUntil(
            [&]
            {
                return fixture.window->GetSnapshot().state == WindowState::Failed;
            }));
        REQUIRE(fixture.window->GetSnapshot().error.nativeError == -99);
        REQUIRE(fixture.window->GetSnapshot().timing.epochReason == pbpresenttiming::EpochReason::DeviceLost);
    }
    SECTION("Device removed detected by idle statistics query")
    {
        Fixture fixture;
        fixture.control->statistics.push_back({pbpresenttiming::StatisticsStatus::Error,
                                               {},
                                               -97,
                                               PresentationStatus::Failure(PresentationErrorCode::DeviceLost, PresentationStage::Statistics, -98)});
        fixture.Start();
        REQUIRE(WaitUntil(
            [&]
            {
                return fixture.window->GetSnapshot().state == WindowState::Failed;
            }));
        const auto snapshot = fixture.window->GetSnapshot();
        REQUIRE(snapshot.error.nativeError == -98);
        REQUIRE(snapshot.timing.lastStatisticsNativeStatus == -97);
        REQUIRE(snapshot.timing.epochReason == pbpresenttiming::EpochReason::DeviceLost);
        REQUIRE(snapshot.timing.state == pbpresenttiming::TimingState::Failed);
        REQUIRE(snapshot.totalPresentCalls == 0);
        REQUIRE(snapshot.swapChainGeneration == 1);
    }
    SECTION("Upload failure")
    {
        Fixture fixture;
        fixture.control->uploadStatus = PresentationStatus::Failure(PresentationErrorCode::OutOfMemory, PresentationStage::Upload, -44);
        fixture.Start();
        REQUIRE(static_cast<bool>(fixture.Submit()));
        fixture.Permit();
        REQUIRE(WaitUntil(
            [&]
            {
                return fixture.window->GetSnapshot().state == WindowState::Failed;
            }));
        REQUIRE(fixture.window->GetSnapshot().totalPresentCalls == 0);
        REQUIRE(fixture.window->GetSnapshot().error.nativeError == -44);
    }
}

TEST_CASE("Stop cancels a resize GPU drain without turning cancellation into a device failure")
{
    Fixture fixture;
    fixture.control->holdReconfigure = true;
    fixture.Start();
    REQUIRE(static_cast<bool>(fixture.Submit()));
    fixture.Change(
        [](FakeControl& state)
        {
            state.environment.clientWidth--;
        });
    REQUIRE(WaitUntil(
        [&]
        {
            const std::lock_guard lock(fixture.control->mutex);
            return fixture.control->reconfigureEntered;
        }));
    REQUIRE(fixture.window->GetSnapshot().state == WindowState::Paused);
    fixture.window->Stop();
    REQUIRE(fixture.window->GetSnapshot().state == WindowState::Stopped);
    REQUIRE(static_cast<bool>(fixture.window->GetSnapshot().error));
    REQUIRE(fixture.control->shutdowns == 1);
    REQUIRE(fixture.control->presents == 0);
    REQUIRE(fixture.control->wrongOwnerCalls == 0);
}

TEST_CASE("Stop wakes waits, cancels in-flight work and is safe under concurrent calls")
{
    for (const bool duringUpload : {false, true})
    {
        Fixture fixture;
        fixture.control->holdUpload = duringUpload;
        fixture.Start();
        REQUIRE(static_cast<bool>(fixture.Submit()));
        if (duringUpload)
        {
            fixture.Permit();
            REQUIRE(WaitUntil(
                [&]
                {
                    const std::lock_guard lock(fixture.control->mutex);
                    return fixture.control->uploadEntered;
                }));
        }
        std::thread first(
            [&]
            {
                fixture.window->Stop();
            });
        std::thread second(
            [&]
            {
                fixture.window->Stop();
            });
        first.join();
        second.join();
        REQUIRE(fixture.window->GetSnapshot().state == WindowState::Stopped);
        REQUIRE_FALSE(fixture.window->GetSnapshot().inFlightFrame);
        REQUIRE(fixture.control->shutdowns == 1);
        REQUIRE(fixture.control->wrongOwnerCalls == 0);
        REQUIRE(fixture.Submit().code == PresentationErrorCode::NotRunning);
    }
}

TEST_CASE("Concurrent producers and snapshots do not race the bounded mailbox or shutdown")
{
    Fixture fixture;
    fixture.Start();
    std::atomic<unsigned int> unexpectedErrors = 0;
    std::vector<std::thread> producers;
    for (unsigned int producer = 0; producer < 3; producer++)
    {
        producers.emplace_back(
            [&, producer]
            {
                for (unsigned int index = 0; index < 500; index++)
                {
                    const auto status = fixture.Submit(static_cast<std::uint64_t>(producer) * 1000 + index);
                    if (!status && status.code != PresentationErrorCode::NotRunning && status.code != PresentationErrorCode::EpochMismatch)
                    {
                        unexpectedErrors.fetch_add(1);
                    }
                    static_cast<void>(fixture.window->GetSnapshot());
                }
            });
    }
    std::this_thread::sleep_for(2ms);
    fixture.window->Stop();
    for (auto& producer : producers)
    {
        producer.join();
    }
    REQUIRE(unexpectedErrors.load() == 0);
    REQUIRE(fixture.control->wrongOwnerCalls == 0);
    REQUIRE(fixture.control->shutdowns == 1);
    REQUIRE(fixture.window->GetSnapshot().totalPresentCalls == 0);
}

TEST_CASE("Telemetry uses explicit unavailable values and escapes bounded native names")
{
    DataWindowSnapshot snapshot;
    snapshot.environment.displayName[0] = L'"';
    snapshot.environment.displayName[1] = L'\\';
    snapshot.environment.displayName[2] = L'\n';
    snapshot.environment.adapterDescription[0] = L'显';
    snapshot.timing.presentCallFps = std::numeric_limits<double>::infinity();
    std::ostringstream output;
    WriteDataWindowSnapshotJson(output, snapshot);
    const auto text = output.str();
    REQUIRE(text.find("\"PresentCallFPS\":null") != std::string::npos);
    REQUIRE(text.find("\"PresentedVisualFPS\":null") != std::string::npos);
    REQUIRE(text.find("\\u0022\\u005c\\u000a") != std::string::npos);
    REQUIRE(text.find("\\u663e") != std::string::npos);
    REQUIRE(text.find("UniqueVisualFPS") == std::string::npos);
    REQUIRE(text.find("not-capture-certification") != std::string::npos);
    REQUIRE(text.size() < 8192);
}

TEST_CASE("JSON diagnostics are independent of and do not modify the caller's numeric formatting")
{
    DataWindowSnapshot snapshot;
    snapshot.timing.presentationEpoch = 31;
    snapshot.timing.presentCallFps = 12.5;
    snapshot.timing.lastPresent = pbpresenttiming::PresentSample{17, 100, 120, pbpresenttiming::PresentOutcome::Success, 21};
    std::ostringstream output;
    output << std::hex << std::showbase << std::scientific << std::setprecision(2);
    const auto flags = output.flags();
    WriteDataWindowSnapshotJson(output, snapshot);
    REQUIRE(output.flags() == flags);
    REQUIRE(output.precision() == 2);
    const auto text = output.str();
    REQUIRE(text.find("\"PresentationEpoch\":31") != std::string::npos);
    REQUIRE(text.find("\"PresentCallFPS\":12.5") != std::string::npos);
    REQUIRE(text.find("\"FrameSequence\":17") != std::string::npos);
    REQUIRE(text.find("\"presentId\":21") != std::string::npos);
    REQUIRE(text.find("0x") == std::string::npos);
}
