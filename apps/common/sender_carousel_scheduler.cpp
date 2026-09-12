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
static_assert(senderUnifiedInitialRepairPercentNumerator != 0);
static_assert(senderUnifiedInitialRepairPercentDenominator % senderUnifiedInitialRepairPercentNumerator == 0);
static_assert(senderUnifiedSweepPhaseHold != 0);
static_assert(senderUnifiedSweepPhaseStep != 0 && senderUnifiedSweepPhaseStep < senderUnifiedActiveSegmentWindowSize);

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
    const std::uint32_t repairPercentNumerator, const std::uint32_t repairPercentDenominator,
    const std::uint32_t minimumRepairBlocks, const std::uint32_t additionalRepairBlocks,
    std::uint64_t& scheduledEquationCount, std::uint64_t& repairEquationCount) noexcept
{
    repairEquationCount = wirehair ?
        (std::max)(static_cast<std::uint64_t>(minimumRepairBlocks),
            (static_cast<std::uint64_t>(systematicBlockCount) - 1ULL) /
                (repairPercentDenominator / repairPercentNumerator) + 1ULL) : 0;
    if ((wirehair && !AssignChecked(pbprotocol::CheckedAddUint64(
            repairEquationCount, additionalRepairBlocks), repairEquationCount)) ||
        !AssignChecked(pbprotocol::CheckedAddUint64(systematicBlockCount, repairEquationCount),
        scheduledEquationCount))
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }
    return {};
}

[[nodiscard]] pbmodulation::UnifiedControlPriority GetControlPriority(
    const std::uint64_t controlItemIndex, const std::uint64_t controlRecordKindCount) noexcept
{
    // Interleave record kinds before repeating one kind. With the product's four repetitions and
    // eight-slot cap, either Control-bearing frame contains every descriptor despite periodic frame decimation.
    const std::uint64_t recordIndex = controlItemIndex % controlRecordKindCount;
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
        config.systematicBlockCount, config.wirehair, senderCarouselRepairPercentNumerator,
        senderCarouselRepairPercentDenominator, senderCarouselMinimumRepairBlocks,
        0, scheduledEquationCount, repairEquationCount);
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
        config.periodicControlPhaseCount == 0 ||
        config.periodicControlPhaseCount > senderUnifiedActiveSegmentWindowSize ||
        config.periodicControlPhaseIndex >= config.periodicControlPhaseCount ||
        (config.wirehair && config.systematicBlockCount < 2))
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::InvalidConfiguration);
    }

    std::uint64_t scheduledEquationCount = 0;
    std::uint64_t systematicEquationCount = config.systematicBlockCount;
    std::uint64_t repairEquationCount = 0;
    if (config.systematicBlockCount != 0)
    {
        if (config.wirehair && config.carouselPass != 0)
        {
            if (config.logicalFramesPerSecond <= senderUnifiedSerialRepairFpsThreshold)
            {
                // Historical FullRepairPass: one K+20% batch of fresh repair
                // equations per pass. At low frame rates the receiver's
                // admission budget saturates on the Pass-0 stream alone, and
                // field data showed cheap multi-wrap fountain passes only add
                // duplicate admissions there (findings document section 11.7).
                const SenderCarouselSchedulerStatus equationStatus = CalculateEquationCounts(
                    config.systematicBlockCount, config.wirehair, senderCarouselRepairPercentNumerator,
                    senderCarouselRepairPercentDenominator, senderCarouselMinimumRepairBlocks, 0,
                    scheduledEquationCount, repairEquationCount);
                if (!equationStatus)
                {
                    return equationStatus;
                }
                systematicEquationCount = 0;
                repairEquationCount = scheduledEquationCount;
            }
            else
            {
                // Incremental fountain repair pass with a doubling budget.
                // Pass 1 schedules only max(16, ceil(K*20%)) fresh repair
                // equations, so a Carousel re-sweep of an already-recovered
                // Segment costs one small batch instead of a full K-sized pass.
                // Each later pass doubles the fraction (20% -> 40% -> 80%,
                // capped at 160%) so a Segment whose erasure deficit exceeds
                // one batch converges within a few wraps instead of many
                // minimal wraps whose re-visits of recovered Segments dominate
                // the airtime.
                const std::uint64_t doublingShift = (std::min<std::uint64_t>)(config.carouselPass - 1ULL, 3ULL);
                const auto scaledNumerator = pbprotocol::CheckedMultiplyUint64(
                    senderCarouselRepairPercentNumerator, 1ULL << doublingShift);
                const auto scaledRepair = scaledNumerator ?
                    pbprotocol::CheckedMultiplyUint64(config.systematicBlockCount - 1ULL, scaledNumerator.Value()) :
                    scaledNumerator;
                if (!scaledNumerator || !scaledRepair)
                {
                    return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
                }
                repairEquationCount = (std::max)(static_cast<std::uint64_t>(senderCarouselMinimumRepairBlocks),
                    scaledRepair.Value() / senderCarouselRepairPercentDenominator + 1ULL);
                if (config.repairBudgetOverride != 0)
                {
                    if (config.repairBudgetOverride > config.systematicBlockCount * 16ULL)
                    {
                        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::InvalidConfiguration);
                    }
                    repairEquationCount = (std::max)(static_cast<std::uint64_t>(senderCarouselMinimumRepairBlocks),
                        config.repairBudgetOverride);
                }
                scheduledEquationCount = repairEquationCount;
                systematicEquationCount = 0;
            }
        }
        else
        {
            const SenderCarouselSchedulerStatus equationStatus = CalculateEquationCounts(
                config.systematicBlockCount, config.wirehair, senderUnifiedInitialRepairPercentNumerator,
                senderUnifiedInitialRepairPercentDenominator, senderUnifiedMinimumInitialRepairBlocks,
                senderUnifiedInitialTransitionGuardBlocks, scheduledEquationCount, repairEquationCount);
            if (!equationStatus)
            {
                return equationStatus;
            }
        }
    }
    const std::uint64_t controlRecordKindCount = config.systematicBlockCount == 0 ? 2 : 3;
    std::uint64_t totalControlItemsPerBurst = 0;
    std::uint64_t periodicControlItemsPerBurst = 0;
    if (!AssignChecked(pbprotocol::CheckedMultiplyUint64(
        controlRecordKindCount, config.controlRepetitions), totalControlItemsPerBurst) ||
        !AssignChecked(pbprotocol::CheckedMultiplyUint64(controlRecordKindCount,
            senderUnifiedPeriodicControlRepetitions), periodicControlItemsPerBurst) ||
        totalControlItemsPerBurst == 0 || periodicControlItemsPerBurst == 0)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }

    SenderUnifiedCarouselScheduler scheduler;
    scheduler.config_ = config;
    scheduler.scheduledEquationCount_ = scheduledEquationCount;
    scheduler.systematicEquationCount_ = systematicEquationCount;
    scheduler.repairEquationCount_ = repairEquationCount;
    scheduler.totalControlItemsPerBurst_ = totalControlItemsPerBurst;
    scheduler.periodicControlItemsPerBurst_ = periodicControlItemsPerBurst;
    scheduler.complete_ = false;
    output = scheduler;
    return {};
}

