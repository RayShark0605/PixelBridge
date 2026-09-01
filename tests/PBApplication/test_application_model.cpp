#include "application_model.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <string_view>
#include <type_traits>
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
    REQUIRE(identity.Observe(0, 2, 40000001, 42) == pbapp::VisualIdentityDisposition::Unique);
    REQUIRE(identity.Observe(0, 2, 40000002, 42) == pbapp::VisualIdentityDisposition::Duplicate);
    REQUIRE(identity.Observe(2, 0, 50000000) == pbapp::VisualIdentityDisposition::Invalid);

    const pbapp::VisualIdentitySnapshot snapshot = identity.GetSnapshot();
    REQUIRE(snapshot.uniqueFrames == 6);
    REQUIRE(snapshot.duplicateFrames == 2);
    REQUIRE(snapshot.reorderedFrames == 1);
    REQUIRE(snapshot.gapEvents == 1);
    REQUIRE(snapshot.skippedSequences == 2);
    REQUIRE(snapshot.framesPerSecond == 1.0);
}

TEST_CASE("Channel stall telemetry distinguishes capture loss from continued duplicate visuals",
    "[application][stall][remote-visual]")
{
    pbapp::ChannelStallTracker stalls;
    stalls.Observe(0, 0, 0);
    stalls.Observe(500, 1, 0);
    stalls.Observe(999, 2, 0);
    REQUIRE(stalls.GetSnapshot().visual.count == 0);
    stalls.Observe(1000, 3, 0);
    auto snapshot = stalls.GetSnapshot();
    REQUIRE(snapshot.visual.count == 1);
    REQUIRE(snapshot.visual.active);
    REQUIRE(snapshot.visual.currentMilliseconds == 1000);
    REQUIRE_FALSE(snapshot.capture.active);

    stalls.Observe(1500, 4, 1);
    snapshot = stalls.GetSnapshot();
    REQUIRE_FALSE(snapshot.visual.active);
    REQUIRE(snapshot.visual.totalMilliseconds == 1500);
    REQUIRE(snapshot.visual.maximumMilliseconds == 1500);

    stalls.Observe(2499, 4, 1);
    REQUIRE_FALSE(stalls.GetSnapshot().capture.active);
    stalls.Observe(2500, 4, 1);
    snapshot = stalls.GetSnapshot();
    REQUIRE(snapshot.capture.count == 1);
    REQUIRE(snapshot.capture.active);
    REQUIRE(snapshot.capture.currentMilliseconds == 1000);
    REQUIRE_FALSE(snapshot.visual.active);
    stalls.Finish(3000);
    snapshot = stalls.GetSnapshot();
    REQUIRE_FALSE(snapshot.capture.active);
    REQUIRE(snapshot.capture.totalMilliseconds == 1500);
    REQUIRE(snapshot.capture.maximumMilliseconds == 1500);
}

TEST_CASE("Channel stall domain reset closes active intervals without discarding cumulative evidence",
    "[application][stall][capture-epoch]")
{
    pbapp::ChannelStallTracker stalls;
    stalls.Observe(100, 10, 5);
    stalls.Observe(1100, 10, 5);
    REQUIRE(stalls.GetSnapshot().capture.active);
    stalls.ResetDomain(1600, 0, 5);
    auto snapshot = stalls.GetSnapshot();
    REQUIRE_FALSE(snapshot.capture.active);
    REQUIRE(snapshot.capture.count == 1);
    REQUIRE(snapshot.capture.totalMilliseconds == 1500);
    stalls.Observe(2600, 0, 5);
    snapshot = stalls.GetSnapshot();
    REQUIRE(snapshot.capture.active);
    REQUIRE(snapshot.capture.count == 2);
}

TEST_CASE("Capture loss closes an active visual stall at the last capture observation",
    "[application][stall][remote-visual][classification]")
{
    pbapp::ChannelStallTracker stalls;
    stalls.Observe(0, 0, 0);
    stalls.Observe(1000, 1, 0);
    auto snapshot = stalls.GetSnapshot();
    REQUIRE(snapshot.visual.active);
    REQUIRE(snapshot.visual.currentMilliseconds == 1000);

    stalls.Observe(1999, 1, 0);
    REQUIRE_FALSE(stalls.GetSnapshot().capture.active);
    stalls.Observe(2000, 1, 0);
    snapshot = stalls.GetSnapshot();
    REQUIRE(snapshot.capture.active);
    REQUIRE(snapshot.capture.currentMilliseconds == 1000);
    REQUIRE_FALSE(snapshot.visual.active);
    REQUIRE(snapshot.visual.totalMilliseconds == 1000);
    REQUIRE(snapshot.visual.maximumMilliseconds == 1000);
}

