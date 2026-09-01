#include "evidence_journal.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <new>
#include <optional>
#include <sstream>
#include <utility>

namespace pbapp
{
namespace
{

[[nodiscard]] bool IsValidLimits(const RunJournalLimits& limits) noexcept
{
    return limits.samplingIntervalMilliseconds > 0 && limits.maximumDurationMilliseconds > 0 &&
        limits.maximumBytes > 0 && limits.maximumBytes <= static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()) &&
        limits.maximumRecordBytes >= 64 && limits.maximumRecordBytes <= 1024U * 1024U;
}

[[nodiscard]] bool IsBoundedJsonRecord(const std::string_view record, const std::uint32_t maximumBytes) noexcept
{
    return record.size() >= 2 && record.size() <= maximumBytes && record.front() == '{' && record.back() == '}' &&
        record.find('\0') == std::string_view::npos && record.find('\r') == std::string_view::npos &&
        record.find('\n') == std::string_view::npos;
}

void WriteEscaped(std::ostream& stream, const std::string_view value)
{
    stream << '"';
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': stream << "\\\""; break;
        case '\\': stream << "\\\\"; break;
        case '\b': stream << "\\b"; break;
        case '\f': stream << "\\f"; break;
        case '\n': stream << "\\n"; break;
        case '\r': stream << "\\r"; break;
        case '\t': stream << "\\t"; break;
        default:
            if (character < 0x20)
            {
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0') <<
                    static_cast<unsigned int>(character) << std::dec;
            }
            else
            {
                stream << static_cast<char>(character);
            }
            break;
        }
    }
    stream << '"';
}

void WriteOptionalFinite(std::ostream& stream, const std::optional<double>& value)
{
    if (value && std::isfinite(*value))
    {
        stream << *value;
    }
    else
    {
        stream << "null";
    }
}

[[nodiscard]] std::string NativeFailure(const char* operation, const DWORD error)
{
    return std::string(operation) + " failed; win32=" + std::to_string(error);
}

} // namespace

struct RunEvidenceJournal::Implementation
{
    Implementation() = default;
    Implementation(const Implementation&) = delete;
    Implementation& operator=(const Implementation&) = delete;
    Implementation(Implementation&&) = delete;
    Implementation& operator=(Implementation&&) = delete;

    ~Implementation()
    {
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
        }
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    RunJournalLimits limits;
    RunJournalSnapshot snapshot{true, true, false, false, 0, 0, {}};
    std::optional<std::uint64_t> lastSampleElapsedMilliseconds;
};

RunEvidenceJournal::RunEvidenceJournal() noexcept = default;
RunEvidenceJournal::RunEvidenceJournal(std::unique_ptr<Implementation> implementation) noexcept :
    implementation_(std::move(implementation)) {}
RunEvidenceJournal::RunEvidenceJournal(RunEvidenceJournal&&) noexcept = default;
RunEvidenceJournal& RunEvidenceJournal::operator=(RunEvidenceJournal&&) noexcept = default;
RunEvidenceJournal::~RunEvidenceJournal() = default;

RunJournalSnapshot RunEvidenceJournal::Create(const std::filesystem::path& path,
    const RunJournalLimits& limits, std::unique_ptr<RunEvidenceJournal>& output) noexcept
{
    output.reset();
    RunJournalSnapshot failure;
    failure.enabled = true;
    failure.valid = false;
    if (path.empty() || !IsValidLimits(limits))
    {
        failure.invalidReason = "invalid journal path or resource limits";
        return failure;
    }
    try
    {
        auto implementation = std::make_unique<Implementation>();
        implementation->limits = limits;
        implementation->file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (implementation->file == INVALID_HANDLE_VALUE)
        {
            failure.invalidReason = NativeFailure("journal CREATE_NEW", GetLastError());
            return failure;
        }
        output = std::unique_ptr<RunEvidenceJournal>(new RunEvidenceJournal(std::move(implementation)));
        return output->GetSnapshot();
    }
    catch (const std::bad_alloc&)
    {
        failure.invalidReason = "journal allocation failed";
        return failure;
    }
    catch (...)
    {
        failure.invalidReason = "journal creation failed";
        return failure;
    }
}

