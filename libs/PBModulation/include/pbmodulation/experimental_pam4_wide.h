#pragma once

#include "pbmodulation/unified_visual.h"

namespace pbmodulation
{

// Experimental, never in the product/Unified-gray catalogs. The explicit
// API is the opt-in boundary until application integration is separately gated.
// Canonical 3px cells require fixed center-point 4/3 presentation, not
// native 3px screen cells. This contract does not change desktop settings.
inline constexpr std::uint32_t kExperimentalPam4WidePresentationWidth = 2560;
inline constexpr std::uint32_t kExperimentalPam4WidePresentationHeight = 1440;
inline constexpr std::uint32_t kExperimentalPam4WidePhysicalCellPixels = 4;
inline constexpr std::uint32_t kExperimentalPam4WideCellPixels = 3;
inline constexpr std::uint32_t kExperimentalPam4WideCellCount = 165192;
inline constexpr std::uint32_t kExperimentalPam4WideCodewordCount = 20;
inline constexpr std::uint32_t kExperimentalPam4WideCodewordBits = 16200;
inline constexpr std::uint32_t kExperimentalPam4WideCodewordBytes = 2025;
inline constexpr std::uint32_t kExperimentalPam4WideControlInformationBytes = 1350;
inline constexpr std::uint32_t kExperimentalPam4WideDataInformationBytes = 1665;
inline constexpr std::uint32_t kExperimentalPam4WideTransportPayloadBytes = 1629;
inline constexpr std::size_t kExperimentalPam4WideCodedFrameBytes = 20U * 2025U;
inline constexpr std::size_t kExperimentalPam4WideMetricCount = 20U * 16200U;
inline constexpr std::uint32_t kExperimentalPam4WideTransportPayloadPerFrame = 19U * 1629U;
inline constexpr std::array<std::uint8_t, 4> kExperimentalPam4WideLevels{0, 85, 170, 255};
inline constexpr UnifiedExpectedFrameIdentity kExperimentalPam4WideExpectedIdentity{
    false, {}, false, 0, pbprotocol::kPam4WideExperimentalProfile.visualProfileId, pbprotocol::kPam4WideExperimentalProfile.visualLayoutVersion};

struct ExperimentalPam4WideCalibration
{
    bool valid = false;
    std::array<std::array<double, 4>, 4> coarse{};
    std::array<std::array<double, 4>, 4> heldOutFine{};
    std::array<double, 4> heldOutNeutral{};
    std::array<double, 4> dataCentroids{};
    double maximumHeldOutResidual = 0;
};

enum class ExperimentalPam4WideFrameErasure : std::uint8_t
{
    None, InvalidInput, IdentityConflict, BootstrapFailure, CanvasClipped,
    FreshnessFailure, CalibrationFailure, SamplingFailure
};

// Deliberately NOT a UnifiedVisualObservation: the old 15/18-slot lane and
// erasure-scope telemetry must not silently classify this twenty-slot frame.
// All PAM4 pre-FEC erasures are frame-wide; no Fine/Chroma lane is claimed.
struct ExperimentalPam4WideObservation
{
    bool inputValid = false;
    ExperimentalPam4WideFrameErasure frameErasure = ExperimentalPam4WideFrameErasure::InvalidInput;
    LocalDesktopObservation bootstrap;
    pbprotocol::BootstrapRecord bootstrapRecord;
    std::array<UnifiedFreshnessObservation, kUnifiedFreshnessRegionCount> freshness{};
    std::array<UnifiedSlotObservation, kExperimentalPam4WideCodewordCount> slotObservations{};
    UnifiedLaneMetricObservation lumaMetrics;
    std::uint32_t frameSlotCount = kExperimentalPam4WideCodewordCount;
    std::uint32_t acceptedBlocks = 0;
    std::uint32_t acceptedControlRecords = 0;
    std::uint32_t acceptedTransportBlocks = 0;

