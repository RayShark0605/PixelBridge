#include "sender_carousel_scheduler.h"

#include "pbmodulation/unified_visual.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/control_plane_receiver.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_version.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace
{

[[nodiscard]] std::uint64_t GetTickDeadlineNanoseconds(
    const std::uint32_t logicalFramesPerSecond, const std::uint64_t logicalTickOrdinal)
{
    const std::uint64_t wholeSeconds = logicalTickOrdinal / logicalFramesPerSecond;
    const std::uint64_t fractionalTick = logicalTickOrdinal % logicalFramesPerSecond;
    const std::uint64_t fractionalNanoseconds =
        (fractionalTick * pbapp::senderLogicalFrameNanosecondsPerSecond + logicalFramesPerSecond - 1ULL) /
        logicalFramesPerSecond;
    return wholeSeconds * pbapp::senderLogicalFrameNanosecondsPerSecond + fractionalNanoseconds;
}

void RequireExactSlotAccounting(const pbapp::SenderUnifiedScheduledFrame& frame)
{
    std::array<pbmodulation::UnifiedSlotAssignment, pbapp::senderUnifiedCodewordSlotCount> assignments{};
    std::uint32_t controlSlots = 0;
    std::uint32_t transportSlots = 0;
    std::uint32_t scheduledEquations = 0;
    std::uint32_t paddingDuplicates = 0;
    std::uint32_t inactiveTransportSlots = 0;
    for (std::size_t slotIndex = 0; slotIndex < frame.slots.size(); slotIndex++)
    {
        const pbapp::SenderUnifiedScheduledSlot& slot = frame.slots[slotIndex];
        assignments[slotIndex] = slot.assignment;
        REQUIRE(slot.assignment.codewordSlot == slotIndex);
        if (slot.assignment.kind == pbmodulation::UnifiedSlotKind::Control)
        {
            controlSlots++;
            REQUIRE(slot.transportDisposition == pbapp::SenderUnifiedTransportSlotDisposition::NotTransport);
            REQUIRE(slot.assignment.controlPriority != pbmodulation::UnifiedControlPriority::NotApplicable);
            REQUIRE_FALSE(slot.repairEquation);
            REQUIRE(slot.repairEquationOffset == 0);
        }
        else
        {
            transportSlots++;
            REQUIRE(slot.assignment.controlPriority == pbmodulation::UnifiedControlPriority::NotApplicable);
            REQUIRE(slot.transportDisposition != pbapp::SenderUnifiedTransportSlotDisposition::NotTransport);
            scheduledEquations += static_cast<std::uint32_t>(
                slot.transportDisposition == pbapp::SenderUnifiedTransportSlotDisposition::ScheduledEquation);
            paddingDuplicates += static_cast<std::uint32_t>(
                slot.transportDisposition == pbapp::SenderUnifiedTransportSlotDisposition::PaddingDuplicate);
            inactiveTransportSlots += static_cast<std::uint32_t>(
                slot.transportDisposition == pbapp::SenderUnifiedTransportSlotDisposition::InactiveZeroByteSession);
            if (!slot.repairEquation)
            {
                REQUIRE(slot.repairEquationOffset == 0);
            }
        }
    }
    REQUIRE(pbmodulation::ValidateUnifiedMixedSlotPlan(assignments));
    REQUIRE(controlSlots == frame.controlSlotCount);
    REQUIRE(frame.controlBurstSlotCount <= frame.controlSlotCount);
    REQUIRE(transportSlots == frame.transportSlotCount);
    REQUIRE(scheduledEquations == frame.scheduledEquationCount);
    REQUIRE(paddingDuplicates == frame.paddingDuplicateSlotCount);
    REQUIRE(inactiveTransportSlots == frame.inactiveTransportSlotCount);
    REQUIRE(controlSlots + transportSlots == frame.slots.size());
}

[[nodiscard]] pbprotocol::SessionId MakeSessionId()
{
    pbprotocol::SessionId sessionId{};
    for (std::size_t byteIndex = 0; byteIndex < sessionId.bytes.size(); byteIndex++)
    {
        sessionId.bytes[byteIndex] = static_cast<std::byte>(0x40U + byteIndex);
    }
    return sessionId;
}

[[nodiscard]] std::vector<std::byte> WrapControlRecord(
    const pbprotocol::ControlRecordType recordType, const std::uint64_t controlSequence,
    const pbprotocol::SessionTag sessionTag, const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView record{
        pbprotocol::kControlVersion, recordType, controlSequence, sessionTag, payload};
    const auto serializedSize = pbprotocol::GetSerializedSize(record);
    REQUIRE(serializedSize);
    std::vector<std::byte> bytes(serializedSize.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    return bytes;
}

struct EmptySessionControls
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::GetDefaultReceiverResourcePolicy();
    pbprotocol::SessionDescriptor session;
    pbprotocol::FinalManifest manifest;
    pbprotocol::SessionTag sessionTag;
    std::vector<std::byte> sessionControl;
    std::vector<std::byte> manifestControl;

    EmptySessionControls()
    {
        session.protocolVersion = pbprotocol::GetProtocolVersion();
        session.sessionId = MakeSessionId();
        session.originalFileSize = 0;
        session.segmentCount = 0;
        session.digestAlgorithm = pbprotocol::DigestAlgorithm::Blake3_256;
        session.sessionVisualProfileId = pbprotocol::kUnifiedVisualProfileId;
        session.fileNameUtf8 = "empty.bin";
        sessionTag = pbprotocol::DeriveSessionTag(session.sessionId);
        manifest = {session.sessionId, 0, 0, pbprotocol::GetEmptyBlake3WholeFileDigest(),
            pbprotocol::DigestAlgorithm::Blake3_256};

        const auto sessionPayloadSize = pbprotocol::GetSerializedSize(session);
        REQUIRE(sessionPayloadSize);
        std::vector<std::byte> sessionPayload(sessionPayloadSize.Value());
        REQUIRE(pbprotocol::SerializeSessionDescriptor(session, resourcePolicy, sessionPayload));
        sessionControl = WrapControlRecord(
            pbprotocol::ControlRecordType::SessionDescriptor, 1, sessionTag, sessionPayload);

        const std::size_t manifestPayloadSize = pbprotocol::GetSerializedSize(manifest);
        REQUIRE(manifestPayloadSize != 0);
        std::vector<std::byte> manifestPayload(manifestPayloadSize);
        REQUIRE(pbprotocol::SerializeFinalManifest(manifest, session, resourcePolicy, manifestPayload));
        manifestControl = WrapControlRecord(
            pbprotocol::ControlRecordType::FinalManifest, 2, sessionTag, manifestPayload);
    }
};

[[nodiscard]] std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeBootstrap(
    const pbprotocol::SessionTag sessionTag, const std::uint64_t frameSequence)
{
    const pbprotocol::BootstrapRecord record{pbprotocol::kBootstrapVersion,
        pbprotocol::GetProtocolVersion(), pbmodulation::kUnifiedVisualProfile.productProfile.visualLayoutVersion,
        pbmodulation::kUnifiedVisualProfile.productProfile.visualProfileId, sessionTag, frameSequence, 0, 0};
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bytes{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
    return bytes;
}

[[nodiscard]] pbmodulation::LumaView MakeView(const std::vector<std::byte>& pixels)
{
    return {pixels, pbmodulation::kUnifiedVisualProfile.canvasWidth,
        pbmodulation::kUnifiedVisualProfile.canvasHeight,
        static_cast<std::size_t>(pbmodulation::kUnifiedVisualProfile.canvasWidth) * 4,
        pbmodulation::LumaPixelFormat::Bgra8};
}

} // namespace

TEST_CASE("Unified periodic phase preserves startup and shifts only the first refresh deadline",
    "[application][scheduler][control][control-phase]")
{
    constexpr std::uint64_t second = pbapp::senderLogicalFrameNanosecondsPerSecond;
    for (std::uint32_t phaseIndex = 0; phaseIndex < pbapp::senderUnifiedActiveSegmentWindowSize; phaseIndex++)
    {
        CAPTURE(phaseIndex);
        pbapp::SenderUnifiedCarouselScheduler original;
        pbapp::SenderUnifiedCarouselScheduler candidate;
        REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({64000, 4, 15, true, 1}, original));
        REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({64000, 4, 15, true, 1, phaseIndex,
            pbapp::senderUnifiedActiveSegmentWindowSize}, candidate));
        for (std::uint64_t tick = 0; tick < 2; tick++)
        {
            pbapp::SenderUnifiedScheduledFrame originalFrame;
            pbapp::SenderUnifiedScheduledFrame candidateFrame;
            REQUIRE(original.PrepareFrameAt(tick, tick * second / 15, originalFrame));
            REQUIRE(candidate.PrepareFrameAt(tick, tick * second / 15, candidateFrame));
            REQUIRE(candidateFrame == originalFrame);
            REQUIRE(original.CommitPreparedFrame());
            REQUIRE(candidate.CommitPreparedFrame());
        }
        const std::uint64_t firstDeadline = 10 * second +
            10 * second * phaseIndex / pbapp::senderUnifiedActiveSegmentWindowSize;
        pbapp::SenderUnifiedScheduledFrame before;
        REQUIRE(candidate.PrepareFrameAt(2, firstDeadline - 1, before));
        REQUIRE(before.controlBurstSlotCount == 0);
        REQUIRE(before.controlSlotCount == 1);
        REQUIRE(candidate.CommitPreparedFrame());
        pbapp::SenderUnifiedScheduledFrame periodic;
        REQUIRE(candidate.PrepareFrameAt(3, firstDeadline, periodic));
        REQUIRE(periodic.controlBurstSlotCount == 3);
        RequireExactSlotAccounting(periodic);
        pbapp::SenderUnifiedScheduledFrame retry;
        REQUIRE(candidate.PrepareFrameAt(3, firstDeadline + 90 * second, retry));
        REQUIRE(retry == periodic);
        REQUIRE(candidate.CommitPreparedFrame());
        REQUIRE(candidate.PrepareFrameAt(4, firstDeadline + 10 * second - 1, before));
        REQUIRE(before.controlBurstSlotCount == 0);
        REQUIRE(candidate.CommitPreparedFrame());
        REQUIRE(candidate.PrepareFrameAt(5, firstDeadline + 10 * second, periodic));
        REQUIRE(periodic.controlBurstSlotCount == 3);
        REQUIRE(candidate.CommitPreparedFrame());
        REQUIRE(candidate.GetSnapshot().controlBurstCount == 3);
    }
}

