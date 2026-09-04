#pragma once

#include "pbmodulation/local_desktop_decode.h"
#include "pbmodulation/modulation_result.h"
#include "pbmodulation/unified_visual_mapping.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace pbmodulation
{

inline constexpr std::uint8_t kUnifiedDataLowLuma = 80;
inline constexpr std::uint8_t kUnifiedDataHighLuma = 176;
inline constexpr std::uint8_t kUnifiedNeutralLuma = 128;
inline constexpr std::array<std::uint8_t, 4> kUnifiedLumaPilotLevels{32, 80, 176, 224};
inline constexpr std::uint32_t kUnifiedCodewordBytes = kUnifiedVisualProfile.innerCodewordBits / 8;
inline constexpr std::uint32_t kUnifiedInformationBytes = kUnifiedVisualProfile.innerInformationBits / 8;
inline constexpr std::uint32_t kUnifiedCodewordCount = static_cast<std::uint32_t>(kUnifiedFrameCapacity.capacity.codewordCount);
inline constexpr std::size_t kUnifiedCodedFrameBytes = static_cast<std::size_t>(kUnifiedCodewordCount) * kUnifiedCodewordBytes;
inline constexpr std::size_t kUnifiedSoftMetricCount = static_cast<std::size_t>(kUnifiedCodewordCount) * kUnifiedVisualProfile.innerCodewordBits;
inline constexpr std::size_t kUnifiedFrameBgraBytes = static_cast<std::size_t>(kUnifiedVisualProfile.canvasWidth) *
    kUnifiedVisualProfile.canvasHeight * 4;
inline constexpr std::uint32_t kUnifiedDataRegionCount = 10;
inline constexpr std::uint32_t kUnifiedFreshnessRegionCount = 9;

// Calibration regions use this exact vertical micro-layout. The first 24
// rows are four 32-pixel-wide grayscale cells in kUnifiedLumaPilotLevels
// order, the next eight rows are neutral gray, and the final 32 rows are the
// four active chroma labels in numeric label order. Phase checker regions are
// complete 4x4 tiles; their labels are a deterministic function of tile index
// and FrameSequence as implemented by GetUnifiedPhasePilotLabel.
inline constexpr std::uint32_t kUnifiedCalibrationLumaRows = 24;
inline constexpr std::uint32_t kUnifiedCalibrationNeutralRows = 8;
inline constexpr std::uint32_t kUnifiedCalibrationChromaRows = 32;
inline constexpr std::array<std::uint32_t, 2> kUnifiedFreshnessColumnBoundaries{560, 1360};
inline constexpr std::array<std::uint32_t, 2> kUnifiedFreshnessRowBoundaries{382, 698};

struct UnifiedDataTile
{
    bool valid = false;
    UnifiedPixelRegion bounds;
    std::uint8_t dataRegion = 0;
    std::uint8_t freshnessRegion = 0;

    bool operator==(const UnifiedDataTile&) const = default;
};

// Tile ordinal is the G07-frozen historical 4x4 physical order: full-width
// bands are row-major; rows intersecting timing patches enumerate their left
// and right data spans on that same row before advancing vertically. The ten
// manifest rectangles are metadata regions and never redefine this ordering.
[[nodiscard]] UnifiedDataTile GetUnifiedDataTile(std::uint32_t tileOrdinal) noexcept;
[[nodiscard]] std::uint8_t GetUnifiedPhasePilotLabel(bool finePilot, std::uint32_t tileOrdinal,
    std::uint64_t frameSequence) noexcept;

struct UnifiedExpectedFrameIdentity
{
    bool requireSessionTag = false;
    pbprotocol::SessionTag sessionTag{};
    bool requireFrameSequence = false;
    std::uint64_t frameSequence = 0;
};

struct UnifiedVisualDecodePolicy
{
    LocalDesktopDecodePolicy locator;
    double minimumLumaLevelGap = 32;
    double maximumPilotDeviation = 8;
    double minimumChromaSeparation = 16;
    double maximumPhasePilotResidual = 0.125;
    double maximumTimingBitErrorFraction = 0.02;
    std::int16_t minimumDecisionMetric = 64;
    std::uint32_t maximumFecIterations = 12;
};

struct UnifiedSoftMetric
{
    // Positive values prefer zero and negative values prefer one, matching
    // PBInnerFec. A zero value is an erasure, never a positive observation.
    std::int16_t value = 0;
    UnifiedLane lane = UnifiedLane::BaseLuma;
    std::uint8_t codewordSlot = 0;
    std::uint8_t dataRegion = 0;
    std::uint8_t freshnessRegion = 0;
    UnifiedErasureReason erasureReason = UnifiedErasureReason::LocalSamplingFailure;

    bool operator==(const UnifiedSoftMetric&) const = default;
};

enum class UnifiedSlotRejection : std::uint8_t
{
    None,
    FrameErasure,
    LaneErasure,
    InnerFecFailure,
    InvalidInformation,
    NonCanonicalPadding,
    TransportCrcFailure,
    ControlCrcFailure,
    IdentityFailure
};

struct UnifiedSlotObservation
{
    UnifiedLane lane = UnifiedLane::BaseLuma;
    UnifiedSlotKind kind = UnifiedSlotKind::Transport;
    UnifiedSlotRejection rejection = UnifiedSlotRejection::FrameErasure;
    std::uint32_t iterationsUsed = 0;
    std::uint32_t acceptedBytes = 0;
    bool fecValid = false;
    bool paddingValid = false;
    bool crcValid = false;
    bool identityValid = false;
    bool accepted = false;
};

struct UnifiedFreshnessObservation
{
    bool current = false;
    std::uint16_t bitErrors = 0;
    double residual = 1;
};

struct UnifiedAcceptedBlock
{
    std::uint8_t codewordSlot = 0;
    UnifiedSlotKind kind = UnifiedSlotKind::Transport;
    std::uint32_t size = 0;
    std::array<std::byte, kUnifiedInformationBytes> bytes{};
};

// Read-only statistics of the quantized, locally erased metrics actually sent
// to the shared FEC gate. No sender truth or pre-erasure GPU values are inferred.
struct UnifiedLaneMetricObservation
{
    bool available = false;
    std::uint32_t samples = 0;
    std::uint32_t zeroMetrics = 0;
    std::uint32_t erasedMetrics = 0;
    std::uint32_t minimumAbsoluteMetric = 0;
    std::uint64_t absoluteMetricSum = 0;
};

struct UnifiedVisualObservation
{
    bool inputValid = false;
    UnifiedErasureReason frameErasure = UnifiedErasureReason::CanvasClipped;
    LocalDesktopObservation bootstrap;
    pbprotocol::BootstrapRecord bootstrapRecord;
    UnifiedBaseLumaObservation baseLuma;
    UnifiedFineLumaObservation fineLuma;
    UnifiedChromaObservation chroma;
    std::array<UnifiedFreshnessObservation, kUnifiedFreshnessRegionCount> freshness;
    std::array<UnifiedSlotObservation, kUnifiedCodewordCount> slots;
    std::array<UnifiedLaneMetricObservation, 3> laneMetrics{};
    std::uint32_t acceptedBlocks = 0;
    std::uint32_t acceptedTransportBlocks = 0;
    std::uint32_t acceptedControlRecords = 0;
    std::uint32_t falseAcceptedBlocks = 0;

    [[nodiscard]] bool IsFrameAvailable() const noexcept
    {
        return inputValid && frameErasure == UnifiedErasureReason::None;
    }
};

// Compact, same-frame handoff from a pixel demodulation backend into the
// canonical CPU FEC/protocol gate. logicalMetrics are already in global
// codeword-bit order. tileSamplingFailures contains one 0/1 entry for every
// physical data tile; it never carries pixels or sender-side slot metadata.
struct UnifiedPreparedMetricFrame
{
    LocalDesktopObservation bootstrap;
    std::span<const float> logicalMetrics;
    std::span<const std::uint8_t> tileSamplingFailures;
    std::array<UnifiedFreshnessObservation, kUnifiedFreshnessRegionCount> freshness;
    UnifiedBaseLumaObservation baseLuma;
    UnifiedFineLumaObservation fineLuma;
    UnifiedChromaObservation chroma;
};

[[nodiscard]] bool ValidateUnifiedVisualDecodePolicy(const UnifiedVisualDecodePolicy& policy) noexcept;
[[nodiscard]] bool ResolveUnifiedVisualSamplingGeometry(const LocalDesktopGeometry& geometry,
    std::uint32_t frameWidth, std::uint32_t frameHeight, const UnifiedVisualDecodePolicy& policy,
    LocalDesktopGeometry& output) noexcept;
[[nodiscard]] bool BuildUnifiedFreshnessBits(std::span<const std::byte> canonicalRecord,
    std::uint32_t freshnessRegion, std::span<std::uint8_t> output) noexcept;

// Each logical slot is explicitly typed before any protocol packing or Inner
// FEC encoding occurs. block is one exact PB-Control-1 or Transport Block wire
// object when active is true. Their existing canonical prefixes are mutually
// exclusive, so DecodeMixedFrame recovers the type without a side channel and
// without reducing the frozen 1314-byte Transport payload. Inactive slots are
// permitted only for Transport and encode the deterministic all-zero
// information word; this is the zero-byte Session filler and never represents
// an accepted Data Block.
struct UnifiedFrameSlotInput
{
    UnifiedSlotAssignment assignment;
    bool active = false;
    std::span<const std::byte> block;
};

// The spans borrow caller-owned storage for the duration of the call. Slot
// entries may be in any order, but their assignment.codewordSlot values must
// form the exact 0..30 set required by ValidateUnifiedMixedSlotPlan.
struct UnifiedVisualFrameInput
{
    std::span<const std::byte> bootstrapRecord;
    std::span<const UnifiedFrameSlotInput> slots;
};

// Validates Bootstrap/profile identity, every explicit slot type, canonical
// protocol bytes and SessionTag binding before producing any output. Control
// and Transport are zero-padded by their PBProtocol framing helpers and then
// encoded with the frozen Robust QC-LDPC profile.
[[nodiscard]] ModulationStatus PackUnifiedVisualFrame(
    const UnifiedVisualFrameInput& input, std::span<std::byte> outCodedFrame) noexcept;

// Convenience reference path used by the application scheduler and tests.
// It packs all slots and renders the complete immutable canonical raster in
// one call; scheduling state remains owned by the caller.
[[nodiscard]] ModulationStatus EncodeUnifiedVisualFrame(
    const UnifiedVisualFrameInput& input, std::span<std::byte> outBgra) noexcept;

// Maps exactly 31 packed Robust codewords, in global slot order, into one
// complete 1920x1080 BGRA8 canonical raster. The renderer redraws every pixel.
// Slot scheduling and Control repetition policy are deliberately outside this
// modulation layer.
[[nodiscard]] ModulationStatus EncodeUnifiedVisualFrame(std::span<const std::byte> bootstrapRecord,
    std::span<const std::byte> codedFrame, std::span<std::byte> outBgra) noexcept;

// Bounded synthetic CPU receive oracle. One instance owns reusable metrics,
// LDPC workspace, accepted output, and scratch buffers and is single-owner.
// Decode never combines observations from another frame, so stale pixels can
// only erase this frame's affected metrics and cannot be stitched across
// FrameSequence values.
class UnifiedVisualCpuOracle
{
public:
    UnifiedVisualCpuOracle() noexcept;
    UnifiedVisualCpuOracle(const UnifiedVisualCpuOracle&) = delete;
    UnifiedVisualCpuOracle& operator=(const UnifiedVisualCpuOracle&) = delete;
    UnifiedVisualCpuOracle(UnifiedVisualCpuOracle&& other) noexcept;
    UnifiedVisualCpuOracle& operator=(UnifiedVisualCpuOracle&& other) noexcept;
    ~UnifiedVisualCpuOracle();

    [[nodiscard]] static std::uint64_t RequiredBytes() noexcept;
    [[nodiscard]] static ModulationResult<UnifiedVisualCpuOracle> Create(std::uint64_t maximumBytes) noexcept;
    // Product receive path. Slot kind is inferred after Inner FEC from the
    // mutually exclusive canonical prefixes: PB-Control-1 starts with PBCR,
    // while a Transport Block starts with BlockType 1. No sender-side plan or
    // out-of-band scheduler state is required.
    [[nodiscard]] UnifiedVisualObservation DecodeMixedFrame(const LumaView& view,
        const UnifiedExpectedFrameIdentity& expectedIdentity = {},
        const UnifiedVisualDecodePolicy& policy = {}) noexcept;
    // Product GPU handoff. The same canonical Bootstrap observation that
    // selected geometry must accompany the compact metrics. This function
    // applies lane/local erasures and then reuses the exact DecodeMixedFrame
    // QC-LDPC and protocol-admission backend.
    [[nodiscard]] UnifiedVisualObservation DecodePreparedMixedFrame(const UnifiedPreparedMetricFrame& input,
        const UnifiedExpectedFrameIdentity& expectedIdentity = {},
        const UnifiedVisualDecodePolicy& policy = {}) noexcept;
    // Explicit-plan oracle retained for mapping/negative tests. Product
    // application ingress should use DecodeMixedFrame.
    [[nodiscard]] UnifiedVisualObservation Decode(const LumaView& view,
        std::span<const UnifiedSlotAssignment> slotPlan, const UnifiedExpectedFrameIdentity& expectedIdentity = {},
        const UnifiedVisualDecodePolicy& policy = {}) noexcept;
    [[nodiscard]] std::span<const UnifiedSoftMetric> GetSoftMetrics() const noexcept;
    [[nodiscard]] std::span<const UnifiedAcceptedBlock> GetAcceptedBlocks() const noexcept;

private:
    [[nodiscard]] UnifiedVisualObservation DecodeInternal(const LumaView& view,
        std::span<const UnifiedSlotAssignment> slotPlan, bool inferSlotKinds,
        const UnifiedExpectedFrameIdentity& expectedIdentity,
        const UnifiedVisualDecodePolicy& policy) noexcept;
    [[nodiscard]] UnifiedVisualObservation FinalizeDecodedMetrics(UnifiedVisualObservation observation,
        std::span<const UnifiedSlotAssignment> slotPlan, bool inferSlotKinds,
        const UnifiedVisualDecodePolicy& policy) noexcept;

    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

static_assert(kUnifiedCodewordBytes == 2025);
static_assert(kUnifiedInformationBytes == 1350);
static_assert(kUnifiedCodewordCount == 31);
static_assert(kUnifiedCodedFrameBytes == 62775);
static_assert(kUnifiedSoftMetricCount == 502200);
static_assert(kUnifiedCalibrationLumaRows + kUnifiedCalibrationNeutralRows + kUnifiedCalibrationChromaRows == 64);

} // namespace pbmodulation
