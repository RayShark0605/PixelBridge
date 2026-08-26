#include "pbprotocol/resume_state.h"

#include "pbprotocol/byte_io.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/descriptor_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pbprotocol {

namespace {

constexpr std::uint8_t kResumeRecordVersion = 1;
// magic + version + reserved + payloadLength.
constexpr std::size_t kResumeRecordHeaderBytes = 14;
constexpr std::array<std::byte, 4> kResumeRecordMagic{
    std::byte{'P'},
    std::byte{'B'},
    std::byte{'R'},
    std::byte{'S'}};

// One resume document describes one session segment progress and every record
// names a distinct segment ordinal (cross-category and same-key conflicts fail
// closed before any count gate, so the total accepted records across all
// categories is bounded by maxSegmentCount. This implements the design doc
// section 31.4 active incomplete segment limit on untrusted input: without it a
// CRC-valid document could carry near budget/min-record-size distinct-ordinal
// cache records and grow the conflict scans and parsed state memory far beyond
// maxResumeBytes.

[[nodiscard]] ProtocolResult<std::uint64_t> CountResumeRecords(
    const std::size_t completedCount,
    const std::size_t wirehairCacheCount,
    const std::size_t directRepeatCount) noexcept
{
    using CountResult = ProtocolResult<std::uint64_t>;
    const auto completedValue = CheckedNarrowUnsigned<std::uint64_t>(completedCount);
    if (!completedValue)
    {
        return CountResult::Failure(completedValue.Error().code, 0);
    }
    const auto wirehairCacheValue = CheckedNarrowUnsigned<std::uint64_t>(wirehairCacheCount);
    if (!wirehairCacheValue)
    {
        return CountResult::Failure(wirehairCacheValue.Error().code, 0);
    }
    const auto directRepeatValue = CheckedNarrowUnsigned<std::uint64_t>(directRepeatCount);
    if (!directRepeatValue)
    {
        return CountResult::Failure(directRepeatValue.Error().code, 0);
    }
    const auto partialTotal = CheckedAddUnsigned<std::uint64_t>(
        completedValue.Value(), wirehairCacheValue.Value());
    if (!partialTotal)
    {
        return CountResult::Failure(partialTotal.Error().code, 0);
    }
    return CheckedAddUnsigned<std::uint64_t>(partialTotal.Value(), directRepeatValue.Value());
}

// Quota comparison shared by LoadResumeState and ResumeStateBuilder. The caller
// applies its own failure offset: the loader reports the offending record start,
// the builder 0, mirroring its other gates.

[[nodiscard]] ProtocolStatus CheckResumeSegmentQuota(
    const std::uint64_t currentRecordTotal,
    const std::size_t maxSegmentCount) noexcept
{
    if (currentRecordTotal >= static_cast<std::uint64_t>(maxSegmentCount))
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceLimitExceeded, 0);
    }
    return ProtocolStatus::Success();
}

} // namespace

ProtocolStatus SerializeResumeRecord(
    const std::span<const std::byte> payload,
    const std::span<std::byte> output)
{
    const auto requiredBytesResult = CheckedAddSize(
        kResumeRecordEnvelopeBytes,
        payload.size());
    if (!requiredBytesResult ||
        output.size() < requiredBytesResult.Value())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::OutputBufferTooSmall,
            0);
    }

    ByteWriter writer(output);
    ProtocolStatus status = writer.WriteFixedBytes(kResumeRecordMagic);
    if (status)
    {
        status = writer.WriteUint8(kResumeRecordVersion);
    }
    if (status)
    {
        status = writer.WriteUint8(0);
    }
    if (status)
    {
        status = writer.WriteUint64(static_cast<std::uint64_t>(payload.size()));
    }
    if (status)
    {
        status = writer.WriteBytes(payload);
    }
    if (!status)
    {
        return status;
    }

    // The CRC covers header plus payload, i.e. everything written so far.
    const std::uint32_t recordCrc = ComputeCrc32c(
        std::span<const std::byte>(output.data(), writer.Position()));
    return writer.WriteUint32(recordCrc);
}

ProtocolResult<std::vector<std::byte>> ParseResumeRecord(
    const std::span<const std::byte> input,
    const ReceiverResourcePolicy& resourcePolicy)
{
    using ResumeParseResult = ProtocolResult<std::vector<std::byte>>;

    // Resume state is untrusted persistent input: the policy gate runs before
    // any byte is interpreted.
    const ProtocolStatus policyStatus = ValidateReceiverResourcePolicy(
        resourcePolicy);
    if (!policyStatus)
    {
        return ResumeParseResult::Failure(
            policyStatus.Error().code,
            policyStatus.Error().offset);
    }
    const auto inputSizeResult = CheckedNarrowUnsigned<std::uint64_t>(
        input.size());
    if (!inputSizeResult)
    {
        return ResumeParseResult::Failure(
            inputSizeResult.Error().code,
            inputSizeResult.Error().offset);
    }
    if (inputSizeResult.Value() > resourcePolicy.maxResumeBytes)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            0);
    }

    ByteReader reader(input);
    const auto magicResult = reader.ReadFixedBytes<4>();
    if (!magicResult)
    {
        return ResumeParseResult::Failure(
            magicResult.Error().code,
            magicResult.Error().offset);
    }
    if (magicResult.Value() != kResumeRecordMagic)
    {
        return ResumeParseResult::Failure(ProtocolErrorCode::InvalidMagic, 0);
    }

    const std::size_t versionOffset = reader.Position();
    const auto versionResult = reader.ReadUint8();
    if (!versionResult)
    {
        return ResumeParseResult::Failure(
            versionResult.Error().code,
            versionResult.Error().offset);
    }
    if (versionResult.Value() != kResumeRecordVersion)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::InvalidEnumValue,
            versionOffset);
    }

    const std::size_t reservedOffset = reader.Position();
    const auto reservedResult = reader.ReadUint8();
    if (!reservedResult)
    {
        return ResumeParseResult::Failure(
            reservedResult.Error().code,
            reservedResult.Error().offset);
    }
    if (reservedResult.Value() != 0)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::NonZeroReservedByte,
            reservedOffset);
    }

    const auto payloadLengthResult = reader.ReadUint64();
    if (!payloadLengthResult)
    {
        return ResumeParseResult::Failure(
            payloadLengthResult.Error().code,
            payloadLengthResult.Error().offset);
    }

    // 18 + UINT64_MAX must fail as overflow before any budget comparison.
    const auto totalBytesResult = CheckedAddUint64(
        kResumeRecordEnvelopeBytes,
        payloadLengthResult.Value());
    if (!totalBytesResult)
    {
        return ResumeParseResult::Failure(
            totalBytesResult.Error().code,
            totalBytesResult.Error().offset);
    }

    const std::uint64_t totalRecordBytes = totalBytesResult.Value();
    if (totalRecordBytes > resourcePolicy.maxResumeBytes)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            0);
    }

    const auto totalSizeResult = CheckedUint64ToSize(totalRecordBytes);
    if (!totalSizeResult)
    {
        return ResumeParseResult::Failure(
            totalSizeResult.Error().code,
            totalSizeResult.Error().offset);
    }
    const std::size_t totalRecordSize = totalSizeResult.Value();

    if (input.size() < totalRecordSize)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::TruncatedInput,
            input.size());
    }
    if (input.size() > totalRecordSize)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::TrailingBytes,
            totalRecordSize);
    }

    const auto payloadSizeResult = CheckedUint64ToSize(
        payloadLengthResult.Value());
    if (!payloadSizeResult)
    {
        return ResumeParseResult::Failure(
            payloadSizeResult.Error().code,
            payloadSizeResult.Error().offset);
    }
    const std::size_t payloadLength = payloadSizeResult.Value();
    const auto payloadSpanResult = reader.ReadBytes(payloadLength);
    if (!payloadSpanResult)
    {
        return ResumeParseResult::Failure(
            payloadSpanResult.Error().code,
            payloadSpanResult.Error().offset);
    }

    const auto storedCrcResult = reader.ReadUint32();
    if (!storedCrcResult)
    {
        return ResumeParseResult::Failure(
            storedCrcResult.Error().code,
            storedCrcResult.Error().offset);
    }

    // The exact-size equality above already pins consumption; keep the check
    // explicit so a future edit to the size arithmetic fails closed here.
    const ProtocolStatus consumedStatus = reader.RequireFullyConsumed();
    if (!consumedStatus)
    {
        return ResumeParseResult::Failure(
            consumedStatus.Error().code,
            consumedStatus.Error().offset);
    }

    // totalRecordSize >= 18, so the subtraction cannot underflow.
    const std::uint32_t computedCrc = ComputeCrc32c(
        std::span<const std::byte>(input.data(), totalRecordSize - 4));
    if (computedCrc != storedCrcResult.Value())
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::CrcMismatch,
            totalRecordSize - 4);
    }

    std::vector<std::byte> payload;
    try
    {
        payload.assign(
            payloadSpanResult.Value().begin(),
            payloadSpanResult.Value().end());
    }
    catch (const std::bad_alloc&)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::ResourceExhausted,
            0);
    }
    catch (const std::length_error&)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::ResourceExhausted,
            0);
    }

    return ResumeParseResult::Success(std::move(payload));
}