TEST_CASE("Five striped Segment control phases disperse at unchanged complete-window slot budget",
    "[application][scheduler][control][control-phase]")
{
    struct ScheduleResult
    {
        std::uint64_t controlSlots = 0;
        std::uint64_t equations = 0;
        std::vector<std::uint64_t> periodicTicks;
        std::uint64_t maximumSteadyGapTicks = 0;
    };
    const auto RunSchedule = [](const bool dispersed)
    {
        ScheduleResult result;
        std::array<pbapp::SenderUnifiedCarouselScheduler, 5> schedulers;
        for (std::uint32_t segmentIndex = 0; segmentIndex < schedulers.size(); segmentIndex++)
        {
            REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create(
                {64000, 4, 15, true, 1, dispersed ? segmentIndex : 0U, 5}, schedulers[segmentIndex]));
        }
        for (std::uint64_t tick = 0; tick < 600; tick++)
        {
            const std::uint64_t phaseEpoch = tick / schedulers.size() / pbapp::senderUnifiedSweepPhaseHold;
            const std::size_t segmentIndex = static_cast<std::size_t>(
                (phaseEpoch * pbapp::senderUnifiedSweepPhaseStep + tick % schedulers.size()) % schedulers.size());
            auto& scheduler = schedulers[segmentIndex];
            const auto snapshot = scheduler.GetSnapshot();
            pbapp::SenderUnifiedScheduledFrame frame;
            REQUIRE(scheduler.PrepareFrameAt(tick, GetTickDeadlineNanoseconds(15, tick), frame));
            RequireExactSlotAccounting(frame);
            REQUIRE(frame.firstEquationIndex == snapshot.committedEquationCount);
            result.controlSlots += frame.controlSlotCount;
            result.equations += frame.scheduledEquationCount;
            if (frame.controlBurstSlotCount == 3)
            {
                if (!result.periodicTicks.empty() && result.periodicTicks.back() >= 300)
                {
                    result.maximumSteadyGapTicks = (std::max)(result.maximumSteadyGapTicks, tick - result.periodicTicks.back());
                }
                result.periodicTicks.push_back(tick);
            }
            REQUIRE(scheduler.CommitPreparedFrame());
        }
        return result;
    };
    const auto original = RunSchedule(false);
    const auto candidate = RunSchedule(true);
    REQUIRE(original.periodicTicks.size() == 15);
    REQUIRE(candidate.periodicTicks.size() == original.periodicTicks.size());
    REQUIRE(candidate.controlSlots == original.controlSlots);
    REQUIRE(candidate.equations == original.equations);
    REQUIRE(candidate.maximumSteadyGapTicks <= 45);
    REQUIRE(original.maximumSteadyGapTicks >= 135);
    std::cout << "{\"schema\":\"PixelBridge.ControlPhase.ScheduleProof.1\",\"syntheticScheduleOnly\":true,\"fps\":15,\"framesPerVariant\":600,\"segments\":5,\"periodicTripletsPerVariant\":"
        << original.periodicTicks.size() << ",\"controlSlotsPerVariant\":" << original.controlSlots
        << ",\"equationsPerVariant\":" << original.equations << ",\"originalMaxGapTicks\":" << original.maximumSteadyGapTicks
        << ",\"candidateMaxGapTicks\":" << candidate.maximumSteadyGapTicks << "}\n";
}

