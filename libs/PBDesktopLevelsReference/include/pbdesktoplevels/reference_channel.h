#pragma once

#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/reference_visual_profile.h"
#include "pbmodulation/remote_visual.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbmodulation/shape_chroma.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>

// Experimental diagnostic orchestration only. No file-receiver state or
// alternate codec lives here: both the application and offline probe use the
// existing modulation, QC-LDPC and Transport implementations through this API.
namespace pbdesktoplevels
{

inline constexpr std::size_t kInfoBytes = 1350;
inline constexpr std::size_t kCodewordBytes = 2025;
inline constexpr std::size_t kPayloadBytes = 1314;
inline constexpr std::size_t kCodewordBits = 16200;
inline constexpr std::size_t kMaximumCodewords = 42;
// Conservative complete processing reservation, including modulation scratch,
// caller outputs, one pre-created LDPC workspace, pending/aggregate histograms
// and bounded application events. Capture/readback storage is charged separately.
inline constexpr std::uint64_t kProcessingReservationBytes = 16ULL * 1024 * 1024;
inline constexpr double kSoftMetricScale = 4096;

// Diagnostic payload domain + canonical 44-byte Bootstrap + slot LE32 + chunk
// LE32, each BLAKE3-256 digest contributing up to 32 bytes. The final 1314 bytes
// fill a Transport block exactly; the remaining raster tail is canonical zero.
// Stages output: invalid binding/size/alias or codec failure changes no bytes.
[[nodiscard]] pbmodulation::ModulationStatus GenerateDiagnosticData(std::span<const std::byte> bootstrapRecord,
                                                                  std::span<std::byte> logicalData) noexcept;
// Explicit, fixed, receiver-local adapter. Nonfinite inputs reject the whole
// operation without output mutation. This is NOT a calibrated statistical LLR.
[[nodiscard]] bool AdaptSoftMetrics(std::span<const float> metrics, std::span<std::int16_t> output) noexcept;

struct FrameEvaluation
{
    bool evaluated = false;
    bool paddingValid = false;
    std::uint32_t codewords = 0;
    std::uint32_t fecFailures = 0;
    std::uint32_t crcFailures = 0;
    std::uint32_t identityFailures = 0;
    std::uint32_t falseAcceptedCodewords = 0;
    std::uint32_t acceptedTransportBlocks = 0;
    std::uint32_t acceptedRemoteControlBlocks = 0;
    // Actual iterative passes; a received soft-decision codeword with an
    // initially zero syndrome needs zero passes, but still undergoes CRC/truth.
    std::uint32_t iterationsTotal = 0;
    std::uint32_t iterationsMaximum = 0;
    std::uint64_t comparedCodedBits = 0;
    std::uint64_t erroneousCodedBits = 0;