ProtocolStatus ValidateResumeStateBudget(
    const std::uint64_t totalBytes,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    const ProtocolStatus policyStatus = ValidateReceiverResourcePolicy(
        resourcePolicy);
    if (!policyStatus)
    {
        return policyStatus;
    }

    if (totalBytes > resourcePolicy.maxResumeBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            0);
    }
    return ProtocolStatus::Success();
}

// =====================================================================
// Typed resume.state records, document load and builder (design doc section 31).
// =====================================================================
namespace {

// Typed resume.state body layout sizes in bytes (tag included, little-endian).
constexpr std::size_t kCompletedSegmentBodyBytes = 73; // tag + sessionId(16) + ordinal(8) + rawOffset(8) + rawSize(8) + digest(32)
constexpr std::size_t kWirehairCacheHeaderBodyBytes = 45; // tag + ordinal(8) + profile(32) + entryCount(4)
constexpr std::size_t kDirectRepeatRecordHeaderBodyBytes = 17; // tag + ordinal(8) + directBlockCount(4) + entryCount(4)
// Minimum bytes per wirehair entry: id (4) + payload length prefix (4). A zero
// payload is invalid, so this lower bound stays exact for allocation guards.
constexpr std::uint64_t kWirehairEntryPrefixBytes = 8;
// Minimum bytes per DirectRepeat entry: id (4) + realPayloadBytes (4) + paddedLen (4).
constexpr std::uint64_t kDirectRepeatEntryPrefixBytes = 12;

[[nodiscard]] ProtocolStatus ValidateResumeCompletedSegment(
    const ResumeCompletedSegmentRecord& record,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    if (record.rawSize == 0)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidRecordSize, 0);
    }
    if (record.rawSize > resourcePolicy.maxRawSegmentBytes)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceLimitExceeded, 0);
    }
    const auto segmentEndResult = CheckedAddUint64(record.rawOffset, record.rawSize);
    if (!segmentEndResult)
    {
        return ProtocolStatus::Failure(segmentEndResult.Error().code, 0);
    }
    if (segmentEndResult.Value() > resourcePolicy.maxAcceptedFileBytes)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceLimitExceeded, 0);
    }
    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus ValidateResumeWirehairEntryPayloadSize(
    std::uint64_t payloadSize,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    if (payloadSize == 0)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidRecordSize, 0);
    }
    if (payloadSize > resourcePolicy.maxOuterBlockBytes)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceLimitExceeded, 0);
    }
    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus ValidateResumeWirehairEntry(
    const ResumeWirehairCacheEntry& entry,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    const auto payloadSizeResult = CheckedNarrowUnsigned<std::uint64_t>(entry.payload.size());
    if (!payloadSizeResult)
    {
        return ProtocolStatus::Failure(payloadSizeResult.Error().code, 0);
    }
    return ValidateResumeWirehairEntryPayloadSize(
        payloadSizeResult.Value(), resourcePolicy);
}

[[nodiscard]] ProtocolStatus ValidateResumeDirectRepeatCount(
    const std::uint32_t directBlockCount,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    if (directBlockCount == 0)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidRecordSize, 0);
    }
    if (static_cast<std::uint64_t>(directBlockCount) > resourcePolicy.maxDirectRepeatBlockCount)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceLimitExceeded, 0);
    }
    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus ValidateResumeDirectRepeatEntry(
    const ResumeDirectRepeatEntry& entry,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    const auto paddedSizeResult = CheckedNarrowUnsigned<std::uint64_t>(entry.paddedPayload.size());
    if (!paddedSizeResult)
    {
        return ProtocolStatus::Failure(paddedSizeResult.Error().code, 0);
    }
    // Field consistency before policy bounds so a zero region is never
    // misreported as an over-budget entry.
    if (entry.realPayloadBytes == 0 || paddedSizeResult.Value() == 0)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidRecordSize, 0);
    }
    if (static_cast<std::uint64_t>(entry.realPayloadBytes) > paddedSizeResult.Value())
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidRecordSize, 0);
    }
    if (paddedSizeResult.Value() > resourcePolicy.maxOuterBlockBytes)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceLimitExceeded, 0);
    }
    return ProtocolStatus::Success();
}

