#pragma once

#include "pbprotocol/protocol_types.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pbapp
{

inline constexpr std::uint64_t step1MaximumDurationNanoseconds = 1800ULL * 1000000000ULL;
inline constexpr std::uint64_t step1MaximumEvidenceBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t step1MaximumSourceBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t step1MaximumSegments = 8;
inline constexpr std::size_t step1SubmittedQueueCapacity = 4096;
inline constexpr std::uint64_t step1MaximumSubmittedRecords = 131072;

enum class RunMilestone : std::uint8_t
{
    StartAccepted,
    CaptureReady,
    FirstVisualObservation,
    FirstAcceptedBootstrap,
    FirstControlAccepted,
    FirstUsefulEquation,
    LastSegmentStored,
    WholeDigestVerified,
    FinalRenameSucceeded,
    FinalReopenVerified,
    Terminal,
    PreparationComplete,
    Count
};

enum class MeasurementFailure : std::uint8_t
{
    None,
    InvalidClock,
    DurationLimit,
    QueueFull,
    RecordLimit,
    SourceLimit,
    SourceLedgerInvalid,
    IoFailure,
    EvidenceByteLimit,
    RuntimeFailure,
    ReusedRecorder
};

struct SubmittedFrameIdentity
{
    std::uint64_t sessionTag = 0;
    std::uint64_t frameSequence = 0;
    std::uint64_t carouselPass = 0;
    std::uint64_t segmentOrdinal = 0;
    std::uint64_t cyclePosition = 0;
    std::uint64_t submittedOffsetNanoseconds = 0;
};

struct MeasuredSegment
{
    std::uint64_t ordinal = 0;
    std::uint64_t rawOffset = 0;
    std::uint64_t rawBytes = 0;
    std::uint64_t encodedBytes = 0;
    pbprotocol::CompressionCodec codec = pbprotocol::CompressionCodec::Raw;
    pbprotocol::RawDigest rawDigest;
    pbprotocol::EncodedDigest encodedDigest;
    bool operator==(const MeasuredSegment &) const = default;
};

struct RunMeasurementSnapshot
{
    std::uint64_t runGeneration = 0;
    std::array<std::optional<std::uint64_t>, static_cast<std::size_t>(RunMilestone::Count)> offsetsNanoseconds;
    std::optional<std::uint64_t> firstSessionTag;
    std::optional<std::uint64_t> firstFrameSequence;
    std::optional<std::uint64_t> firstCaptureEpoch;
    std::optional<std::int64_t> firstCaptureTimestamp100ns;
    std::optional<bool> terminalSucceeded;
    MeasurementFailure failure = MeasurementFailure::None;
    std::uint64_t submittedRecords = 0;
    std::uint64_t drainedRecords = 0;
    std::uint64_t sourceBytes = 0;
    std::uint64_t encodedBytes = 0;
    std::size_t segmentCount = 0;
    std::array<MeasuredSegment, step1MaximumSegments> segments;
    std::array<std::byte, pbprotocol::kDigestBytes> sourceBlake3{};
    std::array<char, 512> compressionIdentity{};
    bool sourceLedgerComplete = false;
};

// One instance per accepted run. No payload, GPU object, filesystem or callback
// is retained here. Frame tracing is SPSC (sender worker -> evidence owner).
// Evidence failure is sticky and never enters protocol/admission decisions.
class RunMeasurementRecorder
{
  public:
    void Begin(std::uint64_t runGeneration, std::uint64_t absoluteNanoseconds) noexcept;
    void Record(RunMilestone milestone) noexcept;
    void RecordAt(RunMilestone milestone, std::uint64_t absoluteNanoseconds) noexcept;
    void RecordBootstrap(const pbprotocol::BootstrapRecord &bootstrap, std::uint64_t captureEpoch,
                         std::int64_t captureTimestamp100ns) noexcept;
    void Finish(bool succeeded) noexcept;
    void Fail(MeasurementFailure failure) noexcept;
    void CheckDuration(std::uint64_t absoluteNanoseconds) noexcept;
    void RecordSubmitted(SubmittedFrameIdentity identity, std::uint64_t absoluteNanoseconds) noexcept;
    [[nodiscard]] bool TakeSubmitted(SubmittedFrameIdentity &output) noexcept;
    void RecordSegment(const pbprotocol::SegmentDescriptor &descriptor) noexcept;
    void CompleteSource(std::uint64_t sourceBytes, std::span<const std::byte, pbprotocol::kDigestBytes> digest,
                        std::string_view compressionIdentity) noexcept;
    [[nodiscard]] RunMeasurementSnapshot GetSnapshot() const;

  private:
    [[nodiscard]] std::optional<std::uint64_t> Offset(std::uint64_t absoluteNanoseconds) noexcept;
    mutable std::mutex mutex_;
    RunMeasurementSnapshot snapshot_;
    std::atomic<std::uint64_t> originNanoseconds_ = 0;
    std::atomic<bool> begun_ = false;
    std::atomic<MeasurementFailure> failure_ = MeasurementFailure::None;
    std::array<std::atomic<bool>, static_cast<std::size_t>(RunMilestone::Count)> recorded_{};
    std::array<SubmittedFrameIdentity, step1SubmittedQueueCapacity> submitted_{};
    std::atomic<std::uint64_t> writePosition_ = 0;
    std::atomic<std::uint64_t> readPosition_ = 0;
};

[[nodiscard]] std::uint64_t MeasurementNowNanoseconds() noexcept;
[[nodiscard]] std::uint64_t MeasurementNanoseconds(std::chrono::steady_clock::time_point time) noexcept;
[[nodiscard]] const char *GetRunMilestoneName(RunMilestone milestone) noexcept;
[[nodiscard]] const char *GetMeasurementFailureName(MeasurementFailure failure) noexcept;
[[nodiscard]] std::string BuildSourceLedgerJson(const RunMeasurementSnapshot &snapshot);
[[nodiscard]] std::string BuildSubmittedFrameJson(const SubmittedFrameIdentity &identity);
[[nodiscard]] std::string BuildRunMeasurementJson(const RunMeasurementSnapshot &snapshot,
                                                  std::optional<std::uint64_t> rawBytes, std::optional<std::uint64_t> encodedBytes,
                                                  bool completedAndVerified, bool resumed);
[[nodiscard]] std::string GetMeasurementBuildIdentityJson();

} // namespace pbapp
