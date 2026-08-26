// resume.state structured fuzz driver (design doc section 31, FastResume
// phase). Dual-mode harness: with libFuzzer this compiles only the invariant
// entry point; without it a deterministic mutation runner drives the same
// oracle plus a pinned structured self-test and corpus replay.
//
// Oracle strategy: every structurally meaningful input class (canonical
// documents, truncations at all record boundaries, single-byte flips with an
// exact error-code/offset expectation, zero-byte tails, quota edges, and
// hand-crafted semantic cases) is checked against expectations derived from
// the documented v1 envelope/typed-body layout using local reference code
// (independent CRC-32C, independent little-endian framing), never by
// re-implementing LoadResumeState's own decisions. Arbitrary garbage inputs
// are held to a fail-closed invariant: success or one of the documented
// error codes with an in-range offset, plus bit-exact determinism across two
// loads.

#include "pbprotocol/crc32c.h"
#include "pbprotocol/protocol_result.h"
#include "pbprotocol/protocol_types.h"
#include "pbprotocol/resume_state.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kFuzzMaxResumeBytes = 1024U;
constexpr std::uint32_t kFuzzMaxOuterBlockBytes = 32U;
constexpr std::uint64_t kDefaultIterations = 100000ULL;
constexpr std::uint64_t kDefaultSeed = 0x5EED1CAFE0BAD5E7ULL;

// v1 envelope layout, mirrored locally so the reference oracle shares no code
// with the module under test (pbprotocol/resume_state.h documents the same).
constexpr std::size_t kEnvelopeBytes = 18U;
constexpr std::size_t kHeaderBytes = 14U;
const std::array<std::byte, 4> kRecordMagic{
    std::byte{'P'},
    std::byte{'B'},
    std::byte{'R'},
    std::byte{'S'}};

// Typed body layouts in bytes (tag byte included), little-endian:
//   completed     tag(1)+sessionId(16)+ordinal(8)+rawOffset(8)+rawSize(8)+digest(32)
//   wirehair      tag(1)+ordinal(8)+profile(32)+entryCount(4)
//                 + entries{outerBlockId(4),payloadLen(4),payload[...]}
//   directrepeat  tag(1)+ordinal(8)+directBlockCount(4)+entryCount(4)
//                 + entries{blockOrdinal(4),realPayloadBytes(4),paddedLen(4),padded[...]}
constexpr std::size_t kCompletedBodyBytes = 73U;
constexpr std::size_t kWirehairHeaderBodyBytes = 45U;
constexpr std::uint64_t kWirehairEntryPrefixBytes = 8U;
constexpr std::size_t kDirectHeaderBodyBytes = 17U;
constexpr std::uint64_t kDirectEntryPrefixBytes = 12U;

// Body-relative offsets pinned for exact error-offset assertions.
constexpr std::size_t kWirehairEntryCountBodyOffset = 41U;   // tag+ordinal+profile
constexpr std::size_t kDirectBlockCountBodyOffset = 9U;      // tag+ordinal
constexpr std::size_t kDirectEntryCountBodyOffset = 13U;     // +directBlockCount(4)
constexpr std::size_t kDirectFirstBlockOrdinalBodyOffset = 17U;
constexpr std::size_t kDirectRealPayloadBytesBodyOffset = 21U;
constexpr std::size_t kWirehairFirstPayloadLenBodyOffset = 49U; // header(45)+id(4)

// Generation field domains stay inside the fuzz policy so every generated
// record is individually valid (failures must only come from mutation).
constexpr std::uint64_t kGeneratedMaxRawSizeBytes = 64ULL;
constexpr std::uint32_t kGeneratedMaxPayloadBytes = 32U;     // == kFuzzMaxOuterBlockBytes
constexpr std::uint32_t kGeneratedMaxDirectBlockCount = 8U;  // <= policy max (64)
constexpr std::uint64_t kOrdinalPoolSize = 2048ULL;

[[nodiscard]] std::uint64_t NextRandom(std::uint64_t& state) noexcept
{
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

[[nodiscard]] bool ParseUint64(
    const std::string_view text,
    std::uint64_t& value) noexcept
{
    const auto parseResult = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value);
    return parseResult.ec == std::errc{} &&
        parseResult.ptr == text.data() + text.size();
}

struct FuzzFailure
{
    [[noreturn]] static void Report(const char* caseLabel) noexcept
    {
        std::cerr << "FUZZ_INVARIANT_FAILED case=" << caseLabel << '\n';
        std::abort();
    }

    static void Check(const bool condition, const char* caseLabel)
    {
        if (!condition)
        {
            Report(caseLabel);
        }
    }
};

// Independent reference CRC-32C (reflected Castagnoli 0x82F63B78, init/xorout
// 0xFFFFFFFF). Semantic mutations repair envelope checksums through this path
// so the harness never trusts pbprotocol::ComputeCrc32c; the structured
// self-test differentially cross-checks both implementations.
class ReferenceCrc32c
{
public:
    static std::uint32_t Compute(std::span<const std::byte> data) noexcept
    {
        const auto& table = Table();
        std::uint32_t crcState = 0xFFFFFFFFu;
        for (const std::byte dataByte : data)
        {
            const std::uint8_t index = static_cast<std::uint8_t>(crcState ^
                static_cast<std::uint8_t>(dataByte));
            crcState = table[index] ^ (crcState >> 8U);
        }
        return crcState ^ 0xFFFFFFFFu;
    }

private:
    [[nodiscard]] static const std::array<std::uint32_t, 256>& Table() noexcept
    {
        static const std::array<std::uint32_t, 256> table = BuildTable();
        return table;
    }

    [[nodiscard]] static std::array<std::uint32_t, 256> BuildTable() noexcept
    {
        std::array<std::uint32_t, 256> table{};
        for (std::uint32_t entryIndex = 0U; entryIndex < 256U; entryIndex++)
        {
            std::uint32_t registerValue = entryIndex;
            for (std::uint32_t bitIndex = 0U; bitIndex < 8U; bitIndex++)
            {
                const bool lowBitSet = (registerValue & 1U) != 0U;
                registerValue >>= 1U;
                if (lowBitSet)
                {
                    registerValue ^= 0x82F63B78u;
                }
            }
            table[entryIndex] = registerValue;
        }
        return table;
    }
};

// Little-endian framing helpers over raw bytes (bounds are guaranteed by the
// caller: every read site sits inside a verified envelope region).
[[nodiscard]] std::uint16_t ReadU16LE(const std::byte* base) noexcept
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint8_t>(base[0]) |
        (static_cast<std::uint8_t>(base[1]) << 8));
}

[[nodiscard]] std::uint32_t ReadU32LE(const std::byte* base) noexcept
{
    return static_cast<std::uint32_t>(
        static_cast<std::uint8_t>(base[0]) |
        (static_cast<std::uint8_t>(base[1]) << 8) |
        (static_cast<std::uint8_t>(base[2]) << 16) |
        (static_cast<std::uint8_t>(base[3]) << 24));
}

[[nodiscard]] std::uint64_t ReadU64LE(const std::byte* base) noexcept
{
    const auto lowHalf = static_cast<std::uint64_t>(ReadU32LE(base));
    const auto highHalf = static_cast<std::uint64_t>(ReadU32LE(base + 4));
    return lowHalf | (highHalf << 32);
}

void AppendFixedBytes(
    std::vector<std::byte>& output,
    const std::span<const std::byte> bytes)
{
    for (const auto& byteValue : bytes)
    {
        output.push_back(byteValue);
    }
}

void AppendU32LE(std::vector<std::byte>& output, const std::uint32_t value)
{
    output.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(value)));
    output.push_back(static_cast<std::byte>((value >> 8) & 0xFFU));
    output.push_back(static_cast<std::byte>((value >> 16) & 0xFFU));
    output.push_back(static_cast<std::byte>((value >> 24) & 0xFFU));
}

void AppendU64LE(std::vector<std::byte>& output, const std::uint64_t value)
{
    AppendU32LE(output, static_cast<std::uint32_t>(static_cast<std::uint32_t>(value)));
    AppendU32LE(output, static_cast<std::uint32_t>(value >> 32));
}

// std::span has no vector constructor in C++20; these keep call sites explicit.
[[nodiscard]] std::span<const std::byte> AsConstSpan(const std::vector<std::byte>& bytes) noexcept
{
    return std::span<const std::byte>(bytes.data(), bytes.size());
}

[[nodiscard]] std::span<std::byte> AsMutSpan(std::vector<std::byte>& bytes) noexcept
{
    return std::span<std::byte>(bytes.data(), bytes.size());
}

// Independent v1 envelope framing with a reference CRC: magic | version(1) |
// reserved(0) | payloadLength u64 | payload | crc32c over everything before.
std::vector<std::byte> FrameEnvelopePayload(const std::span<const std::byte> payload)
{
    std::vector<std::byte> frame;
    frame.reserve(kEnvelopeBytes + payload.size());
    AppendFixedBytes(frame, kRecordMagic);
    frame.push_back(std::byte{1});
    frame.push_back(std::byte{0});
    AppendU64LE(frame, static_cast<std::uint64_t>(payload.size()));
    AppendFixedBytes(frame, payload);
    const std::uint32_t frameCrc = ReferenceCrc32c::Compute(
        std::span<const std::byte>(frame.data(), frame.size()));
    AppendU32LE(frame, frameCrc);
    return frame;
}

void RefreshFrameCrc(std::vector<std::byte>& frame)
{
    const std::uint32_t recomputedCrc = ReferenceCrc32c::Compute(
        std::span<const std::byte>(frame.data(), frame.size() - 4U));
    for (std::size_t byteIndex = 0; byteIndex < 4U; byteIndex++)
    {
        frame[frame.size() - 4U + byteIndex] = static_cast<std::byte>(
            static_cast<std::uint8_t>((recomputedCrc >> (8U * byteIndex)) & 0xFFU));
    }
}

[[nodiscard]] std::vector<std::byte> MakeCompletedBody(
    const std::span<const std::byte, 16> sessionIdBytes,
    const std::uint64_t segmentOrdinal,
    const std::uint64_t rawOffset,
    const std::uint64_t rawSize)
{
    std::vector<std::byte> body;
    body.reserve(kCompletedBodyBytes);
    body.push_back(static_cast<std::byte>(pbprotocol::ResumeRecordType::CompletedSegment));
    AppendFixedBytes(body, sessionIdBytes);
    AppendU64LE(body, segmentOrdinal);
    AppendU64LE(body, rawOffset);
    AppendU64LE(body, rawSize);
    body.resize(kCompletedBodyBytes, std::byte{0xA5}); // digest bytes (integrity metadata only)
    return body;
}

[[nodiscard]] std::vector<std::byte> MakeWirehairCacheBody(
    const std::uint64_t segmentOrdinal,
    const std::span<const std::byte, 32> profileBytes,
    const std::vector<std::pair<std::uint32_t, std::span<const std::byte>>>& entries)
{
    std::vector<std::byte> body;
    body.reserve(kWirehairHeaderBodyBytes);
    body.push_back(static_cast<std::byte>(pbprotocol::ResumeRecordType::ActiveWirehairCache));
    AppendU64LE(body, segmentOrdinal);
    AppendFixedBytes(body, profileBytes);
    AppendU32LE(body, static_cast<std::uint32_t>(entries.size()));
    for (const auto& [entryId, entryPayload] : entries)
    {
        AppendU32LE(body, entryId);
        AppendU32LE(body, static_cast<std::uint32_t>(entryPayload.size()));
        AppendFixedBytes(body, entryPayload);
    }
    return body;
}

[[nodiscard]] std::vector<std::byte> MakeDirectRepeatBody(
    const std::uint64_t segmentOrdinal,
    const std::uint32_t directBlockCount,
    const std::vector<std::tuple<std::uint32_t, std::uint32_t, std::span<const std::byte>>>& entries)
{
    // entry tuple: (blockOrdinal, realPayloadBytes, paddedPayload)
    std::vector<std::byte> body;
    body.reserve(kDirectHeaderBodyBytes);
    body.push_back(static_cast<std::byte>(pbprotocol::ResumeRecordType::DirectRepeatReceivedBlocks));
    AppendU64LE(body, segmentOrdinal);
    AppendU32LE(body, directBlockCount);
    AppendU32LE(body, static_cast<std::uint32_t>(entries.size()));
    for (const auto& [blockOrdinal, realPayloadBytes, paddedPayload] : entries)
    {
        AppendU32LE(body, blockOrdinal);
        AppendU32LE(body, realPayloadBytes);
        AppendU32LE(body, static_cast<std::uint32_t>(paddedPayload.size()));
        AppendFixedBytes(body, paddedPayload);
    }
    return body;
}

} // namespace

