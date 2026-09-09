#include "run_measurement.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

#ifndef PB_STEP1_SOURCE_FINGERPRINT
#define PB_STEP1_SOURCE_FINGERPRINT "unsealed"
#endif
#ifndef PB_STEP1_BASE_COMMIT
#define PB_STEP1_BASE_COMMIT "unsealed"
#endif

namespace pbapp
{
namespace
{
constexpr std::size_t Index(const RunMilestone milestone) noexcept
{
    return static_cast<std::size_t>(milestone);
}

std::string Hex(const std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        const auto value = std::to_integer<unsigned int>(bytes[index]);
        result[index * 2] = digits[value >> 4];
        result[index * 2 + 1] = digits[value & 15];
    }
    return result;
}

void WriteOptional(std::ostream &stream, const std::optional<std::uint64_t> value)
{
    if (value)
    {
        stream << *value;
    }
    else
    {
        stream << "null";
    }
}

void WriteIdentity(std::ostream &stream, const std::optional<std::uint64_t> value)
{
    if (value)
    {
        stream << '"' << *value << '"';
    }
    else
    {
        stream << "null";
    }
}

const char *TimingUnavailable(const RunMeasurementSnapshot &snapshot, const std::optional<std::uint64_t> rawBytes,
                              const std::optional<std::uint64_t> encodedBytes, const bool completedAndVerified, const bool resumed)
{
    if (snapshot.failure != MeasurementFailure::None)
    {
        return GetMeasurementFailureName(snapshot.failure);
    }
    if (resumed)
    {
        return "ResumeHasNoFullRunTimingCoverage";
    }
    if (!completedAndVerified)
    {
        return "NotVerifiedPublishedAndReopened";
    }
    if (snapshot.terminalSucceeded != true)
    {
        return "TerminalNotClean";
    }
    if (!rawBytes || !encodedBytes)
    {
        return "MissingVerifiedByteCoverage";
    }
    if (*rawBytes != 0 && *encodedBytes == 0)
    {
        return "MissingVerifiedByteCoverage";
    }
    if (*rawBytes > step1MaximumSourceBytes || *encodedBytes > step1MaximumSourceBytes)
    {
        return "MeasurementSourceScopeExceeded";
    }
    if (snapshot.runGeneration == 0)
    {
        return "InvalidRunGeneration";
    }
    for (const auto offset : snapshot.offsetsNanoseconds)
    {
        if (offset && *offset > step1MaximumDurationNanoseconds)
        {
            return "InvalidDuration";
        }
    }
    for (std::size_t index = 0; index <= Index(RunMilestone::Terminal); index++)
    {
        if (*rawBytes == 0 && (index == Index(RunMilestone::FirstUsefulEquation) || index == Index(RunMilestone::LastSegmentStored)))
        {
            continue;
        }
        if (!snapshot.offsetsNanoseconds[index])
        {
            return "MissingMilestone";
        }
    }
    const auto &offsets = snapshot.offsetsNanoseconds;
    if (*offsets[Index(RunMilestone::StartAccepted)] != 0 || *offsets[Index(RunMilestone::FinalReopenVerified)] == 0)
    {
        return "InvalidDuration";
    }
    // Capture Start may synchronously deliver a frame before returning ready.
    // Control and useful data may also be independently interleaved. Only the
    // actual partial order is required, never an invented total order.
    const std::array<std::pair<RunMilestone, RunMilestone>, 7> requiredOrder{{{RunMilestone::FirstVisualObservation, RunMilestone::FirstAcceptedBootstrap},
                                                                              {RunMilestone::FirstAcceptedBootstrap, RunMilestone::FirstControlAccepted},
                                                                              {RunMilestone::FirstControlAccepted, RunMilestone::WholeDigestVerified},
                                                                              {RunMilestone::CaptureReady, RunMilestone::FinalReopenVerified},
                                                                              {RunMilestone::WholeDigestVerified, RunMilestone::FinalRenameSucceeded},
                                                                              {RunMilestone::FinalRenameSucceeded, RunMilestone::FinalReopenVerified},
                                                                              {RunMilestone::FinalReopenVerified, RunMilestone::Terminal}}};
    for (const auto &[before, after] : requiredOrder)
    {
        if (*offsets[Index(before)] > *offsets[Index(after)])
        {
            return "InvalidMilestoneOrder";
        }
    }
    if (*rawBytes != 0 && (*offsets[Index(RunMilestone::FirstUsefulEquation)] > *offsets[Index(RunMilestone::LastSegmentStored)] ||
                           *offsets[Index(RunMilestone::LastSegmentStored)] > *offsets[Index(RunMilestone::WholeDigestVerified)]))
    {
        return "InvalidMilestoneOrder";
    }
    return nullptr;
}
} // namespace

