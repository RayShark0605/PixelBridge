#pragma once

#include <cstdint>
#include <limits>

namespace phase1gate
{

inline constexpr std::uint64_t minimumUniqueVisualCadenceIntervals = 60;

[[nodiscard]] constexpr bool HasMinimumUniqueVisualCadence(const std::uint64_t cadenceIntervals) noexcept
{
    return cadenceIntervals >= minimumUniqueVisualCadenceIntervals;
}

enum class VisualIdentityDisposition : std::uint8_t
{
    Unique,
    Duplicate,
    Reordered
};

enum class SessionIdentityDisposition : std::uint8_t
{
    Unbound,
    Matching,
    Foreign
};

// The file Gate owns exactly one output Session. Before its authoritative
// SessionDescriptor arrives, a Bootstrap identity is not enough to bind output
// state. After binding, every other SessionTag is an erasure and must not enter
// Control, Transport, FER, or UniqueVisualFPS accounting.
[[nodiscard]] constexpr SessionIdentityDisposition ClassifySessionIdentity(const bool hasActiveSession,
    const std::uint64_t activeSessionTag, const std::uint64_t observedSessionTag) noexcept
{
    if (!hasActiveSession)
    {
        return SessionIdentityDisposition::Unbound;
    }
    return activeSessionTag == observedSessionTag ?
        SessionIdentityDisposition::Matching : SessionIdentityDisposition::Foreign;
}

struct UniqueVisualRateSnapshot
{
    std::uint64_t uniqueFrames = 0;
    std::uint64_t duplicateFrames = 0;
    std::uint64_t reorderedFrames = 0;
    std::uint64_t cadenceIntervals = 0;
    std::uint64_t cadenceTime100ns = 0;
    std::uint64_t gapEvents = 0;
    std::uint64_t skippedSequences = 0;
    std::uint64_t captureEpochBoundaries = 0;
    bool operator==(const UniqueVisualRateSnapshot&) const = default;
};

struct SenderPreparationDecision
{
    bool buildFrame = false;
    bool submitFrame = false;
    bool operator==(const SenderPreparationDecision&) const = default;
};

// Frame construction is deliberately allowed while DataWindow owns a copied
// pending frame. Serializing reference FEC/raster construction after vsync
// would make this Gate harness, rather than the physical path, cap the
// receiver-observed unique cadence. Submission still waits for the one-frame
// pending slot and therefore cannot replace queued visuals.
[[nodiscard]] constexpr SenderPreparationDecision GetSenderPreparationDecision(
    const bool running, const bool frameBuilt, const bool pendingFrame) noexcept
{
    return {running && !frameBuilt, running && !pendingFrame};
}

// Measures receiver-observed unique visual cadence without allowing an
// intentional CaptureEpoch recovery or an erased/dropped sequence gap to
// masquerade as a slow display cadence. Only adjacent, strictly increasing
// FrameSequence observations in the same CaptureEpoch contribute a timing
// interval. Duplicates and reordering remain explicit counters. Failure is
// atomic and leaves both the accumulator and disposition output unchanged.
class UniqueVisualRateAccumulator
{
public:
    [[nodiscard]] bool Observe(const std::uint64_t frameSequence, const std::uint64_t captureEpoch,
        const std::int64_t captureMonotonic100ns, VisualIdentityDisposition& disposition) noexcept
    {
        if (captureEpoch == 0 || captureMonotonic100ns < 0)
        {
            return false;
        }
        UniqueVisualRateAccumulator next = *this;
        VisualIdentityDisposition nextDisposition = VisualIdentityDisposition::Unique;
        if (next.hasMaximum_ && captureEpoch < next.lastUniqueEpoch_)
        {
            return false;
        }
        if (!next.hasMaximum_)
        {
            if (!Increment(next.snapshot_.uniqueFrames))
            {
                return false;
            }
            next.hasMaximum_ = true;
            next.maximumSequence_ = frameSequence;
            next.lastUniqueEpoch_ = captureEpoch;
            next.lastUniqueTimestamp100ns_ = captureMonotonic100ns;
        }
        else if (frameSequence > next.maximumSequence_)
        {
            if (captureEpoch == next.lastUniqueEpoch_ && captureMonotonic100ns <= next.lastUniqueTimestamp100ns_)
            {
                return false;
            }
            const std::uint64_t sequenceDistance = frameSequence - next.maximumSequence_;
            if (captureEpoch != next.lastUniqueEpoch_)
            {
                if (!Increment(next.snapshot_.captureEpochBoundaries))
                {
                    return false;
                }
            }
            else if (sequenceDistance == 1)
            {
                const std::uint64_t elapsed100ns = static_cast<std::uint64_t>(
                    captureMonotonic100ns - next.lastUniqueTimestamp100ns_);
                if (!Increment(next.snapshot_.cadenceIntervals) ||
                    !Add(next.snapshot_.cadenceTime100ns, elapsed100ns))
                {
                    return false;
                }
            }
            else
            {
                if (!Increment(next.snapshot_.gapEvents) ||
                    !Add(next.snapshot_.skippedSequences, sequenceDistance - 1))
                {
                    return false;
                }
            }
            if (!Increment(next.snapshot_.uniqueFrames))
            {
                return false;
            }
            next.maximumSequence_ = frameSequence;
            next.lastUniqueEpoch_ = captureEpoch;
            next.lastUniqueTimestamp100ns_ = captureMonotonic100ns;
        }
        else if (frameSequence == next.maximumSequence_)
        {
            nextDisposition = VisualIdentityDisposition::Duplicate;
            if (!Increment(next.snapshot_.duplicateFrames))
            {
                return false;
            }
        }
        else
        {
            nextDisposition = VisualIdentityDisposition::Reordered;
            if (!Increment(next.snapshot_.reorderedFrames))
            {
                return false;
            }
        }
        *this = next;
        disposition = nextDisposition;
        return true;
    }

    [[nodiscard]] UniqueVisualRateSnapshot GetSnapshot() const noexcept
    {
        return snapshot_;
    }

    [[nodiscard]] double GetCadenceFps() const noexcept
    {
        return snapshot_.cadenceIntervals == 0 || snapshot_.cadenceTime100ns == 0 ? 0 :
            static_cast<double>(snapshot_.cadenceIntervals) * 10000000.0 /
                static_cast<double>(snapshot_.cadenceTime100ns);
    }

private:
    [[nodiscard]] static bool Increment(std::uint64_t& value) noexcept
    {
        if (value == std::numeric_limits<std::uint64_t>::max())
        {
            return false;
        }
        value++;
        return true;
    }

    [[nodiscard]] static bool Add(std::uint64_t& destination, const std::uint64_t value) noexcept
    {
        if (value > std::numeric_limits<std::uint64_t>::max() - destination)
        {
            return false;
        }
        destination += value;
        return true;
    }

    UniqueVisualRateSnapshot snapshot_;
    std::uint64_t maximumSequence_ = 0;
    std::uint64_t lastUniqueEpoch_ = 0;
    std::int64_t lastUniqueTimestamp100ns_ = 0;
    bool hasMaximum_ = false;
};

} // namespace phase1gate
