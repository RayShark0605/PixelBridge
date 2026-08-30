#pragma once

#include "pbcapturenormalize/screen_capture_frame.h"
#include "pbdesktoplevels/reference_channel.h"
#include "pbpresenttiming/present_timing.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>

namespace pbtelemetry
{

enum class TelemetryError : std::uint8_t
{
    None, InvalidEpoch, InactiveEpoch, DomainMismatch, ObservationOrder, TimestampOrder, InvalidSample, CounterOverflow
};

struct TelemetryStatus
{
    TelemetryError code = TelemetryError::None;
    [[nodiscard]] explicit operator bool() const noexcept { return code == TelemetryError::None; }
    [[nodiscard]] static TelemetryStatus Failure(TelemetryError code) noexcept
    {
        return {code == TelemetryError::None ? TelemetryError::InvalidSample : code};
    }
    bool operator==(const TelemetryStatus&) const = default;
};

struct CaptureSample
{
    pbcapturenormalize::ScreenCaptureDomain domain;
    std::uint64_t captureObservation = 0;
    std::int64_t timestamp100ns = 0;
    std::optional<std::uint64_t> roiCopyTime100ns;
    // BLAKE3-256 or another collision-resistant content identity over the
    // admitted, format-preserving ROI bytes. Absence is explicit coverage loss;
    // no timestamp, observation ID, or sender cadence is guessed as uniqueness.
    std::optional<std::array<std::byte, 32>> pixelDigest;
};

struct BootstrapSample
{
    pbcapturenormalize::ScreenCaptureDomain domain;
    std::uint64_t captureObservation = 0;
    bool success = false;
    std::optional<double> scaleX;
    std::optional<double> scaleY;
    std::optional<double> phaseX;
    std::optional<double> phaseY;
};

struct FecSample
{
    pbcapturenormalize::ScreenCaptureDomain domain;
    std::uint64_t captureObservation = 0;
    pbdesktoplevels::FrameEvaluation evaluation;
};

enum class OuterSymbolDisposition : std::uint8_t
{
    AcceptedUnique, Duplicate, Conflict, Rejected
};

struct TelemetrySnapshot
{
    bool active = false;
    pbcapturenormalize::ScreenCaptureDomain domain;
    std::int64_t epochStart100ns = 0;
    std::uint64_t epochStarts = 0;
    std::optional<pbpresenttiming::TimingSnapshot> presentation;

    std::uint64_t capturedFrames = 0;
    std::uint64_t fingerprintedFrames = 0;
    std::uint64_t uniqueVisualFrames = 0;
    std::uint64_t duplicateFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t captureIntervals = 0;
    std::optional<double> captureFps;
    std::optional<double> uniqueVisualFps;
    std::optional<double> duplicateFrameRatio;
    std::optional<double> frameArrivalJitterMilliseconds;

    std::uint64_t roiCopyTimingSamples = 0;
    std::uint64_t roiCopyTimeTotal100ns = 0;
    std::uint64_t roiCopyTimeMaximum100ns = 0;
    std::optional<double> roiCopyTimeAverageMilliseconds;
    std::optional<double> roiCopyTimeMaximumMilliseconds;

    std::uint64_t bootstrapAttempts = 0;
    std::uint64_t bootstrapSuccesses = 0;
    std::optional<double> bootstrapSuccessRate;
    std::optional<double> scaleX;
    std::optional<double> scaleY;
    std::optional<double> phaseX;
    std::optional<double> phaseY;

    std::uint64_t comparedCodedBits = 0;
    std::uint64_t erroneousCodedBits = 0;
    std::uint64_t fecEvaluatedFrames = 0;
    std::uint64_t fecCodewords = 0;
    std::uint64_t fecFailures = 0;
    std::uint64_t crcFailures = 0;
    std::uint64_t postFecFailedFrames = 0;
    std::optional<double> preFecBerEstimate;
    std::optional<double> fecFrameErrorRate;
    std::optional<double> fecCodewordFailureRate;

    std::uint64_t uniqueOuterSymbols = 0;
    std::uint64_t duplicateOuterSymbols = 0;
    std::uint64_t acceptedOuterSymbols = 0;
    std::uint64_t outerSymbolConflicts = 0;
    std::uint64_t rejectedOuterSymbols = 0;

    std::uint64_t verifiedEncodedBytes = 0;
    std::optional<double> verifiedEncodedGoodputBitsPerSecond;
    bool counterSaturated = false;
};

// Single-owner accumulator. It consumes already-authoritative component
// observations; it does not infer PresentedVisualFPS, decode pixels, accept
// Transport blocks, or mark bytes verified on its own.
class TelemetryAccumulator
{
public:
    [[nodiscard]] TelemetryStatus BeginCaptureEpoch(const pbcapturenormalize::ScreenCaptureDomain& domain,
        std::int64_t epochStart100ns) noexcept;
    // Closes admission while retaining the final epoch counters for reporting.
    // A later BeginCaptureEpoch starts a fresh per-epoch snapshot.
    void EndCaptureEpoch() noexcept;
    [[nodiscard]] TelemetryStatus RecordPresentation(const pbpresenttiming::TimingSnapshot& sample) noexcept;
    [[nodiscard]] TelemetryStatus RecordCapture(const CaptureSample& sample) noexcept;
    void RecordDroppedFrames(std::uint64_t count) noexcept;
    [[nodiscard]] TelemetryStatus RecordBootstrap(const BootstrapSample& sample) noexcept;
    // The caller must apply FrameSequence/admission deduplication first. One
    // independently admitted visual frame contributes at most one sample.
    // Production demodulation without sender truth uses compared/erroneous
    // coded bits 0/0: FER remains covered while PreFecBER stays unavailable.
    // Any nonzero comparison must cover the complete coded frame.
    [[nodiscard]] TelemetryStatus RecordFec(const FecSample& sample) noexcept;
    void RecordOuterSymbol(OuterSymbolDisposition disposition) noexcept;
    // Call only after the authoritative receiver/storage digest gate has
    // accepted these encoded bytes. Timestamp is the corresponding completion
    // time in the same monotonic 100ns domain as capture samples.
    [[nodiscard]] TelemetryStatus RecordVerifiedEncodedBytes(std::uint64_t bytes, std::int64_t timestamp100ns) noexcept;
    [[nodiscard]] TelemetrySnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] bool Add(std::uint64_t& target, std::uint64_t value) noexcept;
    [[nodiscard]] TelemetryStatus ValidateDomain(const pbcapturenormalize::ScreenCaptureDomain& domain) const noexcept;

    TelemetrySnapshot snapshot_;
    std::uint64_t lifetimeEpochStarts_ = 0;
    std::uint64_t lastCaptureObservation_ = 0;
    std::uint64_t lastBootstrapObservation_ = 0;
    std::uint64_t lastFecObservation_ = 0;
    std::int64_t firstCaptureTimestamp100ns_ = 0;
    std::int64_t lastCaptureTimestamp100ns_ = 0;
    std::int64_t firstVisualTimestamp100ns_ = 0;
    std::int64_t lastVisualTimestamp100ns_ = 0;
    std::int64_t verifiedTimestamp100ns_ = 0;
    double intervalMean100ns_ = 0;
    double intervalM2_ = 0;
    std::optional<std::array<std::byte, 32>> previousPixelDigest_;
};

void WriteTelemetryJson(std::ostream& output, const TelemetrySnapshot& snapshot);
[[nodiscard]] const char* GetTelemetryErrorName(TelemetryError error) noexcept;

} // namespace pbtelemetry
