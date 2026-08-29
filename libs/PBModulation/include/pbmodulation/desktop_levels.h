#pragma once

#include "pbmodulation/local_desktop_decode.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace pbmodulation
{

inline constexpr std::uint64_t kDesktopLevels2ProfileId = 0xEBB15DCE41AB436EULL;
inline constexpr std::uint64_t kDesktopLevels4ProfileId = 0xF9B7490A9251F15CULL;
inline constexpr std::uint8_t kDesktopLevelsLayoutVersion = 3;
inline constexpr std::array<std::uint8_t, 4> kDesktopLevelsLuma{32, 96, 160, 224};
inline constexpr std::array<std::uint8_t, 4> kDesktopLevelsLabels{0, 1, 3, 2};
inline constexpr LocalDesktopRegion kDesktopLevelsDataGrid{96, 96, 1728, 888};
inline constexpr std::array<LocalDesktopRegion, 4> kDesktopLevelsLadders{
    LocalDesktopRegion{736, 16, 128, 64}, LocalDesktopRegion{1696, 16, 128, 64},
    LocalDesktopRegion{96, 1000, 128, 64}, LocalDesktopRegion{1056, 1000, 128, 64}};
inline constexpr std::array<LocalDesktopRegion, 2> kDesktopLevelsPhasePilots{
    LocalDesktopRegion{896, 16, 128, 64}, LocalDesktopRegion{896, 1000, 128, 64}};
inline constexpr std::size_t kDesktopLevelsMaximumDataBytes = 86688;
inline constexpr std::size_t kDesktopLevelsMaximumBits = kDesktopLevelsMaximumDataBytes * 8;
inline constexpr std::size_t kDesktopLevelsMarginBins = 4096;

struct DesktopLevelsProfile
{
    std::uint64_t visualProfileId;
    std::uint32_t tilePixels;
    std::uint32_t tileCount;
    std::uint32_t dataBytes;
    std::uint32_t codewords;
    std::uint32_t paddingBytes;
};

[[nodiscard]] const DesktopLevelsProfile* GetDesktopLevelsProfile(std::uint64_t profileId) noexcept;
// Shared LocalDesktop data geometry used by Direct-Level and the initial
// ShapeChroma A/B baseline. Valid tile sizes are 2 and 4 pixels.
[[nodiscard]] bool GetLocalDesktopDataTile(std::uint32_t tilePixels, std::uint32_t physicalIndex, LocalDesktopRegion& output) noexcept;
// Physical indices enumerate the grid row-major, omitting all nine Timing
// regions. Unknown profile/index leaves output unchanged.
[[nodiscard]] bool GetDesktopLevelsTile(std::uint64_t profileId, std::uint32_t physicalIndex, LocalDesktopRegion& output) noexcept;

enum class DesktopLevelsErasure : std::uint8_t
{
    None, InvalidInput, InvalidPolicy, WorkspaceUnavailable, OutputBufferTooSmall, OverlappingSpans,
    BootstrapErasure, UnsupportedProfile, ScaleOutOfRange, AlignmentOutOfRange, FrameOutOfBounds,
    PilotClipping, PilotOrder, PilotVariance, PilotSpatialMismatch, PhasePilotMismatch, PixelReadFailure, WorkBudgetExceeded
};

struct DesktopLevelsDecodePolicy
{
    LocalDesktopDecodePolicy locator;
    double maximumScaleDriftPixels = 0.125;
    double maximumPhaseErrorPixels = 0.125;
    double maximumMarkerResidualPixels = 0.125;
    double maximumPhasePilotResidual = 0.125;
    double minimumLevelGap = 32;
    double maximumPilotStandardDeviation = 8;
    double maximumPilotSpatialDeviation = 8;
    std::uint64_t maximumDataWorkUnits = 8000000;
};

struct DesktopLevelsCalibration
{
    std::array<double, 4> centroids{};
    std::array<double, 4> variances{};
    double minimumGap = 0;
    double spatialDeviation = 0;
    double phaseResidual = 0;
    std::array<std::array<double, 4>, 4> ladderCentroids{};
    std::array<std::array<double, 4>, 4> ladderVariances{};
    std::array<std::array<double, 2>, 2> phaseResiduals{};
    bool operator==(const DesktopLevelsCalibration&) const = default;
};

struct DesktopLevelsMargin
{
    std::uint64_t samples = 0;
    double minimum = 0;
    double p50 = 0;
    double p01 = 0;
    double p001 = 0;
};

struct DesktopLevelsObservation
{
    DesktopLevelsErasure erasure = DesktopLevelsErasure::InvalidInput;
    LocalDesktopObservation bootstrap;
    std::uint64_t profileId = 0;
    DesktopLevelsCalibration calibration;
    DesktopLevelsMargin margin;
    std::uint32_t dataBytes = 0;
    std::uint32_t unreliableTiles = 0;
    std::uint64_t dataWorkUnits = 0;
    LocalDesktopErasureReason pixelError = LocalDesktopErasureReason::None;
    [[nodiscard]] bool IsAccepted() const noexcept
    {
        return erasure == DesktopLevelsErasure::None;
    }
};

// Fixed capacity, allocated once before any captured input is examined.
// One owner per workspace; separate instances may be used concurrently.
class DesktopLevelsWorkspace
{
public:
    DesktopLevelsWorkspace() noexcept;
    DesktopLevelsWorkspace(DesktopLevelsWorkspace&&) noexcept;
    DesktopLevelsWorkspace& operator=(DesktopLevelsWorkspace&&) noexcept;
    ~DesktopLevelsWorkspace();
    DesktopLevelsWorkspace(const DesktopLevelsWorkspace&) = delete;
    DesktopLevelsWorkspace& operator=(const DesktopLevelsWorkspace&) = delete;
    [[nodiscard]] static std::uint64_t RequiredBytes() noexcept;
    [[nodiscard]] static ModulationResult<DesktopLevelsWorkspace> Create(std::uint64_t maximumBytes) noexcept;
    // Histogram of the most recent successful decode only. Erasure invalidates
    // this view; never retain it across another Decode call or a workspace move.
    [[nodiscard]] std::span<const std::uint64_t> GetMarginHistogram() const noexcept;
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
    friend DesktopLevelsObservation DecodeDesktopLevelsFrame(const LumaView&, DesktopLevelsWorkspace&, std::span<std::byte>, std::span<float>, const DesktopLevelsDecodePolicy&) noexcept;
};

// logicalData is exactly the profile capacity, including canonical zero tail.
// Encoder rejects overlapping input/output spans before drawing. All errors
// preserve output. A successful call deterministically redraws the full frame.
[[nodiscard]] ModulationStatus EncodeDesktopLevelsFrame(std::span<const std::byte> bootstrapRecord, std::span<const std::byte> logicalData,
                                                       std::span<std::byte> outBgra) noexcept;
// Hard bytes and soft samples are already reverse-interleaved, LSB-first.
// Positive metric prefers zero. Metrics are normalized distance differences,
// NOT empirically calibrated statistical LLRs. Only the used output prefixes
// change on success; both complete output spans remain unchanged on failure.
[[nodiscard]] DesktopLevelsObservation DecodeDesktopLevelsFrame(const LumaView& view, DesktopLevelsWorkspace& workspace,
    std::span<std::byte> hardBits, std::span<float> softMetrics, const DesktopLevelsDecodePolicy& policy = {}) noexcept;
[[nodiscard]] DesktopLevelsErasure ValidateDesktopLevelsGeometry(const LocalDesktopGeometry& geometry, const DesktopLevelsDecodePolicy& policy = {}) noexcept;
[[nodiscard]] DesktopLevelsMargin SummarizeDesktopLevelsMargin(std::span<const std::uint64_t> histogram, double minimum) noexcept;
[[nodiscard]] const char* GetDesktopLevelsErasureName(DesktopLevelsErasure reason) noexcept;

} // namespace pbmodulation
