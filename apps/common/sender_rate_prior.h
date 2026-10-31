#pragma once

#include <cstdint>
#include <optional>

namespace pbapp
{

// Historical Decoder raw-byte average, in the GUI's 1024-byte KB units.
// Sender-only scheduling policy: never a receipt, bandwidth or prefix claim.
struct SenderRatePrior
{
    std::uint32_t minimumKibPerSecond = 200;
    std::uint32_t maximumKibPerSecond = 280;
    bool operator==(const SenderRatePrior&) const = default;
};

enum class SenderRatePriorRegion : std::uint8_t
{
    BeforeFrontier,
    NearFrontier,
    AheadOfFrontier,
    SafetyRepair
};

struct SenderRatePriorDecision
{
    std::uint64_t lowerRawBytes = 0;
    std::uint64_t upperRawBytes = 0;
    std::uint64_t visitEquationBudget = 0;
    SenderRatePriorRegion region = SenderRatePriorRegion::SafetyRepair;
};

struct SenderRatePriorSnapshot
{
    std::optional<SenderRatePrior> configured;
    bool effective = false;
    std::uint64_t activeNanoseconds = 0;
    std::uint64_t clampedSenderGaps = 0;
    std::uint64_t predictedLowerRawBytes = 0;
    std::uint64_t predictedUpperRawBytes = 0;
    std::uint64_t beforeFrontierEquations = 0;
    std::uint64_t nearFrontierEquations = 0;
    std::uint64_t aheadOfFrontierEquations = 0;
    std::uint64_t safetyRepairEquations = 0;
};

[[nodiscard]] bool ValidateSenderRatePrior(const SenderRatePrior& prior) noexcept;
[[nodiscard]] std::optional<SenderRatePriorDecision> CalculateSenderRatePriorDecision(const SenderRatePrior& prior,
    std::uint64_t activeNanoseconds, std::uint64_t rawOffset, std::uint64_t rawSize, std::uint64_t fileBytes,
    std::uint32_t blockCount, bool initialVisit, std::uint64_t relativeCarouselPass) noexcept;

// Only successfully submitted frames advance this clock. Preparation/retry
// never earns time or equation credit. Locally observed gaps above one second
// are capped, so a known sender suspension cannot fast-forward the prior.
class SenderRatePriorClock
{
public:
    [[nodiscard]] bool CommitFrame(std::uint64_t nowNanoseconds) noexcept;
    [[nodiscard]] std::uint64_t GetActiveNanoseconds() const noexcept
    {
        return activeNanoseconds_;
    }
    [[nodiscard]] std::uint64_t GetClampedGapCount() const noexcept
    {
        return clampedGapCount_;
    }

private:
    std::optional<std::uint64_t> lastCommittedNanoseconds_;
    std::uint64_t activeNanoseconds_ = 0;
    std::uint64_t clampedGapCount_ = 0;
};

}