// Deterministic sort-scan over entry ids. When rejectIdenticalDuplicates is
// false (load path), identical-content duplicates are tolerated and deduped by
// the caller; any conflicting payload fails with ResumeRecordConflict. O(n log n)
// with no hash surface, so untrusted input cannot force collision behaviour.
[[nodiscard]] ProtocolStatus ScanResumeWirehairEntryIds(
    const std::vector<ResumeWirehairCacheEntry>& entries,
    const bool rejectIdenticalDuplicates) noexcept
{
    if (entries.size() <= 1U)
    {
        return ProtocolStatus::Success();
    }

    std::vector<std::pair<std::uint32_t, std::size_t>> entryOrder;
    try
    {
        entryOrder.reserve(entries.size());
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }
    for (std::size_t entryIndex = 0; entryIndex < entries.size(); entryIndex++)
    {
        entryOrder.emplace_back(entries[entryIndex].outerBlockId, entryIndex);
    }
    std::sort(entryOrder.begin(), entryOrder.end());

    std::uint32_t previousEntryId = 0;
    std::size_t previousEntryIndex = 0;
    bool hasPreviousEntry = false;
    for (const auto& [entryId, entryIndex] : entryOrder)
    {
        if (hasPreviousEntry && entryId == previousEntryId)
        {
            if (rejectIdenticalDuplicates ||
                entries[previousEntryIndex].payload != entries[entryIndex].payload)
            {
                return ProtocolStatus::Failure(ProtocolErrorCode::ResumeRecordConflict, 0);
            }
        }
        previousEntryId = entryId;
        previousEntryIndex = entryIndex;
        hasPreviousEntry = true;
    }
    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus ScanResumeDirectRepeatEntryIds(
    const std::vector<ResumeDirectRepeatEntry>& entries,
    const bool rejectIdenticalDuplicates) noexcept
{
    if (entries.size() <= 1U)
    {
        return ProtocolStatus::Success();
    }

    std::vector<std::pair<std::uint32_t, std::size_t>> entryOrder;
    try
    {
        entryOrder.reserve(entries.size());
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }
    for (std::size_t entryIndex = 0; entryIndex < entries.size(); entryIndex++)
    {
        entryOrder.emplace_back(entries[entryIndex].blockOrdinal, entryIndex);
    }
    std::sort(entryOrder.begin(), entryOrder.end());

    std::uint32_t previousEntryId = 0;
    std::size_t previousEntryIndex = 0;
    bool hasPreviousEntry = false;
    for (const auto& [entryId, entryIndex] : entryOrder)
    {
        if (hasPreviousEntry && entryId == previousEntryId)
        {
            if (rejectIdenticalDuplicates ||
                entries[previousEntryIndex] != entries[entryIndex])
            {
                return ProtocolStatus::Failure(ProtocolErrorCode::ResumeRecordConflict, 0);
            }
        }
        previousEntryId = entryId;
        previousEntryIndex = entryIndex;
        hasPreviousEntry = true;
    }
    return ProtocolStatus::Success();
}

// In-place dedup keeping the first occurrence of each id in original order.
// Only called after ScanResume*EntryIds confirmed identical-content duplicates,
// so no conflict is silently swallowed here.
void DeduplicateResumeWirehairEntries(std::vector<ResumeWirehairCacheEntry>& entries) noexcept
{
    if (entries.size() <= 1U)
    {
        return;
    }

    // Best-effort dedup on already-loaded bounded state: every auxiliary
    // allocation is guarded, and a failure leaves the record intact. The
    // caller id scan already proved duplicates are byte-identical, and the
    // codec accepts a re-injected identical block idempotently, so an
    // undeduped record is still safe to load and replay.
    std::vector<std::pair<std::uint32_t, std::size_t>> entryOrder;
    try
    {
        entryOrder.reserve(entries.size());
    }
    catch (const std::bad_alloc&)
    {
        return;
    }
    for (std::size_t entryIndex = 0; entryIndex < entries.size(); entryIndex++)
    {
        entryOrder.emplace_back(entries[entryIndex].outerBlockId, entryIndex);
    }
    std::sort(entryOrder.begin(), entryOrder.end());

    // Mark every non-first occurrence of a duplicated id.
    std::vector<bool> dropped;
    try
    {
        dropped.assign(entries.size(), false);
    }
    catch (const std::bad_alloc&)
    {
        return;
    }
    std::size_t groupStart = 0;
    while (groupStart < entryOrder.size())
    {
        std::size_t groupEnd = groupStart + 1U;
        while (groupEnd < entryOrder.size() &&
               entryOrder[groupEnd].first == entryOrder[groupStart].first)
        {
            groupEnd++;
        }
        for (std::size_t groupIndex = groupStart + 1U; groupIndex < groupEnd; groupIndex++)
        {
            dropped[entryOrder[groupIndex].second] = true;
        }
        groupStart = groupEnd;
    }

    std::vector<ResumeWirehairCacheEntry> keptEntries;
    try
    {
        keptEntries.reserve(entries.size());
    }
    catch (const std::bad_alloc&)
    {
        // Dedup only runs on already-loaded bounded state; a failed reserve
        // leaves the record intact and every entry still valid.
        return;
    }
    for (std::size_t entryIndex = 0; entryIndex < entries.size(); entryIndex++)
    {
        if (!dropped[entryIndex])
        {
            keptEntries.push_back(std::move(entries[entryIndex]));
        }
    }
    entries = std::move(keptEntries);
}

void DeduplicateResumeDirectRepeatEntries(std::vector<ResumeDirectRepeatEntry>& entries) noexcept
{
    if (entries.size() <= 1U)
    {
        return;
    }

    // Best-effort dedup on already-loaded bounded state: every auxiliary
    // allocation is guarded, and a failure leaves the record intact. The
    // caller id scan already proved duplicates are byte-identical, so an
    // undeduped record is still safe to load and replay.
    std::vector<std::pair<std::uint32_t, std::size_t>> entryOrder;
    try
    {
        entryOrder.reserve(entries.size());
    }
    catch (const std::bad_alloc&)
    {
        return;
    }
    for (std::size_t entryIndex = 0; entryIndex < entries.size(); entryIndex++)
    {
        entryOrder.emplace_back(entries[entryIndex].blockOrdinal, entryIndex);
    }
    std::sort(entryOrder.begin(), entryOrder.end());

    std::vector<bool> dropped;
    try
    {
        dropped.assign(entries.size(), false);
    }
    catch (const std::bad_alloc&)
    {
        return;
    }
    std::size_t groupStart = 0;
    while (groupStart < entryOrder.size())
    {
        std::size_t groupEnd = groupStart + 1U;
        while (groupEnd < entryOrder.size() &&
               entryOrder[groupEnd].first == entryOrder[groupStart].first)
        {
            groupEnd++;
        }
        for (std::size_t groupIndex = groupStart + 1U; groupIndex < groupEnd; groupIndex++)
        {
            dropped[entryOrder[groupIndex].second] = true;
        }
        groupStart = groupEnd;
    }

    std::vector<ResumeDirectRepeatEntry> keptEntries;
    try
    {
        keptEntries.reserve(entries.size());
    }
    catch (const std::bad_alloc&)
    {
        return;
    }
    for (std::size_t entryIndex = 0; entryIndex < entries.size(); entryIndex++)
    {
        if (!dropped[entryIndex])
        {
            keptEntries.push_back(std::move(entries[entryIndex]));
        }
    }
    entries = std::move(keptEntries);
}

// Body parsers. Error offsets are relative to the record payload start (the tag
// byte is offset 0); LoadResumeState maps them into document coordinates.
[[nodiscard]] ProtocolResult<ResumeCompletedSegmentRecord> ParseResumeCompletedSegmentBody(
    std::span<const std::byte> payload,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    using CompletedResult = ProtocolResult<ResumeCompletedSegmentRecord>;

    ByteReader reader(payload);
    const std::size_t tagOffset = reader.Position();
    const auto recordTypeResult = reader.ReadUint8();
    if (!recordTypeResult)
    {
        return CompletedResult::Failure(
            recordTypeResult.Error().code,
            recordTypeResult.Error().offset);
    }
    if (recordTypeResult.Value() != static_cast<std::uint8_t>(ResumeRecordType::CompletedSegment))
    {
        return CompletedResult::Failure(ProtocolErrorCode::InvalidEnumValue, tagOffset);
    }

    const auto sessionIdBytesResult = reader.ReadFixedBytes<kSessionIdBytes>();
    if (!sessionIdBytesResult)
    {
        return CompletedResult::Failure(
            sessionIdBytesResult.Error().code,
            sessionIdBytesResult.Error().offset);
    }
    const SessionId sessionId{sessionIdBytesResult.Value()};

    const auto segmentOrdinalResult = reader.ReadUint64();
    if (!segmentOrdinalResult)
    {
        return CompletedResult::Failure(
            segmentOrdinalResult.Error().code,
            segmentOrdinalResult.Error().offset);
    }
    const auto rawOffsetResult = reader.ReadUint64();
    if (!rawOffsetResult)
    {
        return CompletedResult::Failure(
            rawOffsetResult.Error().code,
            rawOffsetResult.Error().offset);
    }
    const auto rawSizeResult = reader.ReadUint64();
    if (!rawSizeResult)
    {
        return CompletedResult::Failure(
            rawSizeResult.Error().code,
            rawSizeResult.Error().offset);
    }

    const auto digestBytesResult = reader.ReadFixedBytes<kDigestBytes>();
    if (!digestBytesResult)
    {
        return CompletedResult::Failure(
            digestBytesResult.Error().code,
            digestBytesResult.Error().offset);
    }
    const RawDigest rawDigest{digestBytesResult.Value()};

    const ProtocolStatus consumedStatus = reader.RequireFullyConsumed();
    if (!consumedStatus)
    {
        return CompletedResult::Failure(consumedStatus.Error().code, consumedStatus.Error().offset);
    }

    const ResumeCompletedSegmentRecord record{
        sessionId,
        segmentOrdinalResult.Value(),
        rawOffsetResult.Value(),
        rawSizeResult.Value(),
        rawDigest};
    const auto boundsStatus = ValidateResumeCompletedSegment(record, resourcePolicy);
    if (!boundsStatus)
    {
        return CompletedResult::Failure(boundsStatus.Error().code, boundsStatus.Error().offset);
    }
    return CompletedResult::Success(record);
}
[[nodiscard]] ProtocolResult<ResumeActiveWirehairCacheRecord> ParseResumeActiveWirehairBody(
    std::span<const std::byte> payload,
    const ReceiverResourcePolicy& resourcePolicy)
{
    using WirehairResult = ProtocolResult<ResumeActiveWirehairCacheRecord>;

    ByteReader reader(payload);
    const std::size_t tagOffset = reader.Position();
    const auto recordTypeResult = reader.ReadUint8();
    if (!recordTypeResult)
    {
        return WirehairResult::Failure(
            recordTypeResult.Error().code,
            recordTypeResult.Error().offset);
    }
    if (recordTypeResult.Value() != static_cast<std::uint8_t>(ResumeRecordType::ActiveWirehairCache))
    {
        return WirehairResult::Failure(ProtocolErrorCode::InvalidEnumValue, tagOffset);
    }

    const auto segmentOrdinalResult = reader.ReadUint64();
    if (!segmentOrdinalResult)
    {
        return WirehairResult::Failure(
            segmentOrdinalResult.Error().code,
            segmentOrdinalResult.Error().offset);
    }

    const auto profileBytesResult =
        reader.ReadFixedBytes<kWirehairV2SerializedProfileBytes>();
    if (!profileBytesResult)
    {
        return WirehairResult::Failure(
            profileBytesResult.Error().code,
            profileBytesResult.Error().offset);
    }
    const WirehairV2SerializedProfile wirehairProfile{profileBytesResult.Value()};

    const std::size_t entryCountOffset = reader.Position();
    const auto entryCountResult = reader.ReadUint32();
    if (!entryCountResult)
    {
        return WirehairResult::Failure(
            entryCountResult.Error().code,
            entryCountResult.Error().offset);
    }

    // Bound the declared entry count against the remaining payload BEFORE any
    // allocation: a crafted huge count must fail closed without touching heap.
    const auto minimumEntryBytesResult = CheckedMultiplyUint64(
        static_cast<std::uint64_t>(entryCountResult.Value()),
        kWirehairEntryPrefixBytes);
    if (!minimumEntryBytesResult)
    {
        return WirehairResult::Failure(ProtocolErrorCode::TruncatedInput, entryCountOffset);
    }
    const auto remainingSizeResult = CheckedNarrowUnsigned<std::uint64_t>(reader.Remaining());
    if (!remainingSizeResult)
    {
        return WirehairResult::Failure(
            remainingSizeResult.Error().code,
            remainingSizeResult.Error().offset);
    }
    if (minimumEntryBytesResult.Value() > remainingSizeResult.Value())
    {
        return WirehairResult::Failure(ProtocolErrorCode::TruncatedInput, entryCountOffset);
    }

    ResumeActiveWirehairCacheRecord record;
    record.segmentOrdinal = segmentOrdinalResult.Value();
    record.wirehairProfile = wirehairProfile;
    std::vector<ResumeWirehairCacheEntry> entries;
    try
    {
        entries.reserve(entryCountResult.Value());
    }
    catch (const std::bad_alloc&)
    {
        return WirehairResult::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }
    catch (const std::length_error&)
    {
        return WirehairResult::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }

    for (std::uint64_t entryIndex = 0;
         entryIndex < static_cast<std::uint64_t>(entryCountResult.Value());
         entryIndex++)
    {
        const auto outerBlockIdResult = reader.ReadUint32();
        if (!outerBlockIdResult)
        {
            return WirehairResult::Failure(
                outerBlockIdResult.Error().code,
                outerBlockIdResult.Error().offset);
        }

        const std::size_t payloadLengthOffset = reader.Position();
        const auto payloadLengthResult = reader.ReadUint32();
        if (!payloadLengthResult)
        {
            return WirehairResult::Failure(
                payloadLengthResult.Error().code,
                payloadLengthResult.Error().offset);
        }
        const auto boundsStatus = ValidateResumeWirehairEntryPayloadSize(
            static_cast<std::uint64_t>(payloadLengthResult.Value()), resourcePolicy);
        if (!boundsStatus)
        {
            return WirehairResult::Failure(boundsStatus.Error().code, payloadLengthOffset);
        }

        const auto payloadSpanResult = reader.ReadBytes(payloadLengthResult.Value());
        if (!payloadSpanResult)
        {
            return WirehairResult::Failure(
                payloadSpanResult.Error().code,
                payloadSpanResult.Error().offset);
        }

        ResumeWirehairCacheEntry entry;
        entry.outerBlockId = outerBlockIdResult.Value();
        try
        {
            entry.payload.assign(
                payloadSpanResult.Value().begin(),
                payloadSpanResult.Value().end());
        }
        catch (const std::bad_alloc&)
        {
            return WirehairResult::Failure(ProtocolErrorCode::ResourceExhausted, 0);
        }
        catch (const std::length_error&)
        {
            return WirehairResult::Failure(ProtocolErrorCode::ResourceExhausted, 0);
        }
        entries.push_back(std::move(entry));
    }

    const ProtocolStatus consumedStatus = reader.RequireFullyConsumed();
    if (!consumedStatus)
    {
        return WirehairResult::Failure(consumedStatus.Error().code, consumedStatus.Error().offset);
    }

    // Duplicate outerBlockId: identical payload dedupes on load; any conflict
    // fails the whole document (never latest-wins).
    const auto idScanStatus = ScanResumeWirehairEntryIds(entries, false);
    if (!idScanStatus)
    {
        return WirehairResult::Failure(idScanStatus.Error().code, 0);
    }
    DeduplicateResumeWirehairEntries(entries);
    record.entries = std::move(entries);
    return WirehairResult::Success(std::move(record));
}

[[nodiscard]] ProtocolResult<ResumeActiveDirectRepeatRecord> ParseResumeActiveDirectRepeatBody(
    std::span<const std::byte> payload,
    const ReceiverResourcePolicy& resourcePolicy)
{
    using DirectRepeatResult = ProtocolResult<ResumeActiveDirectRepeatRecord>;

    ByteReader reader(payload);
    const std::size_t tagOffset = reader.Position();
    const auto recordTypeResult = reader.ReadUint8();
    if (!recordTypeResult)
    {
        return DirectRepeatResult::Failure(
            recordTypeResult.Error().code,
            recordTypeResult.Error().offset);
    }
    if (recordTypeResult.Value() != static_cast<std::uint8_t>(ResumeRecordType::DirectRepeatReceivedBlocks))
    {
        return DirectRepeatResult::Failure(ProtocolErrorCode::InvalidEnumValue, tagOffset);
    }

    const auto segmentOrdinalResult = reader.ReadUint64();
    if (!segmentOrdinalResult)
    {
        return DirectRepeatResult::Failure(
            segmentOrdinalResult.Error().code,
            segmentOrdinalResult.Error().offset);
    }

    const std::size_t directBlockCountOffset = reader.Position();
    const auto directBlockCountResult = reader.ReadUint32();
    if (!directBlockCountResult)
    {
        return DirectRepeatResult::Failure(
            directBlockCountResult.Error().code,
            directBlockCountResult.Error().offset);
    }
    const auto countBoundsStatus = ValidateResumeDirectRepeatCount(
        directBlockCountResult.Value(), resourcePolicy);
    if (!countBoundsStatus)
    {
        return DirectRepeatResult::Failure(countBoundsStatus.Error().code, directBlockCountOffset);
    }

    const std::size_t entryCountOffset = reader.Position();
    const auto entryCountResult = reader.ReadUint32();
    if (!entryCountResult)
    {
        return DirectRepeatResult::Failure(
            entryCountResult.Error().code,
            entryCountResult.Error().offset);
    }

    // Same allocation guard as the wirehair body: declared entry count must be
    // representable within the remaining payload before any heap is touched.
    const auto minimumEntryBytesResult = CheckedMultiplyUint64(
        static_cast<std::uint64_t>(entryCountResult.Value()),
        kDirectRepeatEntryPrefixBytes);
    if (!minimumEntryBytesResult)
    {
        return DirectRepeatResult::Failure(ProtocolErrorCode::TruncatedInput, entryCountOffset);
    }
    const auto remainingSizeResult = CheckedNarrowUnsigned<std::uint64_t>(reader.Remaining());
    if (!remainingSizeResult)
    {
        return DirectRepeatResult::Failure(
            remainingSizeResult.Error().code,
            remainingSizeResult.Error().offset);
    }
    if (minimumEntryBytesResult.Value() > remainingSizeResult.Value())
    {
        return DirectRepeatResult::Failure(ProtocolErrorCode::TruncatedInput, entryCountOffset);
    }

    ResumeActiveDirectRepeatRecord record;
    record.segmentOrdinal = segmentOrdinalResult.Value();
    record.directBlockCount = directBlockCountResult.Value();
    std::vector<ResumeDirectRepeatEntry> entries;
    try
    {
        entries.reserve(entryCountResult.Value());
    }
    catch (const std::bad_alloc&)
    {
        return DirectRepeatResult::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }
    catch (const std::length_error&)
    {
        return DirectRepeatResult::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }

    for (std::uint64_t entryIndex = 0;
         entryIndex < static_cast<std::uint64_t>(entryCountResult.Value());
         entryIndex++)
    {
        const std::size_t blockOrdinalOffset = reader.Position();
        const auto blockOrdinalResult = reader.ReadUint32();
        if (!blockOrdinalResult)
        {
            return DirectRepeatResult::Failure(
                blockOrdinalResult.Error().code,
                blockOrdinalResult.Error().offset);
        }
        if (static_cast<std::uint64_t>(blockOrdinalResult.Value()) >=
            static_cast<std::uint64_t>(record.directBlockCount))
        {
            return DirectRepeatResult::Failure(
                ProtocolErrorCode::SegmentOrdinalOutOfRange, blockOrdinalOffset);
        }

        const std::size_t realPayloadBytesOffset = reader.Position();
        const auto realPayloadBytesResult = reader.ReadUint32();
        if (!realPayloadBytesResult)
        {
            return DirectRepeatResult::Failure(
                realPayloadBytesResult.Error().code,
                realPayloadBytesResult.Error().offset);
        }

        const std::size_t paddedLengthOffset = reader.Position();
        const auto paddedLengthResult = reader.ReadUint32();
        if (!paddedLengthResult)
        {
            return DirectRepeatResult::Failure(
                paddedLengthResult.Error().code,
                paddedLengthResult.Error().offset);
        }

        // Field consistency first (zero region / oversized real payload), then
        // the policy bound on the padded length; order is pinned by tests.
        if (realPayloadBytesResult.Value() == 0 || paddedLengthResult.Value() == 0)
        {
            return DirectRepeatResult::Failure(ProtocolErrorCode::InvalidRecordSize, realPayloadBytesOffset);
        }
        if (static_cast<std::uint64_t>(realPayloadBytesResult.Value()) >
            static_cast<std::uint64_t>(paddedLengthResult.Value()))
        {
            return DirectRepeatResult::Failure(ProtocolErrorCode::InvalidRecordSize, realPayloadBytesOffset);
        }
        const auto paddedBoundsStatus = ValidateResumeWirehairEntryPayloadSize(
            static_cast<std::uint64_t>(paddedLengthResult.Value()), resourcePolicy);
        if (!paddedBoundsStatus)
        {
            return DirectRepeatResult::Failure(paddedBoundsStatus.Error().code, paddedLengthOffset);
        }

        const auto paddedSpanResult = reader.ReadBytes(paddedLengthResult.Value());
        if (!paddedSpanResult)
        {
            return DirectRepeatResult::Failure(
                paddedSpanResult.Error().code,
                paddedSpanResult.Error().offset);
        }

        ResumeDirectRepeatEntry entry;
        entry.blockOrdinal = blockOrdinalResult.Value();
        entry.realPayloadBytes = realPayloadBytesResult.Value();
        try
        {
            entry.paddedPayload.assign(
                paddedSpanResult.Value().begin(),
                paddedSpanResult.Value().end());
        }
        catch (const std::bad_alloc&)
        {
            return DirectRepeatResult::Failure(ProtocolErrorCode::ResourceExhausted, 0);
        }
        catch (const std::length_error&)
        {
            return DirectRepeatResult::Failure(ProtocolErrorCode::ResourceExhausted, 0);
        }
        entries.push_back(std::move(entry));
    }

    const ProtocolStatus consumedStatus = reader.RequireFullyConsumed();
    if (!consumedStatus)
    {
        return DirectRepeatResult::Failure(consumedStatus.Error().code, consumedStatus.Error().offset);
    }

    const auto idScanStatus = ScanResumeDirectRepeatEntryIds(entries, false);
    if (!idScanStatus)
    {
        return DirectRepeatResult::Failure(idScanStatus.Error().code, 0);
    }
    DeduplicateResumeDirectRepeatEntries(entries);
    record.entries = std::move(entries);
    return DirectRepeatResult::Success(std::move(record));
}

[[nodiscard]] ProtocolStatus SerializeResumeCompletedSegmentBody(
    const ResumeCompletedSegmentRecord& record,
    std::span<std::byte> output) noexcept
{
    ByteWriter writer(output);
    ProtocolStatus status = writer.WriteUint8(
        static_cast<std::uint8_t>(ResumeRecordType::CompletedSegment));
    if (!status)
    {
        return status;
    }
    status = writer.WriteFixedBytes(record.sessionId.bytes);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(record.segmentOrdinal);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(record.rawOffset);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(record.rawSize);
    if (!status)
    {
        return status;
    }
    return writer.WriteFixedBytes(record.rawDigest.bytes);
}

// Returns the exact body byte count for an active wirehair cache record, or a
// checked-arithmetic failure when any intermediate sum overflows.
[[nodiscard]] ProtocolResult<std::uint64_t> GetResumeActiveWirehairBodySize(
    const ResumeActiveWirehairCacheRecord& record) noexcept
{
    std::uint64_t bodySize = kWirehairCacheHeaderBodyBytes;
    for (const auto& entry : record.entries)
    {
        const auto payloadSizeResult = CheckedNarrowUnsigned<std::uint64_t>(entry.payload.size());
        if (!payloadSizeResult)
        {
            return ProtocolResult<std::uint64_t>::Failure(
                payloadSizeResult.Error().code, 0);
        }
        const auto prefixAdditionResult = CheckedAddUint64(bodySize, kWirehairEntryPrefixBytes);
        if (!prefixAdditionResult)
        {
            return ProtocolResult<std::uint64_t>::Failure(prefixAdditionResult.Error().code, 0);
        }
        bodySize = prefixAdditionResult.Value();
        const auto entryAdditionResult = CheckedAddUint64(bodySize, payloadSizeResult.Value());
        if (!entryAdditionResult)
        {
            return ProtocolResult<std::uint64_t>::Failure(entryAdditionResult.Error().code, 0);
        }
        bodySize = entryAdditionResult.Value();
    }
    return ProtocolResult<std::uint64_t>::Success(bodySize);
}

[[nodiscard]] ProtocolStatus SerializeResumeActiveWirehairBody(
    const ResumeActiveWirehairCacheRecord& record,
    std::span<std::byte> output) noexcept
{
    ByteWriter writer(output);
    ProtocolStatus status = writer.WriteUint8(
        static_cast<std::uint8_t>(ResumeRecordType::ActiveWirehairCache));
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(record.segmentOrdinal);
    if (!status)
    {
        return status;
    }
    status = writer.WriteFixedBytes(record.wirehairProfile.bytes);
    if (!status)
    {
        return status;
    }
    const auto entryCountResult = CheckedNarrowUnsigned<std::uint32_t>(record.entries.size());
    if (!entryCountResult)
    {
        return ProtocolStatus::Failure(entryCountResult.Error().code, 0);
    }
    status = writer.WriteUint32(entryCountResult.Value());
    if (!status)
    {
        return status;
    }
    for (const auto& entry : record.entries)
    {
        status = writer.WriteUint32(entry.outerBlockId);
        if (!status)
        {
            return status;
        }
        const auto payloadSizeResult = CheckedNarrowUnsigned<std::uint32_t>(entry.payload.size());
        if (!payloadSizeResult)
        {
            return ProtocolStatus::Failure(payloadSizeResult.Error().code, 0);
        }
        status = writer.WriteUint32(payloadSizeResult.Value());
        if (!status)
        {
            return status;
        }
        status = writer.WriteBytes(entry.payload);
        if (!status)
        {
            return status;
        }
    }
    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolResult<std::uint64_t> GetResumeActiveDirectRepeatBodySize(
    const ResumeActiveDirectRepeatRecord& record) noexcept
{
    std::uint64_t bodySize = kDirectRepeatRecordHeaderBodyBytes;
    for (const auto& entry : record.entries)
    {
        const auto paddedSizeResult = CheckedNarrowUnsigned<std::uint64_t>(entry.paddedPayload.size());
        if (!paddedSizeResult)
        {
            return ProtocolResult<std::uint64_t>::Failure(
                paddedSizeResult.Error().code, 0);
        }
        const auto prefixAdditionResult = CheckedAddUint64(bodySize, kDirectRepeatEntryPrefixBytes);
        if (!prefixAdditionResult)
        {
            return ProtocolResult<std::uint64_t>::Failure(prefixAdditionResult.Error().code, 0);
        }
        bodySize = prefixAdditionResult.Value();
        const auto entryAdditionResult = CheckedAddUint64(bodySize, paddedSizeResult.Value());
        if (!entryAdditionResult)
        {
            return ProtocolResult<std::uint64_t>::Failure(entryAdditionResult.Error().code, 0);
        }
        bodySize = entryAdditionResult.Value();
    }
    return ProtocolResult<std::uint64_t>::Success(bodySize);
}

[[nodiscard]] ProtocolStatus SerializeResumeActiveDirectRepeatBody(
    const ResumeActiveDirectRepeatRecord& record,
    std::span<std::byte> output) noexcept
{
    ByteWriter writer(output);
    ProtocolStatus status = writer.WriteUint8(
        static_cast<std::uint8_t>(ResumeRecordType::DirectRepeatReceivedBlocks));
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(record.segmentOrdinal);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint32(record.directBlockCount);
    if (!status)
    {
        return status;
    }
    const auto entryCountResult = CheckedNarrowUnsigned<std::uint32_t>(record.entries.size());
    if (!entryCountResult)
    {
        return ProtocolStatus::Failure(entryCountResult.Error().code, 0);
    }
    status = writer.WriteUint32(entryCountResult.Value());
    if (!status)
    {
        return status;
    }
    for (const auto& entry : record.entries)
    {
        status = writer.WriteUint32(entry.blockOrdinal);
        if (!status)
        {
            return status;
        }
        status = writer.WriteUint32(entry.realPayloadBytes);
        if (!status)
        {
            return status;
        }
        const auto paddedSizeResult = CheckedNarrowUnsigned<std::uint32_t>(entry.paddedPayload.size());
        if (!paddedSizeResult)
        {
            return ProtocolStatus::Failure(paddedSizeResult.Error().code, 0);
        }
        status = writer.WriteUint32(paddedSizeResult.Value());
        if (!status)
        {
            return status;
        }
        status = writer.WriteBytes(entry.paddedPayload);
        if (!status)
        {
            return status;
        }
    }
    return ProtocolStatus::Success();
}

} // namespace
ProtocolResult<LoadedResumeState> LoadResumeState(
    std::span<const std::byte> document,
    const ReceiverResourcePolicy& resourcePolicy)
{
    using ResumeStateResult = ProtocolResult<LoadedResumeState>;

    // Resume state is untrusted persistent input: the policy gate and the whole-
    // document budget both run before any byte is interpreted.
    const auto policyStatus = ValidateReceiverResourcePolicy(resourcePolicy);
    if (!policyStatus)
    {
        return ResumeStateResult::Failure(policyStatus.Error().code, policyStatus.Error().offset);
    }

    const auto documentBytesResult = CheckedNarrowUnsigned<std::uint64_t>(document.size());
    if (!documentBytesResult)
    {
        return ResumeStateResult::Failure(documentBytesResult.Error().code, 0);
    }
    if (documentBytesResult.Value() > resourcePolicy.maxResumeBytes)
    {
        return ResumeStateResult::Failure(ProtocolErrorCode::ResourceLimitExceeded, 0);
    }

    LoadedResumeState state;
    std::size_t offset = 0;
    while (offset < document.size())
    {
        const std::size_t remainingBytes = document.size() - offset; // exact: offset <= size

        if (remainingBytes < kResumeRecordEnvelopeBytes)
        {
            // A torn tail write that stopped before a complete envelope could be
            // formed. FastResume phase semantics: drop the partial region and flag it.
            state.hasTruncatedTail = true;
            break;
        }

        // Peek the 14-byte header to classify torn tail vs in-bounds corruption.
        // All four fields are fully present here (remainingBytes >= 18); the
        // defensive branches keep a future envelope edit fail-closed.
        ByteReader headerPeek(document.subspan(offset));
        const auto peekMagicResult = headerPeek.ReadFixedBytes<4>();
        if (!peekMagicResult)
        {
            state.hasTruncatedTail = true;
            break;
        }
        const auto peekVersionResult = headerPeek.ReadUint8();
        if (!peekVersionResult)
        {
            state.hasTruncatedTail = true;
            break;
        }
        const auto peekReservedResult = headerPeek.ReadUint8();
        if (!peekReservedResult)
        {
            state.hasTruncatedTail = true;
            break;
        }
        const auto declaredPayloadLengthResult = headerPeek.ReadUint64();
        if (!declaredPayloadLengthResult)
        {
            state.hasTruncatedTail = true;
            break;
        }

        // A declared total that overflows or runs past EOF means the tail region
        // cannot hold one complete structurally-valid envelope: torn tail.
        const auto recordTotalBytesResult = CheckedAddUint64(
            static_cast<std::uint64_t>(kResumeRecordEnvelopeBytes),
            declaredPayloadLengthResult.Value());
        if (!recordTotalBytesResult)
        {
            state.hasTruncatedTail = true;
            break;
        }
        const auto recordSizeResult = CheckedUint64ToSize(recordTotalBytesResult.Value());
        std::size_t recordSizeInBytes = 0;
        bool declaredRecordComplete = false;
        if (recordSizeResult && recordSizeResult.Value() <= remainingBytes)
        {
            recordSizeInBytes = recordSizeResult.Value();
            declaredRecordComplete = true;
        }
        if (!declaredRecordComplete)
        {
            state.hasTruncatedTail = true;
            break;
        }

        // A complete envelope is present from here on: any failure is in-bounds
        // corruption and fails the entire load.
        const std::size_t recordStart = offset;
        const auto parseResult = ParseResumeRecord(
            document.subspan(recordStart, recordSizeInBytes), resourcePolicy);
        if (!parseResult)
        {
            return ResumeStateResult::Failure(
                parseResult.Error().code,
                recordStart + parseResult.Error().offset);
        }

        const std::vector<std::byte> payload = std::move(parseResult).Value();
        offset += recordSizeInBytes;

        // Document offset of the body tag byte (envelope header is 14 bytes).
        const std::size_t bodyBaseOffset = recordStart + kResumeRecordHeaderBytes;
        if (payload.empty())
        {
            return ResumeStateResult::Failure(ProtocolErrorCode::InvalidRecordSize, bodyBaseOffset);
        }

        const std::uint8_t recordTypeByte = static_cast<std::uint8_t>(payload.front());
        switch (recordTypeByte)
        {
        case static_cast<std::uint8_t>(ResumeRecordType::CompletedSegment):
        {
            const auto bodyResult = ParseResumeCompletedSegmentBody(
                std::span<const std::byte>(payload), resourcePolicy);
            if (!bodyResult)
            {
                return ResumeStateResult::Failure(
                    bodyResult.Error().code,
                    bodyBaseOffset + bodyResult.Error().offset);
            }

            const ResumeCompletedSegmentRecord& newRecord = bodyResult.Value();
            bool duplicateOfExistingRecord = false;
            for (const auto& existingRecord : state.completedSegments)
            {
                if (existingRecord.sessionId != newRecord.sessionId ||
                    existingRecord.segmentOrdinal != newRecord.segmentOrdinal)
                {
                    continue;
                }
                if (existingRecord == newRecord)
                {
                    duplicateOfExistingRecord = true;
                }
                else
                {
                    return ResumeStateResult::Failure(
                        ProtocolErrorCode::ResumeRecordConflict, recordStart);
                }
            }

            // One ordinal is either completed or active in a cache, never both.
            for (const auto& existingCache : state.activeWirehairCaches)
            {
                if (existingCache.segmentOrdinal == newRecord.segmentOrdinal)
                {
                    return ResumeStateResult::Failure(
                        ProtocolErrorCode::ResumeRecordConflict, recordStart);
                }
            }
            for (const auto& existingCache : state.activeDirectRepeatRecords)
            {
                if (existingCache.segmentOrdinal == newRecord.segmentOrdinal)
                {
                    return ResumeStateResult::Failure(
                        ProtocolErrorCode::ResumeRecordConflict, recordStart);
                }
            }

            if (!duplicateOfExistingRecord)
            {
                const auto totalRecordCount = CountResumeRecords(
                    state.completedSegments.size(),
                    state.activeWirehairCaches.size(),
                    state.activeDirectRepeatRecords.size());
                if (!totalRecordCount)
                {
                    return ResumeStateResult::Failure(totalRecordCount.Error().code, 0);
                }
                const auto quotaStatus = CheckResumeSegmentQuota(
                    totalRecordCount.Value(), resourcePolicy.maxSegmentCount);
                if (!quotaStatus)
                {
                    return ResumeStateResult::Failure(quotaStatus.Error().code, recordStart);
                }
                try
                {
                    state.completedSegments.push_back(newRecord);
                }
                catch (const std::bad_alloc&)
                {
                    return ResumeStateResult::Failure(ProtocolErrorCode::ResourceExhausted, 0);
                }
            }
            break;
        }
        case static_cast<std::uint8_t>(ResumeRecordType::ActiveWirehairCache):
        {
            const auto bodyResult = ParseResumeActiveWirehairBody(
                std::span<const std::byte>(payload), resourcePolicy);
            if (!bodyResult)
            {
                return ResumeStateResult::Failure(
                    bodyResult.Error().code,
                    bodyBaseOffset + bodyResult.Error().offset);
            }

            const ResumeActiveWirehairCacheRecord& newRecord = bodyResult.Value();
            bool duplicateOfExistingRecord = false;
            for (const auto& existingCache : state.activeWirehairCaches)
            {
                if (existingCache.segmentOrdinal != newRecord.segmentOrdinal)
                {
                    continue;
                }
                if (existingCache == newRecord)
                {
                    duplicateOfExistingRecord = true;
                }
                else
                {
                    return ResumeStateResult::Failure(
                        ProtocolErrorCode::ResumeRecordConflict, recordStart);
                }
            }

            for (const auto& existingRecord : state.completedSegments)
            {
                if (existingRecord.segmentOrdinal == newRecord.segmentOrdinal)
                {
                    return ResumeStateResult::Failure(
                        ProtocolErrorCode::ResumeRecordConflict, recordStart);
                }
            }
            for (const auto& existingCache : state.activeDirectRepeatRecords)
            {
                if (existingCache.segmentOrdinal == newRecord.segmentOrdinal)
                {
                    return ResumeStateResult::Failure(
                        ProtocolErrorCode::ResumeRecordConflict, recordStart);
                }
            }

            if (!duplicateOfExistingRecord)
            {
                const auto totalRecordCount = CountResumeRecords(
                    state.completedSegments.size(),
                    state.activeWirehairCaches.size(),
                    state.activeDirectRepeatRecords.size());
                if (!totalRecordCount)
                {
                    return ResumeStateResult::Failure(totalRecordCount.Error().code, 0);
                }
                const auto quotaStatus = CheckResumeSegmentQuota(
                    totalRecordCount.Value(), resourcePolicy.maxSegmentCount);
                if (!quotaStatus)
                {
                    return ResumeStateResult::Failure(quotaStatus.Error().code, recordStart);
                }
                try
                {
                    state.activeWirehairCaches.push_back(newRecord);
                }
                catch (const std::bad_alloc&)
                {
                    return ResumeStateResult::Failure(ProtocolErrorCode::ResourceExhausted, 0);
                }
            }
            break;
        }
        case static_cast<std::uint8_t>(ResumeRecordType::DirectRepeatReceivedBlocks):
        {
            const auto bodyResult = ParseResumeActiveDirectRepeatBody(
                std::span<const std::byte>(payload), resourcePolicy);
            if (!bodyResult)
            {
                return ResumeStateResult::Failure(
                    bodyResult.Error().code,
                    bodyBaseOffset + bodyResult.Error().offset);
            }

            const ResumeActiveDirectRepeatRecord& newRecord = bodyResult.Value();
            bool duplicateOfExistingRecord = false;
            for (const auto& existingCache : state.activeDirectRepeatRecords)
            {
                if (existingCache.segmentOrdinal != newRecord.segmentOrdinal)
                {
                    continue;
                }
                if (existingCache == newRecord)
                {
                    duplicateOfExistingRecord = true;
                }
                else
                {
                    return ResumeStateResult::Failure(
                        ProtocolErrorCode::ResumeRecordConflict, recordStart);
                }
            }

            for (const auto& existingRecord : state.completedSegments)
            {
                if (existingRecord.segmentOrdinal == newRecord.segmentOrdinal)
                {
                    return ResumeStateResult::Failure(
                        ProtocolErrorCode::ResumeRecordConflict, recordStart);
                }
            }
            for (const auto& existingCache : state.activeWirehairCaches)
            {
                if (existingCache.segmentOrdinal == newRecord.segmentOrdinal)
                {
                    return ResumeStateResult::Failure(
                        ProtocolErrorCode::ResumeRecordConflict, recordStart);
                }
            }

            if (!duplicateOfExistingRecord)
            {
                const auto totalRecordCount = CountResumeRecords(
                    state.completedSegments.size(),
                    state.activeWirehairCaches.size(),
                    state.activeDirectRepeatRecords.size());
                if (!totalRecordCount)
                {
                    return ResumeStateResult::Failure(totalRecordCount.Error().code, 0);
                }
                const auto quotaStatus = CheckResumeSegmentQuota(
                    totalRecordCount.Value(), resourcePolicy.maxSegmentCount);
                if (!quotaStatus)
                {
                    return ResumeStateResult::Failure(quotaStatus.Error().code, recordStart);
                }
                try
                {
                    state.activeDirectRepeatRecords.push_back(newRecord);
                }
                catch (const std::bad_alloc&)
                {
                    return ResumeStateResult::Failure(ProtocolErrorCode::ResourceExhausted, 0);
                }
            }
            break;
        }
        default:
            // Unknown tag: fail closed so future record types are never
            // misinterpreted as valid state by this reader.
            return ResumeStateResult::Failure(ProtocolErrorCode::InvalidEnumValue, bodyBaseOffset);
        }
    }

    return ResumeStateResult::Success(std::move(state));
}

ResumeStateBuilder::ResumeStateBuilder(const ReceiverResourcePolicy& resourcePolicy) noexcept
    : resourcePolicy_(resourcePolicy)
{
}

ResumeStateBuilder::ResumeStateBuilder(ResumeStateBuilder&& other) noexcept = default;

ResumeStateBuilder& ResumeStateBuilder::operator=(ResumeStateBuilder&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    resourcePolicy_ = std::move(other.resourcePolicy_);
    document_ = std::move(other.document_);
    completedKeys_ = std::move(other.completedKeys_);
    wirehairCacheOrdinals_ = std::move(other.wirehairCacheOrdinals_);
    directRepeatOrdinals_ = std::move(other.directRepeatOrdinals_);
    return *this;
}

ProtocolResult<ResumeStateBuilder> ResumeStateBuilder::Create(
    const ReceiverResourcePolicy& resourcePolicy)
{
    using BuilderResult = ProtocolResult<ResumeStateBuilder>;
    const auto policyStatus = ValidateReceiverResourcePolicy(resourcePolicy);
    if (!policyStatus)
    {
        return BuilderResult::Failure(policyStatus.Error().code, policyStatus.Error().offset);
    }
    return BuilderResult::Success(ResumeStateBuilder(resourcePolicy));
}

std::span<const std::byte> ResumeStateBuilder::GetDocument() const noexcept
{
    return document_;
}

std::size_t ResumeStateBuilder::GetByteCount() const noexcept
{
    return document_.size();
}

bool ResumeStateBuilder::IsOrdinalClaimedByOtherCategory(
    const std::uint64_t segmentOrdinal) const
{
    for (const auto& [sessionId, ordinal] : completedKeys_)
    {
        if (ordinal == segmentOrdinal)
        {
            return true;
        }
    }
    for (const auto ordinal : wirehairCacheOrdinals_)
    {
        if (ordinal == segmentOrdinal)
        {
            return true;
        }
    }
    for (const auto ordinal : directRepeatOrdinals_)
    {
        if (ordinal == segmentOrdinal)
        {
            return true;
        }
    }
    return false;
}

ProtocolStatus ResumeStateBuilder::AppendRecordBytes(
    const std::span<const std::byte> bodyBytes)
{
    // Budget gate with checked arithmetic before any allocation or growth.
    const auto bodySizeResult = CheckedNarrowUnsigned<std::uint64_t>(bodyBytes.size());
    if (!bodySizeResult)
    {
        return ProtocolStatus::Failure(bodySizeResult.Error().code, 0);
    }
    std::uint64_t recordTotalBytes = static_cast<std::uint64_t>(kResumeRecordEnvelopeBytes);
    const auto recordTotalAdditionResult = CheckedAddUint64(recordTotalBytes, bodySizeResult.Value());
    if (!recordTotalAdditionResult)
    {
        return ProtocolStatus::Failure(recordTotalAdditionResult.Error().code, 0);
    }
    const std::uint64_t recordSizeWithEnvelope = recordTotalAdditionResult.Value();

    const auto currentDocumentSizeResult = CheckedNarrowUnsigned<std::uint64_t>(document_.size());
    if (!currentDocumentSizeResult)
    {
        return ProtocolStatus::Failure(currentDocumentSizeResult.Error().code, 0);
    }
    const auto newDocumentTotalResult = CheckedAddUint64(
        currentDocumentSizeResult.Value(), recordSizeWithEnvelope);
    if (!newDocumentTotalResult)
    {
        return ProtocolStatus::Failure(newDocumentTotalResult.Error().code, 0);
    }
    if (newDocumentTotalResult.Value() > resourcePolicy_.maxResumeBytes)
    {
        // Over-budget append is rejected before any byte or key changes.
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceLimitExceeded, 0);
    }

    const auto newDocumentSizeResult = CheckedUint64ToSize(newDocumentTotalResult.Value());
    if (!newDocumentSizeResult)
    {
        return ProtocolStatus::Failure(newDocumentSizeResult.Error().code, 0);
    }

    const std::size_t previousSize = document_.size();
    try
    {
        document_.resize(newDocumentSizeResult.Value());
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }
    catch (const std::length_error&)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }

    const std::size_t appendRegionBytes = newDocumentSizeResult.Value() - previousSize;
    const auto status = SerializeResumeRecord(
        bodyBytes,
        std::span<std::byte>(document_.data() + previousSize, appendRegionBytes));
    if (!status)
    {
        // Capacity was computed exactly above; this rollback keeps the class
        // fail-closed if a future edit breaks that invariant.
        document_.resize(previousSize);
        return status;
    }
    return ProtocolStatus::Success();
}

