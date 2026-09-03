#include "sender_carousel_scheduler.h"

#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <limits>

namespace pbapp
{
namespace
{

static_assert(senderCarouselRepairPercentNumerator != 0);
static_assert(senderCarouselRepairPercentDenominator % senderCarouselRepairPercentNumerator == 0);

[[nodiscard]] bool AssignChecked(
    const pbprotocol::ProtocolResult<std::uint64_t>& result,
    std::uint64_t& output) noexcept
{
    if (!result)
    {
        return false;
    }
    output = result.Value();
    return true;
}

[[nodiscard]] std::uint64_t CalculateLogicalFrameIntervalNanoseconds(
    const std::uint32_t logicalFramesPerSecond) noexcept
{
    return 1ULL + (senderLogicalFrameNanosecondsPerSecond - 1ULL) / logicalFramesPerSecond;
}

[[nodiscard]] SenderCarouselSchedulerStatus CalculateEquationCounts(
    const std::uint32_t systematicBlockCount, const bool wirehair,
    std::uint64_t& scheduledEquationCount, std::uint64_t& repairEquationCount) noexcept
{
    repairEquationCount = wirehair ?
        (std::max)(static_cast<std::uint64_t>(senderCarouselMinimumRepairBlocks),
            (static_cast<std::uint64_t>(systematicBlockCount) - 1ULL) /
                (senderCarouselRepairPercentDenominator / senderCarouselRepairPercentNumerator) + 1ULL) : 0;
    if (!AssignChecked(pbprotocol::CheckedAddUint64(systematicBlockCount, repairEquationCount),
        scheduledEquationCount))
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }
    return {};
}

[[nodiscard]] pbmodulation::UnifiedControlPriority GetControlPriority(
    const std::uint64_t controlItemIndex, const std::uint32_t controlRepetitions) noexcept
{
    const std::uint64_t recordIndex = controlItemIndex / controlRepetitions;
    if (recordIndex == 0)
    {
        return pbmodulation::UnifiedControlPriority::SessionDescriptor;
    }
    if (recordIndex == 1)
    {
        return pbmodulation::UnifiedControlPriority::FinalManifest;
    }
    return pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor;
}

} // namespace