TEST_CASE("Unified periodic phase validates bounds and retains a pending frame after deadline overflow",
    "[application][scheduler][control][control-phase]")
{
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    const std::uint32_t phaseCount = pbapp::senderUnifiedActiveSegmentWindowSize;
    for (const auto [phaseIndex, count] : std::array<std::pair<std::uint32_t, std::uint32_t>, 4>{{{0, 0}, {0, 9}, {8, 8}, {UINT32_MAX, 8}}})
    {
        REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create({64000, 4, 15, true, 0, phaseIndex, count}, scheduler));
        REQUIRE(scheduler.IsComplete());
    }
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({64000, 4, 15, true, 0, phaseCount - 1, phaseCount}, scheduler));
    const std::uint64_t start = UINT64_MAX - 7 * pbapp::senderLogicalFrameNanosecondsPerSecond;
    pbapp::SenderUnifiedScheduledFrame frame;
    REQUIRE(scheduler.PrepareFrameAt(0, start, frame));
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(scheduler.PrepareFrameAt(1, start + 1, frame));
    const auto before = scheduler.GetSnapshot();
    const auto failed = scheduler.CommitPreparedFrame();
    REQUIRE_FALSE(failed);
    REQUIRE(failed.code == pbapp::SenderCarouselSchedulerError::ArithmeticOverflow);
    REQUIRE(scheduler.GetSnapshot() == before);
    pbapp::SenderUnifiedScheduledFrame retry;
    REQUIRE(scheduler.PrepareFrameAt(1, start + 2, retry));
    REQUIRE(retry == frame);
}

TEST_CASE("Dispersed periodic control still drops elapsed intervals instead of emitting catch-up bursts",
    "[application][scheduler][control][control-phase]")
{
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({64000, 4, 1, true, 1,
        pbapp::senderUnifiedActiveSegmentWindowSize - 1, pbapp::senderUnifiedActiveSegmentWindowSize}, scheduler));
    pbapp::SenderUnifiedScheduledFrame frame;
    for (std::uint64_t tick = 0; tick < 2; tick++)
    {
        REQUIRE(scheduler.PrepareFrame(tick, frame));
        REQUIRE(scheduler.CommitPreparedFrame());
    }
    REQUIRE(scheduler.PrepareFrame(17, frame));
    REQUIRE(frame.controlBurstSlotCount == 0);
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(scheduler.PrepareFrame(100, frame));
    REQUIRE(frame.controlBurstSlotCount == 3);
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(scheduler.PrepareFrame(101, frame));
    REQUIRE(frame.controlBurstSlotCount == 0);
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(scheduler.GetSnapshot().controlBurstCount == 2);
}

