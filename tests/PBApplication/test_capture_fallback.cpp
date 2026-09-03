#include "decoder_capture_controller.h"
#include "local_desktop_runtime.h"
#include "run_report.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <vector>

namespace
{
using namespace pbapp;
using namespace pbcapturenormalize;

class MockConsumer final : public ScreenCaptureConsumer
{
public:
    CaptureStatus DomainStarted(const ScreenCaptureDomain&, const CaptureEnvironment&, ID3D11Device*) override
    {
        return {};
    }
    void DomainInvalidated(const ScreenCaptureDomain&) noexcept override
    {
    }
    CaptureStatus Submit(const ScreenCaptureFrame&, ID3D11DeviceContext*) override
    {
        return {};
    }
};

struct Script
{
    CaptureStatus startStatus;
    CaptureSnapshot capture;
    CaptureNormalizeSnapshot normalized;
    std::vector<std::string>* events = nullptr;
    std::string name;
    std::uint64_t initialEpoch = 0;
    std::optional<std::int64_t> minUpdateInterval;
    std::weak_ptr<ScreenCaptureConsumer> consumer;
    bool unsafeDrain = false;
    bool stopped = false;
};

class MockSession final : public DecoderCaptureSession
{
public:
    explicit MockSession(std::shared_ptr<Script> script) : script_(std::move(script))
    {
    }
    CaptureStatus Start(const CaptureNormalizeConfig& config, std::shared_ptr<ScreenCaptureConsumer> consumer) noexcept override
    {
        script_->events->push_back(script_->name + ":start");
        script_->consumer = consumer;
        script_->initialEpoch = config.capture.initialCaptureEpoch;
        script_->minUpdateInterval = config.capture.minUpdateInterval100ns;
        script_->capture.captureEpoch = config.capture.initialCaptureEpoch;
        script_->capture.state = script_->startStatus ? CaptureState::Running : CaptureState::Failed;
        script_->capture.error = script_->startStatus;
        script_->normalized.enabled = true;
        script_->normalized.active = static_cast<bool>(script_->startStatus);
        script_->normalized.domain.captureEpoch = config.capture.initialCaptureEpoch;
        script_->normalized.domain.sourceId[0] = script_->name == "wgc" ? std::byte{1} : std::byte{2};
        return script_->startStatus;
    }
    CaptureSnapshot GetSnapshot() const noexcept override
    {
        return script_->capture;
    }
    CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept override
    {
        return script_->normalized;
    }
    void RequestStop() noexcept override
    {
    }
    CaptureStatus Stop() noexcept override
    {
        if (!script_->stopped)
        {
            script_->events->push_back(script_->name + ":invalidate");
            script_->normalized.active = false;
            script_->events->push_back(script_->name + ":drain");
            script_->capture.shutdownComplete = !script_->unsafeDrain;
            script_->capture.deferredCleanup = script_->unsafeDrain;
            script_->stopped = true;
        }
        return script_->capture.error;
    }

private:
    std::shared_ptr<Script> script_;
};

struct Fixture
{
    Fixture()
    {
        wgc->events = &events;
        wgc->name = "wgc";
        dxgi->events = &events;
        dxgi->name = "dxgi";
        config.capture.region = {reinterpret_cast<HMONITOR>(std::uintptr_t{1}), {0, 0, 100, 80}, {0, 0, 100, 80}, 96, 96, DXGI_MODE_ROTATION_IDENTITY};
    }
    DecoderCaptureController Make(const CaptureBackend requested = CaptureBackend::Auto)
    {
        return DecoderCaptureController(
            requested, config,
            [&](std::shared_ptr<ScreenCaptureConsumer>& consumer)
            {
                if (!wgc->consumer.expired())
                {
                    oldConsumerReleased = false;
                }
                consumersCreated++;
                consumer = std::make_shared<MockConsumer>();
                return CaptureStatus{};
            },
            [&](const CaptureBackend backend)
            {
                return std::make_unique<MockSession>(backend == CaptureBackend::Wgc ? wgc : dxgi);
            },
            [&]
            {
                clockSawRetirement = wgc->stopped;
                return nowUnixMilliseconds;
            });
    }
    std::vector<std::string> events;
    std::shared_ptr<Script> wgc = std::make_shared<Script>();
    std::shared_ptr<Script> dxgi = std::make_shared<Script>();
    CaptureNormalizeConfig config;
    std::uint32_t consumersCreated = 0;
    bool oldConsumerReleased = true;
    bool clockSawRetirement = false;
    std::uint64_t nowUnixMilliseconds = 0;
};

ScreenCaptureFrameMetadata Frame(const Script& script)
{
    ScreenCaptureFrameMetadata frame;
    frame.domain = script.normalized.domain;
    frame.backend = script.name == "wgc" ? CaptureBackendKind::Wgc : CaptureBackendKind::Dxgi;
    frame.captureObservation = 1;
    frame.timestamp.monotonic100ns = 10000000;
    frame.timestamp.arrivalQpc100ns = 10000000;
    return frame;
}
} // namespace