RunJournalSnapshot RunEvidenceJournal::AppendSample(const std::uint64_t elapsedMilliseconds,
    const std::string_view recordJson) noexcept
{
    return Append(elapsedMilliseconds, recordJson, false);
}

RunJournalSnapshot RunEvidenceJournal::AppendTerminal(const std::uint64_t elapsedMilliseconds,
    const std::string_view recordJson) noexcept
{
    return Append(elapsedMilliseconds, recordJson, true);
}

RunJournalSnapshot RunEvidenceJournal::Append(const std::uint64_t elapsedMilliseconds,
    const std::string_view recordJson, const bool terminal) noexcept
{
    if (!implementation_)
    {
        return {true, false, false, false, 0, 0, "journal is not initialized"};
    }
    auto& implementation = *implementation_;
    if (!implementation.snapshot.valid || implementation.snapshot.finished || implementation.file == INVALID_HANDLE_VALUE)
    {
        return implementation.snapshot;
    }
    if (implementation.lastSampleElapsedMilliseconds && elapsedMilliseconds < *implementation.lastSampleElapsedMilliseconds)
    {
        implementation.snapshot.valid = false;
        implementation.snapshot.invalidReason = "journal elapsed time moved backwards";
        return implementation.snapshot;
    }
    if (elapsedMilliseconds >= implementation.limits.maximumDurationMilliseconds)
    {
        implementation.snapshot.truncated = true;
        return implementation.snapshot;
    }
    if (!terminal && implementation.lastSampleElapsedMilliseconds &&
        elapsedMilliseconds - *implementation.lastSampleElapsedMilliseconds < implementation.limits.samplingIntervalMilliseconds)
    {
        return implementation.snapshot;
    }
    if (!IsBoundedJsonRecord(recordJson, implementation.limits.maximumRecordBytes))
    {
        implementation.snapshot.valid = false;
        implementation.snapshot.invalidReason = "journal record is not a bounded single-line JSON object";
        return implementation.snapshot;
    }
    const std::uint64_t lineBytes = static_cast<std::uint64_t>(recordJson.size()) + 1ULL;
    if (lineBytes > implementation.limits.maximumBytes - implementation.snapshot.bytesWritten)
    {
        implementation.snapshot.truncated = true;
        return implementation.snapshot;
    }
    DWORD written = 0;
    if (!WriteFile(implementation.file, recordJson.data(), static_cast<DWORD>(recordJson.size()), &written, nullptr) ||
        written != recordJson.size())
    {
        implementation.snapshot.valid = false;
        implementation.snapshot.invalidReason = NativeFailure("journal record write", GetLastError());
        return implementation.snapshot;
    }
    const char newline = '\n';
    written = 0;
    if (!WriteFile(implementation.file, &newline, 1, &written, nullptr) || written != 1)
    {
        implementation.snapshot.valid = false;
        implementation.snapshot.invalidReason = NativeFailure("journal newline write", GetLastError());
        return implementation.snapshot;
    }
    implementation.snapshot.bytesWritten += lineBytes;
    implementation.snapshot.samples++;
    implementation.lastSampleElapsedMilliseconds = elapsedMilliseconds;
    return implementation.snapshot;
}

RunJournalSnapshot RunEvidenceJournal::Finish() noexcept
{
    if (!implementation_)
    {
        return {true, false, false, false, 0, 0, "journal is not initialized"};
    }
    auto& implementation = *implementation_;
    if (implementation.snapshot.finished || implementation.file == INVALID_HANDLE_VALUE)
    {
        return implementation.snapshot;
    }
    if (!FlushFileBuffers(implementation.file))
    {
        implementation.snapshot.valid = false;
        implementation.snapshot.invalidReason = NativeFailure("journal flush", GetLastError());
    }
    const HANDLE file = std::exchange(implementation.file, INVALID_HANDLE_VALUE);
    if (!CloseHandle(file))
    {
        implementation.snapshot.valid = false;
        implementation.snapshot.invalidReason = NativeFailure("journal close", GetLastError());
    }
    implementation.snapshot.finished = true;
    return implementation.snapshot;
}

