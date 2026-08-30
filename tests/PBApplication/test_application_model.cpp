#include "application_model.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <string_view>
#include <thread>
#include <vector>

TEST_CASE("Encoder state machine broadcasts until an explicit idempotent stop", "[application][encoder-state]")
{
    pbapp::EncoderStateMachine state;
    REQUIRE(state.GetState() == pbapp::EncoderState::Idle);
    REQUIRE(state.Start(1) == pbapp::TransitionResult::Applied);
    REQUIRE(state.GetState() == pbapp::EncoderState::Preparing);
    REQUIRE(state.Start(2) == pbapp::TransitionResult::Rejected);
    REQUIRE(state.MarkBroadcasting(1) == pbapp::TransitionResult::Applied);
    REQUIRE(state.GetState() == pbapp::EncoderState::Broadcasting);
    REQUIRE(state.RequestStop(1) == pbapp::TransitionResult::Applied);
    REQUIRE(state.GetState() == pbapp::EncoderState::Stopping);
    REQUIRE(state.RequestStop(1) == pbapp::TransitionResult::NoChange);
    REQUIRE(state.MarkStopped(1) == pbapp::TransitionResult::Applied);
    REQUIRE(state.GetState() == pbapp::EncoderState::Stopped);
    REQUIRE(state.MarkStopped(1) == pbapp::TransitionResult::NoChange);
}

TEST_CASE("Encoder can stop during preparation and stale callbacks cannot mutate a new run", "[application][stale]")
{
    pbapp::EncoderStateMachine state;
    REQUIRE(state.Start(10) == pbapp::TransitionResult::Applied);
    REQUIRE(state.RequestStop(10) == pbapp::TransitionResult::Applied);
    REQUIRE(state.MarkStopped(10) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Start(11) == pbapp::TransitionResult::Applied);
    REQUIRE(state.MarkBroadcasting(10) == pbapp::TransitionResult::Stale);
    REQUIRE(state.Fail(10) == pbapp::TransitionResult::Stale);
    REQUIRE(state.GetState() == pbapp::EncoderState::Preparing);
    REQUIRE(state.MarkBroadcasting(11) == pbapp::TransitionResult::Applied);
}

TEST_CASE("Completing a carousel cycle never completes the Encoder", "[application][carousel]")
{
    pbapp::EncoderStateMachine state;
    REQUIRE(state.Start(1) == pbapp::TransitionResult::Applied);
    REQUIRE(state.MarkBroadcasting(1) == pbapp::TransitionResult::Applied);
    pbapp::CarouselCounter carousel;
    REQUIRE(carousel.Reset(4));
    for (std::uint32_t index = 0; index < 4; index++)
    {
        REQUIRE(carousel.Advance());
    }
    const auto firstCycle = carousel.GetSnapshot();
    REQUIRE(firstCycle.cycleCount == 1);
    REQUIRE(firstCycle.cyclePosition == 0);
    REQUIRE(state.GetState() == pbapp::EncoderState::Broadcasting);
    for (std::uint32_t index = 0; index < 4; index++)
    {
        REQUIRE(carousel.Advance());
    }
    REQUIRE(carousel.GetSnapshot().cycleCount == 2);
    REQUIRE(state.GetState() == pbapp::EncoderState::Broadcasting);
}

TEST_CASE("Decoder completion is reachable only through verifying and publishing", "[application][decoder-state]")
{
    pbapp::DecoderStateMachine state;
    REQUIRE(state.Start(21) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Start(22) == pbapp::TransitionResult::Rejected);
    REQUIRE(state.Advance(21, pbapp::DecoderState::Publishing) == pbapp::TransitionResult::Rejected);
    REQUIRE(state.Advance(21, pbapp::DecoderState::ReceivingControl) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Advance(21, pbapp::DecoderState::Receiving) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Advance(21, pbapp::DecoderState::Recovering) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Advance(21, pbapp::DecoderState::Verifying) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Advance(21, pbapp::DecoderState::Completed) == pbapp::TransitionResult::Rejected);
    REQUIRE(state.Advance(21, pbapp::DecoderState::Publishing) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Advance(21, pbapp::DecoderState::Completed) == pbapp::TransitionResult::Applied);
    REQUIRE(state.GetState() == pbapp::DecoderState::Completed);
}

