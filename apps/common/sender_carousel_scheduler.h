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
// Unified Pass 0 uses a 12.5% LocalRepairBurst plus a small fixed transition
// guard. The guard spans more than two ordinary 14-Transport-slot frames, so a
// near-threshold Segment is not abandoned at the bounded-window transition.
// Later passes retain the complete K + 20% FullRepairPass budget for late join
// and loss.
inline constexpr std::uint32_t senderUnifiedInitialRepairPercentNumerator = 1;
inline constexpr std::uint32_t senderUnifiedInitialRepairPercentDenominator = 8;
inline constexpr std::uint32_t senderUnifiedMinimumInitialRepairBlocks = 16;
inline constexpr std::uint32_t senderUnifiedInitialTransitionGuardBlocks = 32;
inline constexpr std::uint32_t senderCarouselControlCadenceSeconds = 10;
inline constexpr std::uint32_t senderCarouselMaximumLogicalFramesPerSecond = 240;
inline constexpr std::uint32_t senderCarouselMaximumSystematicBlockCount = 64000;
inline constexpr std::uint32_t senderUnifiedMinimumLogicalFramesPerSecond = 1;
inline constexpr std::uint32_t senderUnifiedMaximumLogicalFramesPerSecond = 60;
inline constexpr std::uint32_t senderUnifiedMaximumControlRepetitions = 64;
// The certified Unified sender stripes one logical frame at a time across this
// bounded Segment window. The matching Decoder policy and resume cache must
// cover the same count; this is scheduler tuning and does not alter wire data.
inline constexpr std::uint32_t senderUnifiedActiveSegmentWindowSize = 8;
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
    std::uint32_t logicalFramesPerSecond = 0;
    std::uint32_t requestedLogicalFramesPerSecond = 0;
    bool rateChangePending = false;

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
    // A request made while a complete logical frame is pending is committed
    // only after that frame. Re-anchoring at the completion timestamp prevents
    // either a partial-frame cadence switch or a catch-up burst.
    [[nodiscard]] SenderCarouselSchedulerStatus RequestFramesPerSecond(
        std::uint32_t logicalFramesPerSecond, std::uint64_t nowNanoseconds) noexcept;
    [[nodiscard]] SenderCarouselSchedulerStatus Commit(std::uint64_t completedAtNanoseconds) noexcept;
    [[nodiscard]] SenderLogicalFrameClockSnapshot GetSnapshot() const noexcept;

private:
    std::uint32_t logicalFramesPerSecond_ = 0;
    std::uint32_t requestedLogicalFramesPerSecond_ = 0;
    std::uint64_t anchorNanoseconds_ = 0;
    std::uint64_t anchorLogicalTickOrdinal_ = 0;
    std::uint64_t nextLogicalTickOrdinal_ = 0;
    std::uint64_t pendingLogicalTickOrdinal_ = 0;
    std::uint64_t pendingDroppedTickCount_ = 0;
    std::uint64_t droppedTickCount_ = 0;
    bool framePending_ = false;
    bool rateChangePending_ = false;
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
    std::uint64_t repairEquationOffset = 0;
    bool repairEquation = false;

    bool operator==(const SenderUnifiedScheduledSlot&) const = default;
};

struct SenderUnifiedScheduledFrame
{
    std::uint64_t logicalTickOrdinal = 0;
    std::uint64_t firstEquationIndex = 0;
    std::uint32_t scheduledEquationCount = 0;
    // Control slots consumed from the periodic repetition burst. Any remaining
    // Control slot is the mandatory current-Segment prelude for this frame.
    std::uint32_t controlBurstSlotCount = 0;
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
    // Pass 0 includes systematic equations. A later Wirehair FullRepairPass
    // keeps the same K+R budget but maps every equation to a fresh repair ID.
    std::uint64_t carouselPass = 0;
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

// Product scheduler for PB-Unified-SC6-V3. It schedules one Segment round, or
// one zero-byte Control round, and never emits a whole-frame Control mode. Each
// non-empty mixed frame carries its current SegmentDescriptor, so the Decoder's
// Control-before-Transport pass can bind the Segment without orphan admission.
// PrepareFrame is idempotent for one tick and freezes the exact 15-slot plan;
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
    // Runtime cadence uses monotonic time, not the changing FPS or dropped
    // tick ordinal. A pending frame freezes both its plan and timestamp.
    // Do not mix tick-based and time-based calls on the same round.
    [[nodiscard]] SenderCarouselSchedulerStatus PrepareFrameAt(std::uint64_t logicalTickOrdinal,
        std::uint64_t nowNanoseconds, SenderUnifiedScheduledFrame& output) noexcept;
    [[nodiscard]] SenderCarouselSchedulerStatus CommitPreparedFrame() noexcept;
    [[nodiscard]] SenderUnifiedCarouselSnapshot GetSnapshot() const noexcept;
    [[nodiscard]] bool IsComplete() const noexcept;

private:
    [[nodiscard]] SenderCarouselSchedulerStatus BuildFrame(
        std::uint64_t logicalTickOrdinal, std::uint64_t cadencePosition,
        SenderUnifiedScheduledFrame& output) const noexcept;
    [[nodiscard]] SenderCarouselSchedulerStatus PrepareFrameInternal(std::uint64_t logicalTickOrdinal,
        std::uint64_t cadencePosition, bool monotonicCadence, SenderUnifiedScheduledFrame& output) noexcept;

    SenderUnifiedCarouselSchedulerConfig config_{};
    SenderUnifiedScheduledFrame preparedFrame_{};
    std::uint64_t scheduledEquationCount_ = 0;
    std::uint64_t systematicEquationCount_ = 0;
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
    std::uint64_t currentControlBurstStartPosition_ = 0;
    std::uint64_t nextControlBurstPosition_ = 0;
    std::uint64_t lastCommittedLogicalTickOrdinal_ = 0;
    std::uint64_t preparedCadencePosition_ = 0;
    std::uint64_t lastCommittedCadencePosition_ = 0;
    bool monotonicCadence_ = false;
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