TEST_CASE("Remote duplicate refinement retries only an unadmitted current identity and never combines frames",
    "[application][remote-visual][duplicate][admission]")
{
    pbapp::RemoteDuplicateRefinementGate gate;
    gate.StartSequence(1, 100);
    REQUIRE_FALSE(gate.ShouldAttemptDuplicate(1, 100, false));
    REQUIRE(gate.GetSnapshot().attempts == 1);
    REQUIRE(gate.ShouldAttemptDuplicate(1, 100, true));
    REQUIRE(gate.GetSnapshot().attempts == 2);
    REQUIRE(gate.MarkAdmission(1, 100, true));
    auto snapshot = gate.GetSnapshot();
    REQUIRE(snapshot.currentSequenceAdmitted);
    REQUIRE(snapshot.recoveries == 1);
    REQUIRE_FALSE(gate.ShouldAttemptDuplicate(1, 100, true));
    REQUIRE_FALSE(gate.MarkAdmission(1, 100, true));
    REQUIRE(gate.GetSnapshot().recoveries == 1);

    gate.StartSequence(1, 101);
    REQUIRE_FALSE(gate.ShouldAttemptDuplicate(1, 100, true));
    REQUIRE(gate.MarkAdmission(1, 101, false));
    REQUIRE(gate.GetSnapshot().recoveries == 1);
    gate.ResetEpoch();
    REQUIRE_FALSE(gate.ShouldAttemptDuplicate(1, 101, true));
    gate.StartSequence(2, 0);
    REQUIRE(gate.ShouldAttemptDuplicate(2, 0, true));
}

