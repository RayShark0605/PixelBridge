#pragma once

#include "pbmodulation/remote_visual.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace pbmodulation
{

// Experimental low-refresh RemoteVisual carrier. It reuses the established
// locator, freshness map and logical transport/FEC stack, but each 8x8 data
// tile carries a balanced four-bit Walsh symbol instead of one uniform bit.
inline constexpr std::uint64_t kRemoteVisualLowFpsProfileId = 0x504252564C463431ULL;
inline constexpr std::uint8_t kRemoteVisualLowFpsLayoutVersion = 7;
inline constexpr std::uint32_t kRemoteVisualLowFpsBitsPerTile = 4;
inline constexpr std::uint32_t kRemoteVisualLowFpsCodedBitsPerPlane = kRemoteVisualCodedBits;
inline constexpr std::uint32_t kRemoteVisualLowFpsCodedBits =
    kRemoteVisualLowFpsBitsPerTile * kRemoteVisualLowFpsCodedBitsPerPlane;
inline constexpr std::uint32_t kRemoteVisualLowFpsDataBytes = kRemoteVisualLowFpsCodedBits / 8;
inline constexpr std::uint32_t kRemoteVisualLowFpsCodewords = 4;
inline constexpr std::uint32_t kRemoteVisualLowFpsPaddingBytes = 0;
inline constexpr std::array<std::uint16_t, 16> kRemoteVisualLowFpsSymbolMasks{
    0x3333, 0x00FF, 0xCC33, 0x9999, 0xF00F, 0x6699, 0x3CC3, 0x9669,
    0xCCCC, 0xFF00, 0x33CC, 0x6666, 0x0FF0, 0x9966, 0xC33C, 0x6996};
inline constexpr std::array<std::uint64_t, kRemoteVisualLowFpsBitsPerTile> kRemoteVisualLowFpsPlaneSequenceOffsets{0, 4, 8, 12};
static_assert(kRemoteVisualLowFpsDataBytes == 8100);
static_assert(kRemoteVisualLowFpsCodewords * 2025 + kRemoteVisualLowFpsPaddingBytes == kRemoteVisualLowFpsDataBytes);

// Step 07 selected this provider-generic candidate using Train/Validation and
// opened Holdout/External only after selection. Step 08 freezes its exact
// float32 outputs and provenance, but does not make it the production default.
struct RemoteVisualLowFpsMetricCalibrationBin
{
    double rawMagnitudeUpper = 0;
    std::uint32_t calibratedMagnitudeFloatBits = 0;
    std::uint64_t trainingSamples = 0;
    std::uint64_t trainingErrors = 0;
    bool operator==(const RemoteVisualLowFpsMetricCalibrationBin&) const = default;
};

inline constexpr std::uint32_t kRemoteVisualLowFpsMetricCalibrationVersion = 1;
inline constexpr char kRemoteVisualLowFpsMetricCalibrationCandidateId[] = "lf4-default/PiecewiseLookup";
inline constexpr std::array<RemoteVisualLowFpsMetricCalibrationBin, 16> kRemoteVisualLowFpsMetricCalibrationBins{
    RemoteVisualLowFpsMetricCalibrationBin{0.01, 0x00000000, 1669, 1450},
    RemoteVisualLowFpsMetricCalibrationBin{0.02, 0x3E75C28F, 0, 0},
    RemoteVisualLowFpsMetricCalibrationBin{0.04, 0x3EF5C28F, 0, 0},
    RemoteVisualLowFpsMetricCalibrationBin{0.08, 0x3F75C28F, 0, 0},
    RemoteVisualLowFpsMetricCalibrationBin{0.12, 0x3FCCCCCD, 0, 0},
    RemoteVisualLowFpsMetricCalibrationBin{0.2, 0x40B2108D, 130, 0},
    RemoteVisualLowFpsMetricCalibrationBin{0.35, 0x40FFFE00, 10466, 0},
    RemoteVisualLowFpsMetricCalibrationBin{0.5, 0x40FFFE00, 40695, 0},
    RemoteVisualLowFpsMetricCalibrationBin{0.75, 0x40FFFE00, 90435, 0},
    RemoteVisualLowFpsMetricCalibrationBin{1.0, 0x40FFFE00, 243054, 0},
    RemoteVisualLowFpsMetricCalibrationBin{1.5, 0x40FFFE00, 2351, 0},
    RemoteVisualLowFpsMetricCalibrationBin{2.0, 0x40FFFE00, 0, 0},
    RemoteVisualLowFpsMetricCalibrationBin{3.0, 0x40FFFE00, 0, 0},
    RemoteVisualLowFpsMetricCalibrationBin{4.0, 0x40FFFE00, 0, 0},
    RemoteVisualLowFpsMetricCalibrationBin{8.0, 0x40FFFE00, 0, 0},
    RemoteVisualLowFpsMetricCalibrationBin{1000000.0, 0x40FFFE00, 0, 0}};

enum class RemoteVisualLowFpsErasure : std::uint8_t
{
    None, InvalidInput, InvalidPolicy, WorkspaceUnavailable, OutputBufferTooSmall, OverlappingSpans,
    BootstrapErasure, UnsupportedProfile, ScaleOutOfRange, AlignmentOutOfRange, FrameOutOfBounds,
    PilotClipping, PilotOrder, PilotVariance, PilotSpatialMismatch, PixelReadFailure, WorkBudgetExceeded
};

struct RemoteVisualLowFpsDecodePolicy
{
    LocalDesktopDecodePolicy locator;
    double minimumScale = 0.5;
    double maximumScale = 2.0;
    double maximumMarkerResidualPixels = 1.25;
    double minimumEndpointSeparation = 96;
    double maximumPilotStandardDeviation = 32;
    double maximumPilotSpatialDeviation = 32;
    double minimumSymbolRms = 0.35;
    double maximumSymbolResidual = 0.7;
    double minimumSymbolMargin = 0.08;
    double minimumFreshnessMetric = 0.2;
    std::uint64_t maximumDataWorkUnits = 8000000;
};

struct RemoteVisualLowFpsMetricResolution
{
    bool valid = false;
    std::uint32_t freshnessRegions = 0;
    std::uint32_t staleRegions = 0;
    std::uint32_t freshnessTagMismatches = 0;
    std::uint32_t freshnessTagErasures = 0;
    std::uint32_t erasedDataMetrics = 0;
};

struct RemoteVisualLowFpsObservation
{
    RemoteVisualLowFpsErasure erasure = RemoteVisualLowFpsErasure::InvalidInput;
    LocalDesktopObservation bootstrap;
    RemoteVisualCalibration calibration;
    RemoteVisualMargin margin;
    std::uint32_t dataBytes = 0;
    std::uint32_t unreliableSymbols = 0;
    std::uint32_t freshnessRegions = 0;
    std::uint32_t staleRegions = 0;
    std::uint32_t freshnessTagMismatches = 0;
    std::uint32_t freshnessTagErasures = 0;
    std::uint32_t erasedDataMetrics = 0;
    std::uint64_t dataWorkUnits = 0;
    LocalDesktopErasureReason pixelError = LocalDesktopErasureReason::None;

    [[nodiscard]] bool IsAccepted() const noexcept
    {
        return erasure == RemoteVisualLowFpsErasure::None;
    }
};

class RemoteVisualLowFpsWorkspace
{
public:
    RemoteVisualLowFpsWorkspace() noexcept;
    RemoteVisualLowFpsWorkspace(RemoteVisualLowFpsWorkspace&&) noexcept;
    RemoteVisualLowFpsWorkspace& operator=(RemoteVisualLowFpsWorkspace&&) noexcept;
    ~RemoteVisualLowFpsWorkspace();
    RemoteVisualLowFpsWorkspace(const RemoteVisualLowFpsWorkspace&) = delete;
    RemoteVisualLowFpsWorkspace& operator=(const RemoteVisualLowFpsWorkspace&) = delete;
    [[nodiscard]] static std::uint64_t RequiredBytes() noexcept;
    [[nodiscard]] static ModulationResult<RemoteVisualLowFpsWorkspace> Create(std::uint64_t maximumBytes) noexcept;
    [[nodiscard]] std::span<const std::uint64_t> GetMarginHistogram() const noexcept;
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
    friend RemoteVisualLowFpsObservation DecodeRemoteVisualLowFpsFrame(const LumaView&, RemoteVisualLowFpsWorkspace&,
        std::span<std::byte>, std::span<float>, const RemoteVisualLowFpsDecodePolicy&) noexcept;
};

[[nodiscard]] std::uint32_t GetRemoteVisualLowFpsLogicalBit(std::uint32_t dataOrdinal, std::uint32_t plane,
    std::uint64_t frameSequence) noexcept;
// Applies only the frozen candidate mapping above. Invalid/nonfinite or
// out-of-domain input returns false without modifying calibratedMetric.
[[nodiscard]] bool CalibrateRemoteVisualLowFpsMetric(float rawMetric, float& calibratedMetric) noexcept;
[[nodiscard]] RemoteVisualLowFpsMetricResolution ResolveRemoteVisualLowFpsPhysicalMetrics(
    std::span<const float> physicalBitMetrics, std::span<const float> physicalFreshnessMetrics,
    std::uint64_t sessionTag, std::uint64_t frameSequence, std::span<float> logicalMetrics,
    double minimumFreshnessMetric = 0.2) noexcept;
[[nodiscard]] ModulationStatus EncodeRemoteVisualLowFpsFrame(std::span<const std::byte> bootstrapRecord,
    std::span<const std::byte> logicalData, std::span<std::byte> outBgra) noexcept;
[[nodiscard]] RemoteVisualLowFpsObservation DecodeRemoteVisualLowFpsFrame(const LumaView& view,
    RemoteVisualLowFpsWorkspace& workspace, std::span<std::byte> hardBits, std::span<float> softMetrics,
    const RemoteVisualLowFpsDecodePolicy& policy = {}) noexcept;
[[nodiscard]] RemoteVisualLowFpsErasure ValidateRemoteVisualLowFpsGeometry(const LocalDesktopGeometry& geometry,
    const RemoteVisualLowFpsDecodePolicy& policy = {}) noexcept;
[[nodiscard]] const char* GetRemoteVisualLowFpsErasureName(RemoteVisualLowFpsErasure reason) noexcept;

} // namespace pbmodulation