TEST_CASE("Unified logical clock and mixed scheduler drop missed ticks without a catch-up queue",
    "[application][g09][scheduler][clock]")
{
    constexpr std::array<std::uint32_t, 3> testedRates{1, 15, 60};
    constexpr std::uint64_t simulationSeconds = 30;
    constexpr std::uint64_t startNanoseconds = 123456789ULL;
    for (const std::uint32_t logicalFramesPerSecond : testedRates)
    {
        CAPTURE(logicalFramesPerSecond);
        pbapp::SenderLogicalFrameClock clock;
        REQUIRE(pbapp::SenderLogicalFrameClock::Create(
            logicalFramesPerSecond, startNanoseconds, clock));
        pbapp::SenderUnifiedCarouselScheduler scheduler;
        const pbapp::SenderUnifiedCarouselSchedulerConfig config{
            pbapp::senderCarouselMaximumSystematicBlockCount, 4, logicalFramesPerSecond, true};
        REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create(config, scheduler));

        const std::uint64_t logicalTickCount = simulationSeconds * logicalFramesPerSecond;
        const std::uint64_t controlCadenceTicks =
            logicalFramesPerSecond * pbapp::senderCarouselControlCadenceSeconds;
        const std::uint32_t maximumControlSlots = pbmodulation::GetUnifiedMaximumControlSlots();
        std::uint64_t expectedEquationIndex = 0;
        std::uint64_t controlBearingFrames = 0;
        for (std::uint64_t logicalTickOrdinal = 0; logicalTickOrdinal < logicalTickCount; logicalTickOrdinal++)
        {
            const std::uint64_t deadline = startNanoseconds +
                GetTickDeadlineNanoseconds(logicalFramesPerSecond, logicalTickOrdinal);
            if (logicalTickOrdinal != 0)
            {
                pbapp::SenderLogicalFrameTick early;
                REQUIRE(clock.Acquire(deadline - 1, early));
                REQUIRE(early.disposition == pbapp::SenderLogicalFrameTickDisposition::NotDue);
            }
            pbapp::SenderLogicalFrameTick tick;
            REQUIRE(clock.Acquire(deadline, tick));
            REQUIRE(tick == pbapp::SenderLogicalFrameTick{
                pbapp::SenderLogicalFrameTickDisposition::Ready, logicalTickOrdinal, 0});

            pbapp::SenderUnifiedScheduledFrame frame;
            REQUIRE(scheduler.PrepareFrame(tick.logicalTickOrdinal, frame));
            RequireExactSlotAccounting(frame);
            REQUIRE(frame.firstEquationIndex == expectedEquationIndex);
            const std::uint64_t cadenceOffset = logicalTickOrdinal % controlCadenceTicks;
            if (logicalTickOrdinal == 0)
            {
                REQUIRE(frame.controlBurstSlotCount == maximumControlSlots);
                REQUIRE(frame.controlSlotCount == maximumControlSlots);
                REQUIRE(frame.transportSlotCount == pbapp::senderUnifiedCodewordSlotCount - maximumControlSlots);
                constexpr std::array expectedPriorities{
                    pbmodulation::UnifiedControlPriority::SessionDescriptor,
                    pbmodulation::UnifiedControlPriority::FinalManifest,
                    pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor,
                    pbmodulation::UnifiedControlPriority::SessionDescriptor,
                    pbmodulation::UnifiedControlPriority::FinalManifest,
                    pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor,
                    pbmodulation::UnifiedControlPriority::SessionDescriptor,
                    pbmodulation::UnifiedControlPriority::FinalManifest};
                for (std::size_t slot = 0; slot < expectedPriorities.size(); slot++)
                {
                    REQUIRE(frame.slots[slot].assignment.controlPriority == expectedPriorities[slot]);
                }
            }
            else if (logicalTickOrdinal == 1)
            {
                REQUIRE(frame.controlBurstSlotCount == 4);
                REQUIRE(frame.controlSlotCount == 4);
                REQUIRE(frame.transportSlotCount == pbapp::senderUnifiedCodewordSlotCount - 4);
                constexpr std::array expectedPriorities{
                    pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor,
                    pbmodulation::UnifiedControlPriority::SessionDescriptor,
                    pbmodulation::UnifiedControlPriority::FinalManifest,
                    pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor};
                for (std::size_t slot = 0; slot < expectedPriorities.size(); slot++)
                {
                    REQUIRE(frame.slots[slot].assignment.controlPriority == expectedPriorities[slot]);
                }
            }
            else if (cadenceOffset == 0)
            {
                REQUIRE(frame.controlBurstSlotCount == 3);
                REQUIRE(frame.controlSlotCount == 3);
                REQUIRE(frame.transportSlotCount == pbapp::senderUnifiedCodewordSlotCount - 3);
                constexpr std::array expectedPriorities{
                    pbmodulation::UnifiedControlPriority::SessionDescriptor,
                    pbmodulation::UnifiedControlPriority::FinalManifest,
                    pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor};
                for (std::size_t slot = 0; slot < expectedPriorities.size(); slot++)
                {
                    REQUIRE(frame.slots[slot].assignment.controlPriority == expectedPriorities[slot]);
                }
            }
            else
            {
                REQUIRE(frame.controlBurstSlotCount == 0);
                REQUIRE(frame.controlSlotCount == 1);
                REQUIRE(frame.slots.front().assignment.controlPriority ==
                    pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor);
                REQUIRE(frame.transportSlotCount == pbapp::senderUnifiedCodewordSlotCount - 1);
            }
            controlBearingFrames++;
            for (const pbapp::SenderUnifiedScheduledSlot& slot : frame.slots)
            {
                if (slot.transportDisposition == pbapp::SenderUnifiedTransportSlotDisposition::ScheduledEquation)
                {
                    REQUIRE(slot.equationIndex == expectedEquationIndex);
                    expectedEquationIndex++;
                }
            }
            pbapp::SenderUnifiedScheduledFrame repeated;
            REQUIRE(scheduler.PrepareFrame(tick.logicalTickOrdinal, repeated));
            REQUIRE(repeated == frame);
            pbapp::SenderUnifiedScheduledFrame forbiddenNext;
            const auto pendingStatus = scheduler.PrepareFrame(tick.logicalTickOrdinal + 1, forbiddenNext);
            REQUIRE_FALSE(pendingStatus);
            REQUIRE(pendingStatus.code == pbapp::SenderCarouselSchedulerError::FrameAlreadyPrepared);
            REQUIRE(scheduler.CommitPreparedFrame());
            REQUIRE(clock.Commit(deadline));
        }
        const pbapp::SenderUnifiedCarouselSnapshot snapshot = scheduler.GetSnapshot();
        REQUIRE_FALSE(snapshot.complete);
        REQUIRE(snapshot.committedFrameCount == logicalTickCount);
        REQUIRE(snapshot.controlBurstCount == 3);
        REQUIRE(snapshot.controlBearingFrameCount == controlBearingFrames);
        REQUIRE(snapshot.controlSlotCount == logicalTickCount + 14);
        REQUIRE(snapshot.transportSlotCount + snapshot.controlSlotCount ==
            logicalTickCount * pbapp::senderUnifiedCodewordSlotCount);
        REQUIRE(snapshot.committedEquationCount == expectedEquationIndex);
        REQUIRE(snapshot.committedEquationCount == snapshot.transportSlotCount);
        REQUIRE(clock.GetSnapshot() == pbapp::SenderLogicalFrameClockSnapshot{
            logicalTickCount, 0, false, logicalFramesPerSecond, logicalFramesPerSecond, false});
    }

    pbapp::SenderLogicalFrameClock clock;
    REQUIRE(pbapp::SenderLogicalFrameClock::Create(60, startNanoseconds, clock));
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({1000, 4, 60, true}, scheduler));
    pbapp::SenderLogicalFrameTick tick;
    REQUIRE(clock.Acquire(startNanoseconds, tick));
    pbapp::SenderUnifiedScheduledFrame firstFrame;
    REQUIRE(scheduler.PrepareFrame(tick.logicalTickOrdinal, firstFrame));
    REQUIRE(firstFrame.firstEquationIndex == 0);
    REQUIRE(firstFrame.controlSlotCount == pbmodulation::GetUnifiedMaximumControlSlots());
    REQUIRE(firstFrame.scheduledEquationCount ==
        pbapp::senderUnifiedCodewordSlotCount - pbmodulation::GetUnifiedMaximumControlSlots());
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(clock.Commit(startNanoseconds));

    REQUIRE(clock.Acquire(startNanoseconds + 5 * pbapp::senderLogicalFrameNanosecondsPerSecond, tick));
    REQUIRE(tick.logicalTickOrdinal == 300);
    REQUIRE(tick.droppedTickCount == 299);
    pbapp::SenderUnifiedScheduledFrame afterStall;
    REQUIRE(scheduler.PrepareFrame(tick.logicalTickOrdinal, afterStall));
    REQUIRE(afterStall.controlSlotCount == 4);
    REQUIRE(afterStall.firstEquationIndex ==
        pbapp::senderUnifiedCodewordSlotCount - pbmodulation::GetUnifiedMaximumControlSlots());
    REQUIRE(afterStall.scheduledEquationCount == pbapp::senderUnifiedCodewordSlotCount - 4);
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(clock.Commit(startNanoseconds + 5 * pbapp::senderLogicalFrameNanosecondsPerSecond));
    REQUIRE(clock.GetSnapshot().droppedTickCount == 299);
    pbapp::SenderLogicalFrameTick noCatchUp;
    REQUIRE(clock.Acquire(startNanoseconds + 5 * pbapp::senderLogicalFrameNanosecondsPerSecond, noCatchUp));
    REQUIRE(noCatchUp.disposition == pbapp::SenderLogicalFrameTickDisposition::NotDue);
}

TEST_CASE("Unified control repetitions survive either two-frame burst sample under temporal decimation",
    "[application][g21][scheduler][control][temporal-decimation]")
{
    constexpr std::array requiredPriorities{
        pbmodulation::UnifiedControlPriority::SessionDescriptor,
        pbmodulation::UnifiedControlPriority::FinalManifest,
        pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor};
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({799, 4, 15, true}, scheduler));

    std::array<std::uint32_t, requiredPriorities.size()> totalPriorityCounts{};
    for (std::uint64_t logicalTickOrdinal = 0; logicalTickOrdinal < 2; logicalTickOrdinal++)
    {
        pbapp::SenderUnifiedScheduledFrame frame;
        REQUIRE(scheduler.PrepareFrame(logicalTickOrdinal, frame));
        RequireExactSlotAccounting(frame);
        REQUIRE(frame.controlSlotCount == (logicalTickOrdinal == 0 ? 8U : 4U));
        for (std::size_t priorityIndex = 0; priorityIndex < requiredPriorities.size(); priorityIndex++)
        {
            const auto priority = requiredPriorities[priorityIndex];
            const auto priorityCount = static_cast<std::uint32_t>(std::ranges::count_if(
                frame.slots.begin(), frame.slots.begin() + frame.controlSlotCount,
                [priority](const pbapp::SenderUnifiedScheduledSlot& slot)
                {
                    return slot.assignment.controlPriority == priority;
                }));
            REQUIRE(priorityCount > 0);
            totalPriorityCounts[priorityIndex] += priorityCount;
        }
        REQUIRE(scheduler.CommitPreparedFrame());
    }
    REQUIRE(totalPriorityCounts == std::array<std::uint32_t, 3>{4, 4, 4});
    REQUIRE(scheduler.GetSnapshot().controlSlotCount == 12);
}