SenderCarouselSchedulerStatus SenderUnifiedCarouselScheduler::BuildFrame(
    const std::uint64_t logicalTickOrdinal, const std::uint64_t cadencePosition,
    SenderUnifiedScheduledFrame& output) const noexcept
{
    if (complete_)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::AlreadyComplete);
    }
    if (hasCommittedLogicalTick_ && logicalTickOrdinal <= lastCommittedLogicalTickOrdinal_)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::LogicalTickRegression);
    }

    const bool controlBurstActive = inControlBurst_ || cadencePosition >= nextControlBurstPosition_;
    const std::uint64_t controlItemOffset = inControlBurst_ ? currentControlItemOffset_ : 0;
    const std::uint64_t currentBurstItems = initialControlBurstCompleted_ ?
        periodicControlItemsPerBurst_ : totalControlItemsPerBurst_;
    if (controlItemOffset > currentBurstItems)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }
    const std::uint64_t remainingControlItems = controlBurstActive ?
        currentBurstItems - controlItemOffset : 0;
    const std::uint32_t maximumControlSlots = pbmodulation::GetUnifiedMaximumControlSlots();
    const std::uint32_t controlBurstSlotCount = static_cast<std::uint32_t>((std::min)(
        remainingControlItems, static_cast<std::uint64_t>(maximumControlSlots)));
    const std::uint32_t descriptorPreludeSlotCount =
        !controlBurstActive && config_.systematicBlockCount != 0 ? 1U : 0U;
    const std::uint32_t controlSlotCount = controlBurstSlotCount + descriptorPreludeSlotCount;
    const std::uint32_t codewordCount = static_cast<std::uint32_t>(
        pbmodulation::kUnifiedFrameCapacity.capacity.codewordCount);
    const std::uint64_t controlRecordKindCount = config_.systematicBlockCount == 0 ? 2 : 3;
    if ((controlBurstActive && controlBurstSlotCount == 0) || controlSlotCount >= codewordCount)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
    }

    SenderUnifiedScheduledFrame frame;
    frame.logicalTickOrdinal = logicalTickOrdinal;
    frame.firstEquationIndex = committedEquationCount_;
    frame.controlBurstSlotCount = controlBurstSlotCount;
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
                codewordSlot < controlBurstSlotCount ?
                    GetControlPriority(controlItemOffset + codewordSlot, controlRecordKindCount) :
                    pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor};
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
            slot.repairEquation = nextEquationIndex >= systematicEquationCount_;
            slot.repairEquationOffset = slot.repairEquation ? nextEquationIndex - systematicEquationCount_ : 0;
            nextEquationIndex++;
            frame.scheduledEquationCount++;
        }
        else
        {
            slot.transportDisposition = SenderUnifiedTransportSlotDisposition::PaddingDuplicate;
            // Tail padding re-broadcasts the lowest IDs this round already
            // scheduled: the systematic range on pass 0, this round's own
            // repair range on a repair-only pass. No new repair ID is allocated
            // and the repair high-water never advances on padding.
            const std::uint64_t duplicateBase = systematicEquationCount_ != 0 ?
                systematicEquationCount_ : repairEquationCount_;
            slot.equationIndex = (paddingDuplicateSlotCount_ + localPaddingDuplicateCount) % duplicateBase;
            slot.repairEquation = systematicEquationCount_ == 0;
            slot.repairEquationOffset = slot.repairEquation ? slot.equationIndex : 0;
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
        frame.controlBurstSlotCount > frame.controlSlotCount ||
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
    return PrepareFrameInternal(logicalTickOrdinal, logicalTickOrdinal, false, output);
}