SenderCarouselSchedulerStatus SenderCarouselScheduler::Create(
    const SenderCarouselSchedulerConfig& config,
    SenderCarouselScheduler& output) noexcept
{
    output = {};
    if (config.systematicBlockCount == 0 ||
        config.systematicBlockCount > senderCarouselMaximumSystematicBlockCount ||
        config.dataSlotsPerFrame == 0 || config.controlRepetitions == 0 ||
        config.logicalFramesPerSecond > senderCarouselMaximumLogicalFramesPerSecond ||
        (config.wirehair && config.systematicBlockCount < 2))
    {
        return SenderCarouselSchedulerStatus::Failure(
            SenderCarouselSchedulerError::InvalidConfiguration);
    }

    std::uint64_t repairEquationCount = 0;
    std::uint64_t scheduledEquationCount = 0;
    const SenderCarouselSchedulerStatus equationStatus = CalculateEquationCounts(
        config.systematicBlockCount, config.wirehair, scheduledEquationCount, repairEquationCount);
    if (!equationStatus)
    {
        return equationStatus;
    }
    const std::uint64_t dataFrameCount = 1ULL +
        (scheduledEquationCount - 1ULL) / config.dataSlotsPerFrame;

    std::uint64_t controlCadenceFrames = 0;
    if (config.logicalFramesPerSecond != 0 &&
        !AssignChecked(pbprotocol::CheckedMultiplyUint64(config.logicalFramesPerSecond,
            senderCarouselControlCadenceSeconds), controlCadenceFrames))
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }
    std::uint64_t controlFramesPerBurst = 0;
    if (!AssignChecked(pbprotocol::CheckedMultiplyUint64(config.controlRepetitions, 3),
        controlFramesPerBurst))
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }
    // Cadence is measured from the first frame of one Control burst to the
    // first frame of the next. If repetitions already consume the complete
    // cadence budget, one Data frame is still required between bursts so a
    // low-rate sender cannot starve its payload forever.
    const std::uint64_t dataFramesBetweenControlBursts = controlCadenceFrames == 0 ? 0 :
        controlCadenceFrames > controlFramesPerBurst ? controlCadenceFrames - controlFramesPerBurst : 1;
    const std::uint64_t periodicControlBurstCount = dataFramesBetweenControlBursts == 0 ? 0 :
        (dataFrameCount - 1ULL) / dataFramesBetweenControlBursts;
    std::uint64_t controlBurstCount = 0;
    std::uint64_t totalControlFrameCount = 0;
    std::uint64_t totalFrameCount = 0;
    std::uint64_t physicalDataSlotCount = 0;
    if (!AssignChecked(pbprotocol::CheckedAddUint64(1, periodicControlBurstCount), controlBurstCount) ||
        !AssignChecked(pbprotocol::CheckedMultiplyUint64(controlBurstCount, controlFramesPerBurst),
            totalControlFrameCount) ||
        !AssignChecked(pbprotocol::CheckedAddUint64(totalControlFrameCount, dataFrameCount), totalFrameCount) ||
        !AssignChecked(pbprotocol::CheckedMultiplyUint64(dataFrameCount, config.dataSlotsPerFrame),
            physicalDataSlotCount) || totalFrameCount > (std::numeric_limits<std::uint32_t>::max)())
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }

    SenderCarouselScheduler scheduler;
    scheduler.config_ = config;
    scheduler.totalFrameCount_ = totalFrameCount;
    scheduler.dataFrameCount_ = dataFrameCount;
    scheduler.controlFramesPerBurst_ = controlFramesPerBurst;
    scheduler.controlBurstCount_ = controlBurstCount;
    scheduler.dataFramesBetweenControlBursts_ = dataFramesBetweenControlBursts;
    scheduler.scheduledEquationCount_ = scheduledEquationCount;
    scheduler.repairEquationCount_ = repairEquationCount;
    scheduler.paddingDuplicateSlotCount_ = physicalDataSlotCount - scheduledEquationCount;
    scheduler.complete_ = false;
    output = scheduler;
    return {};
}

SenderCarouselSchedulerStatus SenderCarouselScheduler::GetCurrentFrame(
    SenderScheduledFrame& output) const noexcept
{
    if (complete_)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::AlreadyComplete);
    }
    SenderScheduledFrame frame;
    if (inControlBurst_)
    {
        const std::uint64_t controlRecordIndex = controlFrameIndex_ / config_.controlRepetitions;
        frame.kind = controlRecordIndex == 0 ? SenderScheduledFrameKind::SessionControl :
            controlRecordIndex == 1 ? SenderScheduledFrameKind::ManifestControl :
            SenderScheduledFrameKind::SegmentControl;
    }
    else
    {
        frame.kind = SenderScheduledFrameKind::Data;
        frame.firstEquationIndex = completedDataFrames_ * config_.dataSlotsPerFrame;
        const std::uint64_t remainingEquations = scheduledEquationCount_ - frame.firstEquationIndex;
        frame.scheduledEquationCount = static_cast<std::uint32_t>((std::min)(remainingEquations,
            static_cast<std::uint64_t>(config_.dataSlotsPerFrame)));
    }
    output = frame;
    return {};
}

SenderCarouselSchedulerStatus SenderCarouselScheduler::Advance() noexcept
{
    if (complete_)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::AlreadyComplete);
    }
    frameIndex_++;
    if (inControlBurst_)
    {
        controlFrameIndex_++;
        if (controlFrameIndex_ == controlFramesPerBurst_)
        {
            controlFrameIndex_ = 0;
            inControlBurst_ = false;
        }
    }
    else
    {
        completedDataFrames_++;
        if (completedDataFrames_ == dataFrameCount_)
        {
            complete_ = true;
        }
        else if (dataFramesBetweenControlBursts_ != 0 &&
            completedDataFrames_ % dataFramesBetweenControlBursts_ == 0)
        {
            inControlBurst_ = true;
        }
    }
    if ((complete_ && frameIndex_ != totalFrameCount_) ||
        (!complete_ && frameIndex_ >= totalFrameCount_))
    {
        complete_ = true;
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }
    return {};
}

