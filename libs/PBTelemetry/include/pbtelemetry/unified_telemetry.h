#pragma once

#include "pbmodulation/unified_visual.h"
#include "telemetry.h"

namespace pbtelemetry
{

struct UnifiedLaneCounters
{
    std::uint64_t metricObservations = 0;
    std::uint64_t metricSamples = 0;
    std::uint64_t zeroMetrics = 0;
    std::uint64_t erasedMetrics = 0;
    std::uint64_t absoluteMetricSum = 0;
    std::optional<std::uint32_t> minimumAbsoluteMetric;
    std::uint64_t evaluatedSlots = 0;
    std::uint64_t fecAttempts = 0;
    std::uint64_t fecFailures = 0;
    std::uint64_t crcFailures = 0;
    std::uint64_t identityFailures = 0;
    std::uint64_t acceptedSlots = 0;
    std::uint64_t erasedSlots = 0;
    std::uint64_t fecIterations = 0;
};

enum class UnifiedCoverageFailureReason : std::uint8_t
{
    ExternalInvalidation,
    InvalidSample,
    TimestampOrder,
    InvalidIdentity,
    ReorderedIdentity,
    SessionBinding,
    AmbiguousEvictedIdentity,
    CounterOverflow
};

struct UnifiedCoverageFailure
{
    UnifiedCoverageFailureReason reason = UnifiedCoverageFailureReason::ExternalInvalidation;
    std::uint64_t observationsBeforeFailure = 0;
    std::uint64_t uniqueFramesBeforeFailure = 0;
    std::optional<std::uint64_t> frameSequence;
    std::optional<std::uint64_t> captureEpoch;
    std::optional<std::int64_t> timestamp100ns;
    std::optional<std::int64_t> previousTimestamp100ns;
};

struct UnifiedTelemetrySnapshot
{
    bool sessionBound = false;
    std::uint64_t sessionTag = 0;
    bool observationAvailable = false;
    bool frameCoverageComplete = true;
    bool counterOverflow = false;
    std::optional<UnifiedCoverageFailure> firstCoverageFailure;
    std::uint64_t observations = 0;
    std::uint64_t frameErasedObservations = 0;
    std::uint64_t uniqueFrames = 0;
    std::uint64_t duplicateObservations = 0;
    std::optional<double> uniqueVisualFps;
    // Carrier occupancy is observable only after FEC classification. Unknown
    // slots remain explicit; they are not guessed to be Transport or Control.
    std::uint64_t classifiedControlSlots = 0;
    std::uint64_t acceptedControlSlots = 0;
    std::uint64_t unclassifiedSlots = 0;
    std::array<UnifiedLaneCounters, 3> lanes{};
};

// Run/Session-scoped, single-owner, bounded telemetry. CaptureEpoch does not
// change a logical frame's identity. This never participates in admission.
class UnifiedTelemetryAccumulator
{
public:
    [[nodiscard]] TelemetryStatus BindSession(std::uint64_t sessionTag) noexcept;
    [[nodiscard]] TelemetryStatus Record(const pbmodulation::UnifiedVisualObservation& observation,
        std::uint64_t captureEpoch, std::int64_t timestamp100ns) noexcept;
    [[nodiscard]] TelemetryStatus RecordUnavailableFrame(const pbprotocol::BootstrapRecord& bootstrap,
        std::uint64_t captureEpoch, std::int64_t timestamp100ns) noexcept;
    [[nodiscard]] UnifiedTelemetrySnapshot GetSnapshot() const noexcept;
    // Fixed-size first-failure evidence only; this never repairs coverage or
    // changes frame counts, temporal admission, or the published-metric gate.
    void InvalidateFrameCoverage(UnifiedCoverageFailureReason reason = UnifiedCoverageFailureReason::ExternalInvalidation,
        const pbprotocol::BootstrapRecord* bootstrap = nullptr, std::uint64_t captureEpoch = 0,
        std::int64_t timestamp100ns = -1) noexcept;

private:
    [[nodiscard]] TelemetryStatus RecordSample(const pbprotocol::BootstrapRecord& bootstrap,
        const pbmodulation::UnifiedVisualObservation* observation, std::uint64_t captureEpoch,
        std::int64_t timestamp100ns) noexcept;
    UnifiedTelemetrySnapshot snapshot_;
    // The fixed ring retains exact recent identities. Once eviction begins, the
    // maximum evicted identity is a conservative boundary for ambiguous old input.
    std::array<std::uint64_t, 4096> recentSequences_{};
    std::size_t recentCount_ = 0;
    std::size_t nextSequence_ = 0;
    bool hasEvictedSequence_ = false;
    std::uint64_t maximumEvictedSequence_ = 0;
    std::int64_t firstUniqueTimestamp100ns_ = 0;
    std::int64_t lastUniqueTimestamp100ns_ = 0;
    std::int64_t lastObservationTimestamp100ns_ = 0;
};

struct PublishedFrameMetric
{
    std::optional<double> bytesPerUniqueFrame;
    const char* unavailableReason = "NotPublished";
};

[[nodiscard]] PublishedFrameMetric EvaluatePublishedFrameMetric(const UnifiedTelemetrySnapshot& telemetry,
    std::uint64_t encodedBytes, bool wholeDigestVerified, bool published, bool finalReopenVerified,
    bool resumed) noexcept;
void WriteUnifiedTelemetryJson(std::ostream& output, const UnifiedTelemetrySnapshot& snapshot);

} // namespace pbtelemetry
