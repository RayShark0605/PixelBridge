#pragma once

#include "pbmodulation/local_desktop_decode.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace pbmodulation
{

// Experimental RemoteVisual profile. The identifier and layout are separate
// from both frozen Phase-1 profiles. The logical PB-Bootstrap-1, Transport,
// QC-LDPC, Outer-FEC, CRC and digest contracts are unchanged.
inline constexpr std::uint64_t kRemoteVisualProfileId = 0xED05C2CC0397572AULL;
inline constexpr std::uint8_t kRemoteVisualLayoutVersion = 6;
inline constexpr std::uint32_t kRemoteVisualTilePixels = 8;
inline constexpr std::uint32_t kRemoteVisualTileSampleInset = 2;
inline constexpr std::uint32_t kRemoteVisualCalibrationSampleInset = 4;
inline constexpr std::uint32_t kRemoteVisualTileCount = 21456;
inline constexpr std::uint32_t kRemoteVisualDataTileCount = 16723;
inline constexpr std::uint32_t kRemoteVisualInterleavePhaseStep = 104;
inline constexpr std::uint32_t kRemoteVisualFreshnessRegionColumns = 14;
inline constexpr std::uint32_t kRemoteVisualFreshnessRegionRows = 7;
inline constexpr std::uint32_t kRemoteVisualFreshnessRegionCount =
    kRemoteVisualFreshnessRegionColumns * kRemoteVisualFreshnessRegionRows;
inline constexpr std::uint32_t kRemoteVisualMinimumFreshnessTags = 24;
inline constexpr std::uint32_t kRemoteVisualEligibleFreshnessRegions = 77;
inline constexpr std::uint32_t kRemoteVisualCodedBits = 16200;
inline constexpr std::uint32_t kRemoteVisualDataBytes = 2025;
inline constexpr std::uint32_t kRemoteVisualCodewords = 1;
inline constexpr std::uint32_t kRemoteVisualPaddingBytes = 0;
inline constexpr std::uint8_t kRemoteVisualZeroLuma = 32;
inline constexpr std::uint8_t kRemoteVisualOneLuma = 224;
inline constexpr std::uint8_t kRemoteVisualUnusedLuma = 128;
inline constexpr std::size_t kRemoteVisualMarginBins = 4096;
inline constexpr std::array<LocalDesktopRegion, 4> kRemoteVisualLadders{
    LocalDesktopRegion{736, 16, 128, 64}, LocalDesktopRegion{1696, 16, 128, 64},
    LocalDesktopRegion{96, 1000, 128, 64}, LocalDesktopRegion{1056, 1000, 128, 64}};
static_assert(kRemoteVisualTileSampleInset * 2 < kRemoteVisualTilePixels);
static_assert(kRemoteVisualCalibrationSampleInset * 2 < 32);
static_assert(kRemoteVisualCalibrationSampleInset * 2 < 64);

enum class RemoteVisualErasure : std::uint8_t
{
    None, InvalidInput, InvalidPolicy, WorkspaceUnavailable, OutputBufferTooSmall, OverlappingSpans,
    BootstrapErasure, UnsupportedProfile, ScaleOutOfRange, AlignmentOutOfRange, FrameOutOfBounds,
    PilotClipping, PilotOrder, PilotVariance, PilotSpatialMismatch, PixelReadFailure, WorkBudgetExceeded
};

struct RemoteVisualDecodePolicy
{
    LocalDesktopDecodePolicy locator;
    double maximumScaleDriftPixels = 0.125;
    double maximumPhaseErrorPixels = 0.125;
    double maximumMarkerResidualPixels = 0.125;
    double minimumEndpointSeparation = 96;
    double maximumPilotStandardDeviation = 24;
    double maximumPilotSpatialDeviation = 24;
    double maximumTileStandardDeviation = 64;
    double minimumFreshnessMetric = 0.2;
    std::uint64_t maximumDataWorkUnits = 8000000;
};

enum class RemoteVisualTileRole : std::uint8_t
{
    Unused,
    FreshnessTag,
    Data
};

struct RemoteVisualTileMapping
{
    RemoteVisualTileRole role = RemoteVisualTileRole::Unused;
    std::uint16_t regionId = 0;
    std::uint32_t dataOrdinal = kRemoteVisualDataTileCount;
};

struct RemoteVisualMetricResolution
{
    bool valid = false;
    std::uint32_t freshnessRegions = 0;
    std::uint32_t staleRegions = 0;
    std::uint32_t freshnessTagMismatches = 0;
    std::uint32_t freshnessTagErasures = 0;
    std::uint32_t erasedDataMetrics = 0;
};

struct RemoteVisualCalibration
{
    std::array<double, 2> centroids{};
    std::array<double, 2> variances{};
    std::array<std::array<double, 2>, 4> ladderCentroids{};
    std::array<std::array<double, 2>, 4> ladderVariances{};
    double separation = 0;
    double spatialDeviation = 0;
    bool operator==(const RemoteVisualCalibration&) const = default;
};

struct RemoteVisualMargin
{
    std::uint64_t samples = 0;
    double minimum = 0;
    double p50 = 0;
    double p01 = 0;
    double p001 = 0;
};

struct RemoteVisualObservation
{
    RemoteVisualErasure erasure = RemoteVisualErasure::InvalidInput;
    LocalDesktopObservation bootstrap;
    RemoteVisualCalibration calibration;
    RemoteVisualMargin margin;
    std::uint32_t dataBytes = 0;
    std::uint32_t unreliableTiles = 0;
    std::uint32_t freshnessRegions = 0;
    std::uint32_t staleRegions = 0;
    std::uint32_t freshnessTagMismatches = 0;
    std::uint32_t freshnessTagErasures = 0;
    std::uint64_t dataWorkUnits = 0;
    LocalDesktopErasureReason pixelError = LocalDesktopErasureReason::None;

    [[nodiscard]] bool IsAccepted() const noexcept
    {
        return erasure == RemoteVisualErasure::None;
    }
};

class RemoteVisualWorkspace
{
public:
    RemoteVisualWorkspace() noexcept;
    RemoteVisualWorkspace(RemoteVisualWorkspace&&) noexcept;
    RemoteVisualWorkspace& operator=(RemoteVisualWorkspace&&) noexcept;
    ~RemoteVisualWorkspace();
    RemoteVisualWorkspace(const RemoteVisualWorkspace&) = delete;
    RemoteVisualWorkspace& operator=(const RemoteVisualWorkspace&) = delete;
    [[nodiscard]] static std::uint64_t RequiredBytes() noexcept;
    [[nodiscard]] static ModulationResult<RemoteVisualWorkspace> Create(std::uint64_t maximumBytes) noexcept;
    [[nodiscard]] std::span<const std::uint64_t> GetMarginHistogram() const noexcept;
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
    friend RemoteVisualObservation DecodeRemoteVisualFrame(const LumaView&, RemoteVisualWorkspace&, std::span<std::byte>,
        std::span<float>, const RemoteVisualDecodePolicy&) noexcept;
};

// Physical indices enumerate seven explicitly 8-pixel-aligned bands. Two
// four-pixel vertical guard strips keep the profile independent from the
// older 188-pixel Direct-Level bands. Invalid indices leave output unchanged.
[[nodiscard]] bool GetRemoteVisualTile(std::uint32_t physicalIndex, LocalDesktopRegion& output) noexcept;
[[nodiscard]] bool GetRemoteVisualTileMapping(std::uint32_t physicalIndex, RemoteVisualTileMapping& output) noexcept;
[[nodiscard]] bool GetRemoteVisualFreshnessBit(std::uint64_t sessionTag, std::uint64_t frameSequence,
    std::uint32_t physicalIndex, bool& output) noexcept;
[[nodiscard]] RemoteVisualMetricResolution ResolveRemoteVisualPhysicalMetrics(std::span<const float> physicalMetrics,
    std::uint64_t sessionTag, std::uint64_t frameSequence, std::span<float> logicalMetrics,
    double minimumFreshnessMetric = 0.2) noexcept;
[[nodiscard]] ModulationStatus EncodeRemoteVisualFrame(std::span<const std::byte> bootstrapRecord,
    std::span<const std::byte> logicalData, std::span<std::byte> outBgra) noexcept;
[[nodiscard]] RemoteVisualObservation DecodeRemoteVisualFrame(const LumaView& view, RemoteVisualWorkspace& workspace,
    std::span<std::byte> hardBits, std::span<float> softMetrics, const RemoteVisualDecodePolicy& policy = {}) noexcept;
[[nodiscard]] RemoteVisualErasure ValidateRemoteVisualGeometry(const LocalDesktopGeometry& geometry,
    const RemoteVisualDecodePolicy& policy = {}) noexcept;
[[nodiscard]] RemoteVisualMargin SummarizeRemoteVisualMargin(std::span<const std::uint64_t> histogram, double minimum) noexcept;
[[nodiscard]] const char* GetRemoteVisualErasureName(RemoteVisualErasure reason) noexcept;

} // namespace pbmodulation