SenderCarouselRoundSnapshot SenderCarouselScheduler::GetSnapshot() const noexcept
{
    return {frameIndex_, totalFrameCount_, dataFrameCount_, controlBurstCount_,
        scheduledEquationCount_, config_.systematicBlockCount, repairEquationCount_,
        paddingDuplicateSlotCount_};
}

bool SenderCarouselScheduler::IsComplete() const noexcept
{
    return complete_;
}

SenderCarouselSchedulerStatus SenderLogicalFrameClock::Create(
    const std::uint32_t logicalFramesPerSecond, const std::uint64_t startNanoseconds,
    SenderLogicalFrameClock& output) noexcept
{
    output = {};
    if (logicalFramesPerSecond < senderUnifiedMinimumLogicalFramesPerSecond ||
        logicalFramesPerSecond > senderUnifiedMaximumLogicalFramesPerSecond)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::InvalidConfiguration);
    }
    SenderLogicalFrameClock clock;
    clock.logicalFramesPerSecond_ = logicalFramesPerSecond;
    clock.requestedLogicalFramesPerSecond_ = logicalFramesPerSecond;
    clock.anchorNanoseconds_ = startNanoseconds;
    output = clock;
    return {};
}

SenderCarouselSchedulerStatus SenderLogicalFrameClock::Acquire(
    const std::uint64_t nowNanoseconds, SenderLogicalFrameTick& output) noexcept
{
    output = {};
    if (framePending_)
    {
        output = {SenderLogicalFrameTickDisposition::Ready,
            pendingLogicalTickOrdinal_, pendingDroppedTickCount_};
        return {};
    }
    if (logicalFramesPerSecond_ == 0)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::InvalidConfiguration);
    }
    if (nowNanoseconds < anchorNanoseconds_)
    {
        return {};
    }
    const std::uint64_t elapsedNanoseconds = nowNanoseconds - anchorNanoseconds_;
    const std::uint64_t elapsedWholeSeconds = elapsedNanoseconds / senderLogicalFrameNanosecondsPerSecond;
    const std::uint64_t elapsedFractionNanoseconds = elapsedNanoseconds % senderLogicalFrameNanosecondsPerSecond;
    std::uint64_t wholeSecondTicks = 0;
    if (!AssignChecked(pbprotocol::CheckedMultiplyUint64(
        elapsedWholeSeconds, logicalFramesPerSecond_), wholeSecondTicks))
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }
    const std::uint64_t fractionTicks =
        elapsedFractionNanoseconds * logicalFramesPerSecond_ / senderLogicalFrameNanosecondsPerSecond;
    std::uint64_t relativeDueTick = 0;
    std::uint64_t latestDueTick = 0;
    if (!AssignChecked(pbprotocol::CheckedAddUint64(wholeSecondTicks, fractionTicks), relativeDueTick) ||
        !AssignChecked(pbprotocol::CheckedAddUint64(anchorLogicalTickOrdinal_, relativeDueTick), latestDueTick))
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }
    if (latestDueTick < nextLogicalTickOrdinal_)
    {
        return {};
    }
    pendingLogicalTickOrdinal_ = latestDueTick;
    pendingDroppedTickCount_ = latestDueTick - nextLogicalTickOrdinal_;
    framePending_ = true;
    output = {SenderLogicalFrameTickDisposition::Ready,
        pendingLogicalTickOrdinal_, pendingDroppedTickCount_};
    return {};
}

