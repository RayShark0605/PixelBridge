#pragma once

#include "pbmodulation/local_desktop_decode.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace pbmodulation
{

// Experimental A/B baseline only. The codebook is intentionally literal and
// unoptimized: 16 balanced 4x4 luma templates plus four approximately
// iso-luma chroma offsets. It is not a learned model or a Certified profile.
inline constexpr std::uint64_t kShapeChromaProfileId = 0x5042534843503031ULL;
inline constexpr std::uint8_t kShapeChromaLayoutVersion = 4;
inline constexpr std::uint32_t kShapeChromaTilePixels = 4;
inline constexpr std::uint32_t kShapeChromaTileCount = 86688;
inline constexpr std::uint32_t kShapeChromaBitsPerTile = 6;
inline constexpr std::uint32_t kShapeChromaShapeBits = 4;
inline constexpr std::uint32_t kShapeChromaChromaBits = 2;
inline constexpr std::uint32_t kShapeChromaDataBytes = 65016;
inline constexpr std::uint32_t kShapeChromaCodewords = 32;
inline constexpr std::uint32_t kShapeChromaPaddingBytes = 216;
inline constexpr std::size_t kShapeChromaMaximumBits = static_cast<std::size_t>(kShapeChromaDataBytes) * 8;
inline constexpr std::size_t kShapeChromaMetricBins = 4096;
inline constexpr std::uint8_t kShapeChromaLowLuma = 80;
inline constexpr std::uint8_t kShapeChromaHighLuma = 176;

// Bit n addresses row-major pixel (n % 4, n / 4); one means high luma.
// Every mask has exactly eight high and eight low pixels. IDs are their
// straightforward four-bit labels for this baseline; label optimization is
// deliberately deferred.
inline constexpr std::array<std::uint16_t, 16> kShapeChromaTemplates{
    0x00FF, 0xFF00, 0x3333, 0xCCCC, 0x0FF0, 0xF00F, 0x6666, 0x9999,
    0x7331, 0x8CCE, 0x1337, 0xECC8, 0x8C73, 0x738C, 0x13EC, 0xEC13};

struct ShapeChromaState
{
    std::int16_t blueOffset;
    std::int16_t greenOffset;
    std::int16_t redOffset;
    std::uint8_t label;
    bool operator==(const ShapeChromaState&) const = default;
};

// Labels follow a Gray cycle around the four capture-space centroids.
inline constexpr std::array<ShapeChromaState, 4> kShapeChromaStates{
    ShapeChromaState{-24, 7, -16, 0}, ShapeChromaState{24, 2, -16, 1},
    ShapeChromaState{24, -7, 16, 3}, ShapeChromaState{-24, -2, 16, 2}};

enum class ShapeChromaErasure : std::uint8_t
{
    None, InvalidInput, InvalidPolicy, WorkspaceUnavailable, OutputBufferTooSmall, OverlappingSpans,
    BootstrapErasure, UnsupportedProfile, UnsupportedFormat, ScaleOutOfRange, AlignmentOutOfRange,
    FrameOutOfBounds, ChromaPilotClipping, ChromaPilotVariance, ChromaPilotSeparation,
    ChromaPilotSpatialMismatch, PixelReadFailure, WorkBudgetExceeded
};

struct ShapeChromaDecodePolicy
{
    LocalDesktopDecodePolicy locator;
    double maximumScaleDriftPixels = 0.125;
    double maximumPhaseErrorPixels = 0.125;
    double maximumMarkerResidualPixels = 0.125;
    double minimumShapeAmplitude = 24;
    double maximumShapeResidual = 0.40;
    double minimumChromaSeparation = 16;
    double maximumChromaPilotStandardDeviation = 8;
    double maximumChromaPilotSpatialDeviation = 8;
    double maximumChromaResidual = 1.5;
    std::uint64_t maximumDataWorkUnits = 8000000;
};

struct ShapeChromaCalibration
{
    std::array<std::array<double, 2>, 4> centroids{};
    std::array<std::array<std::array<double, 2>, 4>, 4> spatialCentroids{};
    std::array<std::array<double, 4>, 4> spatialVariances{};
    double minimumSeparation = 0;
    double spatialDeviation = 0;
    bool operator==(const ShapeChromaCalibration&) const = default;
};

struct ShapeChromaMargin
{
    std::uint64_t samples = 0;
    double minimum = 0;
    double p50 = 0;
    double p01 = 0;
    double p001 = 0;
};

struct ShapeChromaObservation
{
    ShapeChromaErasure erasure = ShapeChromaErasure::InvalidInput;
    LocalDesktopObservation bootstrap;
    ShapeChromaCalibration calibration;
    ShapeChromaMargin shapeMargin;
    ShapeChromaMargin chromaMargin;
    std::uint32_t dataBytes = 0;
    std::uint32_t unreliableShapeTiles = 0;
    std::uint32_t unreliableChromaTiles = 0;
    std::uint64_t dataWorkUnits = 0;
    LocalDesktopErasureReason pixelError = LocalDesktopErasureReason::None;

    [[nodiscard]] bool IsAccepted() const noexcept
    {
        return erasure == ShapeChromaErasure::None;
    }
};

class ShapeChromaWorkspace
{
public:
    ShapeChromaWorkspace() noexcept;
    ShapeChromaWorkspace(ShapeChromaWorkspace&&) noexcept;
    ShapeChromaWorkspace& operator=(ShapeChromaWorkspace&&) noexcept;
    ~ShapeChromaWorkspace();
    ShapeChromaWorkspace(const ShapeChromaWorkspace&) = delete;
    ShapeChromaWorkspace& operator=(const ShapeChromaWorkspace&) = delete;
    [[nodiscard]] static std::uint64_t RequiredBytes() noexcept;
    [[nodiscard]] static ModulationResult<ShapeChromaWorkspace> Create(std::uint64_t maximumBytes) noexcept;
    [[nodiscard]] std::span<const std::uint64_t> GetShapeMarginHistogram() const noexcept;
    [[nodiscard]] std::span<const std::uint64_t> GetChromaMarginHistogram() const noexcept;
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
    friend ShapeChromaObservation DecodeShapeChromaFrame(const LumaView&, ShapeChromaWorkspace&, std::span<std::byte>, std::span<float>, const ShapeChromaDecodePolicy&) noexcept;
};

// logicalData is a complete 65,016-byte coded-bit plane. Its final 216 bytes
// are canonical zero padding because only 32 complete 2,025-byte QC-LDPC
// codewords enter the shared Transport/FEC reference pipeline.
[[nodiscard]] ModulationStatus EncodeShapeChromaFrame(std::span<const std::byte> bootstrapRecord,
    std::span<const std::byte> logicalData, std::span<std::byte> outBgra) noexcept;

// CPU oracle: BGRA8 SDR only for this minimal baseline. Hard bits and Max-Log
// distance differences are reverse-interleaved and LSB-first. Positive soft
// metric prefers zero. Metrics are not statistically calibrated LLRs.
[[nodiscard]] ShapeChromaObservation DecodeShapeChromaFrame(const LumaView& view, ShapeChromaWorkspace& workspace,
    std::span<std::byte> hardBits, std::span<float> softMetrics, const ShapeChromaDecodePolicy& policy = {}) noexcept;
[[nodiscard]] ShapeChromaErasure ValidateShapeChromaGeometry(const LocalDesktopGeometry& geometry,
    const ShapeChromaDecodePolicy& policy = {}) noexcept;
[[nodiscard]] ShapeChromaMargin SummarizeShapeChromaMargin(std::span<const std::uint64_t> histogram, double minimum) noexcept;
[[nodiscard]] const char* GetShapeChromaErasureName(ShapeChromaErasure reason) noexcept;

} // namespace pbmodulation
