#include "sender_rate_prior.h"

#include <algorithm>
#include <limits>

namespace pbapp
{
namespace
{

constexpr std::uint64_t nanosecondsPerSecond = 1000000000;

[[nodiscard]] std::uint64_t CalculateBoundedRawFrontier(const std::uint32_t kibPerSecond,
    const std::uint64_t activeNanoseconds, const std::uint64_t fileBytes) noexcept
{
    const std::uint64_t bytesPerSecond = static_cast<std::uint64_t>(kibPerSecond) * 1024;
    const std::uint64_t seconds = activeNanoseconds / nanosecondsPerSecond;
    if (seconds > fileBytes / bytesPerSecond)
    {
        return fileBytes;
    }
    const std::uint64_t wholeBytes = seconds * bytesPerSecond;
    // Both factors are bounded (rate <= 1 GiB/s, fraction < 1 s).
    const std::uint64_t fractionBytes = (activeNanoseconds % nanosecondsPerSecond) * bytesPerSecond / nanosecondsPerSecond;
    return fractionBytes >= fileBytes - wholeBytes ? fileBytes : wholeBytes + fractionBytes;
}

[[nodiscard]] std::uint64_t RoundedBudget(const std::uint32_t blockCount,
    const std::uint64_t numerator, const std::uint64_t denominator) noexcept
{
    return (std::max)(16ULL, (static_cast<std::uint64_t>(blockCount) * numerator + denominator - 1) / denominator);
}

}

bool ValidateSenderRatePrior(const SenderRatePrior& prior) noexcept
{
    return prior.minimumKibPerSecond != 0 && prior.minimumKibPerSecond <= prior.maximumKibPerSecond &&
        prior.maximumKibPerSecond <= 1048576;
}

std::optional<SenderRatePriorDecision> CalculateSenderRatePriorDecision(const SenderRatePrior& prior,
    const std::uint64_t activeNanoseconds, const std::uint64_t rawOffset, const std::uint64_t rawSize,
    const std::uint64_t fileBytes, const std::uint32_t blockCount, const bool initialVisit,
    const std::uint64_t relativeCarouselPass) noexcept
{
    if (!ValidateSenderRatePrior(prior) || fileBytes == 0 || rawSize == 0 || rawOffset >= fileBytes ||
        rawSize > fileBytes - rawOffset || blockCount == 0 || blockCount > 64000)
    {
        return std::nullopt;
    }
    SenderRatePriorDecision decision;
    decision.lowerRawBytes = CalculateBoundedRawFrontier(prior.minimumKibPerSecond, activeNanoseconds, fileBytes);
    decision.upperRawBytes = CalculateBoundedRawFrontier(prior.maximumKibPerSecond, activeNanoseconds, fileBytes);
    if (relativeCarouselPass % 4 == 3 || decision.lowerRawBytes == fileBytes)
    {
        // Every fourth complete sweep and all post-prediction sweeps retain
        // substantial all-file repair service, including arbitrarily old gaps.
        decision.region = SenderRatePriorRegion::SafetyRepair;
        decision.visitEquationBudget = RoundedBudget(blockCount, initialVisit ? 2 : 5, initialVisit ? 1 : 4);
    }
    else if (rawOffset + rawSize <= decision.lowerRawBytes)
    {
        decision.region = SenderRatePriorRegion::BeforeFrontier;
        decision.visitEquationBudget = RoundedBudget(blockCount, initialVisit ? 5 : 1, initialVisit ? 4 : 16);
    }
    else if (rawOffset <= decision.upperRawBytes)
    {
        decision.region = SenderRatePriorRegion::NearFrontier;
        decision.visitEquationBudget = RoundedBudget(blockCount, initialVisit ? 7 : 1, initialVisit ? 4 : 2);
    }
    else
    {
        decision.region = SenderRatePriorRegion::AheadOfFrontier;
        decision.visitEquationBudget = RoundedBudget(blockCount, initialVisit ? 2 : 1, initialVisit ? 1 : 8);
    }
    return decision;
}

bool SenderRatePriorClock::CommitFrame(const std::uint64_t nowNanoseconds) noexcept
{
    if (lastCommittedNanoseconds_)
    {
        if (nowNanoseconds < *lastCommittedNanoseconds_)
        {
            return false;
        }
        const std::uint64_t gap = nowNanoseconds - *lastCommittedNanoseconds_;
        const std::uint64_t credited = (std::min)(gap, nanosecondsPerSecond);
        if (credited > (std::numeric_limits<std::uint64_t>::max)() - activeNanoseconds_ ||
            (gap > credited && clampedGapCount_ == (std::numeric_limits<std::uint64_t>::max)()))
        {
            return false;
        }
        activeNanoseconds_ += credited;
        clampedGapCount_ += static_cast<std::uint64_t>(gap > credited);
    }
    lastCommittedNanoseconds_ = nowNanoseconds;
    return true;
}

}
