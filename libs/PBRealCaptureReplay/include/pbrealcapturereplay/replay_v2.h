#pragma once

#include "pbrealcapturereplay/replay_file.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pbrealcapturereplay
{

inline constexpr std::uint16_t kReplayV2FormatVersion = 2;
inline constexpr std::size_t kReplayV2FileHeaderBytes = 256;
inline constexpr std::size_t kReplayV2RecordHeaderBytes = 512;
inline constexpr std::size_t kReplayV2FileFooterBytes = 96;
inline constexpr std::uint32_t kReplayV2DefaultMaximumCaptureFrames = 256;
inline constexpr std::uint32_t kReplayV2HardMaximumCaptureFrames = 2048;
inline constexpr std::uint64_t kReplayV2DefaultMaximumFileBytes = 2ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kReplayV2HardMaximumFileBytes = 16ULL * 1024 * 1024 * 1024;
inline constexpr std::uint8_t kReplayV2DemodDetailVersion = 1;
inline constexpr std::uint32_t kReplayV2HardMaximumCodewordsPerObservation = 1024;

enum class ReplayV2RecordType : std::uint32_t
{
    Capture = 1,
    DemodObservation = 2
};

enum class ReplayV2DemodDisposition : std::uint8_t
{
    Unavailable,
    Erasure,
    Rejected,
    Accepted
};

// These enums are Replay evidence vocabulary rather than dependencies on a
// particular demodulator implementation. Unavailable is reserved for legacy
// v2 observations that predate production-detail presence.
enum class ReplayV2DemodResultKind : std::uint8_t
{
    Unavailable,
    Transport,
    ControlRecord,
    ControlFragment,
    TelemetryOnly
};

enum class ReplayV2GeometryStatus : std::uint8_t
{
    Unavailable,
    NotApplicable,
    ExactCanvas,
    Scaled,
    Letterboxed,
    Rejected
};

enum class ReplayV2TemporalDisposition : std::uint8_t
{
    Unavailable,
    NotApplicable,
    Unique,
    DuplicateRefinement,
    DuplicateSuppressed,
    Reordered,
    StaleCompletion
};

struct ReplayV2Limits
{
    std::uint64_t maximumFileBytes = kReplayV2DefaultMaximumFileBytes;
    std::uint64_t maximumRasterBytesPerFrame = 128ULL * 1024 * 1024;
    std::uint64_t maximumTotalRasterBytes = kReplayV2DefaultMaximumFileBytes;
    std::uint32_t maximumCaptureFrames = kReplayV2DefaultMaximumCaptureFrames;
    std::uint32_t maximumMetadataBytes = 256 * 1024;
    std::uint32_t maximumDisplayIdentityBytes = 4096;
    std::uint32_t maximumCanonicalBootstrapBytes = 4096;
    std::uint32_t maximumDimension = 16384;
};

struct ReplayV2FileDescriptor
{
    ReplayDatasetClass datasetClass = ReplayDatasetClass::RemoteVisual;
    std::array<std::byte, 16> datasetId{};
    std::string runId;
    std::uint64_t visualProfileId = 0;
    std::int64_t createdUtc100ns = 0;
    // Versioned UTF-8 experiment metadata. It is evidence only and is never
    // consulted by Bootstrap, demodulation, FEC, Receiver, or acceptance.
    std::string remoteMetadataJsonUtf8;
    bool operator==(const ReplayV2FileDescriptor&) const = default;
};

struct ReplayV2CaptureView
{
    pbcapturenormalize::ScreenCaptureFrameMetadata capture;
    std::uint32_t dpiX = 0;
    std::uint32_t dpiY = 0;
    double scaleX = 0;
    double scaleY = 0;
    std::string_view displayIdentityUtf8;
    ReplayRasterView capturedRoi;
    std::optional<ReplayRasterView> senderCanonicalRaster;
    std::span<const std::byte> canonicalBootstrap;
};

struct ReplayV2DemodObservationView
{
    std::uint64_t captureEpoch = 0;
    std::uint64_t captureObservation = 0;
    std::uint64_t visualProfileId = 0;
    ReplayV2DemodDisposition disposition = ReplayV2DemodDisposition::Unavailable;
    bool bootstrapAttempted = false;
    bool bootstrapSucceeded = false;
    bool frameSequenceAvailable = false;
    std::uint64_t frameSequence = 0;
    bool transportProduced = false;
    bool receiverAdmitted = false;
    std::uint32_t diagnosticCode = 0;

    // Optional production result detail. Absence preserves the byte-exact v2
    // layout emitted before Step 15 and means that every following field is
    // unavailable, not zero. Presence is encoded explicitly in the record
    // flags so receiver-only evidence never invents sender truth.
    bool productionDetailAvailable = false;
    bool layoutAvailable = false;
    std::uint8_t visualLayoutVersion = 0;
    ReplayV2DemodResultKind resultKind = ReplayV2DemodResultKind::Unavailable;
    bool geometryAvailable = false;
    ReplayV2GeometryStatus geometryStatus = ReplayV2GeometryStatus::Unavailable;
    double geometryOriginX = 0;
    double geometryOriginY = 0;
    double geometryScaleX = 0;
    double geometryScaleY = 0;
    ReplayV2TemporalDisposition temporalDisposition = ReplayV2TemporalDisposition::Unavailable;
    bool evaluationAvailable = false;
    bool paddingValid = false;
    bool senderTruthAvailable = false;
    std::uint32_t codewords = 0;
    std::uint32_t fecFailures = 0;
    std::uint32_t crcFailures = 0;
    std::uint32_t identityFailures = 0;
    std::uint32_t falseAcceptedCodewords = 0;
    std::uint32_t acceptedTransportBlocks = 0;
    std::uint32_t acceptedRemoteControlBlocks = 0;
    std::uint32_t admittedTransportBlocks = 0;
    std::uint32_t admittedRemoteControlBlocks = 0;
    std::uint32_t iterationsTotal = 0;
    std::uint32_t iterationsMaximum = 0;
    std::uint64_t comparedCodedBits = 0;
    std::uint64_t erroneousCodedBits = 0;
    bool metricSummaryAvailable = false;
    std::uint32_t metricSamples = 0;
    std::uint32_t zeroMagnitudeMetrics = 0;
    double minimumAbsoluteMetric = 0;
    double meanAbsoluteMetric = 0;
    std::uint32_t freshnessRegions = 0;
    std::uint32_t staleRegions = 0;
    std::uint32_t freshnessTagMismatches = 0;
    std::uint32_t freshnessTagErasures = 0;
    std::uint32_t freshnessErasedDataMetrics = 0;
    std::uint32_t unreliableSymbols = 0;
    std::uint64_t metricReadbackBytes = 0;
    bool gpuTimingAvailable = false;
    std::uint64_t gpuTime100ns = 0;
    // carrierAccepted reports validation by the existing Receiver pipeline;
    // receiverStateAdvanced distinguishes a unique state mutation from an
    // identical duplicate. Legacy receiverAdmitted remains transport-only.
    bool carrierAccepted = false;
    bool receiverStateAdvanced = false;
};

struct ReplayV2Capture
{
    pbcapturenormalize::ScreenCaptureFrameMetadata capture;
    std::uint32_t dpiX = 0;
    std::uint32_t dpiY = 0;
    double scaleX = 0;
    double scaleY = 0;
    std::string displayIdentityUtf8;
    ReplayRaster capturedRoi;
    std::optional<ReplayRaster> senderCanonicalRaster;
    std::vector<std::byte> canonicalBootstrap;
};

struct ReplayV2Record
{
    ReplayV2RecordType type = ReplayV2RecordType::Capture;
    std::uint32_t ordinal = 0;
    ReplayV2Capture capture;
    ReplayV2DemodObservationView demodObservation;
};

struct ReplayV2FileSnapshot
{
    ReplayV2FileDescriptor descriptor;
    std::uint64_t fileBytes = 0;
    std::uint64_t totalRasterBytes = 0;
    std::uint32_t captureFrames = 0;
    std::uint32_t demodObservations = 0;
    std::uint32_t recordsProcessed = 0;
    bool complete = false;
};

// Receiver-side record-based evidence writer. Capture records own only the
// selected ROI. Asynchronous demod observations are separate immutable records
// linked by (CaptureEpoch, CaptureObservation). The writer is single-owner,
// bounded, uses CREATE_NEW for .partial, and never overwrites its final path.
class ReplayV2Writer
{
public:
    struct Implementation;

    ReplayV2Writer() noexcept;
    ReplayV2Writer(ReplayV2Writer&&) noexcept;
    ReplayV2Writer& operator=(ReplayV2Writer&&) noexcept;
    ~ReplayV2Writer();
    ReplayV2Writer(const ReplayV2Writer&) = delete;
    ReplayV2Writer& operator=(const ReplayV2Writer&) = delete;

    [[nodiscard]] static ReplayStatus Create(const std::filesystem::path& targetPath,
        const ReplayV2FileDescriptor& descriptor, const ReplayV2Limits& limits,
        std::unique_ptr<ReplayV2Writer>& output) noexcept;
    [[nodiscard]] ReplayStatus AppendCapture(const ReplayV2CaptureView& capture) noexcept;
    [[nodiscard]] ReplayStatus AppendDemodObservation(const ReplayV2DemodObservationView& observation) noexcept;
    [[nodiscard]] ReplayStatus Finalize() noexcept;
    [[nodiscard]] ReplayV2FileSnapshot GetSnapshot() const noexcept;

private:
    explicit ReplayV2Writer(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

// Open validates fixed and variable headers, footer counts, CRC32C and the
// whole-stream BLAKE3 before exposing records. ReadNext validates all lengths
// before allocation and leaves output unchanged on every failure.
class ReplayV2Reader
{
public:
    struct Implementation;

    ReplayV2Reader() noexcept;
    ReplayV2Reader(ReplayV2Reader&&) noexcept;
    ReplayV2Reader& operator=(ReplayV2Reader&&) noexcept;
    ~ReplayV2Reader();
    ReplayV2Reader(const ReplayV2Reader&) = delete;
    ReplayV2Reader& operator=(const ReplayV2Reader&) = delete;

    [[nodiscard]] static ReplayStatus Open(const std::filesystem::path& path,
        const ReplayV2Limits& limits, std::unique_ptr<ReplayV2Reader>& output) noexcept;
    [[nodiscard]] ReplayStatus ReadNext(ReplayV2Record& output) noexcept;
    [[nodiscard]] ReplayV2FileSnapshot GetSnapshot() const noexcept;

private:
    explicit ReplayV2Reader(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace pbrealcapturereplay