ProtocolStatus ResumeStateBuilder::AppendCompletedSegment(
    const ResumeCompletedSegmentRecord& record)
{
    const auto boundsStatus = ValidateResumeCompletedSegment(record, resourcePolicy_);
    if (!boundsStatus)
    {
        return boundsStatus;
    }

    for (const auto& [sessionId, ordinal] : completedKeys_)
    {
        if (ordinal == record.segmentOrdinal && sessionId == record.sessionId.bytes)
        {
            // Strict writer: duplicate keys are rejected even when identical.
            return ProtocolStatus::Failure(ProtocolErrorCode::ResumeRecordConflict, 0);
        }
    }
    if (IsOrdinalClaimedByOtherCategory(record.segmentOrdinal))
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResumeRecordConflict, 0);
    }

    // Mirror the loader's total-record segment quota (design doc section 31.4) so a builder
    // can never emit more per-segment records than LoadResumeState will accept
    // (round-trip law). It runs after key conflict checks to keep error-code
    // precedence in lockstep with the load path, where duplicate and conflict
    // scans precede it.
    const auto totalRecordCount = CountResumeRecords(
        completedKeys_.size(), wirehairCacheOrdinals_.size(), directRepeatOrdinals_.size());
    if (!totalRecordCount)
    {
        return ProtocolStatus::Failure(totalRecordCount.Error().code, 0);
    }
    const auto quotaStatus = CheckResumeSegmentQuota(
        totalRecordCount.Value(), resourcePolicy_.maxSegmentCount);
    if (!quotaStatus)
    {
        return ProtocolStatus::Failure(quotaStatus.Error().code, 0);
    }

    std::array<std::byte, kCompletedSegmentBodyBytes> body{};
    const auto bodyStatus = SerializeResumeCompletedSegmentBody(record, std::span<std::byte>(body));
    if (!bodyStatus)
    {
        return bodyStatus;
    }
    const auto appendStatus = AppendRecordBytes(std::span<const std::byte>(body));
    if (!appendStatus)
    {
        return appendStatus;
    }

    completedKeys_.emplace_back(record.sessionId.bytes, record.segmentOrdinal);
    return ProtocolStatus::Success();
}