std::uint64_t MeasurementNanoseconds(const std::chrono::steady_clock::time_point time) noexcept
{
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    return value < 0 ? 0 : static_cast<std::uint64_t>(value);
}

std::uint64_t MeasurementNowNanoseconds() noexcept
{
    return MeasurementNanoseconds(std::chrono::steady_clock::now());
}

const char *GetRunMilestoneName(const RunMilestone milestone) noexcept
{
    constexpr std::array names{"startAccepted", "captureReady", "firstVisualObservation", "firstAcceptedBootstrap",
                               "firstControlAccepted", "firstUsefulEquation", "lastSegmentStored", "wholeDigestVerified",
                               "finalRenameSucceeded", "finalReopenVerified", "terminal", "preparationComplete"};
    return Index(milestone) < names.size() ? names[Index(milestone)] : "invalid";
}

const char *GetMeasurementFailureName(const MeasurementFailure failure) noexcept
{
    switch (failure)
    {
    case MeasurementFailure::None:
        return "None";
    case MeasurementFailure::InvalidClock:
        return "InvalidClock";
    case MeasurementFailure::DurationLimit:
        return "DurationLimit";
    case MeasurementFailure::QueueFull:
        return "SubmittedQueueFull";
    case MeasurementFailure::RecordLimit:
        return "SubmittedRecordLimit";
    case MeasurementFailure::SourceLimit:
        return "SourceLedgerLimit";
    case MeasurementFailure::SourceLedgerInvalid:
        return "SourceLedgerInvalid";
    case MeasurementFailure::IoFailure:
        return "EvidenceIoFailure";
    case MeasurementFailure::EvidenceByteLimit:
        return "EvidenceByteLimit";
    case MeasurementFailure::RuntimeFailure:
        return "RuntimeOrCleanupFailure";
    case MeasurementFailure::ReusedRecorder:
        return "ReusedRecorder";
    }
    return "UnknownMeasurementFailure";
}

void RunMeasurementRecorder::Fail(const MeasurementFailure failure) noexcept
{
    auto expected = MeasurementFailure::None;
    static_cast<void>(failure_.compare_exchange_strong(expected, failure));
}

void RunMeasurementRecorder::Begin(const std::uint64_t runGeneration, const std::uint64_t absoluteNanoseconds) noexcept
{
    std::lock_guard lock(mutex_);
    if (begun_.load())
    {
        Fail(MeasurementFailure::ReusedRecorder);
        return;
    }
    snapshot_.runGeneration = runGeneration;
    originNanoseconds_.store(absoluteNanoseconds);
    snapshot_.offsetsNanoseconds[Index(RunMilestone::StartAccepted)] = 0;
    begun_.store(true, std::memory_order_release);
    if (runGeneration == 0)
    {
        Fail(MeasurementFailure::InvalidClock);
    }
}

std::optional<std::uint64_t> RunMeasurementRecorder::Offset(const std::uint64_t absoluteNanoseconds) noexcept
{
    if (!begun_.load(std::memory_order_acquire) || absoluteNanoseconds < originNanoseconds_.load())
    {
        Fail(MeasurementFailure::InvalidClock);
        return {};
    }
    const auto offset = absoluteNanoseconds - originNanoseconds_.load();
    if (offset > step1MaximumDurationNanoseconds)
    {
        Fail(MeasurementFailure::DurationLimit);
        return {};
    }
    return offset;
}

void RunMeasurementRecorder::CheckDuration(const std::uint64_t absoluteNanoseconds) noexcept
{
    static_cast<void>(Offset(absoluteNanoseconds));
}

void RunMeasurementRecorder::Record(const RunMilestone milestone) noexcept
{
    if (Index(milestone) < recorded_.size() && recorded_[Index(milestone)].load(std::memory_order_relaxed))
    {
        return;
    }
    RecordAt(milestone, MeasurementNowNanoseconds());
}