TEST_CASE("G14 Auto defaults preserve explicit enum compatibility and reserve both native backends", "[application][g14]")
{
    STATIC_REQUIRE(static_cast<unsigned int>(CaptureBackend::Wgc) == 0);
    STATIC_REQUIRE(static_cast<unsigned int>(CaptureBackend::Dxgi) == 1);
    REQUIRE(DecoderConfig{}.captureBackend == CaptureBackend::Auto);
    REQUIRE(DecoderSnapshot{}.requestedBackend == CaptureBackend::Auto);
    REQUIRE(std::string(GetCaptureBackendName(CaptureBackend::Auto)) == "Auto");
    Fixture fixture;
    REQUIRE(ValidateDecoderCapturePolicy(CaptureBackend::Auto, fixture.config.capture));
    REQUIRE(ValidateDecoderCapturePolicy(CaptureBackend::Wgc, fixture.config.capture));
    REQUIRE(ValidateDecoderCapturePolicy(CaptureBackend::Dxgi, fixture.config.capture));
    REQUIRE_FALSE(ValidateDecoderCapturePolicy(static_cast<CaptureBackend>(255), fixture.config.capture));
    fixture.config.capture.maximumCaptureBytes = 1;
    REQUIRE_FALSE(ValidateDecoderCapturePolicy(CaptureBackend::Auto, fixture.config.capture));
}

TEST_CASE("G14 healthy WGC waiting erasures timeouts and display rebuilds never trigger fallback or FPS throttling", "[application][g14]")
{
    Fixture fixture;
    fixture.config.capture.minUpdateInterval100ns = 10000000;
    auto controller = fixture.Make();
    fixture.nowUnixMilliseconds = 1000;
    REQUIRE(controller.Start());
    REQUIRE(fixture.wgc->minUpdateInterval == 0);
    for (const auto state : {CaptureState::Running, CaptureState::WaitingForEnvironment, CaptureState::Recreating, CaptureState::Draining})
    {
        fixture.wgc->capture.state = state;
        fixture.wgc->capture.acquireTimeouts++;
        fixture.wgc->capture.expiredFrames++;
        fixture.wgc->capture.pointerOnlyFrames++;
        fixture.wgc->capture.deviceRecoveries = 1;
        fixture.nowUnixMilliseconds = 2000;
        REQUIRE(controller.Poll());
    }
    REQUIRE(fixture.consumersCreated == 1);
    REQUIRE(fixture.events == std::vector<std::string>{"wgc:start"});
    DecoderSnapshot snapshot;
    controller.ApplyBinding(snapshot);
    REQUIRE(snapshot.actualBackend == CaptureBackend::Wgc);
    REQUIRE(snapshot.captureBackendAttempts == 1);
    REQUIRE(snapshot.captureFallbackReason == "None");
    REQUIRE_FALSE(snapshot.captureFallbackUnixMilliseconds);
    REQUIRE(controller.Stop());
    fixture.wgc->capture.state = CaptureState::Failed;
    fixture.wgc->capture.error = CaptureStatus::Failure(CaptureError::AccessLost, CaptureStage::Session);
    fixture.nowUnixMilliseconds = 3000;
    REQUIRE(controller.Poll());
    REQUIRE(fixture.consumersCreated == 1);
}

