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
        }
    }
    REQUIRE(pbmodulation::ValidateUnifiedMixedSlotPlan(assignments));
    REQUIRE(controlSlots == frame.controlSlotCount);
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
            if (logicalTickOrdinal % (logicalFramesPerSecond * pbapp::senderCarouselControlCadenceSeconds) == 0)
            {
                REQUIRE(frame.controlSlotCount == 12);
                REQUIRE(frame.transportSlotCount == 19);
                for (std::uint32_t slot = 0; slot < 4; slot++)
                {
                    REQUIRE(frame.slots[slot].assignment.controlPriority ==
                        pbmodulation::UnifiedControlPriority::SessionDescriptor);
                    REQUIRE(frame.slots[slot + 4].assignment.controlPriority ==
                        pbmodulation::UnifiedControlPriority::FinalManifest);
                    REQUIRE(frame.slots[slot + 8].assignment.controlPriority ==
                        pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor);
                }
                controlBearingFrames++;
            }
            else
            {
                REQUIRE(frame.controlSlotCount == 0);
                REQUIRE(frame.transportSlotCount == pbapp::senderUnifiedCodewordSlotCount);
            }
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
        REQUIRE(snapshot.controlSlotCount == 36);
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
    REQUIRE(firstFrame.scheduledEquationCount == 19);
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(clock.Commit(startNanoseconds));

    REQUIRE(clock.Acquire(startNanoseconds + 5 * pbapp::senderLogicalFrameNanosecondsPerSecond, tick));
    REQUIRE(tick.logicalTickOrdinal == 300);
    REQUIRE(tick.droppedTickCount == 299);
    pbapp::SenderUnifiedScheduledFrame afterStall;
    REQUIRE(scheduler.PrepareFrame(tick.logicalTickOrdinal, afterStall));
    REQUIRE(afterStall.controlSlotCount == 0);
    REQUIRE(afterStall.firstEquationIndex == 19);
    REQUIRE(afterStall.scheduledEquationCount == 31);
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(clock.Commit(startNanoseconds + 5 * pbapp::senderLogicalFrameNanosecondsPerSecond));
    REQUIRE(clock.GetSnapshot().droppedTickCount == 299);
    pbapp::SenderLogicalFrameTick noCatchUp;
    REQUIRE(clock.Acquire(startNanoseconds + 5 * pbapp::senderLogicalFrameNanosecondsPerSecond, noCatchUp));
    REQUIRE(noCatchUp.disposition == pbapp::SenderLogicalFrameTickDisposition::NotDue);
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
        REQUIRE(snapshot.committedFrameCount == 2);
        REQUIRE(snapshot.controlBurstCount == 1);
        REQUIRE(snapshot.controlSlotCount == 12);
        REQUIRE(snapshot.transportSlotCount == 50);
        REQUIRE(snapshot.scheduledEquationCount == 33);
        REQUIRE(snapshot.committedEquationCount == 33);
        REQUIRE(snapshot.paddingDuplicateSlotCount == 17);
        REQUIRE(snapshot.repairEquationCount == 0);
        REQUIRE(expectedEquationIndex == 33);
    }
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

    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create(
        {1, pbapp::senderUnifiedMaximumControlRepetitions, 1, false}, scheduler));
    std::uint64_t logicalTickOrdinal = 0;
    std::uint64_t scheduledEquationSlots = 0;
    while (!scheduler.IsComplete())
    {
        pbapp::SenderUnifiedScheduledFrame frame;
        REQUIRE(scheduler.PrepareFrame(logicalTickOrdinal, frame));
        RequireExactSlotAccounting(frame);
        REQUIRE(frame.controlSlotCount == pbmodulation::GetUnifiedMaximumControlSlots());
        REQUIRE(frame.transportSlotCount == 15);
        REQUIRE(frame.scheduledEquationCount == static_cast<std::uint32_t>(logicalTickOrdinal == 0));
        scheduledEquationSlots += frame.scheduledEquationCount;
        REQUIRE(scheduler.CommitPreparedFrame());
        logicalTickOrdinal++;
    }
    const pbapp::SenderUnifiedCarouselSnapshot snapshot = scheduler.GetSnapshot();
    REQUIRE(snapshot.committedFrameCount == 12);
    REQUIRE(snapshot.controlSlotCount == 192);
    REQUIRE(snapshot.transportSlotCount == 180);
    REQUIRE(snapshot.committedEquationCount == 1);
    REQUIRE(snapshot.paddingDuplicateSlotCount == 179);
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
    REQUIRE(scheduler.GetSnapshot().committedEquationCount == 19);
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
    REQUIRE(frame.transportSlotCount == 23);
    REQUIRE(frame.scheduledEquationCount == 0);
    REQUIRE(frame.inactiveTransportSlotCount == 23);

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
    REQUIRE(completed.inactiveTransportSlotCount == 23);
}