TEST_CASE("Unified logical FPS changes apply after the pending complete frame and never create a catch-up queue",
    "[application][g13][scheduler][clock][dynamic-fps]")
{
    constexpr std::uint64_t startNanoseconds = 9000000000ULL;
    pbapp::SenderLogicalFrameClock clock;
    REQUIRE(pbapp::SenderLogicalFrameClock::Create(15, startNanoseconds, clock));
    pbapp::SenderLogicalFrameTick tick;
    REQUIRE(clock.Acquire(startNanoseconds, tick));
    REQUIRE(tick.logicalTickOrdinal == 0);

    REQUIRE(clock.RequestFramesPerSecond(60, startNanoseconds + 1000000ULL));
    REQUIRE(clock.GetSnapshot() == pbapp::SenderLogicalFrameClockSnapshot{0, 0, true, 15, 60, true});
    pbapp::SenderLogicalFrameTick samePending;
    REQUIRE(clock.Acquire(startNanoseconds + 5000000ULL, samePending));
    REQUIRE(samePending == tick);

    constexpr std::uint64_t firstCompletion = startNanoseconds + 10000000ULL;
    REQUIRE(clock.Commit(firstCompletion));
    REQUIRE(clock.GetSnapshot() == pbapp::SenderLogicalFrameClockSnapshot{1, 0, false, 60, 60, false});
    constexpr std::uint64_t sixtyHertzInterval = 16666667ULL;
    REQUIRE(clock.Acquire(firstCompletion + sixtyHertzInterval - 1, tick));
    REQUIRE(tick.disposition == pbapp::SenderLogicalFrameTickDisposition::NotDue);
    REQUIRE(clock.Acquire(firstCompletion + sixtyHertzInterval, tick));
    REQUIRE(tick.logicalTickOrdinal == 1);

    REQUIRE(clock.RequestFramesPerSecond(30, firstCompletion + sixtyHertzInterval));
    REQUIRE(clock.RequestFramesPerSecond(1, firstCompletion + sixtyHertzInterval));
    constexpr std::uint64_t secondCompletion = firstCompletion + sixtyHertzInterval + 2000000ULL;
    REQUIRE(clock.Commit(secondCompletion));
    REQUIRE(clock.GetSnapshot() == pbapp::SenderLogicalFrameClockSnapshot{2, 0, false, 1, 1, false});
    REQUIRE(clock.Acquire(secondCompletion + pbapp::senderLogicalFrameNanosecondsPerSecond - 1, tick));
    REQUIRE(tick.disposition == pbapp::SenderLogicalFrameTickDisposition::NotDue);
    const std::uint64_t oneHertzDeadline = secondCompletion + pbapp::senderLogicalFrameNanosecondsPerSecond;
    REQUIRE(clock.Acquire(oneHertzDeadline, tick));
    REQUIRE(tick.logicalTickOrdinal == 2);
    REQUIRE(clock.Commit(oneHertzDeadline));

    REQUIRE(clock.Acquire(oneHertzDeadline + 5 * pbapp::senderLogicalFrameNanosecondsPerSecond, tick));
    REQUIRE(tick.logicalTickOrdinal == 7);
    REQUIRE(tick.droppedTickCount == 4);
    const auto beforeInvalidRequest = clock.GetSnapshot();
    REQUIRE_FALSE(clock.RequestFramesPerSecond(0, oneHertzDeadline));
    REQUIRE_FALSE(clock.RequestFramesPerSecond(61, oneHertzDeadline));
    REQUIRE(clock.GetSnapshot() == beforeInvalidRequest);
}

TEST_CASE("Unified mixed scheduler converges independent Segment rounds without allocating IDs to Control",
    "[application][g09][scheduler][carousel][multi-segment]")
{
    std::uint64_t logicalTickOrdinal = 100;
    for (std::uint64_t segmentOrdinal = 0; segmentOrdinal < 2; segmentOrdinal++)
    {
        CAPTURE(segmentOrdinal);
        pbapp::SenderUnifiedCarouselScheduler scheduler;
        REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({33, 4, 15, false}, scheduler));
        std::uint64_t expectedEquationIndex = 0;
        while (!scheduler.IsComplete())
        {
            pbapp::SenderUnifiedScheduledFrame frame;
            REQUIRE(scheduler.PrepareFrame(logicalTickOrdinal, frame));
            RequireExactSlotAccounting(frame);
            for (const pbapp::SenderUnifiedScheduledSlot& slot : frame.slots)
            {
                if (slot.transportDisposition == pbapp::SenderUnifiedTransportSlotDisposition::ScheduledEquation)
                {
                    REQUIRE(slot.equationIndex == expectedEquationIndex);
                    expectedEquationIndex++;
                }
                else if (slot.transportDisposition == pbapp::SenderUnifiedTransportSlotDisposition::PaddingDuplicate)
                {
                    REQUIRE(slot.equationIndex < 33);
                }
            }
            REQUIRE(scheduler.CommitPreparedFrame());
            logicalTickOrdinal++;
        }
        const pbapp::SenderUnifiedCarouselSnapshot snapshot = scheduler.GetSnapshot();
        REQUIRE(snapshot.complete);
        REQUIRE(snapshot.committedFrameCount == 4);
        REQUIRE(snapshot.controlBurstCount == 1);
        REQUIRE(snapshot.controlSlotCount == 14);
        REQUIRE(snapshot.transportSlotCount == 46);
        REQUIRE(snapshot.scheduledEquationCount == 33);
        REQUIRE(snapshot.committedEquationCount == 33);
        REQUIRE(snapshot.paddingDuplicateSlotCount == 13);
        REQUIRE(snapshot.repairEquationCount == 0);
        REQUIRE(expectedEquationIndex == 33);
    }
}