namespace {

using LoadedResumeState = pbprotocol::LoadedResumeState;
using ResumeActiveDirectRepeatRecord = pbprotocol::ResumeActiveDirectRepeatRecord;
using ResumeActiveWirehairCacheRecord = pbprotocol::ResumeActiveWirehairCacheRecord;
using ResumeCompletedSegmentRecord = pbprotocol::ResumeCompletedSegmentRecord;
using ResumeDirectRepeatEntry = pbprotocol::ResumeDirectRepeatEntry;
using ResumeWirehairCacheEntry = pbprotocol::ResumeWirehairCacheEntry;

[[nodiscard]] pbprotocol::ReceiverResourcePolicy MakeFuzzResourcePolicy() noexcept
{
    auto resourcePolicy = pbprotocol::GetDefaultReceiverResourcePolicy();
    // Small budgets make torn tails, quota gates and entry conflicts reachable
    // inside one 1 KiB document (matches the corpus generation parameters).
    resourcePolicy.maxResumeBytes = kFuzzMaxResumeBytes;
    resourcePolicy.maxOuterBlockBytes = kFuzzMaxOuterBlockBytes;
    return resourcePolicy;
}

// Result checks return bool so the structured self-test can chain labelled
// cases while the mutation loop turns the same predicates into hard aborts.
void EmitCheckDetail(
    const char* caseLabel,
    const std::string& detail) noexcept
{
    std::cerr << "resume-state-check-detail case=" << caseLabel << ' '
              << detail << '\n';
}

[[nodiscard]] bool CheckLoadSuccessExactState(
    pbprotocol::ProtocolResult<LoadedResumeState> result,
    const LoadedResumeState& expectedState,
    const char* caseLabel)
{
    if (!result.HasValue())
    {
        EmitCheckDetail(caseLabel, "expected-success code=" +
            std::to_string(static_cast<unsigned>(result.Error().code)) +
            " offset=" + std::to_string(result.Error().offset));
        return false;
    }
    const LoadedResumeState& actualState = result.Value();
    if (actualState == expectedState)
    {
        return true;
    }
    EmitCheckDetail(caseLabel, "state-mismatch completed=" +
        std::to_string(actualState.completedSegments.size()) + "/" +
        std::to_string(expectedState.completedSegments.size()) +
        " wirehair=" + std::to_string(actualState.activeWirehairCaches.size()) + "/" +
        std::to_string(expectedState.activeWirehairCaches.size()) +
        " direct=" + std::to_string(actualState.activeDirectRepeatRecords.size()) + "/" +
        std::to_string(expectedState.activeDirectRepeatRecords.size()) +
        " flag=" + (actualState.hasTruncatedTail ? "1/" : "0/") +
        (expectedState.hasTruncatedTail ? "1" : "0"));
    return false;
}

[[nodiscard]] bool CheckLoadErrorExact(
    pbprotocol::ProtocolResult<LoadedResumeState> result,
    const pbprotocol::ProtocolErrorCode expectedCode,
    const std::size_t expectedOffset,
    const char* caseLabel)
{
    if (result.HasValue())
    {
        EmitCheckDetail(caseLabel, "expected-failure got-success records=" +
            std::to_string(result.Value().completedSegments.size() +
                result.Value().activeWirehairCaches.size() +
                result.Value().activeDirectRepeatRecords.size()));
        return false;
    }
    const auto& actualError = result.Error();
    if (actualError.code != expectedCode || actualError.offset != expectedOffset)
    {
        EmitCheckDetail(caseLabel, "expected code=" +
            std::to_string(static_cast<unsigned>(expectedCode)) +
            " offset=" + std::to_string(expectedOffset) +
            " got code=" + std::to_string(static_cast<unsigned>(actualError.code)) +
            " offset=" + std::to_string(actualError.offset));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Generated document model: a builder-produced canonical document plus the
// generator's own bookkeeping (typed records and envelope boundaries) that an
// independent framing walk re-validates below.
// ---------------------------------------------------------------------------
enum class GeneratedCategory : std::uint8_t
{
    Completed = 1,
    Wirehair = 2,
    DirectRepeat = 3,
};

struct GeneratedRecord
{
    GeneratedCategory category = GeneratedCategory::Completed;
    ResumeCompletedSegmentRecord completed{};
    ResumeActiveWirehairCacheRecord wirehair{};
    ResumeActiveDirectRepeatRecord directRepeat{};
};

struct GeneratedDocument
{
    std::vector<std::byte> bytes{};
    std::vector<GeneratedRecord> records{};
    // Record i occupies [recordBounds[i], recordBounds[i + 1]); the final
    // bound equals bytes.size().
    std::vector<std::size_t> recordBounds{};
};

// Independent body-size model (mirrors the documented layout) used both to
// size candidates inside the budget and as a serializer-drift cross-check.
[[nodiscard]] std::uint64_t GeneratedBodyBytes(const GeneratedRecord& record) noexcept
{
    std::uint64_t bodyBytes = 0ULL;
    switch (record.category)
    {
    case GeneratedCategory::Completed:
        bodyBytes = kCompletedBodyBytes;
        break;
    case GeneratedCategory::Wirehair:
        bodyBytes = kWirehairHeaderBodyBytes;
        for (const auto& wirehairEntry : record.wirehair.entries)
        {
            bodyBytes += kWirehairEntryPrefixBytes +
                static_cast<std::uint64_t>(wirehairEntry.payload.size());
        }
        break;
    case GeneratedCategory::DirectRepeat:
        bodyBytes = kDirectHeaderBodyBytes;
        for (const auto& directEntry : record.directRepeat.entries)
        {
            bodyBytes += kDirectEntryPrefixBytes +
                static_cast<std::uint64_t>(directEntry.paddedPayload.size());
        }
        break;
    default:
        FuzzFailure::Check(false, "generator-unknown-category");
        return 0ULL;
    }
    // Entry counts are capped at 6 with payloads of at most 32 bytes, so this
    // bound is a generator-invariant sanity gate rather than real arithmetic.
    FuzzFailure::Check(bodyBytes < kFuzzMaxResumeBytes + 1U, "generator-body-overflow");
    return bodyBytes;
}

// Independent envelope framing walk over a generated document: verifies magic,
// version, reserved, the declared payload length against the bookkept record
// bounds, and re-computes each stored CRC with the reference implementation.
[[nodiscard]] bool VerifyEnvelopeFraming(const GeneratedDocument& document)
{
    const std::byte* base = document.bytes.data();
    std::size_t position = 0;
    for (std::size_t recordIndex = 0; recordIndex < document.records.size(); recordIndex++)
    {
        if (position + kEnvelopeBytes > document.bytes.size())
        {
            EmitCheckDetail("framing-walk", "record-too-short at=" + std::to_string(position));
            return false;
        }
        const std::byte* recordBase = base + position;
        if (!std::equal(kRecordMagic.begin(), kRecordMagic.end(), recordBase))
        {
            EmitCheckDetail("framing-walk", "magic-mismatch at=" + std::to_string(position));
            return false;
        }
        const auto versionByte = static_cast<std::uint8_t>(recordBase[4]);
        const auto reservedByte = static_cast<std::uint8_t>(recordBase[5]);
        if (versionByte != 1U || reservedByte != 0U)
        {
            EmitCheckDetail("framing-walk", "header-fields version=" +
                std::to_string(versionByte) + " reserved=" + std::to_string(reservedByte));
            return false;
        }
        const auto declaredPayloadBytes = ReadU64LE(recordBase + 6);
        const auto boundaryDeltaBytes =
            document.recordBounds[recordIndex + 1] - document.recordBounds[recordIndex];
        // Size-to-size comparison: the stored length must describe exactly the
        // bytes this record occupies (the bookkept boundary delta), independent
        // of where the record sits in the document.
        if (declaredPayloadBytes > document.bytes.size() ||
            kEnvelopeBytes + static_cast<std::size_t>(declaredPayloadBytes) !=
                boundaryDeltaBytes)
        {
            EmitCheckDetail("framing-walk", "declared-length declared=" +
                std::to_string(declaredPayloadBytes) + " at=" + std::to_string(position) +
                " recordIndex=" + std::to_string(recordIndex) +
                " boundaryDelta=" + std::to_string(boundaryDeltaBytes));
            return false;
        }
        const auto recordSize = boundaryDeltaBytes;
        const auto storedCrc = ReadU32LE(recordBase + recordSize - 4U);
        const auto referenceCrc = ReferenceCrc32c::Compute(
            std::span<const std::byte>(recordBase, recordSize - 4U));
        if (storedCrc != referenceCrc)
        {
            EmitCheckDetail("framing-walk", "crc-mismatch at=" + std::to_string(position));
            return false;
        }
        position += recordSize;
    }
    if (position != document.bytes.size())
    {
        EmitCheckDetail("framing-walk", "total-length walked=" +
            std::to_string(position) + " total=" + std::to_string(document.bytes.size()));
        return false;
    }
    return true;
}

// Expected loaded state after the first recordCount records of a canonical
// document (all keys unique by construction, so load keeps them as-is).
[[nodiscard]] LoadedResumeState ExpectedStateForPrefix(
    const GeneratedDocument& document,
    const std::size_t recordCount,
    const bool truncatedTail)
{
    LoadedResumeState expectedState;
    for (std::size_t recordIndex = 0;
         recordIndex < recordCount && recordIndex < document.records.size();
         recordIndex++)
    {
        switch (document.records[recordIndex].category)
        {
        case GeneratedCategory::Completed:
            expectedState.completedSegments.push_back(
                document.records[recordIndex].completed);
            break;
        case GeneratedCategory::Wirehair:
            expectedState.activeWirehairCaches.push_back(
                document.records[recordIndex].wirehair);
            break;
        case GeneratedCategory::DirectRepeat:
            expectedState.activeDirectRepeatRecords.push_back(
                document.records[recordIndex].directRepeat);
            break;
        default:
            FuzzFailure::Check(false, "expected-state-unknown-category");
            break;
        }
    }
    expectedState.hasTruncatedTail = truncatedTail;
    return expectedState;
}

void FillRandomBytes(
    std::span<std::byte> target,
    std::uint64_t& randomState) noexcept
{
    for (std::size_t byteIndex = 0; byteIndex < target.size(); byteIndex++)
    {
        target[byteIndex] = static_cast<std::byte>(NextRandom(randomState) & 0xFFULL);
    }
}

// Partial Fisher-Yates: returns entryCount unique values from [0, poolSize).
[[nodiscard]] std::vector<std::uint32_t> SampleUniqueIds(
    const std::size_t poolSize,
    const std::size_t entryCount,
    std::uint64_t& randomState)
{
    std::vector<std::uint32_t> idPool;
    for (std::size_t poolIndex = 0; poolIndex < poolSize; poolIndex++)
    {
        idPool.push_back(static_cast<std::uint32_t>(poolIndex));
    }
    const auto swapCount = std::min(entryCount, poolSize);
    if (swapCount >= 2U)
    {
        for (std::size_t shuffleIndex = 0; shuffleIndex + 1U < swapCount; shuffleIndex++)
        {
            const auto partnerIndex = static_cast<std::size_t>(
                NextRandom(randomState) % (poolSize - shuffleIndex)) + shuffleIndex;
            std::swap(idPool[shuffleIndex], idPool[partnerIndex]);
        }
    }
    return std::vector<std::uint32_t>(idPool.begin(), idPool.begin() + swapCount);
}

// Builds one candidate record whose body fits remainingBodyBudget with high
// probability; the caller still performs the exact budget pre-check.
[[nodiscard]] GeneratedRecord BuildCandidateRecord(
    const GeneratedCategory category,
    const std::uint64_t segmentOrdinal,
    const std::size_t remainingBodyBudget,
    std::uint64_t& randomState)
{
    GeneratedRecord record;
    record.category = category;

    if (category == GeneratedCategory::Completed)
    {
        pbprotocol::SessionId sessionId{};
        FillRandomBytes(sessionId.bytes, randomState);
        record.completed.sessionId = std::move(sessionId);
        record.completed.segmentOrdinal = segmentOrdinal;
        record.completed.rawOffset = NextRandom(randomState) % 5ULL;
        record.completed.rawSize =
            1ULL + (NextRandom(randomState) % kGeneratedMaxRawSizeBytes);
        FillRandomBytes(record.completed.rawDigest.bytes, randomState);
        return record;
    }

    // Variable-size records: pick an entry count that fits the body budget so
    // the caller's exact pre-check passes with high probability.
    const auto headerBody = static_cast<std::uint64_t>(category == GeneratedCategory::Wirehair
        ? kWirehairHeaderBodyBytes
        : kDirectHeaderBodyBytes);
    const auto entryPrefix = category == GeneratedCategory::Wirehair
        ? kWirehairEntryPrefixBytes
        : kDirectEntryPrefixBytes;
    std::uint64_t maximumEntries = 0ULL;
    if (remainingBodyBudget > headerBody)
    {
        // Each entry needs at least prefix + 1 payload byte.
        maximumEntries = (static_cast<std::uint64_t>(remainingBodyBudget) - headerBody) /
            (entryPrefix + 1ULL);
    }
    const auto cappedEntryCount = static_cast<std::size_t>(std::min(
        std::max(maximumEntries, 0ULL), 6ULL));
    const auto entryCount = static_cast<std::size_t>(
        NextRandom(randomState) % (cappedEntryCount + 1ULL));

    if (category == GeneratedCategory::Wirehair)
    {
        record.wirehair.segmentOrdinal = segmentOrdinal;
        FillRandomBytes(record.wirehair.wirehairProfile.bytes, randomState);
        const auto entryIds = SampleUniqueIds(96U, entryCount, randomState);
        for (std::size_t entryIndex = 0; entryIndex < entryCount; entryIndex++)
        {
            const auto payloadLength = static_cast<std::size_t>(
                NextRandom(randomState) % kGeneratedMaxPayloadBytes) + 1U;
            ResumeWirehairCacheEntry wirehairEntry;
            wirehairEntry.outerBlockId = entryIds[entryIndex];
            wirehairEntry.payload.resize(payloadLength);
            FillRandomBytes(wirehairEntry.payload, randomState);
            record.wirehair.entries.push_back(std::move(wirehairEntry));
        }
    }

    if (category == GeneratedCategory::DirectRepeat)
    {
        record.directRepeat.segmentOrdinal = segmentOrdinal;
        const auto directBlockCount = static_cast<std::uint32_t>(
            1ULL + (NextRandom(randomState) % kGeneratedMaxDirectBlockCount));
        record.directRepeat.directBlockCount = directBlockCount;
        // Entries stay a bounded subset of [0, directBlockCount).
        const auto usableEntryCount = std::min(
            entryCount, static_cast<std::size_t>(directBlockCount));
        const auto blockOrdinals = SampleUniqueIds(directBlockCount, usableEntryCount, randomState);
        for (std::size_t entryIndex = 0; entryIndex < usableEntryCount; entryIndex++)
        {
            const auto paddedLength = static_cast<std::size_t>(
                NextRandom(randomState) % kGeneratedMaxPayloadBytes) + 1U;
            const auto realPayloadBytes = static_cast<std::uint32_t>(
                (NextRandom(randomState) % static_cast<std::uint64_t>(paddedLength)) + 1ULL);
            ResumeDirectRepeatEntry directEntry;
            directEntry.blockOrdinal = blockOrdinals[entryIndex];
            directEntry.realPayloadBytes = realPayloadBytes;
            // Canonical zero padding: random payload prefix, zero tail.
            directEntry.paddedPayload.resize(paddedLength, std::byte{0});
            FillRandomBytes(
                AsMutSpan(directEntry.paddedPayload).first(realPayloadBytes),
                randomState);
            record.directRepeat.entries.push_back(std::move(directEntry));
        }
    }

    return record;
}

[[nodiscard]] GeneratedCategory PickRandomCategory(std::uint64_t& randomState) noexcept
{
    switch (NextRandom(randomState) % 3ULL)
    {
    case 0ULL:
        return GeneratedCategory::Completed;
    case 1ULL:
        return GeneratedCategory::Wirehair;
    default:
        return GeneratedCategory::DirectRepeat;
    }
}

[[nodiscard]] std::array<GeneratedCategory, 3> ShuffleForceOrder(std::uint64_t& randomState) noexcept
{
    std::array<GeneratedCategory, 3> forceOrder{
        GeneratedCategory::Completed,
        GeneratedCategory::Wirehair,
        GeneratedCategory::DirectRepeat};
    for (std::size_t shuffleIndex = 0; shuffleIndex + 1U < forceOrder.size(); shuffleIndex++)
    {
        const auto partnerIndex = static_cast<std::size_t>(
            NextRandom(randomState) % (forceOrder.size() - shuffleIndex)) + shuffleIndex;
        std::swap(forceOrder[shuffleIndex], forceOrder[partnerIndex]);
    }
    return forceOrder;
}

// Appends one fresh-ordinal record of the given category when it fits under
// sizeCapBytes. Returns false (without touching any state) when no candidate
// fits, which ends generation for this document. The builder must accept every
// candidate: unique keys plus individually valid fields are guaranteed by the
// generator, so a rejection is a model/SUT validation divergence and aborts.
bool TryAppendCandidateRecord(
    pbprotocol::ResumeStateBuilder& builder,
    GeneratedDocument& document,
    std::vector<std::uint64_t>& usedOrdinals,
    const GeneratedCategory category,
    const std::size_t sizeCapBytes,
    std::uint64_t& randomState)
{
    for (std::size_t attemptIndex = 0; attemptIndex < 8U; attemptIndex++)
    {
        // Fresh ordinal unique across all categories: the builder rejects any
        // cross-category ordinal reuse and same-key completed duplicates.
        const auto segmentOrdinal = static_cast<std::uint64_t>(
            NextRandom(randomState) % kOrdinalPoolSize);
        bool ordinalInUse = false;
        for (const auto existingOrdinal : usedOrdinals)
        {
            if (existingOrdinal == segmentOrdinal)
            {
                ordinalInUse = true;
                break;
            }
        }
        if (ordinalInUse)
        {
            continue;
        }

        const std::size_t currentBytes = builder.GetByteCount();
        const bool fitsEnvelope =
            static_cast<std::uint64_t>(currentBytes) + kEnvelopeBytes <= sizeCapBytes;
        const auto remainingBodyBudget = fitsEnvelope
            ? sizeCapBytes - currentBytes - kEnvelopeBytes
            : 0U;
        GeneratedRecord candidateRecord = BuildCandidateRecord(
            category, segmentOrdinal, remainingBodyBudget, randomState);

        // Exact budget pre-check with the independent body-size model: it must
        // agree with the builder's own checked arithmetic. A disagreement is a
        // serializer drift and surfaces as the append check below or in the
        // framing walk after generation.
        const std::uint64_t totalAfterAppend = static_cast<std::uint64_t>(currentBytes) +
            kEnvelopeBytes + GeneratedBodyBytes(candidateRecord);
        if (totalAfterAppend > sizeCapBytes || totalAfterAppend > kFuzzMaxResumeBytes)
        {
            continue; // candidate too large for the cap; retry smaller.
        }

        // ProtocolStatus is not default-constructible; dispatch through a lambda.
        const auto appendStatus = [&]() -> pbprotocol::ProtocolStatus
        {
            switch (category)
            {
            case GeneratedCategory::Completed:
                return builder.AppendCompletedSegment(candidateRecord.completed);
            case GeneratedCategory::Wirehair:
                return builder.AppendActiveWirehairCache(candidateRecord.wirehair);
            case GeneratedCategory::DirectRepeat:
                return builder.AppendDirectRepeatReceivedBlocks(candidateRecord.directRepeat);
            default:
                FuzzFailure::Check(false, "generator-append-unknown-category");
                return pbprotocol::ProtocolStatus::Success(); // unreachable: the check above aborts.
            }
        }();
        FuzzFailure::Check(appendStatus.HasValue(), "generator-builder-rejects-valid-record");

        document.records.push_back(std::move(candidateRecord));
        usedOrdinals.push_back(segmentOrdinal);
        document.recordBounds.push_back(builder.GetByteCount());
        return true;
    }
    return false;
}

// Builds a canonical document of at most sizeCapBytes: one forced record per
// category (when the budget allows) plus random fills. All keys are unique, so
// LoadResumeState keeps every record and ExpectedStateForPrefix is exact.
[[nodiscard]] GeneratedDocument GenerateResumeDocument(
    const std::size_t sizeCapBytes,
    std::uint64_t& randomState)
{
    auto builderResult = pbprotocol::ResumeStateBuilder::Create(MakeFuzzResourcePolicy());
    FuzzFailure::Check(builderResult.HasValue(), "generator-builder-create");
    auto builder = std::move(builderResult).Value();

    GeneratedDocument document;
    document.recordBounds.push_back(0U);
    std::vector<std::uint64_t> usedOrdinals;

    const auto forceOrder = ShuffleForceOrder(randomState);
    for (const auto category : forceOrder)
    {
        TryAppendCandidateRecord(builder, document, usedOrdinals, category, sizeCapBytes, randomState);
    }
    const auto extraAttempts = static_cast<std::size_t>(NextRandom(randomState) % 7ULL);
    for (std::size_t attemptIndex = 0;
         attemptIndex < extraAttempts && document.records.size() < 9U;
         attemptIndex++)
    {
        TryAppendCandidateRecord(
            builder,
            document,
            usedOrdinals,
            PickRandomCategory(randomState),
            sizeCapBytes,
            randomState);
    }

    const auto documentSpan = builder.GetDocument();
    document.bytes.assign(documentSpan.begin(), documentSpan.end());

    // Independent model cross-check: total bytes must equal the per-record
    // envelope sizes computed from the documented layout alone.
    std::uint64_t modelBytes = 0ULL;
    for (const auto& record : document.records)
    {
        modelBytes += kEnvelopeBytes + GeneratedBodyBytes(record);
    }
    FuzzFailure::Check(modelBytes == document.bytes.size(), "generator-size-model-mismatch");
    FuzzFailure::Check(VerifyEnvelopeFraming(document), "generator-framing-walk");

    // Differential CRC cross-check on the first record: the module's serializer
    // and the reference implementation must agree byte for byte.
    if (!document.records.empty())
    {
        const auto firstRecordEnd = document.recordBounds[1U];
        const std::span<const std::byte> coveredBytes(
            document.bytes.data(), firstRecordEnd - 4U);
        FuzzFailure::Check(
            pbprotocol::ComputeCrc32c(coveredBytes) == ReferenceCrc32c::Compute(coveredBytes),
            "generator-crc-differential");
    }
    return document;
}



} // namespace

namespace {

// The complete set of ProtocolErrorCodes LoadResumeState may report for input
// no larger than the policy budget. Every failure site in its call graph is
// covered by this list; length-overflow paths map to torn tails (success) and
// never surface as errors through the document API.
constexpr std::array<pbprotocol::ProtocolErrorCode, 11> kLoadAllowedFailureCodes{
    pbprotocol::ProtocolErrorCode::InvalidMagic,
    pbprotocol::ProtocolErrorCode::InvalidEnumValue,
    pbprotocol::ProtocolErrorCode::NonZeroReservedByte,
    pbprotocol::ProtocolErrorCode::CrcMismatch,
    pbprotocol::ProtocolErrorCode::TruncatedInput,
    pbprotocol::ProtocolErrorCode::TrailingBytes,
    pbprotocol::ProtocolErrorCode::InvalidRecordSize,
    pbprotocol::ProtocolErrorCode::ResourceLimitExceeded,
    pbprotocol::ProtocolErrorCode::SegmentOrdinalOutOfRange,
    pbprotocol::ProtocolErrorCode::ResumeRecordConflict,
    pbprotocol::ProtocolErrorCode::ResourceExhausted,
};

[[nodiscard]] bool IsAllowedFailureCode(const pbprotocol::ProtocolErrorCode code) noexcept
{
    return std::find(kLoadAllowedFailureCodes.begin(), kLoadAllowedFailureCodes.end(), code) !=
        kLoadAllowedFailureCodes.end();
}

// Generic fail-closed invariant for arbitrary input within the budget: either
// success or a documented error with an offset strictly inside the document.
[[nodiscard]] bool CheckAllowedOutcome(
    const pbprotocol::ProtocolResult<LoadedResumeState>& result,
    std::span<const std::byte> input,
    const char* caseLabel)
{
    if (result.HasValue())
    {
        return true; // No model exists for arbitrary success content; determinism is asserted by the caller.
    }
    const auto& actualError = result.Error();
    const bool codeAllowed = IsAllowedFailureCode(actualError.code);
    const bool offsetInRange = !input.empty() && actualError.offset < input.size();
    if (codeAllowed && offsetInRange)
    {
        return true;
    }
    EmitCheckDetail(caseLabel, "unexpected-outcome code=" +
        std::to_string(static_cast<unsigned>(actualError.code)) +
        " offset=" + std::to_string(actualError.offset) +
        " input-size=" + std::to_string(input.size()));
    return false;
}

// Canonical document: independent framing walk plus full load deep-equal to the
// generator model (all keys unique, no dedup expected).
[[nodiscard]] bool CheckCanonicalDocument(const GeneratedDocument& document)
{
    if (!VerifyEnvelopeFraming(document))
    {
        return false;
    }
    const auto loadResult = pbprotocol::LoadResumeState(
        AsConstSpan(document.bytes), MakeFuzzResourcePolicy());
    return CheckLoadSuccessExactState(
        std::move(loadResult),
        ExpectedStateForPrefix(document, document.records.size(), false),
        "canonical-load");
}

// Prefix truncation oracle: k bytes of a canonical document keep exactly the
// records whose end bound is at or before k; any leftover region flags a torn
// tail. Boundaries are known from generation bookkeeping, so off-by-one errors
// in the loader's envelope arithmetic fail this check deterministically.
[[nodiscard]] bool CheckTruncatedPrefix(
    const GeneratedDocument& document,
    const std::size_t truncationBytes)
{
    FuzzFailure::Check(truncationBytes <= document.bytes.size(), "truncation-out-of-range");

    std::size_t expectedCompleteRecords = 0;
    for (std::size_t boundIndex = 1U; boundIndex < document.recordBounds.size(); boundIndex++)
    {
        if (document.recordBounds[boundIndex] <= truncationBytes)
        {
            expectedCompleteRecords++;
        }
        else
        {
            break; // bounds are strictly increasing
        }
    }
    const bool expectTruncatedTail =
        truncationBytes != document.recordBounds[expectedCompleteRecords];

    const auto loadResult = pbprotocol::LoadResumeState(
        std::span<const std::byte>(document.bytes.data(), truncationBytes),
        MakeFuzzResourcePolicy());
    return CheckLoadSuccessExactState(
        std::move(loadResult),
        ExpectedStateForPrefix(document, expectedCompleteRecords, expectTruncatedTail),
        "truncated-prefix");
}

// Exact single-byte-flip oracle over a canonical document. Every position class
// has a deterministic expectation derived from the v1 envelope layout:
//   magic region            -> InvalidMagic at record start
//   version byte            -> InvalidEnumValue at record start + 4
//   reserved byte           -> NonZeroReservedByte at record start + 5
//   declared-length region  -> torn tail (success) when the new total overflows
//                              u64 or runs past EOF; otherwise CrcMismatch, since
//                              the flipped byte stays inside the CRC-covered prefix.
//                              A shifted stored-CRC window that coincidentally
//                              matches (2^-32) is decidable locally and falls back
//                              to the generic allowed-outcome invariant.
//   payload / stored-CRC    -> CrcMismatch at record end - 4 (a flipped covered
//                              byte always changes the computed CRC, and a flipped
//                              stored value can never equal it).
[[nodiscard]] bool CheckFlippedByte(
    const GeneratedDocument& document,
    const std::size_t flipPosition)
{
    FuzzFailure::Check(flipPosition < document.bytes.size(), "flip-position-out-of-range");

    std::size_t recordIndex = 0U;
    while (recordIndex + 1U < document.recordBounds.size() &&
        flipPosition >= document.recordBounds[recordIndex + 1U])
    {
        recordIndex++;
    }
    const auto recordStart = document.recordBounds[recordIndex];
    const auto recordEnd = document.recordBounds[recordIndex + 1U];
    const auto remainingBytes = document.bytes.size() - recordStart;

    std::vector<std::byte> mutatedBytes = document.bytes;
    mutatedBytes[flipPosition] ^= static_cast<std::byte>(static_cast<std::uint8_t>(1U << (flipPosition & 7U)));

    pbprotocol::ProtocolErrorCode expectedCode = pbprotocol::ProtocolErrorCode::None;
    std::size_t expectedOffset = 0U;
    LoadedResumeState expectedState{};

    if (flipPosition < recordStart + 4U)
    {
        expectedCode = pbprotocol::ProtocolErrorCode::InvalidMagic;
        expectedOffset = recordStart;
    }
    else if (flipPosition == recordStart + 4U)
    {
        expectedCode = pbprotocol::ProtocolErrorCode::InvalidEnumValue;
        expectedOffset = recordStart + 4U;
    }
    else if (flipPosition == recordStart + 5U)
    {
        expectedCode = pbprotocol::ProtocolErrorCode::NonZeroReservedByte;
        expectedOffset = recordStart + 5U;
    }
    else if (flipPosition < recordStart + kHeaderBytes)
    {
        const auto newDeclaredPayloadLength = ReadU64LE(mutatedBytes.data() + recordStart + 6);
        const bool totalOverflows = newDeclaredPayloadLength >
            std::numeric_limits<std::uint64_t>::max() - kEnvelopeBytes;
        std::uint64_t totalRecordBytes = 0ULL;
        if (!totalOverflows)
        {
            totalRecordBytes = newDeclaredPayloadLength + kEnvelopeBytes;
        }
        const bool tornTail = totalOverflows ||
            static_cast<std::uint64_t>(remainingBytes) < totalRecordBytes;

        const auto loadResult = pbprotocol::LoadResumeState(
            AsConstSpan(mutatedBytes), MakeFuzzResourcePolicy());
        if (tornTail)
        {
            expectedState = ExpectedStateForPrefix(document, recordIndex, true);
            return CheckLoadSuccessExactState(std::move(loadResult), expectedState, "flip-torn-tail");
        }

        const auto shiftedStoredCrc = ReadU32LE(mutatedBytes.data() + recordStart + totalRecordBytes - 4U);
        const auto computedCoveredCrc = ReferenceCrc32c::Compute(
            std::span<const std::byte>(mutatedBytes.data() + recordStart, totalRecordBytes - 4U));
        if (shiftedStoredCrc == computedCoveredCrc)
        {
            // Coincidental CRC match on the shifted window: parsing proceeds into a
            // typed body of arbitrary bytes; only the generic invariant applies.
            return CheckAllowedOutcome(loadResult, AsConstSpan(mutatedBytes), "flip-length-crc-coincidence");
        }
        expectedCode = pbprotocol::ProtocolErrorCode::CrcMismatch;
        expectedOffset = recordStart + 14U + static_cast<std::size_t>(newDeclaredPayloadLength);
    }
    else
    {
        expectedCode = pbprotocol::ProtocolErrorCode::CrcMismatch;
        expectedOffset = recordEnd - 4U;
    }

    const auto loadResult = pbprotocol::LoadResumeState(
        AsConstSpan(mutatedBytes), MakeFuzzResourcePolicy());
    return CheckLoadErrorExact(std::move(loadResult), expectedCode, expectedOffset, "flip-error-exact");
}

// Zero-byte tail oracle: tails below the envelope size are torn (success plus
// flag); 18 or more zero bytes form a complete envelope whose magic fails at
// the original document end.
[[nodiscard]] bool CheckZeroTail(const GeneratedDocument& document, const std::size_t zeroTailBytes)
{
    FuzzFailure::Check(zeroTailBytes <= kEnvelopeBytes + 5U, "zero-tail-range");

    std::vector<std::byte> paddedBytes(document.bytes.size() + zeroTailBytes);
    std::copy(document.bytes.begin(), document.bytes.end(), paddedBytes.begin());

    const auto loadResult = pbprotocol::LoadResumeState(
        AsConstSpan(paddedBytes), MakeFuzzResourcePolicy());
    if (zeroTailBytes < kEnvelopeBytes)
    {
        return CheckLoadSuccessExactState(
            std::move(loadResult),
            ExpectedStateForPrefix(document, document.records.size(), zeroTailBytes > 0U),
            "zero-tail-success");
    }
    return CheckLoadErrorExact(
        std::move(loadResult),
        pbprotocol::ProtocolErrorCode::InvalidMagic,
        document.bytes.size(),
        "zero-tail-magic");
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// Semantic hand-crafted documents. Every envelope below is framed with the
// local reference framing (independent of the module's serializer) and each
// predicate asserts one exact (code, offset) or success-plus-state expectation.
// ---------------------------------------------------------------------------
const std::array<std::byte, 16> kFixedSessionBytes = []()
{
    std::array<std::byte, 16> session{};
    for (auto& sessionByte : session)
    {
        sessionByte = std::byte{0x11};
    }
    return session;
}();

[[nodiscard]] std::vector<std::byte> MakeCompletedFrame(
    const std::span<const std::byte, 16>& sessionBytes,
    const std::uint64_t segmentOrdinal,
    const std::uint64_t rawOffset,
    const std::uint64_t rawSize)
{
    const auto body = MakeCompletedBody(sessionBytes, segmentOrdinal, rawOffset, rawSize);
    return FrameEnvelopePayload(AsConstSpan(body));
}

[[nodiscard]] std::vector<std::byte> MakeMinimalWirehairFrame(
    const std::uint64_t segmentOrdinal,
    const std::uint8_t profileFillByte)
{
    std::array<std::byte, 32> profile{};
    for (auto& profileByte : profile)
    {
        profileByte = static_cast<std::byte>(profileFillByte);
    }
    const auto body = MakeWirehairCacheBody(
        segmentOrdinal, std::span<const std::byte, 32>(profile), {});
    return FrameEnvelopePayload(AsConstSpan(body));
}

[[nodiscard]] std::vector<std::byte> MakeMinimalDirectFrame(const std::uint64_t segmentOrdinal)
{
    const auto body = MakeDirectRepeatBody(segmentOrdinal, 2U, {});
    return FrameEnvelopePayload(AsConstSpan(body));
}

void AppendFrame(std::vector<std::byte>& document, const std::vector<std::byte>& frameBytes)
{
    document.insert(document.end(), frameBytes.begin(), frameBytes.end());
}

// Builds the expected completed-record literal for a fixed-session frame so the
// model never depends on the module's own serialization.
[[nodiscard]] ResumeCompletedSegmentRecord MakeExpectedCompleted(
    const std::span<const std::byte, 16>& sessionBytes,
    const std::uint64_t segmentOrdinal,
    const std::uint64_t rawOffset,
    const std::uint64_t rawSize)
{
    ResumeCompletedSegmentRecord record{};
    for (std::size_t byteIndex = 0; byteIndex < sessionBytes.size(); byteIndex++)
    {
        record.sessionId.bytes[byteIndex] = sessionBytes[byteIndex];
    }
    record.segmentOrdinal = segmentOrdinal;
    record.rawOffset = rawOffset;
    record.rawSize = rawSize;
    for (auto& digestByte : record.rawDigest.bytes)
    {
        // MakeCompletedBody fills the digest region with 0xA5 bytes.
        digestByte = std::byte{0xA5};
    }
    return record;
}

[[nodiscard]] bool CheckSemanticEmptyDocument()
{
    const auto loadResult = pbprotocol::LoadResumeState(
        std::span<const std::byte>{}, MakeFuzzResourcePolicy());
    return CheckLoadSuccessExactState(std::move(loadResult), LoadedResumeState{}, "semantic-empty");
}

// Any input strictly above the budget fails with ResourceLimitExceeded at
// offset 0 before a single byte is interpreted.
[[nodiscard]] bool CheckOverBudgetInput(const std::size_t inputSize)
{
    if (inputSize <= kFuzzMaxResumeBytes)
    {
        EmitCheckDetail("over-budget", "input not over budget size=" + std::to_string(inputSize));
        return false;
    }
    std::vector<std::byte> zeros(inputSize, std::byte{});
    const auto loadResult = pbprotocol::LoadResumeState(
        AsConstSpan(zeros), MakeFuzzResourcePolicy());
    return CheckLoadErrorExact(
        std::move(loadResult), pbprotocol::ProtocolErrorCode::ResourceLimitExceeded, 0U, "over-budget");
}

// Exact budget-boundary document: ten completed records (91B each) plus one
// wirehair cache record whose two entries size the total to exactly
// kFuzzMaxResumeBytes. Body math: 45 + (8+17) + (8+18) = 96 -> envelope 114;
// 10 * 91 + 114 == 1024. A trailing zero byte then proves the gate rejects at
// size 1025 before parsing.
[[nodiscard]] bool CheckSemanticExactlyAtBudget()
{
    auto builderResult = pbprotocol::ResumeStateBuilder::Create(MakeFuzzResourcePolicy());
    if (!builderResult.HasValue())
    {
        EmitCheckDetail("exactly-at-budget", "builder-create failed");
        return false;
    }
    auto builder = std::move(builderResult).Value();

    LoadedResumeState expectedState;
    for (std::uint64_t completedIndex = 0ULL; completedIndex < 10ULL; completedIndex++)
    {
        ResumeCompletedSegmentRecord record{};
        for (auto& sessionByte : record.sessionId.bytes)
        {
            sessionByte = static_cast<std::byte>(static_cast<std::uint8_t>(completedIndex + 1U));
        }
        record.segmentOrdinal = completedIndex;
        record.rawOffset = completedIndex % 3ULL;
        record.rawSize = 1ULL + completedIndex;
        for (auto& digestByte : record.rawDigest.bytes)
        {
            digestByte = std::byte{0xA5}; // matches MakeExpectedCompleted's literal fill
        }

        const auto appendStatus = builder.AppendCompletedSegment(record);
        if (!appendStatus.HasValue())
        {
            EmitCheckDetail("exactly-at-budget", "append-completed failed index=" +
                std::to_string(completedIndex));
            return false;
        }
        expectedState.completedSegments.push_back(record);
    }

    ResumeActiveWirehairCacheRecord wirehairRecord{};
    wirehairRecord.segmentOrdinal = 10ULL;
    for (auto& profileByte : wirehairRecord.wirehairProfile.bytes)
    {
        profileByte = std::byte{0xA5};
    }
    ResumeWirehairCacheEntry entryOne{};
    entryOne.outerBlockId = 5U;
    entryOne.payload.assign(17U, std::byte{0x33});
    wirehairRecord.entries.push_back(entryOne);
    ResumeWirehairCacheEntry entryTwo{};
    entryTwo.outerBlockId = 6U;
    entryTwo.payload.assign(18U, std::byte{0x44});
    wirehairRecord.entries.push_back(entryTwo);

    const auto wirehairStatus = builder.AppendActiveWirehairCache(wirehairRecord);
    if (!wirehairStatus.HasValue())
    {
        EmitCheckDetail("exactly-at-budget", "append-wirehair failed");
        return false;
    }
    expectedState.activeWirehairCaches.push_back(wirehairRecord);

    if (builder.GetByteCount() != kFuzzMaxResumeBytes)
    {
        EmitCheckDetail("exactly-at-budget", "byte-count=" + std::to_string(builder.GetByteCount()));
        return false;
    }

    const auto documentSpan = builder.GetDocument();
    std::vector<std::byte> document(documentSpan.begin(), documentSpan.end());
    const auto loadResult = pbprotocol::LoadResumeState(
        AsConstSpan(document), MakeFuzzResourcePolicy());
    if (!CheckLoadSuccessExactState(std::move(loadResult), expectedState, "exactly-at-budget"))
    {
        return false;
    }

    document.push_back(std::byte{}); // 1025 bytes: quota gate must fire pre-parse.
    const auto overBudgetResult = pbprotocol::LoadResumeState(
        AsConstSpan(document), MakeFuzzResourcePolicy());
    return CheckLoadErrorExact(
        std::move(overBudgetResult),
        pbprotocol::ProtocolErrorCode::ResourceLimitExceeded,
        0U,
        "exactly-at-budget-plus-one");
}

// u64 length-overflow envelope: the document API classifies it as a torn tail
// (success plus flag) while the single-record API reports LengthOverflow at
// offset 0. A valid record followed by the same tail keeps the prefix and sets
// the flag.
[[nodiscard]] bool CheckSemanticLengthOverflow()
{
    std::vector<std::byte> overflowRecord;
    AppendFixedBytes(overflowRecord, kRecordMagic);
    overflowRecord.push_back(std::byte{1});
    overflowRecord.push_back(std::byte{0});
    AppendU64LE(overflowRecord, std::numeric_limits<std::uint64_t>::max());
    AppendU32LE(overflowRecord, 0U); // stored CRC irrelevant: torn classification precedes it.

    const auto documentResult = pbprotocol::LoadResumeState(
        AsConstSpan(overflowRecord), MakeFuzzResourcePolicy());
    LoadedResumeState expectedTorn;
    expectedTorn.hasTruncatedTail = true;
    if (!CheckLoadSuccessExactState(std::move(documentResult), expectedTorn, "length-overflow-torn"))
    {
        return false;
    }

    const auto parseResult = pbprotocol::ParseResumeRecord(
        AsConstSpan(overflowRecord), MakeFuzzResourcePolicy());
    if (parseResult.HasValue())
    {
        EmitCheckDetail("length-overflow-single-record", "expected failure got success");
        return false;
    }
    if (parseResult.Error().code != pbprotocol::ProtocolErrorCode::LengthOverflow ||
        parseResult.Error().offset != 0U)
    {
        EmitCheckDetail("length-overflow-single-record", "got code=" +
            std::to_string(static_cast<unsigned>(parseResult.Error().code)) +
            " offset=" + std::to_string(parseResult.Error().offset));
        return false;
    }

    std::vector<std::byte> midDocument = MakeCompletedFrame(kFixedSessionBytes, 42ULL, 0ULL, 7ULL);
    AppendFrame(midDocument, overflowRecord);
    LoadedResumeState expectedMid;
    expectedMid.completedSegments.push_back(
        MakeExpectedCompleted(kFixedSessionBytes, 42ULL, 0ULL, 7ULL));
    expectedMid.hasTruncatedTail = true;

    const auto midResult = pbprotocol::LoadResumeState(
        AsConstSpan(midDocument), MakeFuzzResourcePolicy());
    return CheckLoadSuccessExactState(std::move(midResult), expectedMid, "length-overflow-mid");
}

[[nodiscard]] bool CheckSemanticUnknownTag(const std::uint8_t tagByte)
{
    std::vector<std::byte> payload(65U, std::byte{0x7E});
    payload[0] = static_cast<std::byte>(tagByte);
    const auto frame = FrameEnvelopePayload(AsConstSpan(payload));
    const auto loadResult = pbprotocol::LoadResumeState(
        AsConstSpan(frame), MakeFuzzResourcePolicy());
    return CheckLoadErrorExact(
        std::move(loadResult),
        pbprotocol::ProtocolErrorCode::InvalidEnumValue,
        kHeaderBytes, // body tag byte sits at envelope header end.
        "unknown-tag");
}

[[nodiscard]] bool CheckSemanticEmptyPayload()
{
    const auto frame = FrameEnvelopePayload(std::span<const std::byte>{});
    const auto loadResult = pbprotocol::LoadResumeState(
        AsConstSpan(frame), MakeFuzzResourcePolicy());
    return CheckLoadErrorExact(
        std::move(loadResult),
        pbprotocol::ProtocolErrorCode::InvalidRecordSize,
        kHeaderBytes,
        "empty-payload");
}

[[nodiscard]] bool CheckSemanticBadMagic()
{
    auto frame = MakeCompletedFrame(kFixedSessionBytes, 42ULL, 0ULL, 7ULL);
    frame[0] = std::byte{'X'}; // magic check precedes CRC: no repair needed.
    const auto loadResult = pbprotocol::LoadResumeState(
        AsConstSpan(frame), MakeFuzzResourcePolicy());
    return CheckLoadErrorExact(
        std::move(loadResult), pbprotocol::ProtocolErrorCode::InvalidMagic, 0U, "bad-magic");
}

[[nodiscard]] bool CheckSemanticBadVersion()
{
    auto frame = MakeCompletedFrame(kFixedSessionBytes, 42ULL, 0ULL, 7ULL);
    frame[4] = std::byte{0x02}; // version check precedes reserved and CRC.
    RefreshFrameCrc(frame);
    const auto loadResult = pbprotocol::LoadResumeState(
        AsConstSpan(frame), MakeFuzzResourcePolicy());
    return CheckLoadErrorExact(
        std::move(loadResult),
        pbprotocol::ProtocolErrorCode::InvalidEnumValue,
        4U,
        "bad-version");
}

[[nodiscard]] bool CheckSemanticNonZeroReserved()
{
    auto frame = MakeCompletedFrame(kFixedSessionBytes, 42ULL, 0ULL, 7ULL);
    frame[5] = std::byte{1}; // reserved check precedes CRC.
    RefreshFrameCrc(frame);
    const auto loadResult = pbprotocol::LoadResumeState(
        AsConstSpan(frame), MakeFuzzResourcePolicy());
    return CheckLoadErrorExact(
        std::move(loadResult),
        pbprotocol::ProtocolErrorCode::NonZeroReservedByte,
        5U,
        "non-zero-reserved");
}

// Fixed two-record document (completed 91B + minimal wirehair 63B = 154B) with
// independently hand-derived flip expectations: these pin the CheckFlippedByte
// oracle's own boundary arithmetic against literal values.
[[nodiscard]] bool CheckSemanticFixedDocFlipSpot()
{
    const std::vector<std::byte> firstRecord = MakeCompletedFrame(kFixedSessionBytes, 42ULL, 0ULL, 7ULL);
    const std::vector<std::byte> secondRecord = MakeMinimalWirehairFrame(7ULL, 0xA5U);
    if (firstRecord.size() != kEnvelopeBytes + kCompletedBodyBytes ||
        secondRecord.size() != kEnvelopeBytes + kWirehairHeaderBodyBytes)
    {
        EmitCheckDetail("fixed-doc-sizes", "framing mismatch first=" +
            std::to_string(firstRecord.size()) + " second=" + std::to_string(secondRecord.size()));
        return false;
    }

    std::vector<std::byte> document = firstRecord;
    AppendFrame(document, secondRecord);

    LoadedResumeState expectedState;
    expectedState.completedSegments.push_back(
        MakeExpectedCompleted(kFixedSessionBytes, 42ULL, 0ULL, 7ULL));
    ResumeActiveWirehairCacheRecord wirehair{};
    wirehair.segmentOrdinal = 7ULL;
    for (auto& profileByte : wirehair.wirehairProfile.bytes)
    {
        profileByte = std::byte{0xA5}; // matches MakeMinimalWirehairFrame fill.
    }
    expectedState.activeWirehairCaches.push_back(wirehair);

    const auto canonicalResult = pbprotocol::LoadResumeState(
        AsConstSpan(document), MakeFuzzResourcePolicy());
    if (!CheckLoadSuccessExactState(std::move(canonicalResult), expectedState, "fixed-doc-canonical"))
    {
        return false;
    }

    const auto flipAndExpect = [&document](
        const std::size_t flipPosition,
        const pbprotocol::ProtocolErrorCode code,
        const std::size_t offset,
        const char* subLabel) -> bool
    {
        std::vector<std::byte> mutatedBytes = document;
        mutatedBytes[flipPosition] ^= std::byte{1}; // single bit: outcome class is bit-independent.
        return CheckLoadErrorExact(
            pbprotocol::LoadResumeState(AsConstSpan(mutatedBytes), MakeFuzzResourcePolicy()),
            code, offset, subLabel);
    };

    if (!flipAndExpect(0U, pbprotocol::ProtocolErrorCode::InvalidMagic, 0U, "fixed-doc-flip-magic"))
    {
        return false;
    }
    if (!flipAndExpect(4U, pbprotocol::ProtocolErrorCode::InvalidEnumValue, 4U, "fixed-doc-flip-version"))
    {
        return false;
    }
    if (!flipAndExpect(5U, pbprotocol::ProtocolErrorCode::NonZeroReservedByte, 5U, "fixed-doc-flip-reserved"))
    {
        return false;
    }
    if (!flipAndExpect(firstRecord.size() + 4U, pbprotocol::ProtocolErrorCode::InvalidEnumValue, firstRecord.size() + 4U, "fixed-doc-flip-second-version"))
    {
        return false;
    }
    if (!flipAndExpect(document.size() - 1U, pbprotocol::ProtocolErrorCode::CrcMismatch, document.size() - 4U, "fixed-doc-flip-stored-crc"))
    {
        return false;
    }
    return true;
}

} // namespace

namespace {

[[nodiscard]] bool CheckSemanticCrossTypeConflicts()
{
    // completed(7) + wirehair(7): conflict at the second record's start (91).
    std::vector<std::byte> completedWirehairDoc = MakeCompletedFrame(kFixedSessionBytes, 7ULL, 0ULL, 3ULL);
    AppendFrame(completedWirehairDoc, MakeMinimalWirehairFrame(7ULL, 0xA5U));
    if (!CheckLoadErrorExact(
            pbprotocol::LoadResumeState(AsConstSpan(completedWirehairDoc), MakeFuzzResourcePolicy()),
            pbprotocol::ProtocolErrorCode::ResumeRecordConflict,
            kEnvelopeBytes + kCompletedBodyBytes, // 91.
            "cross-type-completed-wirehair"))
    {
        return false;
    }

    // wirehair(9) + direct(9): conflict at the second record's start (63).
    std::vector<std::byte> wirehairDirectDoc = MakeMinimalWirehairFrame(9ULL, 0xB3U);
    AppendFrame(wirehairDirectDoc, MakeMinimalDirectFrame(9ULL));
    if (!CheckLoadErrorExact(
            pbprotocol::LoadResumeState(AsConstSpan(wirehairDirectDoc), MakeFuzzResourcePolicy()),
            pbprotocol::ProtocolErrorCode::ResumeRecordConflict,
            kEnvelopeBytes + kWirehairHeaderBodyBytes, // 63.
            "cross-type-wirehair-direct"))
    {
        return false;
    }

    // direct(11) + completed(11): conflict at the second record's start (35).
    std::vector<std::byte> directCompletedDoc = MakeMinimalDirectFrame(11ULL);
    AppendFrame(directCompletedDoc, MakeCompletedFrame(kFixedSessionBytes, 11ULL, 0ULL, 2ULL));
    if (!CheckLoadErrorExact(
            pbprotocol::LoadResumeState(AsConstSpan(directCompletedDoc), MakeFuzzResourcePolicy()),
            pbprotocol::ProtocolErrorCode::ResumeRecordConflict,
            kEnvelopeBytes + kDirectHeaderBodyBytes, // 35.
            "cross-type-direct-completed"))
    {
        return false;
    }

    // Same ordinal in two completed records with different sessions: distinct
    // keys (sessionId + ordinal), so both are kept, in document order.
    const std::array<std::byte, 16> secondSession = []()
    {
        std::array<std::byte, 16> session{};
        for (auto& sessionByte : session)
        {
            sessionByte = std::byte{0x77};
        }
        return session;
    }();
    std::vector<std::byte> sameOrdinalDoc = MakeCompletedFrame(kFixedSessionBytes, 13ULL, 0ULL, 2ULL);
    AppendFrame(sameOrdinalDoc, MakeCompletedFrame(secondSession, 13ULL, 1ULL, 4ULL));

    LoadedResumeState expectedState;
    expectedState.completedSegments.push_back(MakeExpectedCompleted(kFixedSessionBytes, 13ULL, 0ULL, 2ULL));
    expectedState.completedSegments.push_back(MakeExpectedCompleted(secondSession, 13ULL, 1ULL, 4ULL));
    return CheckLoadSuccessExactState(
        pbprotocol::LoadResumeState(AsConstSpan(sameOrdinalDoc), MakeFuzzResourcePolicy()),
        expectedState,
        "completed-same-ordinal-different-session");
}

[[nodiscard]] bool CheckSemanticCompletedDuplicateAndConflict()
{
    const std::vector<std::byte> recordFrame = MakeCompletedFrame(kFixedSessionBytes, 42ULL, 0ULL, 7ULL);
    if (recordFrame.size() != kEnvelopeBytes + kCompletedBodyBytes)
    {
        EmitCheckDetail("completed-dedup-size", "framing mismatch size=" + std::to_string(recordFrame.size()));
        return false;
    }

    // Byte-identical duplicate: load dedupes to a single record.
    std::vector<std::byte> duplicateDoc = recordFrame;
    AppendFrame(duplicateDoc, recordFrame);
    LoadedResumeState expectedSingle;
    expectedSingle.completedSegments.push_back(MakeExpectedCompleted(kFixedSessionBytes, 42ULL, 0ULL, 7ULL));
    if (!CheckLoadSuccessExactState(
            pbprotocol::LoadResumeState(AsConstSpan(duplicateDoc), MakeFuzzResourcePolicy()),
            expectedSingle,
            "completed-identical-dedup"))
    {
        return false;
    }

    // Same key (session + ordinal) with different content: whole-document
    // conflict at the second record's start.
    const std::vector<std::byte> conflictingFrame = MakeCompletedFrame(kFixedSessionBytes, 42ULL, 0ULL, 8ULL);
    std::vector<std::byte> conflictDoc = recordFrame;
    AppendFrame(conflictDoc, conflictingFrame);
    return CheckLoadErrorExact(
        pbprotocol::LoadResumeState(AsConstSpan(conflictDoc), MakeFuzzResourcePolicy()),
        pbprotocol::ProtocolErrorCode::ResumeRecordConflict,
        recordFrame.size(),
        "completed-same-key-conflict");
}

[[nodiscard]] bool CheckSemanticWirehairEntryCollisions()
{
    std::array<std::byte, 32> profile{};
    for (auto& profileByte : profile)
    {
        profileByte = std::byte{0xB3};
    }
    const auto profileSpan = std::span<const std::byte, 32>(profile);

    std::vector<std::byte> payloadA(5U, std::byte{0x11});
    std::vector<std::byte> payloadB(5U, std::byte{0x22});
    const auto spanA = AsConstSpan(payloadA);
    const auto spanB = AsConstSpan(payloadB);

    // Same outerBlockId with different payloads. The id-scan conflict is
    // synthesized inside the body parser after full consumption, so its
    // document offset points at the record's body base (the 14-byte envelope
    // header), per LoadResumeState's documented offset semantics.
    {
        std::vector<std::pair<std::uint32_t, std::span<const std::byte>>> conflictEntries;
        conflictEntries.emplace_back(3U, spanA);
        conflictEntries.emplace_back(3U, spanB);
        const auto body = MakeWirehairCacheBody(7ULL, profileSpan, conflictEntries);
        const auto frame = FrameEnvelopePayload(AsConstSpan(body));
        if (!CheckLoadErrorExact(
                pbprotocol::LoadResumeState(AsConstSpan(frame), MakeFuzzResourcePolicy()),
                pbprotocol::ProtocolErrorCode::ResumeRecordConflict,
                kHeaderBytes,
                "wirehair-entry-conflict"))
        {
            return false;
        }
    }

    // Same outerBlockId with identical payloads: deduped to one entry.
    {
        std::vector<std::pair<std::uint32_t, std::span<const std::byte>>> duplicateEntries;
        duplicateEntries.emplace_back(3U, spanA);
        duplicateEntries.emplace_back(3U, spanA);
        const auto body = MakeWirehairCacheBody(7ULL, profileSpan, duplicateEntries);
        const auto frame = FrameEnvelopePayload(AsConstSpan(body));

        LoadedResumeState expectedState;
        ResumeActiveWirehairCacheRecord wirehair{};
        wirehair.segmentOrdinal = 7ULL;
        for (std::size_t byteIndex = 0; byteIndex < profileSpan.size(); byteIndex++)
        {
            wirehair.wirehairProfile.bytes[byteIndex] = profileSpan[byteIndex];
        }
        ResumeWirehairCacheEntry expectedEntry{};
        expectedEntry.outerBlockId = 3U;
        expectedEntry.payload = payloadA;
        wirehair.entries.push_back(std::move(expectedEntry));
        expectedState.activeWirehairCaches.push_back(std::move(wirehair));

        return CheckLoadSuccessExactState(
            pbprotocol::LoadResumeState(AsConstSpan(frame), MakeFuzzResourcePolicy()),
            expectedState,
            "wirehair-entry-dedup");
    }
}

[[nodiscard]] bool CheckSemanticDirectEntryCollisions()
{
    const std::uint32_t directBlockCount = 4U;
    std::vector<std::byte> paddedP(6U, std::byte{0x55});
    std::vector<std::byte> paddedQ(6U, std::byte{0x66});
    const auto spanP = AsConstSpan(paddedP);
    const auto spanQ = AsConstSpan(paddedQ);

    // Same blockOrdinal with different content: body-scan conflict points at
    // the single record's body base (see wirehair-entry-conflict above).
    {
        std::vector<std::tuple<std::uint32_t, std::uint32_t, std::span<const std::byte>>> conflictEntries;
        conflictEntries.emplace_back(1U, 3U, spanP);
        conflictEntries.emplace_back(1U, 3U, spanQ);
        const auto body = MakeDirectRepeatBody(7ULL, directBlockCount, conflictEntries);
        const auto frame = FrameEnvelopePayload(AsConstSpan(body));
        if (!CheckLoadErrorExact(
                pbprotocol::LoadResumeState(AsConstSpan(frame), MakeFuzzResourcePolicy()),
                pbprotocol::ProtocolErrorCode::ResumeRecordConflict,
                kHeaderBytes,
                "direct-entry-conflict"))
        {
            return false;
        }
    }

    // Identical entries: deduped to one entry.
    {
        std::vector<std::tuple<std::uint32_t, std::uint32_t, std::span<const std::byte>>> duplicateEntries;
        duplicateEntries.emplace_back(1U, 3U, spanP);
        duplicateEntries.emplace_back(1U, 3U, spanP);
        const auto body = MakeDirectRepeatBody(7ULL, directBlockCount, duplicateEntries);
        const auto frame = FrameEnvelopePayload(AsConstSpan(body));

        LoadedResumeState expectedState;
        ResumeActiveDirectRepeatRecord direct{};
        direct.segmentOrdinal = 7ULL;
        direct.directBlockCount = directBlockCount;
        ResumeDirectRepeatEntry expectedEntry{};
        expectedEntry.blockOrdinal = 1U;
        expectedEntry.realPayloadBytes = 3U;
        expectedEntry.paddedPayload = paddedP;
        direct.entries.push_back(std::move(expectedEntry));
        expectedState.activeDirectRepeatRecords.push_back(std::move(direct));

        return CheckLoadSuccessExactState(
            pbprotocol::LoadResumeState(AsConstSpan(frame), MakeFuzzResourcePolicy()),
            expectedState,
            "direct-entry-dedup");
    }
}

// Entry-count allocation guard: the declared count must fit its minimum byte
// requirement before any heap is touched (body = header + a few junk bytes).
[[nodiscard]] bool CheckSemanticWirehairEntryCountGuard()
{
    std::vector<std::byte> body;
    body.push_back(static_cast<std::byte>(pbprotocol::ResumeRecordType::ActiveWirehairCache));
    AppendU64LE(body, 3ULL);
    body.resize(kWirehairHeaderBodyBytes - 4U, std::byte{0xC7}); // ordinal+profile region.
    AppendU32LE(body, 3U); // entryCount=3 but only four bytes follow: 3 * 8 > 4.
    body.insert(body.end(), 4U, std::byte{0xEE});

    const auto frame = FrameEnvelopePayload(AsConstSpan(body));
    return CheckLoadErrorExact(
        pbprotocol::LoadResumeState(AsConstSpan(frame), MakeFuzzResourcePolicy()),
        pbprotocol::ProtocolErrorCode::TruncatedInput,
        kHeaderBytes + kWirehairEntryCountBodyOffset, // 55.
        "wirehair-entry-count-guard");
}

[[nodiscard]] bool CheckSemanticDirectEntryCountGuard()
{
    std::vector<std::byte> body;
    body.push_back(static_cast<std::byte>(pbprotocol::ResumeRecordType::DirectRepeatReceivedBlocks));
    AppendU64LE(body, 3ULL); // segmentOrdinal.
    AppendU32LE(body, 5U);   // directBlockCount (valid: <= policy max).
    AppendU32LE(body, 2U);   // entryCount=2 but only six bytes follow: 2 * 12 > 6.
    body.insert(body.end(), 6U, std::byte{0xEE});

    const auto frame = FrameEnvelopePayload(AsConstSpan(body));
    return CheckLoadErrorExact(
        pbprotocol::LoadResumeState(AsConstSpan(frame), MakeFuzzResourcePolicy()),
        pbprotocol::ProtocolErrorCode::TruncatedInput,
        kHeaderBytes + kDirectEntryCountBodyOffset, // 27.
        "direct-entry-count-guard");
}

[[nodiscard]] bool CheckSemanticDirectBlockOrdinalRange()
{
    std::vector<std::byte> body;
    body.push_back(static_cast<std::byte>(pbprotocol::ResumeRecordType::DirectRepeatReceivedBlocks));
    AppendU64LE(body, 0ULL); // segmentOrdinal.
    AppendU32LE(body, 5U);   // directBlockCount.
    AppendU32LE(body, 1U);   // entryCount (guard passes: remaining 20 >= 12).
    AppendU32LE(body, 5U);   // blockOrdinal == directBlockCount: out of range.
    AppendU32LE(body, 4U);   // realPayloadBytes.
    AppendU32LE(body, 8U);   // paddedLen.
    body.insert(body.end(), 8U, std::byte{0x9C});

    const auto frame = FrameEnvelopePayload(AsConstSpan(body));
    return CheckLoadErrorExact(
        pbprotocol::LoadResumeState(AsConstSpan(frame), MakeFuzzResourcePolicy()),
        pbprotocol::ProtocolErrorCode::SegmentOrdinalOutOfRange,
        kHeaderBytes + kDirectFirstBlockOrdinalBodyOffset, // 31.
        "direct-block-ordinal-range");
}

[[nodiscard]] bool CheckSemanticDirectRealExceedsPadded()
{
    // realPayloadBytes > paddedLen: field-consistency failure at the real field.
    {
        std::vector<std::byte> body;
        body.push_back(static_cast<std::byte>(pbprotocol::ResumeRecordType::DirectRepeatReceivedBlocks));
        AppendU64LE(body, 0ULL);
        AppendU32LE(body, 5U); // directBlockCount.
        AppendU32LE(body, 1U); // entryCount (guard passes: remaining 16 >= 12).
        AppendU32LE(body, 0U); // blockOrdinal.
        AppendU32LE(body, 8U); // realPayloadBytes.
        AppendU32LE(body, 4U); // paddedLen < real: invalid record size.
        body.insert(body.end(), 4U, std::byte{0x9D});

        const auto frame = FrameEnvelopePayload(AsConstSpan(body));
        if (!CheckLoadErrorExact(
                pbprotocol::LoadResumeState(AsConstSpan(frame), MakeFuzzResourcePolicy()),
                pbprotocol::ProtocolErrorCode::InvalidRecordSize,
                kHeaderBytes + kDirectRealPayloadBytesBodyOffset, // 35.
                "direct-real-exceeds-padded"))
        {
            return false;
        }
    }

    // Zero padded region: same field-consistency gate at the same offset.
    std::vector<std::byte> body;
    body.push_back(static_cast<std::byte>(pbprotocol::ResumeRecordType::DirectRepeatReceivedBlocks));
    AppendU64LE(body, 0ULL);
    AppendU32LE(body, 5U);
    AppendU32LE(body, 1U); // guard passes: remaining 12 >= 12.
    AppendU32LE(body, 0U); // blockOrdinal.
    AppendU32LE(body, 3U); // realPayloadBytes.
    AppendU32LE(body, 0U); // paddedLen == 0: zero region.

    const auto frame = FrameEnvelopePayload(AsConstSpan(body));
    return CheckLoadErrorExact(
        pbprotocol::LoadResumeState(AsConstSpan(frame), MakeFuzzResourcePolicy()),
        pbprotocol::ProtocolErrorCode::InvalidRecordSize,
        kHeaderBytes + kDirectRealPayloadBytesBodyOffset, // 35.
        "direct-zero-padded-region");
}

[[nodiscard]] bool CheckSemanticWirehairPayloadLenBounds()
{
    // Zero-length payload: the entry-count guard passes (8 <= remaining 8) and
    // the size gate fails at the length field itself.
    {
        std::vector<std::byte> body;
        body.push_back(static_cast<std::byte>(pbprotocol::ResumeRecordType::ActiveWirehairCache));
        AppendU64LE(body, 4ULL);
            body.resize(kWirehairHeaderBodyBytes - 4U, std::byte{0xD9}); // ordinal+profile region.
        AppendU32LE(body, 1U); // entryCount (guard passes: remaining 8 >= 8).
        AppendU32LE(body, 7U); // outerBlockId.
        AppendU32LE(body, 0U); // payloadLen == 0: invalid record size at this field.

        const auto frame = FrameEnvelopePayload(AsConstSpan(body));
        if (!CheckLoadErrorExact(
                pbprotocol::LoadResumeState(AsConstSpan(frame), MakeFuzzResourcePolicy()),
                pbprotocol::ProtocolErrorCode::InvalidRecordSize,
                kHeaderBytes + kWirehairFirstPayloadLenBodyOffset, // 63.
                "wirehair-zero-payload-len"))
        {
            return false;
        }
    }

    // Over-budget payload length (33 > maxOuterBlockBytes=32): the policy gate
    // fires before any payload byte is read or allocated.
    std::vector<std::byte> body;
    body.push_back(static_cast<std::byte>(pbprotocol::ResumeRecordType::ActiveWirehairCache));
    AppendU64LE(body, 4ULL);
    body.resize(kWirehairHeaderBodyBytes - 4U, std::byte{0xD9}); // ordinal+profile region.
    AppendU32LE(body, 1U); // entryCount (guard passes: remaining 41 >= 8).
    AppendU32LE(body, 7U); // outerBlockId.
    AppendU32LE(body, 33U); // payloadLen over the policy bound.
    body.insert(body.end(), 33U, std::byte{0x15});

    const auto frame = FrameEnvelopePayload(AsConstSpan(body));
    return CheckLoadErrorExact(
        pbprotocol::LoadResumeState(AsConstSpan(frame), MakeFuzzResourcePolicy()),
        pbprotocol::ProtocolErrorCode::ResourceLimitExceeded,
        kHeaderBytes + kWirehairFirstPayloadLenBodyOffset, // 63.
        "wirehair-payload-over-budget");
}

} // namespace

namespace {

// Generic fail-closed invariant for arbitrary input: over-budget documents are
// rejected before parsing with an exact (code, offset); in-budget inputs must
// be deterministic across two loads and resolve to success or a documented
// error code with an in-range offset.
void ExerciseInput(std::span<const std::byte> input)
{
    const auto resourcePolicy = MakeFuzzResourcePolicy();

    if (input.size() > kFuzzMaxResumeBytes)
    {
        // The whole-document quota gate runs before any byte is interpreted, so
        // the outcome is exact and no parse state exists to compare.
        const auto overBudgetResult = pbprotocol::LoadResumeState(input, resourcePolicy);
        FuzzFailure::Check(
            CheckLoadErrorExact(
                std::move(overBudgetResult),
                pbprotocol::ProtocolErrorCode::ResourceLimitExceeded,
                0U,
                "exercise-over-budget"),
            "exercise-over-budget");
        return;
    }

    auto firstResult = pbprotocol::LoadResumeState(input, resourcePolicy);
    const auto secondResult = pbprotocol::LoadResumeState(input, resourcePolicy);

    bool resultsMatch = false;
    if (firstResult.HasValue() && secondResult.HasValue())
    {
        resultsMatch = firstResult.Value() == secondResult.Value();
    }
    else if (!firstResult.HasValue() && !secondResult.HasValue())
    {
        resultsMatch = firstResult.Error() == secondResult.Error();
    }
    FuzzFailure::Check(resultsMatch, "exercise-determinism");

    FuzzFailure::Check(
        CheckAllowedOutcome(firstResult, input, "exercise-allowed-outcome"),
        "exercise-allowed-outcome");
}

// Deterministic semantic case table shared by the mutation loop (index-driven)
// and documented per-case in the structured self-test below.
constexpr std::size_t kSemanticCaseCount = 22U;

[[nodiscard]] bool RunSemanticCaseByIndex(const std::size_t caseIndex)
{
    switch (caseIndex)
    {
    case 0U: return CheckSemanticEmptyDocument();
    case 1U: return CheckOverBudgetInput(kFuzzMaxResumeBytes + 1U);
    case 2U: return CheckOverBudgetInput(1112U); // corpus quota-document seed size.
    case 3U: return CheckSemanticExactlyAtBudget();
    case 4U: return CheckSemanticLengthOverflow();
    case 5U: return CheckSemanticUnknownTag(0U);
    case 6U: return CheckSemanticUnknownTag(4U);
    case 7U: return CheckSemanticUnknownTag(255U);
    case 8U: return CheckSemanticEmptyPayload();
    case 9U: return CheckSemanticBadMagic();
    case 10U: return CheckSemanticBadVersion();
    case 11U: return CheckSemanticNonZeroReserved();
    case 12U: return CheckSemanticFixedDocFlipSpot();
    case 13U: return CheckSemanticCrossTypeConflicts();
    case 14U: return CheckSemanticCompletedDuplicateAndConflict();
    case 15U: return CheckSemanticWirehairEntryCollisions();
    case 16U: return CheckSemanticDirectEntryCollisions();
    case 17U: return CheckSemanticWirehairEntryCountGuard();
    case 18U: return CheckSemanticDirectEntryCountGuard();
    case 19U: return CheckSemanticDirectBlockOrdinalRange();
    case 20U: return CheckSemanticDirectRealExceedsPadded();
    case 21U: return CheckSemanticWirehairPayloadLenBounds();
    default:
        FuzzFailure::Check(false, "semantic-case-index");
        return false;
    }
}

} // namespace

#if !defined(PB_USE_LIBFUZZER)

namespace {

bool RequireSelfTest(const bool condition, const std::string_view caseLabel)
{
    if (!condition)
    {
        std::cerr << "STRUCTURED_SELF_TEST_FAILED case=" << caseLabel << '\n';
    }
    return condition;
}

int RunStructuredSelfTest()
{
    // CRC reference cross-check: known answer vector plus a differential run of
    // the local table implementation against pbprotocol::ComputeCrc32c.
    {
        // The literal has array type const char[10]; a range-for over it would
        // include the trailing NUL, so pin exactly nine digits explicitly.
        constexpr std::array<char, 9> kKatDigits{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        std::vector<std::byte> katVector;
        for (const char digit : kKatDigits)
        {
            katVector.push_back(static_cast<std::byte>(static_cast<unsigned char>(digit)));
        }
        const auto katSpan = AsConstSpan(katVector);
        bool crcChecksOk = ReferenceCrc32c::Compute(katSpan) == 0xE3069283U &&
            pbprotocol::ComputeCrc32c(katSpan) == 0xE3069283U;

        std::uint64_t differentialSeed = 0xC0FFEE0B3DULL;
        constexpr std::array<std::size_t, 6> differentialBufferSizes{0U, 1U, 7U, 64U, 511U, 1024U};
        for (const auto bufferBytes : differentialBufferSizes)
        {
            std::vector<std::byte> differentialBuffer(bufferBytes);
            FillRandomBytes(differentialBuffer, differentialSeed);
            const auto bufferSpan = AsConstSpan(differentialBuffer);
            crcChecksOk = crcChecksOk &&
                ReferenceCrc32c::Compute(bufferSpan) == pbprotocol::ComputeCrc32c(bufferSpan);
        }
        if (!RequireSelfTest(crcChecksOk, "crc-reference-kat-and-differential"))
        {
            return 1;
        }
    }

    for (std::size_t caseIndex = 0U; caseIndex < kSemanticCaseCount; caseIndex++)
    {
        if (!RequireSelfTest(RunSemanticCaseByIndex(caseIndex), "semantic-case-" + std::to_string(caseIndex)))
        {
            return 1;
        }
    }

    // Exhaustive torn-tail sweep: every prefix length of a pinned generated
    // document is checked against the boundary model (off-by-one pin).
    {
        std::uint64_t sweepSeed = 0xC0FFEE0B3DULL;
        const GeneratedDocument sweepDocument = GenerateResumeDocument(700U, sweepSeed);
        if (!RequireSelfTest(CheckCanonicalDocument(sweepDocument), "sweep-doc-canonical"))
        {
            return 1;
        }
        for (std::size_t prefixBytes = 0U; prefixBytes <= sweepDocument.bytes.size(); prefixBytes++)
        {
            if (!RequireSelfTest(
                    CheckTruncatedPrefix(sweepDocument, prefixBytes),
                    "torn-tail-sweep-k-" + std::to_string(prefixBytes)))
            {
                return 1;
            }
        }
        // Past-EOF tails: zero bytes up to the magic gate.
        for (std::size_t tailBytes = 1U; tailBytes <= kEnvelopeBytes + 5U; tailBytes++)
        {
            if (!RequireSelfTest(
                    CheckZeroTail(sweepDocument, tailBytes),
                    "zero-tail-exhaustive-n-" + std::to_string(tailBytes)))
            {
                return 1;
            }
        }
    }

    // Exhaustive single-byte-flip sweep over a small generated document: every
    // position class (magic/version/reserved/length/payload/CRC, per record) is
    // exercised with its exact expectation.
    {
        std::uint64_t flipSeed = 0xD1F1A5EE0BULL;
        const GeneratedDocument flipDocument = GenerateResumeDocument(320U, flipSeed);
        if (!RequireSelfTest(CheckCanonicalDocument(flipDocument), "flip-doc-canonical"))
        {
            return 1;
        }
        for (std::size_t flipPosition = 0U; flipPosition < flipDocument.bytes.size(); flipPosition++)
        {
            if (!RequireSelfTest(
                    CheckFlippedByte(flipDocument, flipPosition),
                    "flip-exhaustive-j-" + std::to_string(flipPosition)))
            {
                return 1;
            }
        }
    }

    std::cout << "STRUCTURED_SELF_TEST_COMPLETED\n";
    return 0;
}

int RunMutationLoop(const std::uint64_t iterations, const std::uint64_t initialSeed)
{
    std::uint64_t randomState = initialSeed == 0 ? kDefaultSeed : initialSeed;

    for (std::uint64_t iteration = 0ULL; iteration < iterations; iteration++)
    {
        const auto classIndex = static_cast<std::size_t>(NextRandom(randomState) % 7ULL);
        switch (classIndex)
        {
        case 0U: // Raw garbage: generic fail-closed invariants only.
        {
            std::vector<std::byte> input;
            if (NextRandom(randomState) % 8ULL == 0ULL)
            {
                const auto overSize = kFuzzMaxResumeBytes + 1U +
                    static_cast<std::size_t>(NextRandom(randomState) % 64U);
                input.assign(overSize, std::byte{});
            }
            else
            {
                const auto inputSize = static_cast<std::size_t>(
                    NextRandom(randomState) % (kFuzzMaxResumeBytes + 1ULL));
                input.resize(inputSize);
                FillRandomBytes(AsMutSpan(input), randomState);
            }
            ExerciseInput(AsConstSpan(input));
            break;
        }
        case 1U: // Canonical round trip plus periodic byte-determinism re-generation.
        {
            const std::uint64_t stateBeforeGeneration = randomState;
            GeneratedDocument document = GenerateResumeDocument(kFuzzMaxResumeBytes, randomState);
            FuzzFailure::Check(CheckCanonicalDocument(document), "canonical-document");

            if (NextRandom(randomState) % 3ULL == 0ULL)
            {
                // Same input state must yield byte-identical documents.
                std::uint64_t regenerationState = stateBeforeGeneration;
                const GeneratedDocument regeneratedDocument = GenerateResumeDocument(
                    kFuzzMaxResumeBytes, regenerationState);
                FuzzFailure::Check(
                    regeneratedDocument.bytes == document.bytes &&
                        regeneratedDocument.records.size() == document.records.size(),
                    "generator-byte-determinism");
            }
            break;
        }
        case 2U: // Prefix truncation with record-boundary bias.
        {
            GeneratedDocument document = GenerateResumeDocument(kFuzzMaxResumeBytes, randomState);
            if (document.records.empty())
            {
                break;
            }
            const auto totalBytes = document.bytes.size();
            std::size_t truncationBytes = 0U;
            const auto boundaryCount = document.recordBounds.size(); // includes the leading 0
            const auto modeIndex = NextRandom(randomState) % 10ULL;
            if (modeIndex < 5ULL)
            {
                truncationBytes = document.recordBounds[static_cast<std::size_t>(NextRandom(randomState) % boundaryCount)];
            }
            else if (modeIndex < 8ULL)
            {
                const auto chosenBoundary = document.recordBounds[static_cast<std::size_t>(NextRandom(randomState) % boundaryCount)];
                if (NextRandom(randomState) % 2ULL == 0ULL)
                {
                    truncationBytes = chosenBoundary > 0U ? chosenBoundary - 1U : 0U;
                }
                else
                {
                    truncationBytes = std::min(chosenBoundary + 1U, totalBytes);
                }
            }
            else
            {
                truncationBytes = static_cast<std::size_t>(NextRandom(randomState) % (totalBytes + 1ULL));
            }
            FuzzFailure::Check(CheckTruncatedPrefix(document, truncationBytes), "truncated-prefix");
            break;
        }
        case 3U: // Single-byte flip with the exact error/offset expectation.
        {
            GeneratedDocument document = GenerateResumeDocument(kFuzzMaxResumeBytes, randomState);
            if (document.bytes.empty())
            {
                break;
            }
            const auto flipPosition = static_cast<std::size_t>(NextRandom(randomState) % document.bytes.size());
            FuzzFailure::Check(CheckFlippedByte(document, flipPosition), "flip-error-exact");
            break;
        }
        case 4U: // Zero-byte tail: torn success below the envelope size, magic gate above.
        {
            GeneratedDocument document = GenerateResumeDocument(
                kFuzzMaxResumeBytes - (kEnvelopeBytes + 5U), randomState);
            const auto tailBytes = static_cast<std::size_t>(
                NextRandom(randomState) % (kEnvelopeBytes + 6ULL)); // 0..23
            FuzzFailure::Check(CheckZeroTail(document, tailBytes), "zero-tail");
            break;
        }
        default: // Semantic rotation with exact expectations.
        {
            const auto caseIndex = static_cast<std::size_t>(NextRandom(randomState) % kSemanticCaseCount);
            FuzzFailure::Check(RunSemanticCaseByIndex(caseIndex), "semantic-rotation");
            break;
        }
        }
    }

    std::cout << "FUZZ_COMPLETED iterations=" << iterations
              << " seed=" << initialSeed << '\n';
    return 0;
}

constexpr std::size_t kCorpusReplayMaxBytes = 16U * 1024U * 1024U;

// The shipped corpus seeds document one pinned exact LoadResumeState outcome each;
// assert it here beyond the generic fail-closed invariants so on-disk corpus drift
// cannot stay green silently under a different allowed error class or accidental
// validity (see resume-state.md next to the .bin files for the full table).
struct CorpusExpectation
{
    const char* name = nullptr; // seed basename without .bin
    bool expectSuccess = false;
    pbprotocol::ProtocolErrorCode code{};
    std::size_t offset = 0U;
    std::size_t expectedCompletedSegments = 0U;
    std::size_t expectedWirehairCaches = 0U;
    std::size_t expectedDirectRepeatRecords = 0U;
    bool expectTruncatedTail = false;
};

constexpr std::array<CorpusExpectation, 13> kCorpusExpectations{ {
    { "valid-single-completed", true, pbprotocol::ProtocolErrorCode::None, 0U, 1U, 0U, 0U, false },
    { "valid-multi-record", true, pbprotocol::ProtocolErrorCode::None, 0U, 2U, 1U, 1U, false },
    { "malformed-bad-magic", false, pbprotocol::ProtocolErrorCode::InvalidMagic, 0U, 0U, 0U, 0U, false },
    { "malformed-version", false, pbprotocol::ProtocolErrorCode::InvalidEnumValue, 4U, 0U, 0U, 0U, false },
    { "malformed-reserved", false, pbprotocol::ProtocolErrorCode::NonZeroReservedByte, 5U, 0U, 0U, 0U, false },
    { "malformed-crc-payload", false, pbprotocol::ProtocolErrorCode::CrcMismatch, 87U, 0U, 0U, 0U, false },
    { "malformed-body-trailing", false, pbprotocol::ProtocolErrorCode::TrailingBytes, 87U, 0U, 0U, 0U, false },
    { "malformed-unknown-tag", false, pbprotocol::ProtocolErrorCode::InvalidEnumValue, 14U, 0U, 0U, 0U, false },
    { "semantic-cross-type-conflict", false, pbprotocol::ProtocolErrorCode::ResumeRecordConflict, 91U, 0U, 0U, 0U, false },
    { "semantic-wirehair-id-conflict", false, pbprotocol::ProtocolErrorCode::ResumeRecordConflict, 14U, 0U, 0U, 0U, false },
    { "quota-document-over-budget", false, pbprotocol::ProtocolErrorCode::ResourceLimitExceeded, 0U, 0U, 0U, 0U, false },
    { "torn-mid-envelope", true, pbprotocol::ProtocolErrorCode::None, 0U, 0U, 0U, 0U, true },
    { "torn-short-header", true, pbprotocol::ProtocolErrorCode::None, 0U, 0U, 0U, 0U, true },
} };

[[nodiscard]] const CorpusExpectation* FindCorpusExpectation(
    std::string_view inputPath) noexcept
{
    for (const auto& expectation : kCorpusExpectations)
    {
        const std::string_view seedName = expectation.name;
        if (inputPath.size() >= seedName.size() + 4U &&
            inputPath.compare(inputPath.size() - seedName.size() - 4U, seedName.size(), seedName) == 0 &&
            inputPath.compare(inputPath.size() - 4U, 4U, ".bin") == 0)
        {
            return &expectation;
        }
    }
    return nullptr;
}

[[nodiscard]] bool CheckCorpusExpectation(
    const CorpusExpectation& expectation,
    std::span<const std::byte> input) noexcept
{
    const auto loadResult = pbprotocol::LoadResumeState(input, MakeFuzzResourcePolicy());
    if (expectation.expectSuccess)
    {
        if (!loadResult.HasValue())
        {
            return false;
        }
        const auto& state = loadResult.Value();
        return state.completedSegments.size() == expectation.expectedCompletedSegments &&
            state.activeWirehairCaches.size() == expectation.expectedWirehairCaches &&
            state.activeDirectRepeatRecords.size() == expectation.expectedDirectRepeatRecords &&
            state.hasTruncatedTail == expectation.expectTruncatedTail;
    }
    if (loadResult.HasValue())
    {
        return false;
    }
    return loadResult.Error().code == expectation.code &&
        loadResult.Error().offset == expectation.offset;
}


int ReplayInputFile(const char* inputPath)
{
    std::ifstream inputFile(std::string(inputPath), std::ios::binary);
    if (!inputFile)
    {
        std::cerr << "CORPUS_REPLAY_OPEN_FAILED path=" << inputPath << '\n';
        return 2;
    }

    inputFile.seekg(0, std::ios::end);
    const auto fileSize = static_cast<std::size_t>(inputFile.tellg());
    inputFile.seekg(0, std::ios::beg);
    if (fileSize > kCorpusReplayMaxBytes)
    {
        std::cerr << "CORPUS_REPLAY_INPUT_TOO_LARGE path=" << inputPath << '\n';
        return 2;
    }

    std::vector<std::byte> input(fileSize, std::byte{});
    if (fileSize != 0U)
    {
        inputFile.read(
            reinterpret_cast<char*>(input.data()),
            static_cast<std::streamsize>(fileSize));
    }
    // A short read sets failbit without badbit; any non-good stream is a failed
    // replay, not a zero-padded input to validate.
    if (!inputFile)
    {
        std::cerr << "CORPUS_REPLAY_READ_FAILED path=" << inputPath << '\n';
        return 2;
    }

    ExerciseInput(AsConstSpan(input));
    // The shipped corpus seeds document one pinned exact LoadResumeState outcome each;
    // assert it here beyond the generic fail-closed invariants so on-disk corpus drift fails loudly instead of staying green under a different allowed error class.
    const CorpusExpectation* pinnedExpectation =
        FindCorpusExpectation(std::string_view(inputPath));
    if (pinnedExpectation != nullptr &&
        !CheckCorpusExpectation(*pinnedExpectation, AsConstSpan(input)))
    {
        std::cerr << "CORPUS_REPLAY_EXPECTATION_MISMATCH path=" << inputPath << '\n';
        return 2;
    }


    std::cout << "CORPUS_REPLAY_VALIDATED path=" << inputPath
              << " bytes=" << fileSize
              << (pinnedExpectation != nullptr ? " pinned" : " generic") << '\n';
    return 0;
}

} // namespace

#endif // !defined(PB_USE_LIBFUZZER)

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* const data,
    const std::size_t size)
{
    ExerciseInput(std::as_bytes(std::span(data, size)));
    return 0;
}

#if !defined(PB_USE_LIBFUZZER)

int main(const int argumentCount, char* arguments[])
{
    if (argumentCount == 2 && std::string_view(arguments[1]) == "--self-test")
    {
        return RunStructuredSelfTest();
    }
    if (argumentCount == 3 && std::string_view(arguments[1]) == "--input")
    {
        return ReplayInputFile(arguments[2]);
    }

    std::uint64_t iterations = kDefaultIterations;
    std::uint64_t seed = kDefaultSeed;
    if (argumentCount > 1 && !ParseUint64(std::string_view(arguments[1]), iterations))
    {
        std::cerr << "usage: PBProtocolResumeStateFuzz [iterations] [seed]\n"
                     "       PBProtocolResumeStateFuzz --input <corpus-file>\n"
                     "       PBProtocolResumeStateFuzz --self-test\n";
        return 2;
    }
    if (argumentCount > 2 && !ParseUint64(std::string_view(arguments[2]), seed))
    {
        std::cerr << "usage: PBProtocolResumeStateFuzz [iterations] [seed]\n"
                     "       PBProtocolResumeStateFuzz --input <corpus-file>\n"
                     "       PBProtocolResumeStateFuzz --self-test\n";
        return 2;
    }
    if (argumentCount > 3 || iterations == 0ULL)
    {
        std::cerr << "usage: PBProtocolResumeStateFuzz [iterations] [seed]\n"
                     "       PBProtocolResumeStateFuzz --input <corpus-file>\n"
                     "       PBProtocolResumeStateFuzz --self-test\n";
        return 2;
    }

    return RunMutationLoop(iterations, seed);
}

#endif // !defined(PB_USE_LIBFUZZER)