SenderCarouselSchedulerStatus SenderLogicalFrameClock::RequestFramesPerSecond(
    const std::uint32_t logicalFramesPerSecond, const std::uint64_t nowNanoseconds) noexcept
{
    if (logicalFramesPerSecond < senderUnifiedMinimumLogicalFramesPerSecond ||
        logicalFramesPerSecond > senderUnifiedMaximumLogicalFramesPerSecond)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::InvalidConfiguration);
    }
    if (framePending_)
    {
        requestedLogicalFramesPerSecond_ = logicalFramesPerSecond;
        rateChangePending_ = requestedLogicalFramesPerSecond_ != logicalFramesPerSecond_;
        return {};
    }
    if (logicalFramesPerSecond == logicalFramesPerSecond_)
    {
        requestedLogicalFramesPerSecond_ = logicalFramesPerSecond;
        rateChangePending_ = false;
        return {};
    }
    const std::uint64_t intervalNanoseconds = CalculateLogicalFrameIntervalNanoseconds(logicalFramesPerSecond);
    std::uint64_t nextDeadline = 0;
    if (!AssignChecked(pbprotocol::CheckedAddUint64(nowNanoseconds, intervalNanoseconds), nextDeadline))
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }
    logicalFramesPerSecond_ = logicalFramesPerSecond;
    requestedLogicalFramesPerSecond_ = logicalFramesPerSecond;
    anchorNanoseconds_ = nextDeadline;
    anchorLogicalTickOrdinal_ = nextLogicalTickOrdinal_;
    rateChangePending_ = false;
    return {};
}

SenderCarouselSchedulerStatus SenderLogicalFrameClock::Commit(const std::uint64_t completedAtNanoseconds) noexcept
{
    if (!framePending_)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::NoPreparedFrame);
    }
    std::uint64_t nextLogicalTickOrdinal = 0;
    std::uint64_t droppedTickCount = 0;
    if (!AssignChecked(pbprotocol::CheckedAddUint64(pendingLogicalTickOrdinal_, 1), nextLogicalTickOrdinal) ||
        !AssignChecked(pbprotocol::CheckedAddUint64(droppedTickCount_, pendingDroppedTickCount_), droppedTickCount))
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }
    std::uint64_t nextAnchorNanoseconds = anchorNanoseconds_;
    if (rateChangePending_)
    {
        const std::uint64_t intervalNanoseconds =
            CalculateLogicalFrameIntervalNanoseconds(requestedLogicalFramesPerSecond_);
        if (!AssignChecked(pbprotocol::CheckedAddUint64(
            completedAtNanoseconds, intervalNanoseconds), nextAnchorNanoseconds))
        {
            return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
        }
    }
    nextLogicalTickOrdinal_ = nextLogicalTickOrdinal;
    droppedTickCount_ = droppedTickCount;
    pendingLogicalTickOrdinal_ = 0;
    pendingDroppedTickCount_ = 0;
    framePending_ = false;
    if (rateChangePending_)
    {
        logicalFramesPerSecond_ = requestedLogicalFramesPerSecond_;
        anchorNanoseconds_ = nextAnchorNanoseconds;
        anchorLogicalTickOrdinal_ = nextLogicalTickOrdinal_;
        rateChangePending_ = false;
    }
    return {};
}

SenderLogicalFrameClockSnapshot SenderLogicalFrameClock::GetSnapshot() const noexcept
{
    return {nextLogicalTickOrdinal_, droppedTickCount_, framePending_, logicalFramesPerSecond_,
        requestedLogicalFramesPerSecond_, rateChangePending_};
}

SenderCarouselSchedulerStatus SenderUnifiedCarouselScheduler::Create(
    const SenderUnifiedCarouselSchedulerConfig& config,
    SenderUnifiedCarouselScheduler& output) noexcept
{
    output = {};
    if (config.systematicBlockCount > senderCarouselMaximumSystematicBlockCount ||
        config.controlRepetitions == 0 ||
        config.controlRepetitions > senderUnifiedMaximumControlRepetitions ||
        config.logicalFramesPerSecond < senderUnifiedMinimumLogicalFramesPerSecond ||
        config.logicalFramesPerSecond > senderUnifiedMaximumLogicalFramesPerSecond ||
        (config.wirehair && config.systematicBlockCount < 2))
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::InvalidConfiguration);
    }

    std::uint64_t scheduledEquationCount = 0;
    std::uint64_t repairEquationCount = 0;
    if (config.systematicBlockCount != 0)
    {
        const SenderCarouselSchedulerStatus equationStatus = CalculateEquationCounts(
            config.systematicBlockCount, config.wirehair, scheduledEquationCount, repairEquationCount);
        if (!equationStatus)
        {
            return equationStatus;
        }
    }
    const std::uint64_t controlRecordKindCount = config.systematicBlockCount == 0 ? 2 : 3;
    std::uint64_t totalControlItemsPerBurst = 0;
    if (!AssignChecked(pbprotocol::CheckedMultiplyUint64(
        controlRecordKindCount, config.controlRepetitions), totalControlItemsPerBurst) ||
        totalControlItemsPerBurst == 0)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }

    SenderUnifiedCarouselScheduler scheduler;
    scheduler.config_ = config;
    scheduler.scheduledEquationCount_ = scheduledEquationCount;
    scheduler.repairEquationCount_ = repairEquationCount;
    scheduler.totalControlItemsPerBurst_ = totalControlItemsPerBurst;
    scheduler.complete_ = false;
    output = scheduler;
    return {};
}