TEST_CASE("Unified Wirehair later passes scale their repair budget with the logical frame rate",
    "[application][g21][scheduler][carousel][repair-only]")
{
    constexpr std::uint32_t systematicBlockCount = 799;
    constexpr std::uint64_t initialPercentRepairEquationCount = 80;
    constexpr std::uint64_t initialRepairEquationCount =
        initialPercentRepairEquationCount + pbapp::senderUnifiedInitialTransitionGuardBlocks;
    constexpr std::uint64_t initialRoundEquationCount = systematicBlockCount + initialRepairEquationCount;
    constexpr std::uint64_t fullRepairOverheadEquationCount = 160;
    constexpr std::uint64_t fountainRepairEquationCount = 160;
    constexpr std::uint32_t firstInitialRepairId = systematicBlockCount;
    constexpr std::uint32_t firstFullRepairId = static_cast<std::uint32_t>(initialRoundEquationCount);

    const auto runRound = [](const std::uint64_t carouselPass, const std::uint32_t firstRepairId,
        const std::uint32_t logicalFramesPerSecond, const std::uint64_t expectedRepairCount)
    {
        const bool includeSystematicEquations = carouselPass == 0;
        pbapp::SenderUnifiedCarouselScheduler scheduler;
        REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create(
            {systematicBlockCount, 4, logicalFramesPerSecond, true, carouselPass}, scheduler));
        const pbapp::SenderUnifiedCarouselSnapshot initial = scheduler.GetSnapshot();
        const std::uint64_t expectedSystematicCount = includeSystematicEquations ? systematicBlockCount : 0;
        const std::uint64_t expectedRoundEquationCount = includeSystematicEquations ?
            initialRoundEquationCount : expectedRepairCount;
        REQUIRE(initial.scheduledEquationCount == expectedRoundEquationCount);
        REQUIRE(initial.systematicEquationCount == expectedSystematicCount);
        REQUIRE(initial.repairEquationCount == expectedRepairCount);

        std::vector<std::uint32_t> scheduledOuterBlockIds;
        std::uint64_t expectedEquationIndex = 0;
        std::uint64_t logicalTickOrdinal = 0;
        while (!scheduler.IsComplete())
        {
            pbapp::SenderUnifiedScheduledFrame frame;
            REQUIRE(scheduler.PrepareFrame(logicalTickOrdinal, frame));
            RequireExactSlotAccounting(frame);
            for (const pbapp::SenderUnifiedScheduledSlot& slot : frame.slots)
            {
                if (slot.transportDisposition != pbapp::SenderUnifiedTransportSlotDisposition::ScheduledEquation)
                {
                    if (slot.transportDisposition == pbapp::SenderUnifiedTransportSlotDisposition::PaddingDuplicate)
                    {
                        REQUIRE(slot.equationIndex < expectedRepairCount);
                        REQUIRE(slot.repairEquation == !includeSystematicEquations);
                        REQUIRE(slot.repairEquationOffset == (includeSystematicEquations ? 0 : slot.equationIndex));
                    }
                    continue;
                }
                REQUIRE(slot.equationIndex == expectedEquationIndex);
                const bool expectedRepairEquation = expectedEquationIndex >= expectedSystematicCount;
                REQUIRE(slot.repairEquation == expectedRepairEquation);
                const std::uint64_t expectedRepairOffset = expectedRepairEquation ?
                    expectedEquationIndex - expectedSystematicCount : 0;
                REQUIRE(slot.repairEquationOffset == expectedRepairOffset);
                const std::uint64_t outerBlockId = expectedRepairEquation ?
                    static_cast<std::uint64_t>(firstRepairId) + expectedRepairOffset : expectedEquationIndex;
                REQUIRE(outerBlockId <= (std::numeric_limits<std::uint32_t>::max)());
                scheduledOuterBlockIds.push_back(static_cast<std::uint32_t>(outerBlockId));
                expectedEquationIndex++;
            }
            REQUIRE(scheduler.CommitPreparedFrame());
            logicalTickOrdinal++;
        }
        const pbapp::SenderUnifiedCarouselSnapshot final = scheduler.GetSnapshot();
        REQUIRE(final.complete);
        REQUIRE(final.committedEquationCount == expectedRoundEquationCount);
        REQUIRE(final.systematicEquationCount == expectedSystematicCount);
        REQUIRE(final.repairEquationCount == expectedRepairCount);
        REQUIRE(expectedEquationIndex == expectedRoundEquationCount);
        REQUIRE(scheduledOuterBlockIds.size() == expectedRoundEquationCount);
        return scheduledOuterBlockIds;
    };

    // Low frame rates keep the historical K+20% FullRepairPass budget.
    const std::vector<std::uint32_t> initialIds = runRound(0, firstInitialRepairId, 15, initialRepairEquationCount);
    const std::vector<std::uint32_t> serialRepairIds = runRound(1, firstFullRepairId, 15,
        fullRepairOverheadEquationCount + systematicBlockCount);
    for (std::uint32_t equationIndex = 0; equationIndex < initialRoundEquationCount; equationIndex++)
    {
        REQUIRE(initialIds[equationIndex] == equationIndex);
    }
    for (std::uint32_t equationIndex = 0;
        equationIndex < systematicBlockCount + fullRepairOverheadEquationCount; equationIndex++)
    {
        REQUIRE(serialRepairIds[equationIndex] == firstFullRepairId + equationIndex);
    }
    REQUIRE(initialIds.back() < serialRepairIds.front());
    // Above the serial threshold the incremental fountain takes over: Pass 1
    // carries only the repair fraction, Pass 2 doubles it, Pass 3 quadruples
    // it, so a high-deficit Segment converges in a few wraps.
    const std::uint32_t fountainFps = pbapp::senderUnifiedSerialRepairFpsThreshold + 1;
    const std::vector<std::uint32_t> fountainIds = runRound(1, firstFullRepairId, fountainFps,
        fountainRepairEquationCount);
    for (std::uint32_t equationIndex = 0; equationIndex < fountainRepairEquationCount; equationIndex++)
    {
        REQUIRE(fountainIds[equationIndex] == firstFullRepairId + equationIndex);
    }
    for (const std::uint64_t carouselPass : {2ULL, 3ULL})
    {
        std::uint64_t expectedDoubling = (systematicBlockCount - 1ULL) *
            (static_cast<std::uint64_t>(pbapp::senderCarouselRepairPercentNumerator) << (carouselPass - 1ULL)) /
            pbapp::senderCarouselRepairPercentDenominator + 1ULL;
        if (expectedDoubling < pbapp::senderCarouselMinimumRepairBlocks)
        {
            expectedDoubling = pbapp::senderCarouselMinimumRepairBlocks;
        }
        pbapp::SenderUnifiedCarouselScheduler doubling;
        REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create(
            {systematicBlockCount, 4, fountainFps, true, carouselPass}, doubling));
        const pbapp::SenderUnifiedCarouselSnapshot snapshot = doubling.GetSnapshot();
        REQUIRE(snapshot.systematicEquationCount == 0);
        REQUIRE(snapshot.repairEquationCount == expectedDoubling);
        REQUIRE(snapshot.scheduledEquationCount == snapshot.repairEquationCount);
    }
}

TEST_CASE("Unified initial repair preserves the guard while phase balancing must cover the old fixed-phase deficit",
    "[application][g21][scheduler][carousel][transition-guard]")
{
    constexpr std::uint32_t fullSegmentBlockCount = 6385;
    constexpr std::uint64_t percentRepairBlocks = 639;
    constexpr std::uint64_t historicalPercentRepairBlocks = 799;
    constexpr std::uint64_t observedLostBlocks = 800;
    constexpr std::uint64_t expectedRepairBlocks =
        percentRepairBlocks + pbapp::senderUnifiedInitialTransitionGuardBlocks;
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create(
        {fullSegmentBlockCount, 4, 15, true, 0}, scheduler));

    const pbapp::SenderUnifiedCarouselSnapshot snapshot = scheduler.GetSnapshot();
    REQUIRE(snapshot.systematicEquationCount == fullSegmentBlockCount);
    REQUIRE(snapshot.repairEquationCount == expectedRepairBlocks);
    REQUIRE(snapshot.scheduledEquationCount == fullSegmentBlockCount + expectedRepairBlocks);
    REQUIRE(fullSegmentBlockCount + historicalPercentRepairBlocks - observedLostBlocks ==
        fullSegmentBlockCount - 1);
    REQUIRE(fullSegmentBlockCount + historicalPercentRepairBlocks +
        pbapp::senderUnifiedInitialTransitionGuardBlocks - observedLostBlocks >= fullSegmentBlockCount);
    // The smaller repair alone cannot absorb the old fixed-phase loss. The
    // 16-Segment production probe must prove redistribution before any slide;
    // do not silently reinterpret the historical 800 missing equations as 640.
    REQUIRE(snapshot.scheduledEquationCount - observedLostBlocks < fullSegmentBlockCount);
    REQUIRE(pbapp::senderUnifiedInitialTransitionGuardBlocks >
        2 * (pbapp::senderUnifiedCodewordSlotCount - 1));
}

