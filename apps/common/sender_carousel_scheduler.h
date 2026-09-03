#pragma once

#include "pbmodulation/unified_visual_profile.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace pbapp
{

inline constexpr std::uint32_t senderCarouselRepairPercentNumerator = 20;
inline constexpr std::uint32_t senderCarouselRepairPercentDenominator = 100;
inline constexpr std::uint32_t senderCarouselMinimumRepairBlocks = 16;
inline constexpr std::uint32_t senderCarouselControlCadenceSeconds = 10;
inline constexpr std::uint32_t senderCarouselMaximumLogicalFramesPerSecond = 240;
inline constexpr std::uint32_t senderCarouselMaximumSystematicBlockCount = 64000;
inline constexpr std::uint32_t senderUnifiedMinimumLogicalFramesPerSecond = 1;
inline constexpr std::uint32_t senderUnifiedMaximumLogicalFramesPerSecond = 60;
inline constexpr std::uint32_t senderUnifiedMaximumControlRepetitions = 64;
inline constexpr std::uint64_t senderLogicalFrameNanosecondsPerSecond = 1000000000ULL;
inline constexpr std::size_t senderUnifiedCodewordSlotCount =
    static_cast<std::size_t>(pbmodulation::kUnifiedFrameCapacity.capacity.codewordCount);

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
    AlreadyComplete,
    LogicalTickRegression,
    FrameAlreadyPrepared,
    NoPreparedFrame
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

enum class SenderLogicalFrameTickDisposition : std::uint8_t
{
    NotDue,
    Ready
};

struct SenderLogicalFrameTick
{
    SenderLogicalFrameTickDisposition disposition = SenderLogicalFrameTickDisposition::NotDue;
    std::uint64_t logicalTickOrdinal = 0;
    std::uint64_t droppedTickCount = 0;

    bool operator==(const SenderLogicalFrameTick&) const = default;
};

struct SenderLogicalFrameClockSnapshot
{
    std::uint64_t nextLogicalTickOrdinal = 0;
    std::uint64_t droppedTickCount = 0;
    bool framePending = false;

    bool operator==(const SenderLogicalFrameClockSnapshot&) const = default;
};

// Pure monotonic clock arithmetic for the Unified sender. Acquire returns at
// most one due tick even if many deadlines elapsed; Commit consumes only that
// latest tick. Therefore missed deadlines are counted and dropped, never
// queued for catch-up generation.
class SenderLogicalFrameClock
{
public:
    [[nodiscard]] static SenderCarouselSchedulerStatus Create(
        std::uint32_t logicalFramesPerSecond, std::uint64_t startNanoseconds,
        SenderLogicalFrameClock& output) noexcept;
    [[nodiscard]] SenderCarouselSchedulerStatus Acquire(
        std::uint64_t nowNanoseconds, SenderLogicalFrameTick& output) noexcept;
    [[nodiscard]] SenderCarouselSchedulerStatus Commit() noexcept;
    [[nodiscard]] SenderLogicalFrameClockSnapshot GetSnapshot() const noexcept;

private:
    std::uint32_t logicalFramesPerSecond_ = 0;
    std::uint64_t startNanoseconds_ = 0;
    std::uint64_t nextLogicalTickOrdinal_ = 0;
    std::uint64_t pendingLogicalTickOrdinal_ = 0;
    std::uint64_t pendingDroppedTickCount_ = 0;
    std::uint64_t droppedTickCount_ = 0;
    bool framePending_ = false;
};

enum class SenderUnifiedTransportSlotDisposition : std::uint8_t
{
    NotTransport,
    ScheduledEquation,
    PaddingDuplicate,
    InactiveZeroByteSession
};

struct SenderUnifiedScheduledSlot
{
    pbmodulation::UnifiedSlotAssignment assignment;
    SenderUnifiedTransportSlotDisposition transportDisposition =
        SenderUnifiedTransportSlotDisposition::NotTransport;
    std::uint64_t equationIndex = 0;

    bool operator==(const SenderUnifiedScheduledSlot&) const = default;
};

struct SenderUnifiedScheduledFrame
{
    std::uint64_t logicalTickOrdinal = 0;
    std::uint64_t firstEquationIndex = 0;
    std::uint32_t scheduledEquationCount = 0;
    std::uint32_t controlSlotCount = 0;
    std::uint32_t transportSlotCount = 0;
    std::uint32_t paddingDuplicateSlotCount = 0;
    std::uint32_t inactiveTransportSlotCount = 0;
    std::array<SenderUnifiedScheduledSlot, senderUnifiedCodewordSlotCount> slots;

    bool operator==(const SenderUnifiedScheduledFrame&) const = default;
};

struct SenderUnifiedCarouselSchedulerConfig
{
    // Zero denotes the formal zero-byte Session: it has Session and Manifest
    // Control records, no SegmentDescriptor and no Data equation.
    std::uint32_t systematicBlockCount = 0;
    std::uint32_t controlRepetitions = 4;
    std::uint32_t logicalFramesPerSecond = 15;
    bool wirehair = false;
};

struct SenderUnifiedCarouselSnapshot
{
    std::uint64_t committedFrameCount = 0;
    std::uint64_t controlBurstCount = 0;
    std::uint64_t controlBearingFrameCount = 0;
    std::uint64_t controlSlotCount = 0;
    std::uint64_t transportSlotCount = 0;
    std::uint64_t scheduledEquationCount = 0;
    std::uint64_t committedEquationCount = 0;
    std::uint64_t systematicEquationCount = 0;
    std::uint64_t repairEquationCount = 0;
    std::uint64_t paddingDuplicateSlotCount = 0;
    std::uint64_t inactiveTransportSlotCount = 0;
    std::uint64_t lastCommittedLogicalTickOrdinal = 0;
    bool hasCommittedLogicalTick = false;
    bool framePrepared = false;
    bool complete = true;

    bool operator==(const SenderUnifiedCarouselSnapshot&) const = default;
};

// Product scheduler for PB-Unified-LC4-V1. It schedules one Segment round, or
// one zero-byte Control round, and never emits a whole-frame Control mode.
// PrepareFrame is idempotent for one tick and freezes the exact 31-slot plan;
// CommitPreparedFrame is the sole state transition and must be called only
// after the complete canonical raster is ready.
class SenderUnifiedCarouselScheduler
{
public:
    [[nodiscard]] static SenderCarouselSchedulerStatus Create(
        const SenderUnifiedCarouselSchedulerConfig& config,
        SenderUnifiedCarouselScheduler& output) noexcept;
    [[nodiscard]] SenderCarouselSchedulerStatus PrepareFrame(
        std::uint64_t logicalTickOrdinal, SenderUnifiedScheduledFrame& output) noexcept;
    [[nodiscard]] SenderCarouselSchedulerStatus CommitPreparedFrame() noexcept;
    [[nodiscard]] SenderUnifiedCarouselSnapshot GetSnapshot() const noexcept;
    [[nodiscard]] bool IsComplete() const noexcept;

private:
    [[nodiscard]] SenderCarouselSchedulerStatus BuildFrame(
        std::uint64_t logicalTickOrdinal, SenderUnifiedScheduledFrame& output) const noexcept;

    SenderUnifiedCarouselSchedulerConfig config_{};
    SenderUnifiedScheduledFrame preparedFrame_{};
    std::uint64_t scheduledEquationCount_ = 0;
    std::uint64_t repairEquationCount_ = 0;
    std::uint64_t committedEquationCount_ = 0;
    std::uint64_t committedFrameCount_ = 0;
    std::uint64_t controlBurstCount_ = 0;
    std::uint64_t controlBearingFrameCount_ = 0;
    std::uint64_t controlSlotCount_ = 0;
    std::uint64_t transportSlotCount_ = 0;
    std::uint64_t paddingDuplicateSlotCount_ = 0;
    std::uint64_t inactiveTransportSlotCount_ = 0;
    std::uint64_t totalControlItemsPerBurst_ = 0;
    std::uint64_t currentControlItemOffset_ = 0;
    std::uint64_t currentControlBurstStartTick_ = 0;
    std::uint64_t nextControlBurstTick_ = 0;
    std::uint64_t lastCommittedLogicalTickOrdinal_ = 0;
    bool inControlBurst_ = true;
    bool currentControlBurstCounted_ = false;
    bool initialControlBurstCompleted_ = false;
    bool hasCommittedLogicalTick_ = false;
    bool framePrepared_ = false;
    bool complete_ = true;
};

[[nodiscard]] const char* GetSenderCarouselSchedulerErrorName(
    SenderCarouselSchedulerError error) noexcept;

} // namespace pbapp