// GetSnapshot() deliberately returns a value: telemetry sampling and the GUI read a snapshot and keep
// reading it while the tracker moves on. A const-reference return would keep compiling and would
// silently turn every earlier reading into a live alias, so the point-in-time property is pinned.
TEST_CASE("Telemetry snapshots keep the values of the instant they were taken",
    "[application][telemetry][snapshot-semantics]")
{
    // Compile-time pin: a reference return would keep every call site compiling while silently
    // turning an earlier sample into a live alias, so the reviewed value semantics are asserted.
    static_assert(!std::is_reference_v<decltype(std::declval<const pbapp::ChannelStallTracker&>().GetSnapshot())>,
        "ChannelStallTracker::GetSnapshot must return a value so a telemetry sample stays point-in-time");
    static_assert(!std::is_reference_v<decltype(std::declval<const pbapp::DecoderProgressTracker&>().GetSnapshot())>,
        "DecoderProgressTracker::GetSnapshot must return a value so a published progress sample stays point-in-time");
    static_assert(!std::is_reference_v<decltype(std::declval<const pbapp::RemoteDuplicateRefinementGate&>().GetSnapshot())>,
        "RemoteDuplicateRefinementGate::GetSnapshot must return a value so an admission audit stays point-in-time");
    static_assert(!std::is_reference_v<decltype(std::declval<const pbapp::VisualIdentityTracker&>().GetSnapshot())>,
        "VisualIdentityTracker::GetSnapshot must return a value so a frame-identity sample stays point-in-time");

    pbapp::ChannelStallTracker stalls;
    stalls.Observe(0, 0, 0);
    stalls.Observe(500, 1, 0);
    stalls.Observe(999, 2, 0);
    stalls.Observe(1000, 3, 0);
    const pbapp::ChannelStallSnapshot stalled = stalls.GetSnapshot();
    REQUIRE(stalled.visual.count == 1);
    REQUIRE(stalled.visual.active);
    REQUIRE(stalled.visual.currentMilliseconds == 1000);
    REQUIRE(stalled.visual.totalMilliseconds == 0);
    REQUIRE(stalled.capture.count == 0);

    stalls.Observe(1500, 4, 1);
    stalls.Observe(2500, 4, 1);
    stalls.Finish(3000);
    const pbapp::ChannelStallSnapshot laterStalls = stalls.GetSnapshot();
    REQUIRE_FALSE(laterStalls.visual.active);
    REQUIRE(laterStalls.visual.totalMilliseconds == 1500);
    REQUIRE(laterStalls.capture.count == 1);
    REQUIRE(stalled.visual.count == 1);
    REQUIRE(stalled.visual.active);
    REQUIRE(stalled.visual.currentMilliseconds == 1000);
    REQUIRE(stalled.visual.totalMilliseconds == 0);
    REQUIRE(stalled.capture.count == 0);

    pbapp::DecoderProgressTracker progress;
    REQUIRE(progress.BindDescriptor(1000, 100));
    REQUIRE(progress.ObserveVerifiedRawBytes(250, 1100));
    const pbapp::ProgressSnapshot quarter = progress.GetSnapshot();
    REQUIRE(quarter.progress == 0.25);
    REQUIRE(quarter.verifiedRawBytes == 250);
    REQUIRE(quarter.remainingRawBytes == 750);
    REQUIRE_FALSE(quarter.etaMilliseconds.has_value());

    REQUIRE(progress.ObserveVerifiedRawBytes(500, 2100));
    progress.ObserveStall(3100);
    progress.ResetForCaptureEpoch(4000);
    const pbapp::ProgressSnapshot laterProgress = progress.GetSnapshot();
    REQUIRE_FALSE(laterProgress.descriptorKnown);
    REQUIRE(laterProgress.verifiedRawBytes == 0);
    REQUIRE(quarter.progress == 0.25);
    REQUIRE(quarter.verifiedRawBytes == 250);
    REQUIRE(quarter.remainingRawBytes == 750);
    REQUIRE(quarter.totalRawBytes == 1000);
    REQUIRE_FALSE(quarter.etaMilliseconds.has_value());

    pbapp::RemoteDuplicateRefinementGate refinement;
    refinement.StartSequence(1, 100);
    REQUIRE_FALSE(refinement.ShouldAttemptDuplicate(1, 100, false));
    const pbapp::RemoteDuplicateRefinementSnapshot attempts = refinement.GetSnapshot();
    REQUIRE(attempts.attempts == 1);
    REQUIRE(attempts.recoveries == 0);
    REQUIRE_FALSE(attempts.currentSequenceAdmitted);

    REQUIRE(refinement.ShouldAttemptDuplicate(1, 100, true));
    REQUIRE(refinement.MarkAdmission(1, 100, true));
    refinement.ResetEpoch();
    const pbapp::RemoteDuplicateRefinementSnapshot laterRefinement = refinement.GetSnapshot();
    REQUIRE(laterRefinement.attempts == 2);
    REQUIRE(laterRefinement.recoveries == 1);
    REQUIRE(attempts.attempts == 1);
    REQUIRE(attempts.recoveries == 0);
    REQUIRE_FALSE(attempts.currentSequenceAdmitted);

    pbapp::VisualIdentityTracker identity;
    REQUIRE(identity.Observe(100, 1, 0) == pbapp::VisualIdentityDisposition::Unique);
    const pbapp::VisualIdentitySnapshot identityEarly = identity.GetSnapshot();
    REQUIRE(identityEarly.uniqueFrames == 1);
    REQUIRE(identity.Observe(100, 1, 10000000) == pbapp::VisualIdentityDisposition::Duplicate);
    const pbapp::VisualIdentitySnapshot identityLate = identity.GetSnapshot();
    REQUIRE(identityLate.duplicateFrames == 1);
    REQUIRE(identityEarly.uniqueFrames == 1);
    REQUIRE(identityEarly.duplicateFrames == 0);
}

TEST_CASE("Invalid runtime enum values remain fail-visible in diagnostics", "[application][enum][diagnostics]")
{
    REQUIRE(std::string_view(pbapp::GetVisualProfileName(static_cast<pbapp::VisualProfile>(0xff))) == "Unknown");
    REQUIRE(std::string_view(pbapp::GetCaptureBackendName(static_cast<pbapp::CaptureBackend>(0xff))) == "Unknown");
    REQUIRE(std::string_view(pbapp::GetCompressionCodecName(
        static_cast<pbprotocol::CompressionCodec>(0xff))) == "Unknown");
    REQUIRE(std::string_view(pbapp::GetOuterFecModeName(static_cast<pbprotocol::OuterFecMode>(0xff))) == "Unknown");
    REQUIRE(std::string_view(pbapp::GetMetadataProvenanceName(static_cast<pbapp::MetadataProvenance>(0xff))) == "Unknown");
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