TEST_CASE("G14 initialization failure invalidates drains and releases old consumer before one DXGI attempt", "[application][g14]")
{
    Fixture fixture;
    fixture.wgc->startStatus = CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::CaptureItem, E_NOTIMPL);
    auto controller = fixture.Make();
    fixture.nowUnixMilliseconds = 1234;
    REQUIRE(controller.Start());
    REQUIRE(fixture.events == std::vector<std::string>{"wgc:start", "wgc:invalidate", "wgc:drain", "dxgi:start"});
    REQUIRE(fixture.oldConsumerReleased);
    REQUIRE(fixture.clockSawRetirement);
    REQUIRE(fixture.consumersCreated == 2);
    REQUIRE(fixture.dxgi->initialEpoch == 2);
    REQUIRE(fixture.dxgi->minUpdateInterval == 0);
    DecoderSnapshot snapshot;
    controller.ApplyBinding(snapshot);
    REQUIRE(snapshot.requestedBackend == CaptureBackend::Auto);
    REQUIRE(snapshot.actualBackend == CaptureBackend::Dxgi);
    REQUIRE(snapshot.captureBackendAttempts == 2);
    REQUIRE(snapshot.captureFallbackReason == "InitializationFailure");
    REQUIRE(snapshot.captureFallbackUnixMilliseconds == 1234);
    REQUIRE(snapshot.captureFallbackEpoch == 2);
    REQUIRE(snapshot.backendReason.find("Unsupported") != std::string::npos);
    const auto report = BuildDecoderRunReportJson({}, snapshot);
    REQUIRE(report.find("\"requestedBackend\":\"Auto\"") != std::string::npos);
    REQUIRE(report.find("\"actualBackend\":\"DXGI Desktop Duplication\"") != std::string::npos);
    REQUIRE(report.find("\"captureFallbackUnixMilliseconds\":1234") != std::string::npos);
    REQUIRE(report.find("\"captureFallbackEpoch\":2") != std::string::npos);
    REQUIRE(BuildDecoderDiagnostics(snapshot).find("InitializationFailure epoch=2 unixMs=1234") != std::string::npos);
}

TEST_CASE("G14 only typed AccessLost or failed device rebuild switches an established WGC domain", "[application][g14]")
{
    for (const bool rebuildFailed : {false, true})
    {
        Fixture fixture;
        auto controller = fixture.Make();
        fixture.nowUnixMilliseconds = 1000;
        REQUIRE(controller.Start());
        const auto oldFrame = Frame(*fixture.wgc);
        REQUIRE(controller.CanAdmit(oldFrame, 10000000));
        fixture.wgc->capture.captureEpoch = 7;
        fixture.wgc->capture.state = CaptureState::Failed;
        fixture.wgc->capture.error = CaptureStatus::Failure(rebuildFailed ? CaptureError::NativeFailure : CaptureError::AccessLost, CaptureStage::Device);
        fixture.wgc->capture.deviceRebuildFailed = rebuildFailed;
        fixture.nowUnixMilliseconds = 2000;
        REQUIRE(controller.Poll());
        REQUIRE(fixture.dxgi->initialEpoch == 8);
        REQUIRE_FALSE(controller.CanAdmit(oldFrame, 10000001));
        auto forgedFrame = oldFrame;
        forgedFrame.domain.captureEpoch = 8;
        forgedFrame.backend = CaptureBackendKind::Dxgi;
        REQUIRE_FALSE(controller.CanAdmit(forgedFrame, 10000001));
        REQUIRE(controller.CanAdmit(Frame(*fixture.dxgi), 10000001));
        DecoderSnapshot snapshot;
        snapshot.verifiedRawBytes = 8192;
        snapshot.descriptorKnown = true;
        controller.ApplyBinding(snapshot);
        REQUIRE(snapshot.verifiedRawBytes == 8192);
        REQUIRE(snapshot.descriptorKnown);
        REQUIRE(snapshot.captureAdmissionDrops == 2);
        REQUIRE(snapshot.captureFallbackReason == (rebuildFailed ? "DeviceRebuildFailure" : "AccessLost"));
    }
}

TEST_CASE("G14 generic failures and old device recovery counters are not fallback authority", "[application][g14]")
{
    for (const auto error : {CaptureError::NativeFailure, CaptureError::ConsumerFailure, CaptureError::Timeout, CaptureError::RegionChanged,
                             CaptureError::ResourceLimit, CaptureError::DeviceLost})
    {
        Fixture fixture;
        auto controller = fixture.Make();
        fixture.nowUnixMilliseconds = 1000;
        REQUIRE(controller.Start());
        fixture.wgc->capture.state = CaptureState::Failed;
        fixture.wgc->capture.error = CaptureStatus::Failure(error, CaptureStage::Consumer);
        fixture.wgc->capture.deviceRecoveries = 1;
        fixture.nowUnixMilliseconds = 2000;
        REQUIRE(controller.Poll().code == error);
        REQUIRE(fixture.consumersCreated == 1);
        fixture.nowUnixMilliseconds = 3000;
        REQUIRE(controller.Poll().code == error);
        REQUIRE(fixture.consumersCreated == 1);
    }
}