SenderCarouselSchedulerStatus SenderUnifiedCarouselScheduler::BuildFrame(
    const std::uint64_t logicalTickOrdinal, SenderUnifiedScheduledFrame& output) const noexcept
{
    if (complete_)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::AlreadyComplete);
    }
    if (hasCommittedLogicalTick_ && logicalTickOrdinal <= lastCommittedLogicalTickOrdinal_)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::LogicalTickRegression);
    }

    const bool controlBurstActive = inControlBurst_ || logicalTickOrdinal >= nextControlBurstTick_;
    const std::uint64_t controlItemOffset = inControlBurst_ ? currentControlItemOffset_ : 0;
    const std::uint64_t remainingControlItems = controlBurstActive ?
        totalControlItemsPerBurst_ - controlItemOffset : 0;
    const std::uint32_t maximumControlSlots = pbmodulation::GetUnifiedMaximumControlSlots();
    const std::uint32_t controlSlotCount = static_cast<std::uint32_t>((std::min)(
        remainingControlItems, static_cast<std::uint64_t>(maximumControlSlots)));
    const std::uint32_t codewordCount = static_cast<std::uint32_t>(
        pbmodulation::kUnifiedFrameCapacity.capacity.codewordCount);
    if ((controlBurstActive && controlSlotCount == 0) || controlSlotCount >= codewordCount)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }

    SenderUnifiedScheduledFrame frame;
    frame.logicalTickOrdinal = logicalTickOrdinal;
    frame.firstEquationIndex = committedEquationCount_;
    frame.controlSlotCount = controlSlotCount;
    frame.transportSlotCount = codewordCount - controlSlotCount;
    std::uint64_t nextEquationIndex = committedEquationCount_;
    std::uint64_t localPaddingDuplicateCount = 0;
    for (std::uint32_t codewordSlot = 0; codewordSlot < codewordCount; codewordSlot++)
    {
        SenderUnifiedScheduledSlot& slot = frame.slots[codewordSlot];
        if (codewordSlot < controlSlotCount)
        {
            slot.assignment = {codewordSlot, pbmodulation::UnifiedSlotKind::Control,
                GetControlPriority(controlItemOffset + codewordSlot, config_.controlRepetitions)};
            slot.transportDisposition = SenderUnifiedTransportSlotDisposition::NotTransport;
            continue;
        }
        slot.assignment = {codewordSlot, pbmodulation::UnifiedSlotKind::Transport,
            pbmodulation::UnifiedControlPriority::NotApplicable};
        if (config_.systematicBlockCount == 0)
        {
            slot.transportDisposition = SenderUnifiedTransportSlotDisposition::InactiveZeroByteSession;
            frame.inactiveTransportSlotCount++;
        }
        else if (nextEquationIndex < scheduledEquationCount_)
        {
            slot.transportDisposition = SenderUnifiedTransportSlotDisposition::ScheduledEquation;
            slot.equationIndex = nextEquationIndex;
            nextEquationIndex++;
            frame.scheduledEquationCount++;
        }
        else
        {
            slot.transportDisposition = SenderUnifiedTransportSlotDisposition::PaddingDuplicate;
            slot.equationIndex = (paddingDuplicateSlotCount_ + localPaddingDuplicateCount) %
                config_.systematicBlockCount;
            localPaddingDuplicateCount++;
            frame.paddingDuplicateSlotCount++;
        }
    }
    std::array<pbmodulation::UnifiedSlotAssignment, senderUnifiedCodewordSlotCount> assignments{};
    for (std::size_t slotIndex = 0; slotIndex < frame.slots.size(); slotIndex++)
    {
        assignments[slotIndex] = frame.slots[slotIndex].assignment;
    }
    if (!pbmodulation::ValidateUnifiedMixedSlotPlan(assignments) ||
        frame.controlSlotCount + frame.transportSlotCount != frame.slots.size())
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::InvalidConfiguration);
    }
    output = frame;
    return {};
}