RunJournalSnapshot RunEvidenceJournal::GetSnapshot() const noexcept
{
    return implementation_ ? implementation_->snapshot : RunJournalSnapshot{};
}

std::string BuildEncoderJournalRecord(const std::uint64_t unixMilliseconds,
    const EncoderSnapshot& snapshot)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << "{\"schema\":\"PixelBridge.RunJournal.1\",\"role\":\"Encoder\",\"unixMs\":" <<
        unixMilliseconds << ",\"runId\":";
    WriteEscaped(stream, snapshot.runId);
    stream << ",\"state\":";
    WriteEscaped(stream, GetEncoderStateName(snapshot.state));
    stream << ",\"broadcastRuntimeMs\":" << snapshot.broadcastRuntimeMilliseconds <<
        ",\"cycleCount\":" << snapshot.cycleCount << ",\"cyclePosition\":" << snapshot.cyclePosition <<
        ",\"cycleFrameCount\":" << snapshot.cycleFrameCount << ",\"frameSequence\":" << snapshot.frameSequence <<
        ",\"presentationEpoch\":" << snapshot.presentationEpoch << ",\"presentedVisualFps\":";
    WriteOptionalFinite(stream, snapshot.presentedVisualFps);
    stream << ",\"presentCallFps\":";
    WriteOptionalFinite(stream, snapshot.presentCallFps);
    stream << ",\"generatedVisualFps\":" << snapshot.generatedVisualFramesPerSecond <<
        ",\"generatedPayloadBytesPerSecond\":" << snapshot.generatedPayloadBytesPerSecond <<
        ",\"pendingFrames\":" << snapshot.pendingFrames << ",\"pendingHwm\":" << snapshot.pendingHighWater <<
        ",\"processCpuAveragePercent\":";
    WriteOptionalFinite(stream, snapshot.processCpuAveragePercent);
    stream << '}';
    return stream.str();
}