void RunMeasurementRecorder::RecordAt(const RunMilestone milestone, const std::uint64_t absoluteNanoseconds) noexcept
{
    if (Index(milestone) >= Index(RunMilestone::Count))
    {
        Fail(MeasurementFailure::InvalidClock);
        return;
    }
    if (recorded_[Index(milestone)].load(std::memory_order_relaxed))
    {
        return;
    }
    const auto offset = Offset(absoluteNanoseconds);
    if (!offset)
    {
        return;
    }
    std::lock_guard lock(mutex_);
    if (!snapshot_.offsetsNanoseconds[Index(milestone)])
    {
        snapshot_.offsetsNanoseconds[Index(milestone)] = offset;
    }
    recorded_[Index(milestone)].store(true, std::memory_order_relaxed);
}

void RunMeasurementRecorder::RecordBootstrap(const pbprotocol::BootstrapRecord &bootstrap,
                                             const std::uint64_t captureEpoch, const std::int64_t captureTimestamp100ns) noexcept
{
    if (recorded_[Index(RunMilestone::FirstAcceptedBootstrap)].load(std::memory_order_relaxed))
    {
        return;
    }
    const auto offset = Offset(MeasurementNowNanoseconds());
    if (!offset)
    {
        return;
    }
    std::lock_guard lock(mutex_);
    if (snapshot_.firstFrameSequence)
    {
        return;
    }
    snapshot_.firstSessionTag = bootstrap.sessionTag.value;
    snapshot_.firstFrameSequence = bootstrap.frameSequence;
    snapshot_.firstCaptureEpoch = captureEpoch;
    snapshot_.firstCaptureTimestamp100ns = captureTimestamp100ns;
    snapshot_.offsetsNanoseconds[Index(RunMilestone::FirstAcceptedBootstrap)] = offset;
    recorded_[Index(RunMilestone::FirstAcceptedBootstrap)].store(true, std::memory_order_relaxed);
}

void RunMeasurementRecorder::Finish(const bool succeeded) noexcept
{
    Record(RunMilestone::Terminal);
    if (!succeeded)
    {
        Fail(MeasurementFailure::RuntimeFailure);
    }
    std::lock_guard lock(mutex_);
    if (!snapshot_.terminalSucceeded)
    {
        snapshot_.terminalSucceeded = succeeded;
    }
}

void RunMeasurementRecorder::RecordSubmitted(SubmittedFrameIdentity identity, const std::uint64_t absoluteNanoseconds) noexcept
{
    if (failure_.load() != MeasurementFailure::None)
    {
        return;
    }
    const auto offset = Offset(absoluteNanoseconds);
    if (!offset)
    {
        return;
    }
    const auto writePosition = writePosition_.load(std::memory_order_relaxed);
    if (writePosition >= step1MaximumSubmittedRecords)
    {
        Fail(MeasurementFailure::RecordLimit);
        return;
    }
    if (writePosition - readPosition_.load(std::memory_order_acquire) >= step1SubmittedQueueCapacity)
    {
        Fail(MeasurementFailure::QueueFull);
        return;
    }
    identity.submittedOffsetNanoseconds = *offset;
    submitted_[static_cast<std::size_t>(writePosition % step1SubmittedQueueCapacity)] = identity;
    writePosition_.store(writePosition + 1, std::memory_order_release);
}

bool RunMeasurementRecorder::TakeSubmitted(SubmittedFrameIdentity &output) noexcept
{
    const auto readPosition = readPosition_.load(std::memory_order_relaxed);
    if (readPosition == writePosition_.load(std::memory_order_acquire))
    {
        return false;
    }
    output = submitted_[static_cast<std::size_t>(readPosition % step1SubmittedQueueCapacity)];
    readPosition_.store(readPosition + 1, std::memory_order_release);
    return true;
}

void RunMeasurementRecorder::RecordSegment(const pbprotocol::SegmentDescriptor &descriptor) noexcept
{
    std::lock_guard lock(mutex_);
    if (snapshot_.segmentCount >= step1MaximumSegments)
    {
        Fail(MeasurementFailure::SourceLimit);
        return;
    }
    if (descriptor.segmentOrdinal != snapshot_.segmentCount || descriptor.rawOffset != snapshot_.sourceBytes ||
        descriptor.rawSize > step1MaximumSourceBytes - snapshot_.sourceBytes ||
        descriptor.encodedSize > step1MaximumSourceBytes - snapshot_.encodedBytes || snapshot_.sourceLedgerComplete)
    {
        Fail(MeasurementFailure::SourceLedgerInvalid);
        return;
    }
    snapshot_.segments[snapshot_.segmentCount++] = {descriptor.segmentOrdinal, descriptor.rawOffset,
                                                    descriptor.rawSize, descriptor.encodedSize, descriptor.compressionCodec, descriptor.rawDigest, descriptor.encodedDigest};
    snapshot_.sourceBytes += descriptor.rawSize;
    snapshot_.encodedBytes += descriptor.encodedSize;
}