    [[nodiscard]] bool IsFrameAvailable() const noexcept
    {
        return inputValid && frameErasure == ExperimentalPam4WideFrameErasure::None;
    }
};

// Slot 0 is ALWAYS one active PB-Control-1 record (Robust). Slots 1..19
// are Transport (Fast), optionally inactive zero fillers. No inferred type,
// provider name, sender plan, or external per-frame metadata is used to decode.
[[nodiscard]] bool ValidateExperimentalPam4WideSlotPlan(std::span<const UnifiedSlotAssignment> assignments) noexcept;
[[nodiscard]] ModulationStatus PackExperimentalPam4WideFrame(const UnifiedVisualFrameInput& input, std::span<std::byte> output) noexcept;
[[nodiscard]] ModulationStatus EncodeExperimentalPam4WideFrame(const UnifiedVisualFrameInput& input, std::span<std::byte> outBgra) noexcept;
[[nodiscard]] ModulationStatus EncodeExperimentalPam4WideFrame(std::span<const std::byte> bootstrapRecord,
    std::span<const std::byte> codedFrame, std::span<std::byte> outBgra) noexcept;

// Normative, immutable placement: a cell site is row*640+column on the global
// 3-pixel canonical grid; a bit position is cellOrdinal*2+LSB-first plane. Out-of-range
// inputs return UINT32_MAX. The frozen table's hash is in the profile manifest.
[[nodiscard]] std::uint32_t GetExperimentalPam4WideCellSite(std::uint32_t cellOrdinal) noexcept;
[[nodiscard]] std::uint32_t GetExperimentalPam4WideBitPosition(std::uint32_t logicalBit) noexcept;

// Single-owner bounded reference. Captured Gray8/BGRA8 SDR pixels are the only
// payload input. No resize/whole-frame copy and no per-frame heap allocation.
// Accepted blocks are borrowed until the next Decode/Reset/move/destruction.
// Protocol CRC/padding/SessionTag validity here does NOT replace typed Control
// admission, Outer FEC, whole-file digest, safe publication and reopen checks.
class ExperimentalPam4WideCpuDecoder
{
public:
    ExperimentalPam4WideCpuDecoder() noexcept;
    ExperimentalPam4WideCpuDecoder(const ExperimentalPam4WideCpuDecoder&) = delete;
    ExperimentalPam4WideCpuDecoder& operator=(const ExperimentalPam4WideCpuDecoder&) = delete;
    ExperimentalPam4WideCpuDecoder(ExperimentalPam4WideCpuDecoder&&) noexcept;
    ExperimentalPam4WideCpuDecoder& operator=(ExperimentalPam4WideCpuDecoder&&) noexcept;
    ~ExperimentalPam4WideCpuDecoder();

    // Conservative allocation reservation (two fixed FEC workspaces included),
    // not a measured RSS number. The limit is enforced before allocation.
    [[nodiscard]] static std::uint64_t RequiredBytes() noexcept;
    [[nodiscard]] static ModulationResult<ExperimentalPam4WideCpuDecoder> Create(std::uint64_t maximumBytes) noexcept;
    // Caller MUST reset on capture epoch/ROI/device discontinuity. All decode
    // frame erasures also invalidate the cached pixel-derived geometry.
    void Reset() noexcept;
    [[nodiscard]] ExperimentalPam4WideObservation Decode(const LumaView& view,
        const UnifiedExpectedFrameIdentity& expected = kExperimentalPam4WideExpectedIdentity) noexcept;
    [[nodiscard]] std::span<const UnifiedAcceptedBlock> GetAcceptedBlocks() const noexcept;
    [[nodiscard]] std::span<const std::int16_t> GetSoftMetrics() const noexcept;
    [[nodiscard]] ExperimentalPam4WideCalibration GetCalibration() const noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

// An independent observation/container is mandatory; Unified stays18.
static_assert(kExperimentalPam4WideCodewordCount > kUnifiedMaximumFrameSlotCount);
static_assert(kExperimentalPam4WideCodewordCount <= 256);
static_assert(kExperimentalPam4WideDataInformationBytes <= kUnifiedMaximumInformationBytes);
static_assert(kExperimentalPam4WideCellCount * 2 - kExperimentalPam4WideMetricCount == 6384);

}