TEST_CASE("Decoder stop and stale run transitions are idempotent and generation scoped", "[application][decoder-state][stale]")
{
    pbapp::DecoderStateMachine state;
    REQUIRE(state.Start(31) == pbapp::TransitionResult::Applied);
    REQUIRE(state.RequestStop(31) == pbapp::TransitionResult::Applied);
    REQUIRE(state.RequestStop(31) == pbapp::TransitionResult::NoChange);
    REQUIRE(state.MarkStopped(31) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Start(32) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Advance(31, pbapp::DecoderState::ReceivingControl) == pbapp::TransitionResult::Stale);
    REQUIRE(state.Fail(31) == pbapp::TransitionResult::Stale);
    REQUIRE(state.GetState() == pbapp::DecoderState::WaitingForBootstrap);
}

TEST_CASE("Decoder progress is exactly verified raw bytes over descriptor size", "[application][progress]")
{
    pbapp::DecoderProgressTracker progress;
    REQUIRE_FALSE(progress.GetSnapshot().descriptorKnown);
    REQUIRE(progress.BindDescriptor(1000, 100));
    REQUIRE(progress.ObserveVerifiedRawBytes(250, 1100));
    auto snapshot = progress.GetSnapshot();
    REQUIRE(snapshot.progress == 0.25);
    REQUIRE(snapshot.verifiedRawBytes == 250);
    REQUIRE(snapshot.remainingRawBytes == 750);
    REQUIRE(snapshot.instantBytesPerSecond == 250.0);
    REQUIRE(snapshot.averageBytesPerSecond == 250.0);
    REQUIRE_FALSE(snapshot.etaMilliseconds.has_value());
    REQUIRE(progress.ObserveVerifiedRawBytes(500, 2100));
    snapshot = progress.GetSnapshot();
    REQUIRE(snapshot.progress == 0.5);
    REQUIRE(snapshot.smoothedBytesPerSecond == 250.0);
    REQUIRE(snapshot.etaMilliseconds == 2000);
}

TEST_CASE("Decoder goodput smoothing uses verified mutations and handles sparse zero data", "[application][goodput][eta]")
{
    pbapp::DecoderProgressTracker progress;
    REQUIRE(progress.BindDescriptor(1000, 0));
    REQUIRE(progress.ObserveVerifiedRawBytes(100, 1000));
    REQUIRE(progress.ObserveVerifiedRawBytes(300, 2000));
    const auto smoothed = progress.GetSnapshot();
    REQUIRE(smoothed.instantBytesPerSecond == 200.0);
    REQUIRE(smoothed.smoothedBytesPerSecond == 125.0);
    REQUIRE(smoothed.etaMilliseconds == 5600);
    REQUIRE(progress.ObserveVerifiedRawBytes(300, 3000));
    progress.ObserveStall(7000);
    const auto stalled = progress.GetSnapshot();
    REQUIRE(stalled.smoothedBytesPerSecond == 0);
    REQUIRE_FALSE(stalled.etaMilliseconds.has_value());
    REQUIRE_FALSE(progress.ObserveVerifiedRawBytes(299, 8000));
    REQUIRE_FALSE(progress.ObserveVerifiedRawBytes(1001, 8000));
}

TEST_CASE("CaptureEpoch reset invalidates the unpublished descriptor progress and ETA", "[application][capture-epoch]")
{
    pbapp::DecoderProgressTracker progress;
    REQUIRE(progress.BindDescriptor(1000, 0));
    REQUIRE(progress.ObserveVerifiedRawBytes(100, 1000));
    REQUIRE(progress.ObserveVerifiedRawBytes(300, 2000));
    progress.ResetForCaptureEpoch(2500);
    const auto reset = progress.GetSnapshot();
    REQUIRE_FALSE(reset.descriptorKnown);
    REQUIRE(reset.verifiedRawBytes == 0);
    REQUIRE_FALSE(reset.progress.has_value());
    REQUIRE(reset.instantBytesPerSecond == 0);
    REQUIRE(reset.smoothedBytesPerSecond == 0);
    REQUIRE_FALSE(reset.etaMilliseconds.has_value());
}

TEST_CASE("Visual identity establishes a fresh FrameSequence baseline for every CaptureEpoch",
    "[application][capture-epoch][visual-identity]")
{
    pbapp::VisualIdentityTracker identity;
    REQUIRE(identity.Observe(100, 1, 0) == pbapp::VisualIdentityDisposition::Unique);
    REQUIRE(identity.Observe(101, 1, 10000000) == pbapp::VisualIdentityDisposition::Unique);
    REQUIRE(identity.Observe(101, 1, 10000001) == pbapp::VisualIdentityDisposition::Duplicate);
    REQUIRE(identity.Observe(99, 1, 10000002) == pbapp::VisualIdentityDisposition::Reordered);
    REQUIRE(identity.Observe(104, 1, 20000000) == pbapp::VisualIdentityDisposition::Unique);
    REQUIRE(identity.Observe(0, 2, 30000000) == pbapp::VisualIdentityDisposition::Unique);
    REQUIRE(identity.Observe(1, 2, 40000000) == pbapp::VisualIdentityDisposition::Unique);
    REQUIRE(identity.Observe(2, 0, 50000000) == pbapp::VisualIdentityDisposition::Invalid);

    const pbapp::VisualIdentitySnapshot snapshot = identity.GetSnapshot();
    REQUIRE(snapshot.uniqueFrames == 5);
    REQUIRE(snapshot.duplicateFrames == 1);
    REQUIRE(snapshot.reorderedFrames == 1);
    REQUIRE(snapshot.gapEvents == 1);
    REQUIRE(snapshot.skippedSequences == 2);
    REQUIRE(snapshot.framesPerSecond == 1.0);
}