void RunMeasurementRecorder::CompleteSource(const std::uint64_t sourceBytes,
                                            const std::span<const std::byte, pbprotocol::kDigestBytes> digest, const std::string_view compressionIdentity) noexcept
{
    std::lock_guard lock(mutex_);
    if (sourceBytes > step1MaximumSourceBytes || sourceBytes != snapshot_.sourceBytes ||
        compressionIdentity.size() >= snapshot_.compressionIdentity.size())
    {
        Fail(MeasurementFailure::SourceLedgerInvalid);
        return;
    }
    // The compression implementation emits a bounded ASCII identity. Reject
    // rather than escaping an unexpected string into supposedly sealed JSON.
    for (const char value : compressionIdentity)
    {
        if (value < 32 || value > 126 || value == '"' || value == '\\')
        {
            Fail(MeasurementFailure::SourceLedgerInvalid);
            return;
        }
    }
    std::copy(digest.begin(), digest.end(), snapshot_.sourceBlake3.begin());
    std::copy(compressionIdentity.begin(), compressionIdentity.end(), snapshot_.compressionIdentity.begin());
    snapshot_.sourceLedgerComplete = failure_.load() == MeasurementFailure::None;
}

RunMeasurementSnapshot RunMeasurementRecorder::GetSnapshot() const
{
    std::lock_guard lock(mutex_);
    auto result = snapshot_;
    result.failure = failure_.load();
    result.submittedRecords = writePosition_.load();
    result.drainedRecords = readPosition_.load();
    return result;
}

std::string BuildSubmittedFrameJson(const SubmittedFrameIdentity &identity)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "{\"kind\":\"submittedFrame\",\"sessionTag\":\"" << identity.sessionTag
           << "\",\"frameSequence\":\"" << identity.frameSequence << "\",\"carouselPass\":\"" << identity.carouselPass
           << "\",\"segmentOrdinal\":\"" << identity.segmentOrdinal << "\",\"cyclePosition\":\"" << identity.cyclePosition
           << "\",\"submittedOffsetNanoseconds\":" << identity.submittedOffsetNanoseconds << '}';
    return stream.str();
}

std::string BuildSourceLedgerJson(const RunMeasurementSnapshot &snapshot)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::boolalpha << "{\"schema\":\"PixelBridge.Step1.SourceLedger.1\",\"complete\":" << snapshot.sourceLedgerComplete
           << ",\"rawBytes\":" << snapshot.sourceBytes << ",\"encodedBytes\":" << snapshot.encodedBytes
           << ",\"blake3\":\"" << Hex(snapshot.sourceBlake3) << "\",\"compressionIdentity\":\"" << snapshot.compressionIdentity.data()
           << "\",\"segments\":[";
    for (std::size_t index = 0; index < snapshot.segmentCount; index++)
    {
        const auto &segment = snapshot.segments[index];
        if (index != 0)
        {
            stream << ',';
        }
        stream << "{\"ordinal\":" << segment.ordinal << ",\"rawOffset\":" << segment.rawOffset << ",\"rawBytes\":" << segment.rawBytes
               << ",\"encodedBytes\":" << segment.encodedBytes << ",\"codec\":\""
               << (segment.codec == pbprotocol::CompressionCodec::Raw ? "Raw" : "Zstandard")
               << "\",\"rawBlake3\":\"" << Hex(segment.rawDigest.bytes) << "\",\"encodedBlake3\":\"" << Hex(segment.encodedDigest.bytes) << "\"}";
    }
    stream << "]}";
    return stream.str();
}