    [[nodiscard]] bool IsVerified() const noexcept;
};

struct ReferenceObservation
{
    pbmodulation::DesktopLevelsObservation modulation;
    FrameEvaluation evaluation;
};

struct ShapeChromaReferenceObservation
{
    pbmodulation::ShapeChromaObservation modulation;
    FrameEvaluation evaluation;
};

struct RemoteVisualReferenceObservation
{
    pbmodulation::RemoteVisualObservation modulation;
    FrameEvaluation evaluation;
};

struct RemoteVisualLowFpsReferenceObservation
{
    pbmodulation::RemoteVisualLowFpsObservation modulation;
    FrameEvaluation evaluation;
};

struct AcceptedTransportBlock
{
    std::uint32_t slot = 0;
    std::uint32_t byteCount = 0;
    std::array<std::byte, kInfoBytes> bytes{};
    bool operator==(const AcceptedTransportBlock&) const = default;
};

enum class AcceptedRemoteControlKind : std::uint8_t
{
    Record
};

struct AcceptedRemoteControlBlock
{
    AcceptedRemoteControlKind kind = AcceptedRemoteControlKind::Record;
    std::uint32_t byteCount = 0;
    std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> bytes{};
    bool operator==(const AcceptedRemoteControlBlock&) const = default;
};

enum class EvaluationMode : std::uint8_t
{
    DiagnosticTruth, Transport
};

class ReferenceChannel
{
public:
    ReferenceChannel() noexcept;
    ReferenceChannel(ReferenceChannel&&) noexcept;
    ReferenceChannel& operator=(ReferenceChannel&&) noexcept;
    ~ReferenceChannel();
    ReferenceChannel(const ReferenceChannel&) = delete;
    ReferenceChannel& operator=(const ReferenceChannel&) = delete;
    [[nodiscard]] static pbmodulation::ModulationResult<ReferenceChannel> Create(std::uint64_t maximumBytes) noexcept;
    // Borrows pixels only during this call. No allocation or codec creation.
    // Expected data is generated ONLY AFTER every codeword has gone through
    // soft decoding, syndrome, CRC and identity checks; it cannot aid recovery.
    [[nodiscard]] ReferenceObservation Decode(const pbmodulation::LumaView& view,
                                             const pbmodulation::DesktopLevelsDecodePolicy& policy = {}) noexcept;
    [[nodiscard]] ShapeChromaReferenceObservation DecodeShapeChroma(const pbmodulation::LumaView& view,
        const pbmodulation::ShapeChromaDecodePolicy& policy = {}) noexcept;
    [[nodiscard]] RemoteVisualReferenceObservation DecodeRemoteVisual(const pbmodulation::LumaView& view,
        const pbmodulation::RemoteVisualDecodePolicy& policy = {}) noexcept;
    [[nodiscard]] RemoteVisualLowFpsReferenceObservation DecodeRemoteVisualLowFps(const pbmodulation::LumaView& view,
        const pbmodulation::RemoteVisualLowFpsDecodePolicy& policy = {},
        EvaluationMode mode = EvaluationMode::DiagnosticTruth) noexcept;
    // Shared post-demod pipeline, also useful for independent channel tests.
    // No expected payload is accepted as an argument.
    // DiagnosticTruth additionally compares against the deterministic Gate
    // payload and binds its synthetic segment/block identifiers to the visual
    // sequence/slot. Transport performs the production trust boundary only:
    // QC-LDPC, canonical Transport framing/CRC, Bootstrap SessionTag and
    // canonical zero padding. It never maps FrameSequence to SegmentOrdinal.
    [[nodiscard]] FrameEvaluation EvaluateCodewords(std::span<const std::byte> bootstrapRecord,
        std::span<const std::byte> hardData, std::span<const float> softMetrics,
        EvaluationMode mode = EvaluationMode::DiagnosticTruth) noexcept;
    [[nodiscard]] std::span<const std::uint64_t> GetMarginHistogram() const noexcept;
    [[nodiscard]] std::span<const std::uint64_t> GetShapeMarginHistogram() const noexcept;
    [[nodiscard]] std::span<const std::uint64_t> GetChromaMarginHistogram() const noexcept;
    [[nodiscard]] std::span<const std::uint64_t> GetRemoteVisualMarginHistogram() const noexcept;
    [[nodiscard]] std::span<const std::uint64_t> GetRemoteVisualLowFpsMarginHistogram() const noexcept;
    // Exact serialized Transport blocks that passed FEC, canonical padding,
    // CRC and identity in the most recent evaluation. Invalid after the next
    // Decode/Evaluate call or move.
    [[nodiscard]] std::span<const AcceptedTransportBlock> GetAcceptedTransportBlocks() const noexcept;
    // RemoteVisual-only physical carrier. The returned bytes are the original
    // PB-Control record/fragment after the unchanged Robust QC-LDPC and its
    // existing CRC/SessionTag checks; no alternate control-plane parser exists.
    [[nodiscard]] std::span<const AcceptedRemoteControlBlock> GetAcceptedRemoteControlBlocks() const noexcept;
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

struct StatisticsSummary
{
    std::uint64_t frames = 0;
    std::uint64_t verifiedFrames = 0;
    std::uint64_t comparedCodedBits = 0;
    std::uint64_t erroneousCodedBits = 0;
    std::uint64_t preFecFailedFrames = 0;
    std::uint64_t postFecFailedFrames = 0;
    std::uint64_t falseAcceptedCodewords = 0;
    std::uint64_t codewords = 0;
    std::uint64_t fecFailures = 0;
    std::uint64_t crcFailures = 0;
    std::uint64_t identityFailures = 0;
    std::uint64_t paddingFailedFrames = 0;
    std::uint64_t iterationsTotal = 0;
    std::uint32_t iterationsMaximum = 0;
    std::uint16_t observedPhases = 0;
    std::uint16_t verifiedPhases = 0;
    pbmodulation::DesktopLevelsMargin margin;
};

// Caller is responsible for admission/deduplication BEFORE Add. Separate
// instances per candidate/backend; never combines soft data across frames.
class ReferenceStatistics
{
public:
    [[nodiscard]] bool Add(const FrameEvaluation& evaluation, std::uint64_t sequence,
        std::span<const std::uint64_t> histogram, double minimumMargin) noexcept;
    [[nodiscard]] StatisticsSummary GetSummary() const noexcept;
    void Reset() noexcept;
private:
    StatisticsSummary summary_;
    std::array<std::uint64_t, pbmodulation::kDesktopLevelsMarginBins> histogram_{};
};

// Stable JSON object shared by offline and real-capture reports. Ratios with
// zero denominators are null; all integer numerators/denominators are retained.
void WriteStatisticsJson(std::ostream& output, const StatisticsSummary& statistics);
void WriteEvaluationJson(std::ostream& output, const FrameEvaluation& evaluation);
void WriteModulationJson(std::ostream& output, const pbmodulation::DesktopLevelsObservation& observation);

} // namespace pbdesktoplevels