SenderCarouselSchedulerStatus SenderUnifiedCarouselScheduler::PrepareFrame(
    const std::uint64_t logicalTickOrdinal, SenderUnifiedScheduledFrame& output) noexcept
{
    if (framePrepared_)
    {
        if (logicalTickOrdinal != preparedFrame_.logicalTickOrdinal)
        {
            return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::FrameAlreadyPrepared);
        }
        output = preparedFrame_;
        return {};
    }
    SenderUnifiedScheduledFrame frame;
    const SenderCarouselSchedulerStatus status = BuildFrame(logicalTickOrdinal, frame);
    if (!status)
    {
        return status;
    }
    preparedFrame_ = frame;
    framePrepared_ = true;
    output = frame;
    return {};
}

SenderCarouselSchedulerStatus SenderUnifiedCarouselScheduler::CommitPreparedFrame() noexcept
{
    if (!framePrepared_)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::NoPreparedFrame);
    }

    bool inControlBurst = inControlBurst_;
    bool currentControlBurstCounted = currentControlBurstCounted_;
    bool initialControlBurstCompleted = initialControlBurstCompleted_;
    std::uint64_t currentControlItemOffset = currentControlItemOffset_;
    std::uint64_t currentControlBurstStartTick = currentControlBurstStartTick_;
    std::uint64_t nextControlBurstTick = nextControlBurstTick_;
    std::uint64_t controlBurstCount = controlBurstCount_;
    if (preparedFrame_.controlSlotCount != 0)
    {
        if (!inControlBurst)
        {
            inControlBurst = true;
            currentControlItemOffset = 0;
            currentControlBurstCounted = false;
        }
        if (!currentControlBurstCounted)
        {
            currentControlBurstStartTick = preparedFrame_.logicalTickOrdinal;
            if (!AssignChecked(pbprotocol::CheckedAddUint64(controlBurstCount, 1), controlBurstCount))
            {
                return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
            }
            currentControlBurstCounted = true;
        }
        if (!AssignChecked(pbprotocol::CheckedAddUint64(
            currentControlItemOffset, preparedFrame_.controlSlotCount), currentControlItemOffset) ||
            currentControlItemOffset > totalControlItemsPerBurst_)
        {
            return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
        }
        if (currentControlItemOffset == totalControlItemsPerBurst_)
        {
            const std::uint64_t cadenceTicks = static_cast<std::uint64_t>(config_.logicalFramesPerSecond) *
                senderCarouselControlCadenceSeconds;
            if (!AssignChecked(pbprotocol::CheckedAddUint64(
                currentControlBurstStartTick, cadenceTicks), nextControlBurstTick))
            {
                return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
            }
            inControlBurst = false;
            currentControlItemOffset = 0;
            currentControlBurstCounted = false;
            initialControlBurstCompleted = true;
        }
    }

    std::uint64_t committedEquationCount = 0;
    std::uint64_t committedFrameCount = 0;
    std::uint64_t controlBearingFrameCount = controlBearingFrameCount_;
    std::uint64_t controlSlotCount = 0;
    std::uint64_t transportSlotCount = 0;
    std::uint64_t paddingDuplicateSlotCount = 0;
    std::uint64_t inactiveTransportSlotCount = 0;
    if (!AssignChecked(pbprotocol::CheckedAddUint64(
            committedEquationCount_, preparedFrame_.scheduledEquationCount), committedEquationCount) ||
        !AssignChecked(pbprotocol::CheckedAddUint64(committedFrameCount_, 1), committedFrameCount) ||
        !AssignChecked(pbprotocol::CheckedAddUint64(
            controlSlotCount_, preparedFrame_.controlSlotCount), controlSlotCount) ||
        !AssignChecked(pbprotocol::CheckedAddUint64(
            transportSlotCount_, preparedFrame_.transportSlotCount), transportSlotCount) ||
        !AssignChecked(pbprotocol::CheckedAddUint64(
            paddingDuplicateSlotCount_, preparedFrame_.paddingDuplicateSlotCount), paddingDuplicateSlotCount) ||
        !AssignChecked(pbprotocol::CheckedAddUint64(
            inactiveTransportSlotCount_, preparedFrame_.inactiveTransportSlotCount), inactiveTransportSlotCount) ||
        (preparedFrame_.controlSlotCount != 0 &&
            !AssignChecked(pbprotocol::CheckedAddUint64(controlBearingFrameCount_, 1), controlBearingFrameCount)) ||
        committedEquationCount > scheduledEquationCount_)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }

    inControlBurst_ = inControlBurst;
    currentControlBurstCounted_ = currentControlBurstCounted;
    initialControlBurstCompleted_ = initialControlBurstCompleted;
    currentControlItemOffset_ = currentControlItemOffset;
    currentControlBurstStartTick_ = currentControlBurstStartTick;
    nextControlBurstTick_ = nextControlBurstTick;
    controlBurstCount_ = controlBurstCount;
    committedEquationCount_ = committedEquationCount;
    committedFrameCount_ = committedFrameCount;
    controlBearingFrameCount_ = controlBearingFrameCount;
    controlSlotCount_ = controlSlotCount;
    transportSlotCount_ = transportSlotCount;
    paddingDuplicateSlotCount_ = paddingDuplicateSlotCount;
    inactiveTransportSlotCount_ = inactiveTransportSlotCount;
    lastCommittedLogicalTickOrdinal_ = preparedFrame_.logicalTickOrdinal;
    hasCommittedLogicalTick_ = true;
    framePrepared_ = false;
    preparedFrame_ = {};
    complete_ = config_.systematicBlockCount == 0 ?
        initialControlBurstCompleted_ && !inControlBurst_ :
        committedEquationCount_ == scheduledEquationCount_ && !inControlBurst_;
    return {};
}