std::string BuildDecoderJournalRecord(const std::uint64_t unixMilliseconds,
    const DecoderSnapshot& snapshot)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << "{\"schema\":\"PixelBridge.RunJournal.1\",\"role\":\"Decoder\",\"unixMs\":" <<
        unixMilliseconds << ",\"runId\":";
    WriteEscaped(stream, snapshot.runId);
    stream << ",\"state\":";
    WriteEscaped(stream, GetDecoderStateName(snapshot.state));
    stream << ",\"descriptorKnown\":" << (snapshot.descriptorKnown ? "true" : "false") <<
        ",\"verifiedRawBytes\":" << snapshot.verifiedRawBytes << ",\"originalFileBytes\":" << snapshot.originalFileBytes <<
        ",\"verifiedEncodedBytes\":" << snapshot.verifiedEncodedBytes << ",\"captureEpoch\":" << snapshot.captureEpoch <<
        ",\"captureArrivedFrames\":" << snapshot.captureArrivedFrames <<
        ",\"captureDeliveredFrames\":" << snapshot.captureDeliveredFrames <<
        ",\"captureDroppedFrames\":" << snapshot.captureDroppedFrames <<
        ",\"captureExpiredFrames\":" << snapshot.captureExpiredFrames <<
        ",\"captureStaleFrames\":" << snapshot.captureStaleFrames <<
        ",\"captureAcquireTimeouts\":" << snapshot.captureAcquireTimeouts <<
        ",\"captureReadbackDropEvents\":" << snapshot.captureReadbackDropEvents <<
        ",\"captureFps\":";
    WriteOptionalFinite(stream, snapshot.captureFps);
    stream << ",\"uniqueVisualFps\":";
    WriteOptionalFinite(stream, snapshot.uniqueVisualFps);
    stream << ",\"endToEndUniqueVisualFps\":";
    WriteOptionalFinite(stream, snapshot.endToEndUniqueVisualFps);
    stream << ",\"bootstrapAttempts\":" << snapshot.telemetryBootstrapAttempts <<
        ",\"bootstrapSuccesses\":" << snapshot.telemetryBootstrapSuccesses <<
        ",\"fecFailures\":" << snapshot.fecFailures << ",\"crcFailures\":" << snapshot.crcFailures <<
        ",\"acceptedTransportBlocks\":" << snapshot.acceptedTransportBlocks <<
        ",\"outerUniqueSymbols\":" << snapshot.outerUniqueSymbols <<
        ",\"outerIdenticalDuplicateSymbols\":" << snapshot.outerIdenticalDuplicateSymbols <<
        ",\"outerRecoveryAlreadyReadySymbols\":" << snapshot.outerRecoveryAlreadyReadySymbols <<
        ",\"outerAlreadyCompletedSymbols\":" << snapshot.outerAlreadyCompletedSymbols <<
        ",\"outerRecoveryReadyEvents\":" << snapshot.outerRecoveryReadyEvents <<
        ",\"outerResourceRejections\":" << snapshot.outerResourceRejections <<
        ",\"outerConflictRejections\":" << snapshot.outerConflictRejections <<
        ",\"remoteStaleRegions\":" << snapshot.remoteStaleRegions <<
        ",\"remoteFreshnessTagMismatches\":" << snapshot.remoteFreshnessTagMismatches <<
        ",\"remoteFreshnessTagErasures\":" << snapshot.remoteFreshnessTagErasures <<
        ",\"remoteFreshnessErasedDataMetrics\":" << snapshot.remoteFreshnessErasedDataMetrics <<
        ",\"duplicates\":" << snapshot.duplicateFrameSequences << ",\"reordered\":" << snapshot.reorderedFrameSequences <<
        ",\"gapEvents\":" << snapshot.frameSequenceGapEvents << ",\"skippedSequence\":" << snapshot.skippedFrameSequences <<
        ",\"captureStallMs\":" << snapshot.captureStallTotalMilliseconds <<
        ",\"visualStallMs\":" << snapshot.visualStallTotalMilliseconds <<
        ",\"frameLeaseHwm\":" << snapshot.frameLeaseHighWater << ",\"demodPendingHwm\":" << snapshot.demodPendingHighWater <<
        ",\"resultQueueHwm\":" << snapshot.resultQueueHighWater << ",\"staleResultDrops\":" << snapshot.staleResultDrops <<
        ",\"replayCaptureOnly\":" << (snapshot.replayCaptureOnly ? "true" : "false") <<
        ",\"replayWrittenFrames\":" << snapshot.replayWrittenFrames <<
        ",\"replayDroppedFrames\":" << snapshot.replayDroppedFrames <<
        ",\"wholeFileDigestPass\":" << (snapshot.wholeFileDigestVerified ? "true" : "false") <<
        ",\"finalPublishPass\":" << (snapshot.finalPublishSucceeded ? "true" : "false") <<
        ",\"processCpuAveragePercent\":";
    WriteOptionalFinite(stream, snapshot.processCpuAveragePercent);
    stream << '}';
    return stream.str();
}

void ApplyJournalSnapshot(const RunJournalSnapshot& journal, EncoderSnapshot& snapshot)
{
    snapshot.journalEnabled = journal.enabled;
    snapshot.evidenceValid = journal.valid;
    snapshot.journalTruncated = journal.truncated;
    snapshot.journalFinished = journal.finished;
    snapshot.journalSamples = journal.samples;
    snapshot.journalBytes = journal.bytesWritten;
    snapshot.evidenceInvalidReason = journal.invalidReason;
}

void ApplyJournalSnapshot(const RunJournalSnapshot& journal, DecoderSnapshot& snapshot)
{
    snapshot.journalEnabled = journal.enabled;
    snapshot.evidenceValid = journal.valid;
    snapshot.journalTruncated = journal.truncated;
    snapshot.journalFinished = journal.finished;
    snapshot.journalSamples = journal.samples;
    snapshot.journalBytes = journal.bytesWritten;
    snapshot.evidenceInvalidReason = journal.invalidReason;
}

} // namespace pbapp
