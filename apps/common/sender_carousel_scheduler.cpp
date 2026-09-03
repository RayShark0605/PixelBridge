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

    const std::uint64_t repairEquationCount = config.wirehair ?
        (std::max)(static_cast<std::uint64_t>(senderCarouselMinimumRepairBlocks),
            (static_cast<std::uint64_t>(config.systematicBlockCount) - 1ULL) /
                (senderCarouselRepairPercentDenominator / senderCarouselRepairPercentNumerator) + 1ULL) : 0;
    std::uint64_t scheduledEquationCount = 0;
    if (!AssignChecked(pbprotocol::CheckedAddUint64(config.systematicBlockCount, repairEquationCount),
        scheduledEquationCount))
    {
        return SenderCarouselSchedulerStatus::Failure(SenderCarouselSchedulerError::ArithmeticOverflow);
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
    }
    return "Unknown";
}

} // namespace pbapp
