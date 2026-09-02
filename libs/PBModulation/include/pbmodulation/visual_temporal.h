#pragma once

#include <cstdint>
#include <optional>

namespace pbmodulation
{

struct VisualIdentitySnapshot
{
    std::uint64_t uniqueFrames = 0;
    std::uint64_t duplicateFrames = 0;
    std::uint64_t reorderedFrames = 0;
    std::uint64_t gapEvents = 0;
    std::uint64_t skippedSequences = 0;
    std::optional<double> framesPerSecond;
};

enum class VisualIdentityDisposition : std::uint8_t
{
    Invalid,
    Unique,
    Duplicate,
    Reordered
};

// FrameSequence is authoritative only inside one CaptureEpoch and one observed
// visual-stream identity (normally Bootstrap SessionTag). A new epoch or stream
// identity establishes a fresh sequence baseline while diagnostic counters
// remain cumulative for the owning run. This fixed-state tracker performs no
// allocation and never treats a sequence gap as a reordered observation.
class VisualIdentityTracker
{
public:
    [[nodiscard]] VisualIdentityDisposition Observe(std::uint64_t sequence, std::uint64_t captureEpoch,
        std::int64_t timestamp100ns, std::optional<std::uint64_t> streamIdentity = std::nullopt) noexcept;
    void ResetBaseline() noexcept;
    [[nodiscard]] VisualIdentitySnapshot GetSnapshot() const noexcept;

private:
    std::uint64_t maximumSequence_ = 0;
    std::uint64_t lastCaptureEpoch_ = 0;
    std::int64_t lastTimestamp100ns_ = 0;
    std::uint64_t uniqueFrames_ = 0;
    std::uint64_t duplicateFrames_ = 0;
    std::uint64_t reorderedFrames_ = 0;
    std::uint64_t gapEvents_ = 0;
    std::uint64_t skippedSequences_ = 0;
    std::uint64_t intervalCount_ = 0;
    std::uint64_t intervalTime100ns_ = 0;
    std::optional<std::uint64_t> lastStreamIdentity_;
    bool hasBaseline_ = false;
};

} // namespace pbmodulation