TEST_CASE("G14 missing retirement proof cleanup errors and epoch exhaustion fail closed before replacement", "[application][g14]")
{
    for (int failure = 0; failure < 6; failure++)
    {
        Fixture fixture;
        auto controller = fixture.Make();
        fixture.nowUnixMilliseconds = 1000;
        REQUIRE(controller.Start());
        fixture.wgc->capture.state = CaptureState::Failed;
        fixture.wgc->capture.error = CaptureStatus::Failure(CaptureError::AccessLost, CaptureStage::CaptureItem);
        if (failure == 0)
        {
            fixture.wgc->unsafeDrain = true;
        }
        if (failure == 1)
        {
            fixture.wgc->capture.liveFrameLeases = 1;
        }
        if (failure == 2)
        {
            fixture.wgc->capture.busyRoiTextures = 1;
        }
        if (failure == 3)
        {
            fixture.wgc->capture.shutdownStatus = CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Shutdown);
        }
        if (failure == 4)
        {
            fixture.wgc->capture.captureEpoch = (std::numeric_limits<std::uint64_t>::max)();
        }
        if (failure == 5)
        {
            fixture.wgc->capture.captureEpoch = (std::numeric_limits<std::uint64_t>::max)() - 1;
        }
        fixture.nowUnixMilliseconds = 2000;
        REQUIRE_FALSE(controller.Poll());
        REQUIRE(fixture.consumersCreated == 1);
        DecoderSnapshot snapshot;
        controller.ApplyBinding(snapshot);
        REQUIRE(snapshot.captureBackendAttempts == 1);
        REQUIRE_FALSE(snapshot.captureFallbackUnixMilliseconds);
    }
    Fixture fixture;
    fixture.wgc->startStatus = CaptureStatus::Failure(CaptureError::AccessLost, CaptureStage::Session);
    fixture.wgc->unsafeDrain = true;
    auto controller = fixture.Make();
    fixture.nowUnixMilliseconds = 1000;
    REQUIRE_FALSE(controller.Start());
    REQUIRE(fixture.consumersCreated == 1);
}

TEST_CASE("G14 Auto exhausts both backends once while explicit legacy requests never switch", "[application][g14]")
{
    Fixture fixture;
    fixture.wgc->startStatus = CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::Session);
    fixture.dxgi->startStatus = CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Session, E_FAIL);
    auto controller = fixture.Make();
    fixture.nowUnixMilliseconds = 1000;
    REQUIRE(controller.Start() == fixture.dxgi->startStatus);
    for (int poll = 0; poll < 3; poll++)
    {
        fixture.nowUnixMilliseconds = 2000;
        REQUIRE(controller.Poll() == fixture.dxgi->startStatus);
    }
    REQUIRE(fixture.consumersCreated == 2);
    DecoderSnapshot snapshot;
    controller.ApplyBinding(snapshot);
    REQUIRE_FALSE(snapshot.actualBackend);
    REQUIRE(snapshot.backendReason.find("WGC=") != std::string::npos);
    REQUIRE(snapshot.backendReason.find("DXGI=") != std::string::npos);
    for (const auto backend : {CaptureBackend::Wgc, CaptureBackend::Dxgi})
    {
        Fixture explicitFixture;
        explicitFixture.wgc->startStatus = fixture.wgc->startStatus;
        explicitFixture.dxgi->startStatus = fixture.dxgi->startStatus;
        auto explicitController = explicitFixture.Make(backend);
        explicitFixture.nowUnixMilliseconds = 1000;
        REQUIRE_FALSE(explicitController.Start());
        explicitFixture.nowUnixMilliseconds = 2000;
        REQUIRE_FALSE(explicitController.Poll());
        REQUIRE(explicitFixture.consumersCreated == 1);
    }
}