std::string BuildRunMeasurementJson(const RunMeasurementSnapshot &snapshot, const std::optional<std::uint64_t> rawBytes,
                                    const std::optional<std::uint64_t> encodedBytes, const bool completedAndVerified, const bool resumed)
{
    const char *const unavailable = TimingUnavailable(snapshot, rawBytes, encodedBytes, completedAndVerified, resumed);
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::boolalpha << std::setprecision(17) << "{\"schema\":\"PixelBridge.Step1.Measurement.1\",\"clock\":\"process-steady\","
                                                         "\"unit\":\"nanoseconds\",\"origin\":\"startAccepted\",\"mainEnd\":\"finalReopenVerified\",\"runGeneration\":\""
           << snapshot.runGeneration << "\",\"build\":" << GetMeasurementBuildIdentityJson() << ",\"milestones\":{";
    for (std::size_t index = 0; index < Index(RunMilestone::Count); index++)
    {
        if (index != 0)
        {
            stream << ',';
        }
        stream << '"' << GetRunMilestoneName(static_cast<RunMilestone>(index)) << "\":";
        WriteOptional(stream, snapshot.offsetsNanoseconds[index]);
    }
    stream << "},\"zeroBytePayloadMilestones\":" << (rawBytes && *rawBytes == 0 ? "\"NotApplicable\"" : "null")
           << ",\"firstAcceptedBootstrap\":{\"sessionTag\":";
    WriteIdentity(stream, snapshot.firstSessionTag);
    stream << ",\"frameSequence\":";
    WriteIdentity(stream, snapshot.firstFrameSequence);
    stream << ",\"captureEpoch\":";
    WriteIdentity(stream, snapshot.firstCaptureEpoch);
    stream << ",\"captureTimestamp100ns\":";
    if (snapshot.firstCaptureTimestamp100ns)
    {
        stream << '"' << *snapshot.firstCaptureTimestamp100ns << '"';
    }
    else
    {
        stream << "null";
    }
    stream << "},\"submittedRecords\":" << snapshot.submittedRecords << ",\"drainedRecords\":" << snapshot.drainedRecords
           << ",\"evidenceFailure\":\"" << GetMeasurementFailureName(snapshot.failure) << "\",\"terminalSucceeded\":";
    if (snapshot.terminalSucceeded)
    {
        stream << *snapshot.terminalSucceeded;
    }
    else
    {
        stream << "null";
    }
    stream << ",\"receiverTimingEligible\":" << (unavailable == nullptr) << ",\"unavailableReason\":";
    if (unavailable)
    {
        stream << '"' << unavailable << '"';
    }
    else
    {
        stream << "null";
    }
    stream << ",\"elapsedNanoseconds\":";
    const auto elapsed = unavailable ? std::optional<std::uint64_t>{} : snapshot.offsetsNanoseconds[Index(RunMilestone::FinalReopenVerified)];
    WriteOptional(stream, elapsed);
    stream << ",\"captureReadyToReopenNanoseconds\":";
    const auto ready = snapshot.offsetsNanoseconds[Index(RunMilestone::CaptureReady)];
    WriteOptional(stream, elapsed && ready ? std::optional<std::uint64_t>(*elapsed - *ready) : std::nullopt);
    stream << ",\"verifiedRawGoodputBytesPerSecond\":";
    if (elapsed)
    {
        stream << static_cast<double>(*rawBytes) * 1.0e9 / static_cast<double>(*elapsed);
    }
    else
    {
        stream << "null";
    }
    stream << ",\"verifiedEncodedGoodputBytesPerSecond\":";
    if (elapsed)
    {
        stream << static_cast<double>(*encodedBytes) * 1.0e9 / static_cast<double>(*elapsed);
    }
    else
    {
        stream << "null";
    }
    stream << ",\"formalSampleEligibility\":\"RequiresPostStopIdentityAndExternalDualDigestAudit\",\"sourceLedger\":"
           << BuildSourceLedgerJson(snapshot) << '}';
    return stream.str();
}

std::string GetMeasurementBuildIdentityJson()
{
    return "{\"schema\":\"PixelBridge.Step1.MeasurementBuildIdentity.1\",\"candidate\":\"M1\",\"authority\":\"InstrumentedExperiment\","
           "\"baseCommit\":\"" PB_STEP1_BASE_COMMIT "\",\"sourceFingerprintSha256\":\"" PB_STEP1_SOURCE_FINGERPRINT
           "\",\"wireChanged\":false,\"profileChanged\":false}";
}

static_assert(sizeof(RunMeasurementRecorder) < 1024 * 1024);
} // namespace pbapp