TEST_CASE("Unified periodic refresh is one atomic descriptor triplet after startup and a long monotonic stall",
    "[application][g21][scheduler][control][periodic-triplet]")
{
    for (const std::uint64_t carouselPass : {0ULL, 1ULL})
    {
        pbapp::SenderUnifiedCarouselScheduler scheduler;
        REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({64000, 4, 15, true, carouselPass}, scheduler));
        for (std::uint64_t tick = 0; tick < 2; tick++)
        {
            pbapp::SenderUnifiedScheduledFrame frame;
            REQUIRE(scheduler.PrepareFrameAt(tick, tick * 66666667ULL, frame));
            REQUIRE(frame.controlBurstSlotCount == (tick == 0 ? 8U : 4U));
            REQUIRE(scheduler.CommitPreparedFrame());
        }
        constexpr std::uint64_t afterLongStall = 50ULL * pbapp::senderLogicalFrameNanosecondsPerSecond;
        pbapp::SenderUnifiedScheduledFrame refresh;
        REQUIRE(scheduler.PrepareFrameAt(2, afterLongStall, refresh));
        RequireExactSlotAccounting(refresh);
        REQUIRE(refresh.controlBurstSlotCount == 3);
        REQUIRE(refresh.transportSlotCount == 12);
        REQUIRE(refresh.slots[0].assignment.controlPriority == pbmodulation::UnifiedControlPriority::SessionDescriptor);
        REQUIRE(refresh.slots[1].assignment.controlPriority == pbmodulation::UnifiedControlPriority::FinalManifest);
        REQUIRE(refresh.slots[2].assignment.controlPriority == pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor);
        pbapp::SenderUnifiedScheduledFrame retry;
        REQUIRE(scheduler.PrepareFrameAt(2, afterLongStall + 1, retry));
        REQUIRE(retry == refresh);
        REQUIRE(scheduler.CommitPreparedFrame());
        pbapp::SenderUnifiedScheduledFrame next;
        REQUIRE(scheduler.PrepareFrameAt(3, afterLongStall + 66666667ULL, next));
        REQUIRE(next.controlBurstSlotCount == 0);
        REQUIRE(next.controlSlotCount == 1);
        REQUIRE(next.slots[0].assignment.controlPriority == pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor);
        REQUIRE(next.firstEquationIndex == refresh.firstEquationIndex + refresh.scheduledEquationCount);
        REQUIRE(scheduler.CommitPreparedFrame());
        REQUIRE(scheduler.GetSnapshot().controlBurstCount == 2);
    }
}

TEST_CASE("Unified Pass-0 reciprocal repair uses exact ceiling and preserves DirectRepeat and input bounds",
    "[application][g21][scheduler][repair-boundary]")
{
    constexpr std::array<std::pair<std::uint32_t, std::uint64_t>, 7> cases{{
        {2, 48}, {159, 48}, {160, 48}, {161, 49}, {799, 112}, {6385, 671}, {64000, 6432}}};
    for (const auto& [blockCount, repairCount] : cases)
    {
        pbapp::SenderUnifiedCarouselScheduler scheduler;
        REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({blockCount, 4, 15, true}, scheduler));
        REQUIRE(scheduler.GetSnapshot().repairEquationCount == repairCount);
        REQUIRE(scheduler.GetSnapshot().scheduledEquationCount == blockCount + repairCount);
        REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({blockCount, 4, 15, false}, scheduler));
        REQUIRE(scheduler.GetSnapshot().repairEquationCount == 0);
        REQUIRE(scheduler.GetSnapshot().scheduledEquationCount == blockCount);
    }
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create({64001, 4, 15, true}, scheduler));
    REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create({UINT32_MAX, 4, 15, true}, scheduler));
}

TEST_CASE("Unified scheduler rejects invalid products and finishes oversized Control bursts without data starvation",
    "[application][g09][scheduler][boundary]")
{
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create({0, 0, 15, false}, scheduler));
    REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create(
        {0, pbapp::senderUnifiedMaximumControlRepetitions + 1, 15, false}, scheduler));
    REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create({0, 4, 0, false}, scheduler));
    REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create(
        {0, 4, pbapp::senderUnifiedMaximumLogicalFramesPerSecond + 1, false}, scheduler));
    REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create(
        {pbapp::senderCarouselMaximumSystematicBlockCount + 1, 4, 15, false}, scheduler));
    REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create({1, 4, 15, true}, scheduler));
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({33, 4, 15, false, 1}, scheduler));
    REQUIRE(scheduler.GetSnapshot().systematicEquationCount == 33);
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({0, 4, 15, false, 1}, scheduler));
    REQUIRE(scheduler.GetSnapshot().repairEquationCount == 0);

    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create(
        {1, pbapp::senderUnifiedMaximumControlRepetitions, 1, false}, scheduler));
    const std::uint64_t controlItemsPerBurst =
        3ULL * pbapp::senderUnifiedMaximumControlRepetitions;
    const std::uint64_t expectedControlFrames =
        controlItemsPerBurst / pbmodulation::GetUnifiedMaximumControlSlots();
    const std::uint64_t transportSlotsPerControlFrame =
        pbapp::senderUnifiedCodewordSlotCount - pbmodulation::GetUnifiedMaximumControlSlots();
    std::uint64_t logicalTickOrdinal = 0;
    std::uint64_t scheduledEquationSlots = 0;
    while (!scheduler.IsComplete())
    {
        pbapp::SenderUnifiedScheduledFrame frame;
        REQUIRE(scheduler.PrepareFrame(logicalTickOrdinal, frame));
        RequireExactSlotAccounting(frame);
        REQUIRE(frame.controlSlotCount == pbmodulation::GetUnifiedMaximumControlSlots());
        REQUIRE(frame.transportSlotCount == transportSlotsPerControlFrame);
        REQUIRE(frame.scheduledEquationCount == static_cast<std::uint32_t>(logicalTickOrdinal == 0));
        scheduledEquationSlots += frame.scheduledEquationCount;
        REQUIRE(scheduler.CommitPreparedFrame());
        logicalTickOrdinal++;
    }
    const pbapp::SenderUnifiedCarouselSnapshot snapshot = scheduler.GetSnapshot();
    REQUIRE(snapshot.committedFrameCount == expectedControlFrames);
    REQUIRE(snapshot.controlSlotCount == controlItemsPerBurst);
    REQUIRE(snapshot.transportSlotCount == expectedControlFrames * transportSlotsPerControlFrame);
    REQUIRE(snapshot.committedEquationCount == 1);
    REQUIRE(snapshot.paddingDuplicateSlotCount ==
        expectedControlFrames * transportSlotsPerControlFrame - 1);
    REQUIRE(scheduledEquationSlots == 1);

    pbapp::SenderUnifiedScheduledFrame completedFrame;
    REQUIRE_FALSE(scheduler.PrepareFrame(logicalTickOrdinal, completedFrame));
    REQUIRE_FALSE(scheduler.CommitPreparedFrame());

    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({40, 4, 15, false}, scheduler));
    pbapp::SenderUnifiedScheduledFrame frame;
    REQUIRE(scheduler.PrepareFrame(50, frame));
    REQUIRE(scheduler.GetSnapshot().framePrepared);
    REQUIRE(scheduler.GetSnapshot().committedEquationCount == 0);
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(scheduler.GetSnapshot().committedEquationCount ==
        pbapp::senderUnifiedCodewordSlotCount - pbmodulation::GetUnifiedMaximumControlSlots());
    const auto regressionStatus = scheduler.PrepareFrame(50, frame);
    REQUIRE_FALSE(regressionStatus);
    REQUIRE(regressionStatus.code == pbapp::SenderCarouselSchedulerError::LogicalTickRegression);

    pbapp::SenderLogicalFrameClock clock;
    REQUIRE(pbapp::SenderLogicalFrameClock::Create(15, 0, clock));
    REQUIRE_FALSE(clock.Commit(0));
    REQUIRE_FALSE(pbapp::SenderLogicalFrameClock::Create(0, 0, clock));
    REQUIRE_FALSE(pbapp::SenderLogicalFrameClock::Create(61, 0, clock));
}