TEST_CASE("Invalid runtime enum values remain fail-visible in diagnostics", "[application][enum][diagnostics]")
{
    REQUIRE(std::string_view(pbapp::GetVisualProfileName(static_cast<pbapp::VisualProfile>(0xff))) == "Unknown");
    REQUIRE(std::string_view(pbapp::GetCaptureBackendName(static_cast<pbapp::CaptureBackend>(0xff))) == "Unknown");
    REQUIRE(std::string_view(pbapp::GetCompressionCodecName(
        static_cast<pbprotocol::CompressionCodec>(0xff))) == "Unknown");
    REQUIRE(std::string_view(pbapp::GetOuterFecModeName(static_cast<pbprotocol::OuterFecMode>(0xff))) == "Unknown");
}

TEST_CASE("Window close defers exactly one stop while active and accepts after terminal state",
    "[application][window-close]")
{
    REQUIRE(pbapp::IsEncoderStateActive(pbapp::EncoderState::Broadcasting));
    REQUIRE(pbapp::IsDecoderStateActive(pbapp::DecoderState::Receiving));
    REQUIRE(pbapp::GetWindowCloseAction(true, false) == pbapp::WindowCloseAction::RequestStopAndDefer);
    REQUIRE(pbapp::GetWindowCloseAction(true, true) == pbapp::WindowCloseAction::Defer);
    REQUIRE(pbapp::GetWindowCloseAction(false, true) == pbapp::WindowCloseAction::Accept);
}

TEST_CASE("Phase 1.5 capability model keeps future offline and automatic fallback unavailable",
    "[application][capabilities][offline]")
{
    const pbapp::RuntimeCapabilities capabilities = pbapp::GetRuntimeCapabilities();
    REQUIRE(capabilities.instantLocalDesktop);
    REQUIRE_FALSE(capabilities.offlineMp4);
    REQUIRE_FALSE(capabilities.multiSegment);
    REQUIRE_FALSE(capabilities.automaticCaptureFallback);
}

TEST_CASE("Progress reaching one does not force Decoder success after digest failure", "[application][digest-failure]")
{
    pbapp::DecoderProgressTracker progress;
    REQUIRE(progress.BindDescriptor(16, 0));
    REQUIRE(progress.ObserveVerifiedRawBytes(16, 100));
    REQUIRE(progress.GetSnapshot().progress == 1.0);
    pbapp::DecoderStateMachine state;
    REQUIRE(state.Start(1) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Advance(1, pbapp::DecoderState::ReceivingControl) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Advance(1, pbapp::DecoderState::Receiving) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Advance(1, pbapp::DecoderState::Recovering) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Advance(1, pbapp::DecoderState::Verifying) == pbapp::TransitionResult::Applied);
    REQUIRE(state.Fail(1) == pbapp::TransitionResult::Applied);
    REQUIRE(state.GetState() == pbapp::DecoderState::Failed);
}

TEST_CASE("SnapshotStore gives coherent thread-safe immutable copies", "[application][thread-safety]")
{
    pbapp::SnapshotStore<pbapp::DecoderSnapshot> store;
    std::atomic<bool> finished = false;
    std::thread writer([&]
    {
        for (std::uint64_t index = 1; index <= 10000; index++)
        {
            store.Update([index](pbapp::DecoderSnapshot& snapshot)
            {
                snapshot.runGeneration = index;
                snapshot.originalFileBytes = index;
                snapshot.verifiedRawBytes = index;
            });
        }
        finished = true;
    });
    while (!finished)
    {
        const auto snapshot = store.Get();
        REQUIRE(snapshot.originalFileBytes == snapshot.verifiedRawBytes);
        REQUIRE(snapshot.runGeneration == snapshot.originalFileBytes);
    }
    writer.join();
    REQUIRE(store.Get().runGeneration == 10000);
}
