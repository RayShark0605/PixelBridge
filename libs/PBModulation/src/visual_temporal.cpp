#include "pbmodulation/visual_temporal.h"

#include <limits>

namespace pbmodulation
{

VisualIdentityDisposition VisualIdentityTracker::Observe(const std::uint64_t sequence,
    const std::uint64_t captureEpoch, const std::int64_t timestamp100ns,
    const std::optional<std::uint64_t> streamIdentity) noexcept
{
    if (captureEpoch == 0 || timestamp100ns < 0)
    {
        return VisualIdentityDisposition::Invalid;
    }
    if (!hasBaseline_ || captureEpoch != lastCaptureEpoch_ || streamIdentity != lastStreamIdentity_)
    {
        hasBaseline_ = true;
        maximumSequence_ = sequence;
        lastCaptureEpoch_ = captureEpoch;
        lastStreamIdentity_ = streamIdentity;
        lastTimestamp100ns_ = timestamp100ns;
        if (uniqueFrames_ != (std::numeric_limits<std::uint64_t>::max)())
        {
            uniqueFrames_++;
        }
        return VisualIdentityDisposition::Unique;
    }
    if (sequence == maximumSequence_)
    {
        if (duplicateFrames_ != (std::numeric_limits<std::uint64_t>::max)())
        {
            duplicateFrames_++;
        }
        return VisualIdentityDisposition::Duplicate;
    }
    if (sequence < maximumSequence_)
    {
        if (reorderedFrames_ != (std::numeric_limits<std::uint64_t>::max)())
        {
            reorderedFrames_++;
        }
        return VisualIdentityDisposition::Reordered;
    }

    const std::uint64_t distance = sequence - maximumSequence_;
    if (distance == 1 && timestamp100ns > lastTimestamp100ns_)
    {
        if (intervalCount_ != (std::numeric_limits<std::uint64_t>::max)())
        {
            intervalCount_++;
        }
        const std::uint64_t interval = static_cast<std::uint64_t>(timestamp100ns - lastTimestamp100ns_);
        intervalTime100ns_ = interval > (std::numeric_limits<std::uint64_t>::max)() - intervalTime100ns_ ?
            (std::numeric_limits<std::uint64_t>::max)() : intervalTime100ns_ + interval;
    }
    else if (distance > 1)
    {
        if (gapEvents_ != (std::numeric_limits<std::uint64_t>::max)())
        {
            gapEvents_++;
        }
        const std::uint64_t skipped = distance - 1;
        skippedSequences_ = skipped > (std::numeric_limits<std::uint64_t>::max)() - skippedSequences_ ?
            (std::numeric_limits<std::uint64_t>::max)() : skippedSequences_ + skipped;
    }
    maximumSequence_ = sequence;
    lastTimestamp100ns_ = timestamp100ns;
    if (uniqueFrames_ != (std::numeric_limits<std::uint64_t>::max)())
    {
        uniqueFrames_++;
    }
    return VisualIdentityDisposition::Unique;
}

VisualIdentitySnapshot VisualIdentityTracker::GetSnapshot() const noexcept
{
    VisualIdentitySnapshot snapshot;
    snapshot.uniqueFrames = uniqueFrames_;
    snapshot.duplicateFrames = duplicateFrames_;
    snapshot.reorderedFrames = reorderedFrames_;
    snapshot.gapEvents = gapEvents_;
    snapshot.skippedSequences = skippedSequences_;
    if (intervalCount_ != 0 && intervalTime100ns_ != 0)
    {
        snapshot.framesPerSecond = static_cast<double>(intervalCount_) * 10000000.0 /
            static_cast<double>(intervalTime100ns_);
    }
    return snapshot;
}

void VisualIdentityTracker::ResetBaseline() noexcept
{
    maximumSequence_ = 0;
    lastCaptureEpoch_ = 0;
    lastTimestamp100ns_ = 0;
    lastStreamIdentity_.reset();
    hasBaseline_ = false;
}

} // namespace pbmodulation