TEST_CASE("G14 stale invalidated and cross-source frames are rejected immediately before Receiver admission", "[application][g14]")
{
    Fixture fixture;
    auto controller = fixture.Make();
    fixture.nowUnixMilliseconds = 1000;
    REQUIRE(controller.Start());
    const auto frame = Frame(*fixture.wgc);
    REQUIRE(controller.CanAdmit(frame, 10000000));
    REQUIRE_FALSE(controller.CanAdmit(frame, 12500001));
    auto timestampLie = frame;
    timestampLie.timestamp.monotonic100ns = 13000000;
    REQUIRE_FALSE(controller.CanAdmit(timestampLie, 13000000));
    fixture.wgc->normalized.active = false;
    REQUIRE_FALSE(controller.CanAdmit(frame, 10000001));
    fixture.wgc->normalized.active = true;
    fixture.wgc->normalized.domain.captureEpoch++;
    REQUIRE_FALSE(controller.CanAdmit(frame, 10000001));
    REQUIRE(controller.Stop());
    REQUIRE_FALSE(controller.CanAdmit(Frame(*fixture.wgc), 10000001));
}

TEST_CASE("G14 Receiver visual identity tracking suppresses duplicate and reordered sequences in fixed state", "[application][g14]")
{
    VisualIdentityTracker tracker;
    REQUIRE(tracker.Observe(40, 1, 10000000, 9) == VisualIdentityDisposition::Unique);
    REQUIRE(tracker.Observe(40, 1, 10010000, 9) == VisualIdentityDisposition::Duplicate);
    REQUIRE(tracker.Observe(42, 1, 10020000, 9) == VisualIdentityDisposition::Unique);
    REQUIRE(tracker.Observe(41, 1, 10030000, 9) == VisualIdentityDisposition::Reordered);
    REQUIRE(tracker.Observe(42, 1, 10040000, 9) == VisualIdentityDisposition::Duplicate);
    REQUIRE(tracker.Observe(43, 1, 10050000, 9) == VisualIdentityDisposition::Unique);
    const auto snapshot = tracker.GetSnapshot();
    REQUIRE(snapshot.uniqueFrames == 3);
    REQUIRE(snapshot.duplicateFrames == 2);
    REQUIRE(snapshot.reorderedFrames == 1);
    REQUIRE(snapshot.gapEvents == 1);
    REQUIRE(snapshot.skippedSequences == 1);
    REQUIRE(tracker.Observe(43, 2, 10060000, 9) == VisualIdentityDisposition::Unique);
    REQUIRE(tracker.Observe(43, 2, 10070000, 9) == VisualIdentityDisposition::Duplicate);
}

TEST_CASE("G14 deferred owner shutdown is terminal even when the last state is Draining", "[application][g14]")
{
    Fixture fixture;
    auto controller = fixture.Make();
    fixture.nowUnixMilliseconds = 1000;
    REQUIRE(controller.Start());
    fixture.wgc->capture.state = CaptureState::Draining;
    fixture.wgc->capture.deferredCleanup = true;
    fixture.wgc->capture.error = CaptureStatus::Failure(CaptureError::Timeout, CaptureStage::Shutdown);
    fixture.nowUnixMilliseconds = 2000;
    REQUIRE(controller.Poll() == fixture.wgc->capture.error);
    REQUIRE(fixture.consumersCreated == 1);
    REQUIRE_FALSE(controller.CanAdmit(Frame(*fixture.wgc), 10000000));
}

TEST_CASE("G14 DXGI runtime failure never loops back to WGC or creates a third backend", "[application][g14]")
{
    Fixture fixture;
    fixture.wgc->startStatus = CaptureStatus::Failure(CaptureError::AccessLost, CaptureStage::CaptureItem);
    auto controller = fixture.Make();
    fixture.nowUnixMilliseconds = 1000;
    REQUIRE(controller.Start());
    fixture.dxgi->capture.state = CaptureState::Failed;
    fixture.dxgi->capture.error = CaptureStatus::Failure(CaptureError::AccessLost, CaptureStage::Session);
    fixture.nowUnixMilliseconds = 2000;
    REQUIRE_FALSE(controller.Poll());
    fixture.nowUnixMilliseconds = 3000;
    REQUIRE_FALSE(controller.Poll());
    REQUIRE(fixture.consumersCreated == 2);
    DecoderSnapshot snapshot;
    controller.ApplyBinding(snapshot);
    REQUIRE(snapshot.captureBackendAttempts == 2);
    REQUIRE(snapshot.captureFallbackUnixMilliseconds == 1000);
    REQUIRE(snapshot.backendReason.find("DXGI=AccessLost") != std::string::npos);
}