ProtocolStatus ResumeStateBuilder::AppendActiveWirehairCache(
    const ResumeActiveWirehairCacheRecord& record)
{
    for (const auto& entry : record.entries)
    {
        const auto boundsStatus = ValidateResumeWirehairEntry(entry, resourcePolicy_);
        if (!boundsStatus)
        {
            return boundsStatus;
        }
    }

    // Strict writer: any duplicated outerBlockId inside one cache record is a
    // conflict, even with identical payload bytes.
    const auto idScanStatus = ScanResumeWirehairEntryIds(record.entries, true);
    if (!idScanStatus)
    {
        return idScanStatus;
    }

    for (const auto ordinal : wirehairCacheOrdinals_)
    {
        if (ordinal == record.segmentOrdinal)
        {
            return ProtocolStatus::Failure(ProtocolErrorCode::ResumeRecordConflict, 0);
        }
    }
    if (IsOrdinalClaimedByOtherCategory(record.segmentOrdinal))
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResumeRecordConflict, 0);
    }

    const auto totalRecordCount = CountResumeRecords(
        completedKeys_.size(), wirehairCacheOrdinals_.size(), directRepeatOrdinals_.size());
    if (!totalRecordCount)
    {
        return ProtocolStatus::Failure(totalRecordCount.Error().code, 0);
    }
    const auto quotaStatus = CheckResumeSegmentQuota(
        totalRecordCount.Value(), resourcePolicy_.maxSegmentCount);
    if (!quotaStatus)
    {
        return ProtocolStatus::Failure(quotaStatus.Error().code, 0);
    }
    const auto bodySizeResult = GetResumeActiveWirehairBodySize(record);
    if (!bodySizeResult)
    {
        return ProtocolStatus::Failure(bodySizeResult.Error().code, 0);
    }

    std::vector<std::byte> body;
    try
    {
        body.resize(static_cast<std::size_t>(bodySizeResult.Value()));
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }
    catch (const std::length_error&)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }

    const auto bodyStatus = SerializeResumeActiveWirehairBody(record, std::span<std::byte>(body));
    if (!bodyStatus)
    {
        return bodyStatus;
    }
    const auto appendStatus = AppendRecordBytes(std::span<const std::byte>(body));
    if (!appendStatus)
    {
        return appendStatus;
    }

    wirehairCacheOrdinals_.push_back(record.segmentOrdinal);
    return ProtocolStatus::Success();
}