SenderCarouselSchedulerStatus SenderUnifiedCarouselScheduler::PrepareFrameAt(
    const std::uint64_t logicalTickOrdinal, const std::uint64_t nowNanoseconds,
    SenderUnifiedScheduledFrame& output) noexcept
{
    return PrepareFrameInternal(logicalTickOrdinal, nowNanoseconds, true, output);
}

SenderCarouselSchedulerStatus SenderUnifiedCarouselScheduler::PrepareFrameInternal(
    const std::uint64_t logicalTickOrdinal, const std::uint64_t cadencePosition,
    const bool monotonicCadence, SenderUnifiedScheduledFrame& output) noexcept
{
    if ((framePrepared_ || hasCommittedLogicalTick_) && monotonicCadence_ != monotonicCadence)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::InvalidConfiguration);
    }
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
    if (hasCommittedLogicalTick_ && cadencePosition < lastCommittedCadencePosition_)
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::LogicalTickRegression);
    }
    const SenderCarouselSchedulerStatus status = BuildFrame(logicalTickOrdinal, cadencePosition, frame);
    if (!status)
    {
        return status;
    }
    preparedFrame_ = frame;
    preparedCadencePosition_ = cadencePosition;
    monotonicCadence_ = monotonicCadence;
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
    std::uint64_t currentControlBurstStartPosition = currentControlBurstStartPosition_;
    std::uint64_t nextControlBurstPosition = nextControlBurstPosition_;
    std::uint64_t controlBurstCount = controlBurstCount_;
    const std::uint64_t currentBurstItems = initialControlBurstCompleted_ ?
        periodicControlItemsPerBurst_ : totalControlItemsPerBurst_;
    if (preparedFrame_.controlBurstSlotCount != 0)
    {
        if (!inControlBurst)
        {
            inControlBurst = true;
            currentControlItemOffset = 0;
            currentControlBurstCounted = false;
        }
        if (!currentControlBurstCounted)
        {
            currentControlBurstStartPosition = preparedCadencePosition_;
            if (!AssignChecked(pbprotocol::CheckedAddUint64(controlBurstCount, 1), controlBurstCount))
            {
                return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
            }
            currentControlBurstCounted = true;
        }
        if (!AssignChecked(pbprotocol::CheckedAddUint64(
            currentControlItemOffset, preparedFrame_.controlBurstSlotCount), currentControlItemOffset) ||
            currentControlItemOffset > currentBurstItems)
        {
            return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
        }
        if (currentControlItemOffset == currentBurstItems)
        {
            const std::uint64_t cadenceDistance = (monotonicCadence_ ? senderLogicalFrameNanosecondsPerSecond :
                static_cast<std::uint64_t>(config_.logicalFramesPerSecond)) *
                senderCarouselControlCadenceSeconds;
            std::uint64_t nextCadenceDistance = cadenceDistance;
            if (!initialControlBurstCompleted_)
            {
                std::uint64_t phaseProduct = 0;
                if (!AssignChecked(pbprotocol::CheckedMultiplyUint64(cadenceDistance,
                    config_.periodicControlPhaseIndex), phaseProduct) ||
                    !AssignChecked(pbprotocol::CheckedAddUint64(cadenceDistance,
                        phaseProduct / config_.periodicControlPhaseCount), nextCadenceDistance))
                {
                    return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
                }
            }
            if (!AssignChecked(pbprotocol::CheckedAddUint64(
                currentControlBurstStartPosition, nextCadenceDistance), nextControlBurstPosition))
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
    currentControlBurstStartPosition_ = currentControlBurstStartPosition;
    nextControlBurstPosition_ = nextControlBurstPosition;
    controlBurstCount_ = controlBurstCount;
    committedEquationCount_ = committedEquationCount;
    committedFrameCount_ = committedFrameCount;
    controlBearingFrameCount_ = controlBearingFrameCount;
    controlSlotCount_ = controlSlotCount;
    transportSlotCount_ = transportSlotCount;
    paddingDuplicateSlotCount_ = paddingDuplicateSlotCount;
    inactiveTransportSlotCount_ = inactiveTransportSlotCount;
    lastCommittedLogicalTickOrdinal_ = preparedFrame_.logicalTickOrdinal;
    lastCommittedCadencePosition_ = preparedCadencePosition_;
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
        transportSlotCount_, scheduledEquationCount_, committedEquationCount_, systematicEquationCount_,
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
