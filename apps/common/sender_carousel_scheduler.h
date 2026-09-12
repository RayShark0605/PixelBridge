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
// Unified Pass 0 uses a 10% LocalRepairBurst plus a small fixed transition
// guard. The guard spans more than two ordinary 14-Transport-slot frames, so a
// near-threshold Segment is not abandoned at the bounded-window transition.
// Later Wirehair passes are incremental fountain repair passes with a doubling
// budget: Pass 1 schedules only max(16, ceil(K*20%)) fresh repair equations
// and never re-broadcasts the K systematic equations, so a Carousel re-sweep
// of an already-recovered Segment costs one small batch; each later pass
// doubles the fraction (capped at 160% of K) so high-erasure Segments converge
// within a few wraps instead of many minimal wraps.
inline constexpr std::uint32_t senderUnifiedInitialRepairPercentNumerator = 1;
inline constexpr std::uint32_t senderUnifiedInitialRepairPercentDenominator = 10;
inline constexpr std::uint32_t senderUnifiedMinimumInitialRepairBlocks = 16;
inline constexpr std::uint32_t senderUnifiedInitialTransitionGuardBlocks = 32;
// Startup keeps four interleaved copies. Periodic refresh carries one complete
// Session/Manifest/current-Segment triplet in a single mixed frame; every other
// non-empty frame still carries its current SegmentDescriptor. A striped
// window of W Segments therefore repeats Session/Manifest W times per refresh
// interval, without paying four copies per Segment on top of the per-frame
// prelude.
inline constexpr std::uint32_t senderUnifiedPeriodicControlRepetitions = 1;
inline constexpr std::uint32_t senderCarouselControlCadenceSeconds = 10;
inline constexpr std::uint32_t senderCarouselMaximumLogicalFramesPerSecond = 240;
inline constexpr std::uint32_t senderCarouselMaximumSystematicBlockCount = 64000;
inline constexpr std::uint32_t senderUnifiedMinimumLogicalFramesPerSecond = 1;
inline constexpr std::uint32_t senderUnifiedMaximumLogicalFramesPerSecond = 60;
inline constexpr std::uint32_t senderUnifiedMaximumControlRepetitions = 64;
// The certified Unified sender stripes one logical frame at a time across this
// bounded Segment window and rotates the sweep start to avoid fixed capture-
// phase aliasing. The window is sender-side scheduling tuning and is strictly
// smaller than the Receiver decoder quota below: the headroom lets starved
// Segments from earlier windows keep their decoders while the current window
// streams, instead of deferring their late repair equations. Wire data is
// unaffected; resume state and the Decoder policy cover the larger count.
inline constexpr std::uint32_t senderUnifiedActiveSegmentWindowSize = 6;
// Receiver-side concurrent outer-FEC decoder limit for the Unified family.
// Kept at the historical eight-decoder reservation so ReceiverResourcePolicy,
// resume caches, and decoder-memory budgeting do not shrink with the sender
// window.
inline constexpr std::uint32_t senderUnifiedReceiverActiveDecoderLimit = 8;
// Repair-pass budget shape is frame-rate aware. At or below this rate the
// receiver's admission budget saturates on the Pass-0 stream alone, so later
// passes keep the historical K+20% FullRepairPass budget (field data: cheap
// multi-wrap fountain passes only added duplicate admissions at 15 Hz).
// Above it, later passes use the incremental doubling fountain budget.
inline constexpr std::uint32_t senderUnifiedSerialRepairFpsThreshold = 15;
// One sweep gives every active Segment one frame. Thirty-two sweeps per phase
// visit every capture phase twice within an ordinary full-size Pass-0 window,
// rather than leaving half the phases unvisited until its repair tail.
inline constexpr std::uint32_t senderUnifiedSweepPhaseHold = 32;
inline constexpr std::uint32_t senderUnifiedSweepPhaseStep = 1;
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
    // Pass 0 includes systematic equations. A later Wirehair pass schedules
    // only its incremental fountain repair budget, all on fresh repair IDs.
    std::uint64_t carouselPass = 0;
    // Shift only the first periodic deadline, after the unchanged startup
    // burst. Runtime assigns one phase per active Segment; no extra repeat,
    // catch-up queue, wire field or Receiver feedback is introduced.
    std::uint32_t periodicControlPhaseIndex = 0;
    std::uint32_t periodicControlPhaseCount = 1;
    // Zero keeps the pass-formula budget. When positive, a Wirehair pass
    // schedules exactly this many fresh repair equations (still bounded by
    // the Create-time consistency checks); the runtime uses it to graduate a
    // Segment at a precise cumulative-equation target instead of absorbing
    // the doubling formula's coarse last-pass overshoot.
    std::uint64_t repairBudgetOverride = 0;
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
    std::uint64_t periodicControlItemsPerBurst_ = 0;
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