ProtocolStatus ResumeStateBuilder::AppendDirectRepeatReceivedBlocks(
    const ResumeActiveDirectRepeatRecord& record)
{
    const auto countBoundsStatus = ValidateResumeDirectRepeatCount(record.directBlockCount, resourcePolicy_);
    if (!countBoundsStatus)
    {
        return countBoundsStatus;
    }
    for (const auto& entry : record.entries)
    {
        if (static_cast<std::uint64_t>(entry.blockOrdinal) >= static_cast<std::uint64_t>(record.directBlockCount))
        {
            return ProtocolStatus::Failure(ProtocolErrorCode::SegmentOrdinalOutOfRange, 0);
        }
        const auto boundsStatus = ValidateResumeDirectRepeatEntry(entry, resourcePolicy_);
        if (!boundsStatus)
        {
            return boundsStatus;
        }
    }

    const auto idScanStatus = ScanResumeDirectRepeatEntryIds(record.entries, true);
    if (!idScanStatus)
    {
        return idScanStatus;
    }

    for (const auto ordinal : directRepeatOrdinals_)
    {
        if (ordinal == record.segmentOrdinal)
        {
            return ProtocolStatus::Failure(ProtocolErrorCode::ResumeRecordConflict, 0);
        }
    }
    if (IsOrdinalClaimedByOtherCategory(record.segmentOrdinal))
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResumeRecordConflict, 0);
    }

    const auto totalRecordCount = CountResumeRecords(
        completedKeys_.size(), wirehairCacheOrdinals_.size(), directRepeatOrdinals_.size());
    if (!totalRecordCount)
    {
        return ProtocolStatus::Failure(totalRecordCount.Error().code, 0);
    }
    const auto quotaStatus = CheckResumeSegmentQuota(
        totalRecordCount.Value(), resourcePolicy_.maxSegmentCount);
    if (!quotaStatus)
    {
        return ProtocolStatus::Failure(quotaStatus.Error().code, 0);
    }
    const auto bodySizeResult = GetResumeActiveDirectRepeatBodySize(record);
    if (!bodySizeResult)
    {
        return ProtocolStatus::Failure(bodySizeResult.Error().code, 0);
    }

    std::vector<std::byte> body;
    try
    {
        body.resize(static_cast<std::size_t>(bodySizeResult.Value()));
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }
    catch (const std::length_error&)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }

    const auto bodyStatus = SerializeResumeActiveDirectRepeatBody(record, std::span<std::byte>(body));
    if (!bodyStatus)
    {
        return bodyStatus;
    }
    const auto appendStatus = AppendRecordBytes(std::span<const std::byte>(body));
    if (!appendStatus)
    {
        return appendStatus;
    }

    directRepeatOrdinals_.push_back(record.segmentOrdinal);
    return ProtocolStatus::Success();
}

} // namespace pbprotocol
