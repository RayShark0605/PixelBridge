#pragma once

#include "pbmodulation/local_desktop_bootstrap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pbmodulation
{

enum class LumaPixelFormat : std::uint8_t
{
    Gray8, Bgra8, R10G10B10A2, Fp16LinearSdr
};

// Portable borrowed pixels. Rows may be padded; only the declared pixel bytes
// are read. FP16 means little-endian R/G/B/A half floats in linear SDR [0,1],
// not an unspecified HDR/PQ surface. The decoder performs no full-frame copy.
struct LumaView
{
    std::span<const std::byte> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t rowPitch = 0;
    LumaPixelFormat pixelFormat = LumaPixelFormat::Gray8;
};

enum class LocalDesktopErasureReason : std::uint8_t
{
    None, InvalidView, InvalidPolicy, UnsupportedFormat, NonFinitePixel, InvalidPixelValue,
    SampleOutOfBounds, WorkBudgetExceeded, MarkerBudgetExceeded, GeometryBudgetExceeded,
    MarkersNotFound, IncompleteMarkers, InvalidGeometry, AmbiguousGeometry, LowContrast,
    BootstrapFecFailure, BootstrapCrcFailure, UnsupportedRecord, BootstrapMismatch,
    TimingMismatch, DoubleImage, ExcessResidual
};

// Local receiver limits/quality gates, not serialized profile parameters.
// Defaults are fixed together and are shared by all formats/scales. Callers may
// reduce work/candidate limits; no unlimited values or unbounded refine loops.
struct LocalDesktopDecodePolicy
{
    std::uint64_t maximumWorkUnits = 24000000;
    std::uint32_t maximumMarkers = 64;
    std::uint32_t maximumGeometries = 32;
    std::uint32_t maximumRefinementIterations = 4;
    double minimumScale = 0.5;
    double maximumScale = 2.0;
    double minimumContrast = 96.0;
    double maximumMarkerResidual = 0.075;
    double maximumBootstrapResidual = 0.08;
    double maximumTimingResidual = 0.065;
    double maximumTimingBitErrorFraction = 0.02;
    // Applies independently to cell means and the original five core samples;
    // neither statistic may hide mid-gray pixels by averaging the other away.
    double maximumMidGrayFraction = 0.06;
    double midGrayBoundary = 0.18;
    double maximumGeometryResidualPixels = 1.25;
};

// Pixel-edge coordinates in the supplied view: a logical point (x,y) maps to
// (originX + scaleX*x, originY + scaleY*y). No integer origin/scale rounding,
// perspective warp, whole-frame resize, Windows monitor, or capture domain.
struct LocalDesktopGeometry
{
    double originX = 0;
    double originY = 0;
    double scaleX = 0;
    double scaleY = 0;
    double markerResidualPixels = 0;
    bool operator==(const LocalDesktopGeometry&) const = default;
};

struct LocalDesktopCopyObservation
{
    std::array<std::byte, kLocalDesktopBootstrapRecordBytes> canonical44{};
    std::uint32_t correctedSymbols = 0;
    bool fecDecoded = false;
    bool crcValid = false;
    bool recordValid = false;
    double residual = 0;
    double midGrayFraction = 0;
    double sampleMidGrayFraction = 0;
};

struct LocalDesktopObservation
{
    LocalDesktopErasureReason erasure = LocalDesktopErasureReason::MarkersNotFound;
    std::array<std::byte, kLocalDesktopBootstrapRecordBytes> canonical44{};
    std::array<LocalDesktopCopyObservation, 2> copies;
    LocalDesktopGeometry geometry;
    double blackLevel = 0;
    double whiteLevel = 0;
    double quality = 0;
    double timingResidual = 0;
    double timingBitErrorFraction = 0;
    double midGrayFraction = 0;
    // Maximum over each independent A/B copy and each distributed timing
    // patch, using all five original core samples before cell averaging.
    double sampleMidGrayFraction = 0;
    std::uint32_t markerCandidates = 0;
    std::uint32_t geometryCandidates = 0;
    std::uint64_t workUnits = 0;

    [[nodiscard]] bool IsAccepted() const noexcept
    {
        return erasure == LocalDesktopErasureReason::None;
    }
};

// Sample coordinates address pixel centres: (0,0) is the first pixel. Bilinear
// interpolation reads at most four pixels, never clamps outside the view, and
// leaves output unchanged on failure. All sampled components must be finite.
[[nodiscard]] LocalDesktopErasureReason ValidateLumaView(const LumaView& view) noexcept;
[[nodiscard]] LocalDesktopErasureReason SampleLuma(const LumaView& view, double x, double y, double& output) noexcept;

// Fixed-size result on both acceptance and erasure. Only accepted canonical44
// may advance application state. Each A/B copy is independently RS/CRC parsed;
// all 44 bytes must agree and every distributed timing patch must match. This
// function is stateless and does not track capture epochs or duplicate frames.
[[nodiscard]] LocalDesktopObservation DecodeLocalDesktopBootstrap(const LumaView& view, const LocalDesktopDecodePolicy& policy = {}) noexcept;
[[nodiscard]] const char* GetLocalDesktopErasureName(LocalDesktopErasureReason reason) noexcept;

} // namespace pbmodulation
