#pragma once

#include "pbmodulation/unified_visual.h"

namespace pbmodulation
{

// Experimental, never in the product/Unified-gray catalogs. The explicit
// API is the opt-in boundary until application integration is separately gated.
inline constexpr std::uint32_t kExperimentalPam4CellPixels = 4;
inline constexpr std::uint32_t kExperimentalPam4CellCount = 92832;
inline constexpr std::uint32_t kExperimentalPam4CodewordCount = 11;
inline constexpr std::uint32_t kExperimentalPam4CodewordBits = 16200;
inline constexpr std::uint32_t kExperimentalPam4CodewordBytes = 2025;
inline constexpr std::uint32_t kExperimentalPam4ControlInformationBytes = 1350;
inline constexpr std::uint32_t kExperimentalPam4DataInformationBytes = 1665;
inline constexpr std::uint32_t kExperimentalPam4TransportPayloadBytes = 1629;
inline constexpr std::size_t kExperimentalPam4CodedFrameBytes = 11U * 2025U;
inline constexpr std::size_t kExperimentalPam4MetricCount = 11U * 16200U;
inline constexpr std::uint32_t kExperimentalPam4TransportPayloadPerFrame = 10U * 1629U;
inline constexpr std::array<std::uint8_t, 4> kExperimentalPam4Levels{0, 85, 170, 255};
inline constexpr UnifiedExpectedFrameIdentity kExperimentalPam4ExpectedIdentity{
    false, {}, false, 0, pbprotocol::kPam4ExperimentalProfile.visualProfileId, pbprotocol::kPam4ExperimentalProfile.visualLayoutVersion};

struct ExperimentalPam4Calibration
{
    bool valid = false;
    std::array<std::array<double, 4>, 4> coarse{};
    std::array<std::array<double, 4>, 4> heldOutFine{};
    std::array<double, 4> heldOutNeutral{};
    std::array<double, 4> dataCentroids{};
    double maximumHeldOutResidual = 0;
};

enum class ExperimentalPam4FrameErasure : std::uint8_t
{
    None, InvalidInput, IdentityConflict, BootstrapFailure, CanvasClipped,
    FreshnessFailure, CalibrationFailure, SamplingFailure
};

// Deliberately NOT a UnifiedVisualObservation: the old 15/18-slot lane and
// erasure-scope telemetry must not silently classify this eleven-slot frame.
// All PAM4 pre-FEC erasures are frame-wide; no Fine/Chroma lane is claimed.
struct ExperimentalPam4Observation
{
    bool inputValid = false;
    ExperimentalPam4FrameErasure frameErasure = ExperimentalPam4FrameErasure::InvalidInput;
    LocalDesktopObservation bootstrap;
    pbprotocol::BootstrapRecord bootstrapRecord;
    std::array<UnifiedFreshnessObservation, kUnifiedFreshnessRegionCount> freshness{};
    std::array<UnifiedSlotObservation, kExperimentalPam4CodewordCount> slotObservations{};
    UnifiedLaneMetricObservation lumaMetrics;
    std::uint32_t frameSlotCount = kExperimentalPam4CodewordCount;
    std::uint32_t acceptedBlocks = 0;
    std::uint32_t acceptedControlRecords = 0;
    std::uint32_t acceptedTransportBlocks = 0;

    [[nodiscard]] bool IsFrameAvailable() const noexcept
    {
        return inputValid && frameErasure == ExperimentalPam4FrameErasure::None;
    }
};

// Slot 0 is ALWAYS one active PB-Control-1 record (Robust). Slots 1..10
// are Transport (Fast), optionally inactive zero fillers. No inferred type,
// provider name, sender plan, or external per-frame metadata is used to decode.
[[nodiscard]] bool ValidateExperimentalPam4SlotPlan(std::span<const UnifiedSlotAssignment> assignments) noexcept;
[[nodiscard]] ModulationStatus PackExperimentalPam4Frame(const UnifiedVisualFrameInput& input, std::span<std::byte> output) noexcept;
[[nodiscard]] ModulationStatus EncodeExperimentalPam4Frame(const UnifiedVisualFrameInput& input, std::span<std::byte> outBgra) noexcept;
[[nodiscard]] ModulationStatus EncodeExperimentalPam4Frame(std::span<const std::byte> bootstrapRecord,
    std::span<const std::byte> codedFrame, std::span<std::byte> outBgra) noexcept;

// Normative, immutable placement: a cell site is row*480+column on the global
// 4-pixel grid; a bit position is cellOrdinal*2+LSB-first plane. Out-of-range
// inputs return UINT32_MAX. The frozen table's hash is in the profile manifest.
[[nodiscard]] std::uint32_t GetExperimentalPam4CellSite(std::uint32_t cellOrdinal) noexcept;
[[nodiscard]] std::uint32_t GetExperimentalPam4BitPosition(std::uint32_t logicalBit) noexcept;

// Single-owner bounded reference. Captured Gray8/BGRA8 SDR pixels are the only
// payload input. No resize/whole-frame copy and no per-frame heap allocation.
// Accepted blocks are borrowed until the next Decode/Reset/move/destruction.
// Protocol CRC/padding/SessionTag validity here does NOT replace typed Control
// admission, Outer FEC, whole-file digest, safe publication and reopen checks.
class ExperimentalPam4CpuDecoder
{
public:
    ExperimentalPam4CpuDecoder() noexcept;
    ExperimentalPam4CpuDecoder(const ExperimentalPam4CpuDecoder&) = delete;
    ExperimentalPam4CpuDecoder& operator=(const ExperimentalPam4CpuDecoder&) = delete;
    ExperimentalPam4CpuDecoder(ExperimentalPam4CpuDecoder&&) noexcept;
    ExperimentalPam4CpuDecoder& operator=(ExperimentalPam4CpuDecoder&&) noexcept;
    ~ExperimentalPam4CpuDecoder();

    // Conservative allocation reservation (two fixed FEC workspaces included),
    // not a measured RSS number. The limit is enforced before allocation.
    [[nodiscard]] static std::uint64_t RequiredBytes() noexcept;
    [[nodiscard]] static ModulationResult<ExperimentalPam4CpuDecoder> Create(std::uint64_t maximumBytes) noexcept;
    // Caller MUST reset on capture epoch/ROI/device discontinuity. All decode
    // frame erasures also invalidate the cached pixel-derived geometry.
    void Reset() noexcept;
    [[nodiscard]] ExperimentalPam4Observation Decode(const LumaView& view,
        const UnifiedExpectedFrameIdentity& expected = kExperimentalPam4ExpectedIdentity) noexcept;
    [[nodiscard]] std::span<const UnifiedAcceptedBlock> GetAcceptedBlocks() const noexcept;
    [[nodiscard]] std::span<const std::int16_t> GetSoftMetrics() const noexcept;
    [[nodiscard]] ExperimentalPam4Calibration GetCalibration() const noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

static_assert(kExperimentalPam4CodewordCount <= kUnifiedMaximumFrameSlotCount);
static_assert(kExperimentalPam4DataInformationBytes <= kUnifiedMaximumInformationBytes);
static_assert(kExperimentalPam4CellCount * 2 - kExperimentalPam4MetricCount == 7464);

}