SenderUnifiedCarouselSnapshot SenderUnifiedCarouselScheduler::GetSnapshot() const noexcept
{
    return {committedFrameCount_, controlBurstCount_, controlBearingFrameCount_, controlSlotCount_,
        transportSlotCount_, scheduledEquationCount_, committedEquationCount_, config_.systematicBlockCount,
        repairEquationCount_, paddingDuplicateSlotCount_, inactiveTransportSlotCount_,
        lastCommittedLogicalTickOrdinal_, hasCommittedLogicalTick_, framePrepared_, complete_};
}

bool SenderUnifiedCarouselScheduler::IsComplete() const noexcept
{
    return complete_;
}

const char* GetSenderCarouselSchedulerErrorName(const SenderCarouselSchedulerError error) noexcept
{
    switch (error)
    {
    case SenderCarouselSchedulerError::None:
        return "None";
    case SenderCarouselSchedulerError::InvalidConfiguration:
        return "InvalidConfiguration";
    case SenderCarouselSchedulerError::ArithmeticOverflow:
        return "ArithmeticOverflow";
    case SenderCarouselSchedulerError::AlreadyComplete:
        return "AlreadyComplete";
    case SenderCarouselSchedulerError::LogicalTickRegression:
        return "LogicalTickRegression";
    case SenderCarouselSchedulerError::FrameAlreadyPrepared:
        return "FrameAlreadyPrepared";
    case SenderCarouselSchedulerError::NoPreparedFrame:
        return "NoPreparedFrame";
    }
    return "Unknown";
}

} // namespace pbapp
