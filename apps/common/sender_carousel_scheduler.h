#pragma once

#include <cstdint>

namespace pbapp
{

inline constexpr std::uint32_t senderCarouselRepairPercentNumerator = 20;
inline constexpr std::uint32_t senderCarouselRepairPercentDenominator = 100;
inline constexpr std::uint32_t senderCarouselMinimumRepairBlocks = 16;
inline constexpr std::uint32_t senderCarouselControlCadenceSeconds = 10;
inline constexpr std::uint32_t senderCarouselMaximumLogicalFramesPerSecond = 240;
inline constexpr std::uint32_t senderCarouselMaximumSystematicBlockCount = 64000;

enum class SenderScheduledFrameKind : std::uint8_t
{
    SessionControl,
    ManifestControl,
    SegmentControl,
    Data
};

enum class SenderCarouselSchedulerError : std::uint8_t
{
    None,
    InvalidConfiguration,
    ArithmeticOverflow,
    AlreadyComplete
};

struct SenderCarouselSchedulerStatus
{
    SenderCarouselSchedulerError code = SenderCarouselSchedulerError::None;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code == SenderCarouselSchedulerError::None;
    }

    [[nodiscard]] static SenderCarouselSchedulerStatus Failure(
        const SenderCarouselSchedulerError code) noexcept
    {
        return {code};
    }
};

struct SenderCarouselSchedulerConfig
{
    std::uint32_t systematicBlockCount = 0;
    std::uint32_t dataSlotsPerFrame = 0;
    std::uint32_t controlRepetitions = 0;
    std::uint32_t logicalFramesPerSecond = 0;
    bool wirehair = false;
};

struct SenderScheduledFrame
{
    SenderScheduledFrameKind kind = SenderScheduledFrameKind::SessionControl;
    std::uint64_t firstEquationIndex = 0;
    std::uint32_t scheduledEquationCount = 0;
};

struct SenderCarouselRoundSnapshot
{
    std::uint64_t frameIndex = 0;
    std::uint64_t totalFrameCount = 0;
    std::uint64_t dataFrameCount = 0;
    std::uint64_t controlBurstCount = 0;
    std::uint64_t scheduledEquationCount = 0;
    std::uint64_t systematicEquationCount = 0;
    std::uint64_t repairEquationCount = 0;
    std::uint64_t paddingDuplicateSlotCount = 0;
};

// Schedules one Segment round without knowing pixels, codeword placement, or
// the final mixed-slot layout. The caller may map one Data event to any
// physical carrier, but it must preserve the returned equation order and must
// not allocate a new repair ID for padding slots.
class SenderCarouselScheduler
{
public:
    [[nodiscard]] static SenderCarouselSchedulerStatus Create(
        const SenderCarouselSchedulerConfig& config,
        SenderCarouselScheduler& output) noexcept;

    [[nodiscard]] SenderCarouselSchedulerStatus GetCurrentFrame(
        SenderScheduledFrame& output) const noexcept;
    [[nodiscard]] SenderCarouselSchedulerStatus Advance() noexcept;
    [[nodiscard]] SenderCarouselRoundSnapshot GetSnapshot() const noexcept;
    [[nodiscard]] bool IsComplete() const noexcept;

private:
    SenderCarouselSchedulerConfig config_{};
    std::uint64_t totalFrameCount_ = 0;
    std::uint64_t dataFrameCount_ = 0;
    std::uint64_t controlFramesPerBurst_ = 0;
    std::uint64_t controlBurstCount_ = 0;
    std::uint64_t dataFramesBetweenControlBursts_ = 0;
    std::uint64_t scheduledEquationCount_ = 0;
    std::uint64_t repairEquationCount_ = 0;
    std::uint64_t paddingDuplicateSlotCount_ = 0;
    std::uint64_t frameIndex_ = 0;
    std::uint64_t completedDataFrames_ = 0;
    std::uint64_t controlFrameIndex_ = 0;
    bool inControlBurst_ = true;
    bool complete_ = true;
};

[[nodiscard]] const char* GetSenderCarouselSchedulerErrorName(
    SenderCarouselSchedulerError error) noexcept;

} // namespace pbapp