TEST_CASE("Zero-byte Unified Session recovers Session and Manifest from one mixed reference raster",
    "[application][g09][scheduler][zero-byte][reference-raster]")
{
    EmptySessionControls controls;
    const std::uint32_t originalMaxControlRecordBytes = controls.resourcePolicy.maxControlRecordBytes;
    const std::uint64_t originalMaxControlReassemblyBytes = controls.resourcePolicy.maxControlReassemblyBytes;
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({0, 4, 1, false}, scheduler));
    pbapp::SenderUnifiedScheduledFrame frame;
    REQUIRE(scheduler.PrepareFrame(0, frame));
    RequireExactSlotAccounting(frame);
    REQUIRE(frame.controlSlotCount == 8);
    REQUIRE(frame.transportSlotCount == 7);
    REQUIRE(frame.scheduledEquationCount == 0);
    REQUIRE(frame.inactiveTransportSlotCount == 7);

    const auto bootstrap = MakeBootstrap(controls.sessionTag, 0);
    std::array<pbmodulation::UnifiedFrameSlotInput, pbapp::senderUnifiedCodewordSlotCount> inputs{};
    for (std::size_t slotIndex = 0; slotIndex < frame.slots.size(); slotIndex++)
    {
        const pbapp::SenderUnifiedScheduledSlot& scheduledSlot = frame.slots[slotIndex];
        if (scheduledSlot.assignment.kind == pbmodulation::UnifiedSlotKind::Control)
        {
            const std::span<const std::byte> record = scheduledSlot.assignment.controlPriority ==
                pbmodulation::UnifiedControlPriority::SessionDescriptor ?
                    std::span<const std::byte>(controls.sessionControl) :
                    std::span<const std::byte>(controls.manifestControl);
            inputs[slotIndex] = {scheduledSlot.assignment, true, record};
        }
        else
        {
            inputs[slotIndex] = {scheduledSlot.assignment, false, {}};
        }
    }

    std::vector<std::byte> pixels(pbmodulation::kUnifiedFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeUnifiedVisualFrame({bootstrap, inputs}, pixels));
    auto oracleResult = pbmodulation::UnifiedVisualCpuOracle::Create(
        pbmodulation::UnifiedVisualCpuOracle::RequiredBytes());
    REQUIRE(oracleResult);
    pbmodulation::UnifiedVisualCpuOracle oracle = std::move(oracleResult).Value();
    const pbmodulation::UnifiedExpectedFrameIdentity identity{true, controls.sessionTag, true, 0};
    const pbmodulation::UnifiedVisualObservation observation = oracle.DecodeMixedFrame(MakeView(pixels), identity);
    REQUIRE(observation.IsFrameAvailable());
    REQUIRE(observation.acceptedControlRecords == 8);
    REQUIRE(observation.acceptedTransportBlocks == 0);
    REQUIRE(observation.acceptedBlocks == 8);

    auto receiverResult = pbprotocol::ControlPlaneReceiver::Create(controls.resourcePolicy);
    REQUIRE(receiverResult);
    pbprotocol::ControlPlaneReceiver receiver = std::move(receiverResult).Value();
    REQUIRE(receiver.ActiveControlReassemblyCount() == 0);
    REQUIRE(receiver.ControlReassemblyBytesInUse() == 0);
    std::uint32_t insertedRecords = 0;
    std::uint32_t repeatedRecords = 0;
    for (const pbmodulation::UnifiedAcceptedBlock& accepted : oracle.GetAcceptedBlocks())
    {
        REQUIRE(accepted.kind == pbmodulation::UnifiedSlotKind::Control);
        const auto admission = receiver.ReceiveControlRecord(
            std::span(accepted.bytes).first(accepted.size));
        REQUIRE(admission);
        if (admission.Value().bindDisposition == pbprotocol::DescriptorBindDisposition::Inserted)
        {
            insertedRecords++;
        }
        else
        {
            REQUIRE(admission.Value().bindDisposition == pbprotocol::DescriptorBindDisposition::Repeated);
            repeatedRecords++;
        }
    }
    REQUIRE(insertedRecords == 2);
    REQUIRE(repeatedRecords == 6);
    REQUIRE(receiver.GetSessionDescriptor(controls.sessionTag).Value() == controls.session);
    REQUIRE(receiver.BoundSegmentCount(controls.sessionTag).Value() == 0);
    REQUIRE(receiver.HasFinalManifest(controls.sessionTag).Value());
    REQUIRE(receiver.ValidateCompleteSegmentMap(controls.sessionTag));
    const auto finalManifest = receiver.PrepareFinalization(controls.sessionTag);
    REQUIRE(finalManifest);
    REQUIRE(finalManifest.Value() == controls.manifest);
    REQUIRE(receiver.ActiveControlReassemblyCount() == 0);
    REQUIRE(receiver.ControlReassemblyBytesInUse() == 0);
    REQUIRE(receiver.GetRejectedByResourcePolicyCount() == 0);
    REQUIRE(controls.resourcePolicy.maxControlRecordBytes == originalMaxControlRecordBytes);
    REQUIRE(controls.resourcePolicy.maxControlReassemblyBytes == originalMaxControlReassemblyBytes);

    REQUIRE(scheduler.GetSnapshot().committedFrameCount == 0);
    REQUIRE(scheduler.GetSnapshot().committedEquationCount == 0);
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(scheduler.IsComplete());
    const pbapp::SenderUnifiedCarouselSnapshot completed = scheduler.GetSnapshot();
    REQUIRE(completed.committedFrameCount == 1);
    REQUIRE(completed.committedEquationCount == 0);
    REQUIRE(completed.inactiveTransportSlotCount == 7);
}
