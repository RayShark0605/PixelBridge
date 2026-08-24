#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/orphan_transport_block_cache.h"
#include "pbprotocol/output_reservation.h"
#include "pbprotocol/resume_state.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaximumGeneratedInputBytes = 1024;
constexpr std::uint64_t kDefaultIterations = 100000;
constexpr std::uint64_t kDefaultSeed = 0xF00DBA5EBADCAFE1ULL;
// [op][sessionTag][segmentOrdinal][outerBlockId][paddedLength]
// [observationLow][hasObservationFlag][declaredLength/reservationSizeIndex]
constexpr std::size_t kOperationRecordBytes = 8;

[[nodiscard]] pbprotocol::ReceiverResourcePolicy MakeFuzzResourcePolicy() noexcept
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    // Small budgets make quota drops, conflicts, and single-block limit
    // rejections reachable within a few operations.
    resourcePolicy.maxOrphanTransportBytes = 64;
    resourcePolicy.maxOrphanTransportBlocks = 8;
    resourcePolicy.maxOuterBlockBytes = 32;
    resourcePolicy.maxResumeBytes = 1024;
    return resourcePolicy;
}

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

[[noreturn]] void FailInvariant(const std::string_view message)
{
    std::cerr << "FUZZ_INVARIANT_FAILED " << message << '\n';
    std::abort();
}

void RequireInvariant(
    const bool condition,
    const std::string_view message)
{
    if (!condition)
    {
        FailInvariant(message);
    }
}

struct ExpectedStatus
{
    bool success = false;
    pbprotocol::ProtocolErrorCode errorCode =
        pbprotocol::ProtocolErrorCode::None;
};

struct ReferenceOrphanKey
{
    std::uint64_t sessionTagValue = 0;
    std::uint64_t segmentOrdinal = 0;

    bool operator<(const ReferenceOrphanKey& other) const noexcept
    {
        if (sessionTagValue != other.sessionTagValue)
        {
            return sessionTagValue < other.sessionTagValue;
        }
        return segmentOrdinal < other.segmentOrdinal;
    }
};

struct ReferenceOrphanKeyState
{
    std::optional<std::uint64_t> firstSeenObservationOrdinal;
    bool conflicted = false;
    std::vector<pbprotocol::OrphanTransportBlockEntry> entries;
};

class ReferenceOrphanCache
{
public:
    explicit ReferenceOrphanCache(
        const pbprotocol::ReceiverResourcePolicy& resourcePolicy) noexcept
        : resourcePolicy_(resourcePolicy)
    {
    }

    [[nodiscard]] ExpectedStatus Admit(
        const pbprotocol::SessionTag sessionTag,
        const std::uint64_t segmentOrdinal,
        const std::uint32_t outerBlockId,
        const std::uint16_t declaredPayloadBytes,
        const std::span<const std::byte> paddedPayload,
        const std::optional<std::uint64_t> observationOrdinal)
    {
        if (paddedPayload.empty() || declaredPayloadBytes == 0 ||
            declaredPayloadBytes > paddedPayload.size())
        {
            return {false, pbprotocol::ProtocolErrorCode::InvalidRecordSize};
        }
        if (paddedPayload.size() > resourcePolicy_.maxOuterBlockBytes)
        {
            droppedBlockCount_++;
            return {
                false,
                pbprotocol::ProtocolErrorCode::ResourceLimitExceeded};
        }
        for (std::size_t paddingIndex = declaredPayloadBytes;
             paddingIndex < paddedPayload.size();
             paddingIndex++)
        {
            if (paddedPayload[paddingIndex] != std::byte{0})
            {
                return {
                    false,
                    pbprotocol::ProtocolErrorCode::NonCanonicalPadding};
            }
        }

        const ReferenceOrphanKey key{sessionTag.value, segmentOrdinal};
        const auto existingKey = keys_.find(key);
        if (existingKey != keys_.end())
        {
            ReferenceOrphanKeyState& keyState = existingKey->second;
            if (keyState.conflicted)
            {
                return {
                    false,
                    pbprotocol::ProtocolErrorCode::OrphanPayloadConflict};
            }
            for (const pbprotocol::OrphanTransportBlockEntry& entry :
                 keyState.entries)
            {
                if (entry.outerBlockId != outerBlockId)
                {
                    continue;
                }
                if (entry.declaredPayloadBytes == declaredPayloadBytes &&
                    entry.paddedPayload.size() == paddedPayload.size() &&
                    std::equal(
                        entry.paddedPayload.begin(),
                        entry.paddedPayload.end(),
                        paddedPayload.begin()))
                {
                    return {true, pbprotocol::ProtocolErrorCode::None};
                }
                keyState.conflicted = true;
                conflictedKeyCount_++;
                return {
                    false,
                    pbprotocol::ProtocolErrorCode::OrphanPayloadConflict};
            }
        }

        if (cachedBlockCount_ >=
                resourcePolicy_.maxOrphanTransportBlocks ||
            cachedByteCount_ > resourcePolicy_.maxOrphanTransportBytes ||
            paddedPayload.size() >
                resourcePolicy_.maxOrphanTransportBytes - cachedByteCount_)
        {
            droppedBlockCount_++;
            return {
                false,
                pbprotocol::ProtocolErrorCode::ResourceLimitExceeded};
        }

        auto [keyIterator, inserted] = keys_.try_emplace(key);
        ReferenceOrphanKeyState& keyState = keyIterator->second;
        if (inserted && observationOrdinal)
        {
            keyState.firstSeenObservationOrdinal = observationOrdinal;
        }
        keyState.entries.push_back(pbprotocol::OrphanTransportBlockEntry{
            outerBlockId,
            declaredPayloadBytes,
            std::vector<std::byte>(
                paddedPayload.begin(),
                paddedPayload.end())});
        cachedBlockCount_++;
        cachedByteCount_ += paddedPayload.size();
        admittedBlockCount_++;
        return {true, pbprotocol::ProtocolErrorCode::None};
    }

    [[nodiscard]] std::pair<ExpectedStatus,
                            pbprotocol::OrphanTransportBlockDrain>
    Drain(
        const pbprotocol::SessionTag sessionTag,
        const std::uint64_t segmentOrdinal,
        const std::optional<std::uint64_t> observationOrdinal)
    {
        pbprotocol::OrphanTransportBlockDrain drain;
        const ReferenceOrphanKey key{sessionTag.value, segmentOrdinal};
        const auto iterator = keys_.find(key);
        if (iterator == keys_.end())
        {
            return {{true, pbprotocol::ProtocolErrorCode::None}, drain};
        }
        const ReferenceOrphanKeyState& keyState = iterator->second;
        if (keyState.conflicted)
        {
            return {{
                false,
                pbprotocol::ProtocolErrorCode::OrphanPayloadConflict}, drain};
        }
        if (observationOrdinal && keyState.firstSeenObservationOrdinal)
        {
            if (*observationOrdinal <
                *keyState.firstSeenObservationOrdinal)
            {
                return {{
                    false,
                    pbprotocol::ProtocolErrorCode::InvalidObservationOrdinal},
                    drain};
            }
            drain.waitObservations = *observationOrdinal -
                *keyState.firstSeenObservationOrdinal;
        }
        drain.entries = keyState.entries;
        for (const pbprotocol::OrphanTransportBlockEntry& entry :
             keyState.entries)
        {
            cachedByteCount_ -= entry.paddedPayload.size();
            cachedBlockCount_--;
        }
        keys_.erase(iterator);
        return {{true, pbprotocol::ProtocolErrorCode::None}, std::move(drain)};
    }

    void ClearSession(const pbprotocol::SessionTag sessionTag)
    {
        for (auto iterator = keys_.begin(); iterator != keys_.end();)
        {
            if (iterator->first.sessionTagValue != sessionTag.value)
            {
                iterator++;
                continue;
            }
            for (const pbprotocol::OrphanTransportBlockEntry& entry :
                 iterator->second.entries)
            {
                cachedByteCount_ -= entry.paddedPayload.size();
                cachedBlockCount_--;
            }
            if (iterator->second.conflicted)
            {
                conflictedKeyCount_--;
            }
            iterator = keys_.erase(iterator);
        }
    }

    [[nodiscard]] std::uint64_t GetAdmittedBlockCount() const noexcept
    {
        return admittedBlockCount_;
    }

    [[nodiscard]] std::uint64_t GetDroppedBlockCount() const noexcept
    {
        return droppedBlockCount_;
    }

    [[nodiscard]] std::uint64_t GetConflictedKeyCount() const noexcept
    {
        return conflictedKeyCount_;
    }

    [[nodiscard]] std::size_t GetCachedBlockCount() const noexcept
    {
        return cachedBlockCount_;
    }

    [[nodiscard]] std::size_t GetCachedBytes() const noexcept
    {
        return cachedByteCount_;
    }

private:
    pbprotocol::ReceiverResourcePolicy resourcePolicy_;
    std::map<ReferenceOrphanKey, ReferenceOrphanKeyState> keys_;
    std::uint64_t admittedBlockCount_ = 0;
    std::uint64_t droppedBlockCount_ = 0;
    std::uint64_t conflictedKeyCount_ = 0;
    std::size_t cachedBlockCount_ = 0;
    std::size_t cachedByteCount_ = 0;
};

void RequireStatusMatches(
    const pbprotocol::ProtocolStatus& actual,
    const ExpectedStatus& expected,
    const std::string_view context)
{
    if (static_cast<bool>(actual) != expected.success ||
        (!expected.success && actual.Error().code != expected.errorCode))
    {
        FailInvariant(context);
    }
}

void RequireCacheMatchesModel(
    const pbprotocol::OrphanTransportBlockCache& cache,
    const ReferenceOrphanCache& model)
{
    RequireInvariant(
        cache.GetAdmittedBlockCount() == model.GetAdmittedBlockCount(),
        "orphan admitted count");
    RequireInvariant(
        cache.GetDroppedBlockCount() == model.GetDroppedBlockCount(),
        "orphan dropped count");
    RequireInvariant(
        cache.GetResourceExhaustedCount() == 0,
        "unexpected orphan allocation failure");
    RequireInvariant(
        cache.GetConflictedKeyCount() == model.GetConflictedKeyCount(),
        "orphan conflict count");
    RequireInvariant(
        cache.GetCachedBlockCount() == model.GetCachedBlockCount(),
        "orphan block occupancy");
    RequireInvariant(
        cache.GetCachedBytes() == model.GetCachedBytes(),
        "orphan byte occupancy");
}

void ExerciseResumeRecords(
    const std::span<const std::byte> input,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    const auto parseResult = pbprotocol::ParseResumeRecord(
        input,
        resourcePolicy);
    if (parseResult)
    {
        RequireInvariant(
            input.size() == pbprotocol::kResumeRecordEnvelopeBytes +
                parseResult.Value().size(),
            "resume success length");

        std::vector<std::byte> roundTrip(input.size());
        RequireInvariant(
            static_cast<bool>(pbprotocol::SerializeResumeRecord(
                parseResult.Value(),
                roundTrip)),
            "resume round-trip serialization");
        RequireInvariant(
            roundTrip.size() == input.size() &&
                std::equal(
                    roundTrip.begin(),
                    roundTrip.end(),
                    input.begin()),
            "resume round-trip bytes");
    }

    if (!input.empty())
    {
        // A short prefix exercises the truncated-header paths.
        const std::size_t prefixSize = std::min<std::size_t>(4, input.size());
        const auto prefixResult = pbprotocol::ParseResumeRecord(
            input.first(prefixSize),
            resourcePolicy);
        RequireInvariant(!prefixResult, "resume prefix unexpectedly accepted");
    }

    // Mutate the stored CRC only after proving the input is a valid record.
    // Magic/version/reserved/length therefore remain valid and the expected
    // branch is CrcMismatch rather than InvalidMagic.
    if (parseResult && input.size() <= kMaximumGeneratedInputBytes)
    {
        std::array<std::byte, kMaximumGeneratedInputBytes> mutated{};
        std::copy(input.begin(), input.end(), mutated.begin());
        mutated[input.size() - 1U] ^= std::byte{0x01};
        const auto crcResult = pbprotocol::ParseResumeRecord(
            std::span<const std::byte>(mutated).first(input.size()),
            resourcePolicy);
        RequireInvariant(!crcResult, "corrupt resume unexpectedly accepted");
        RequireInvariant(
            crcResult.Error().code ==
                pbprotocol::ProtocolErrorCode::CrcMismatch,
            "resume CRC mutation missed checksum branch");
    }

    const std::uint64_t maximumResumeBytes = resourcePolicy.maxResumeBytes;
    RequireInvariant(
        static_cast<bool>(pbprotocol::ValidateResumeStateBudget(
            maximumResumeBytes - 1ULL,
            resourcePolicy)),
        "resume limit minus one");
    RequireInvariant(
        static_cast<bool>(pbprotocol::ValidateResumeStateBudget(
            maximumResumeBytes,
            resourcePolicy)),
        "resume exact limit");
    const pbprotocol::ProtocolStatus overLimitStatus =
        pbprotocol::ValidateResumeStateBudget(
            maximumResumeBytes + 1ULL,
            resourcePolicy);
    RequireInvariant(
        !overLimitStatus &&
            overLimitStatus.Error().code ==
                pbprotocol::ProtocolErrorCode::ResourceLimitExceeded,
        "resume limit plus one");
}

void ExerciseOrphanOperations(
    const std::span<const std::byte> input,
    pbprotocol::OrphanTransportBlockCache& cache,
    ReferenceOrphanCache& model,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::array<std::uint64_t, 8>& reservationBoundarySizes)
{
    std::size_t recordOffset = 0;
    while (recordOffset + kOperationRecordBytes <= input.size())
    {
        const auto readByte = [&](const std::size_t index) noexcept -> std::uint8_t
        {
            return std::to_integer<std::uint8_t>(input[recordOffset + index]);
        };
        const std::uint64_t operationCode = readByte(0);
        const pbprotocol::SessionTag sessionTag{readByte(1)};
        const std::uint64_t segmentOrdinal = readByte(2);
        const std::uint32_t outerBlockId = readByte(3);
        // 0..64: zero hits InvalidRecordSize, 32 is the exact profile limit,
        // and 33..64 exercise the pre-allocation oversize rejection.
        const std::size_t payloadLength = readByte(4) % 65U;
        if (recordOffset + kOperationRecordBytes + payloadLength > input.size())
        {
            // Incomplete record: stop instead of fabricating payload bytes.
            break;
        }
        const auto payloadSpan = input.subspan(
            recordOffset + kOperationRecordBytes, payloadLength);
        const std::uint16_t declaredPayloadBytes =
            static_cast<std::uint16_t>(
                readByte(7) % (payloadLength + 2U));
        const bool hasObservation = (readByte(6) & 1U) != 0U;
        const std::optional<std::uint64_t> observationOrdinal =
            hasObservation
                ? std::make_optional(static_cast<std::uint64_t>(readByte(5)))
                : std::nullopt;

        switch (operationCode % 4ULL)
        {
        case 0:
        {
            const ExpectedStatus expected = model.Admit(
                sessionTag,
                segmentOrdinal,
                outerBlockId,
                declaredPayloadBytes,
                payloadSpan,
                observationOrdinal);
            const pbprotocol::ProtocolStatus actual = cache.Admit(
                sessionTag,
                segmentOrdinal,
                outerBlockId,
                declaredPayloadBytes,
                payloadSpan,
                observationOrdinal);
            RequireStatusMatches(actual, expected, "orphan admit status");
            break;
        }
        case 1:
        {
            auto expected = model.Drain(
                sessionTag,
                segmentOrdinal,
                observationOrdinal);
            const auto actual = cache.Drain(
                sessionTag,
                segmentOrdinal,
                observationOrdinal);
            RequireInvariant(
                static_cast<bool>(actual) == expected.first.success,
                "orphan drain success");
            if (expected.first.success)
            {
                RequireInvariant(
                    actual.Value() == expected.second,
                    "orphan drain content");
            }
            else
            {
                RequireInvariant(
                    actual.Error().code == expected.first.errorCode,
                    "orphan drain error");
            }
            break;
        }
        case 2:
            cache.ClearSession(sessionTag);
            model.ClearSession(sessionTag);
            break;
        default:
            // Stateless gate: the returned status is the observable event. The
            // record's final byte selects one of eight boundary sizes so every
            // entry of reservationBoundarySizes stays reachable by mutation.
        {
            const std::uint64_t reservationBytes =
                reservationBoundarySizes[readByte(7) % 8ULL];
            const auto reservationResult =
                pbprotocol::EvaluateOutputReservation(
                    reservationBytes,
                    resourcePolicy);
            if (reservationBytes > resourcePolicy.maxAcceptedFileBytes)
            {
                RequireInvariant(
                    !reservationResult &&
                        reservationResult.Error().code ==
                            pbprotocol::ProtocolErrorCode::
                                OutputReservationDenied,
                    "output reservation denied boundary");
            }
            else
            {
                RequireInvariant(
                    static_cast<bool>(reservationResult),
                    "output reservation accepted boundary");
                const pbprotocol::OutputReservationDecision expectedDecision =
                    reservationBytes > resourcePolicy.
                        maxOutputPreallocationBytesWithoutPrompt
                        ? pbprotocol::OutputReservationDecision::
                              RequiresUserConfirmation
                        : pbprotocol::OutputReservationDecision::AutoAccept;
                RequireInvariant(
                    reservationResult.Value() == expectedDecision,
                    "output reservation decision");
            }
            break;
        }
        }

        RequireCacheMatchesModel(cache, model);
        recordOffset += kOperationRecordBytes + payloadLength;
    }
}

void RunSemanticSelfTest()
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        MakeFuzzResourcePolicy();
    resourcePolicy.maxOrphanTransportBytes = 8;
    resourcePolicy.maxOrphanTransportBlocks = 1;
    resourcePolicy.maxOuterBlockBytes = 8;
    RequireInvariant(
        static_cast<bool>(pbprotocol::ValidateReceiverResourcePolicy(
            resourcePolicy)),
        "semantic policy");

    auto fullCacheResult = pbprotocol::OrphanTransportBlockCache::Create(
        resourcePolicy);
    RequireInvariant(static_cast<bool>(fullCacheResult), "semantic cache create");
    auto fullCache = std::move(fullCacheResult).Value();
    const std::array<std::byte, 8> firstPayload{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    std::array<std::byte, 8> conflictingPayload = firstPayload;
    conflictingPayload[0] ^= std::byte{0x80};
    RequireInvariant(
        static_cast<bool>(fullCache.Admit(
            pbprotocol::SessionTag{1},
            0,
            7,
            8,
            firstPayload)),
        "semantic full-cache first admit");
    RequireInvariant(
        static_cast<bool>(fullCache.Admit(
            pbprotocol::SessionTag{1},
            0,
            7,
            8,
            firstPayload)),
        "semantic full-cache duplicate");
    const pbprotocol::ProtocolStatus conflictStatus = fullCache.Admit(
        pbprotocol::SessionTag{1},
        0,
        7,
        8,
        conflictingPayload);
    RequireInvariant(
        !conflictStatus &&
            conflictStatus.Error().code ==
                pbprotocol::ProtocolErrorCode::OrphanPayloadConflict,
        "semantic full-cache conflict");
    RequireInvariant(
        fullCache.GetAdmittedBlockCount() == 1 &&
            fullCache.GetDroppedBlockCount() == 0 &&
            fullCache.GetConflictedKeyCount() == 1 &&
            fullCache.GetCachedBlockCount() == 1 &&
            fullCache.GetCachedBytes() == 8,
        "semantic full-cache telemetry");
    const auto conflictedDrain = fullCache.Drain(
        pbprotocol::SessionTag{1},
        0);
    RequireInvariant(
        !conflictedDrain &&
            conflictedDrain.Error().code ==
                pbprotocol::ProtocolErrorCode::OrphanPayloadConflict,
        "semantic conflict latch");
    fullCache.ClearAll();
    RequireInvariant(
        fullCache.GetCachedBlockCount() == 0 &&
            fullCache.GetCachedBytes() == 0 &&
            fullCache.GetConflictedKeyCount() == 0 &&
            fullCache.GetAdmittedBlockCount() == 1,
        "semantic clear all");

    auto lengthCacheResult = pbprotocol::OrphanTransportBlockCache::Create(
        resourcePolicy);
    RequireInvariant(
        static_cast<bool>(lengthCacheResult),
        "semantic length cache create");
    auto lengthCache = std::move(lengthCacheResult).Value();
    const std::array<std::byte, 4> paddedPayload{
        std::byte{0xAA}, std::byte{0xBB}, std::byte{0}, std::byte{0}};
    RequireInvariant(
        static_cast<bool>(lengthCache.Admit(
            pbprotocol::SessionTag{2},
            0,
            9,
            2,
            paddedPayload)),
        "semantic declared length admit");
    const pbprotocol::ProtocolStatus lengthConflict = lengthCache.Admit(
        pbprotocol::SessionTag{2},
        0,
        9,
        3,
        paddedPayload);
    RequireInvariant(
        !lengthConflict &&
            lengthConflict.Error().code ==
                pbprotocol::ProtocolErrorCode::OrphanPayloadConflict,
        "semantic declared length conflict");

    std::array<std::byte, 4> nonCanonicalPayload = paddedPayload;
    nonCanonicalPayload[3] = std::byte{1};
    const pbprotocol::ProtocolStatus paddingStatus = lengthCache.Admit(
        pbprotocol::SessionTag{3},
        0,
        0,
        2,
        nonCanonicalPayload);
    RequireInvariant(
        !paddingStatus &&
            paddingStatus.Error().code ==
                pbprotocol::ProtocolErrorCode::NonCanonicalPadding,
        "semantic canonical padding");

    const std::array<std::byte, 4> resumePayload{
        std::byte{0x11},
        std::byte{0x22},
        std::byte{0x33},
        std::byte{0x44}};
    std::array<
        std::byte,
        pbprotocol::kResumeRecordEnvelopeBytes + resumePayload.size()>
        resumeRecord{};
    RequireInvariant(
        static_cast<bool>(pbprotocol::SerializeResumeRecord(
            resumePayload,
            resumeRecord)),
        "semantic resume serialization");
    RequireInvariant(
        static_cast<bool>(pbprotocol::ParseResumeRecord(
            resumeRecord,
            resourcePolicy)),
        "semantic resume valid");
    resumeRecord.back() ^= std::byte{1};
    const auto corruptResumeResult = pbprotocol::ParseResumeRecord(
        resumeRecord,
        resourcePolicy);
    RequireInvariant(
        !corruptResumeResult &&
            corruptResumeResult.Error().code ==
                pbprotocol::ProtocolErrorCode::CrcMismatch,
        "semantic resume CRC");
}

void ExerciseInput(const std::span<const std::byte> input)
{
    static const bool semanticSelfTestPassed = []
    {
        RunSemanticSelfTest();
        return true;
    }();
    static_cast<void>(semanticSelfTestPassed);

    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        MakeFuzzResourcePolicy();
    const std::span<const std::byte> boundedInput = input.first(
        std::min(input.size(), kMaximumGeneratedInputBytes));

    ExerciseResumeRecords(boundedInput, resourcePolicy);

    auto cacheResult = pbprotocol::OrphanTransportBlockCache::Create(
        resourcePolicy);
    if (!cacheResult)
    {
        FailInvariant("fixed-policy cache factory");
    }
    auto cache = std::move(cacheResult).Value();
    ReferenceOrphanCache model(resourcePolicy);

    const std::uint64_t promptThreshold =
        resourcePolicy.maxOutputPreallocationBytesWithoutPrompt;
    const std::uint64_t maximumFileBytes = resourcePolicy.maxAcceptedFileBytes;
    // ValidateReceiverResourcePolicy keeps both limits below UINT64_MAX, so
    // the +/-1 boundary probes cannot wrap.
    const std::array<std::uint64_t, 8> reservationBoundarySizes{
        0ULL,
        1ULL,
        promptThreshold - 1ULL,
        promptThreshold,
        promptThreshold + 1ULL,
        maximumFileBytes - 1ULL,
        maximumFileBytes,
        maximumFileBytes + 1ULL};

    ExerciseOrphanOperations(
        boundedInput,
        cache,
        model,
        resourcePolicy,
        reservationBoundarySizes);
}

#if !defined(PB_USE_LIBFUZZER)

template <std::size_t SourceBytes>
void CopySeed(
    const std::array<std::byte, SourceBytes>& source,
    std::array<std::byte, kMaximumGeneratedInputBytes>& destination,
    std::size_t& destinationSize) noexcept
{
    std::copy(source.begin(), source.end(), destination.begin());
    destinationSize = source.size();
}

int RunMutationLoop(
    const std::uint64_t iterations,
    const std::uint64_t initialSeed)
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        MakeFuzzResourcePolicy();

    // Structured seed 1: a valid resume record so the parser exercises its
    // success path before mutations push it onto failure branches.
    constexpr std::size_t kResumePayloadBytes = 16;
    std::array<std::byte, pbprotocol::kResumeRecordEnvelopeBytes + kResumePayloadBytes>
        resumeSeed{};
    std::array<std::byte, kResumePayloadBytes> resumePayload{};
    for (std::size_t byteIndex = 0; byteIndex < resumePayload.size(); byteIndex++)
    {
        resumePayload[byteIndex] = static_cast<std::byte>(byteIndex + 1U);
    }

    // Structured seed 2: admit/drain/clear records with monotonic observation
    // ordinals so waitObservations, quota drops, and teardown are reachable.
    std::array<std::byte, kMaximumGeneratedInputBytes> operationSeed{};
    std::size_t operationSeedSize = 0;
    const auto appendRecord = [&](const std::uint8_t operationCode,
                                  const std::uint8_t sessionTagValue,
                                  const std::uint8_t segmentOrdinal,
                                  const std::uint8_t outerBlockId,
                                  const std::span<const std::byte> payloadBytes,
                                  const std::optional<std::uint64_t> observationOrdinal) -> bool
    {
        if (operationSeedSize + kOperationRecordBytes + payloadBytes.size() >
            operationSeed.size())
        {
            return false;
        }
        auto writeByte = [&](const std::size_t index, const std::uint8_t value) noexcept
        {
            operationSeed[operationSeedSize + index] = static_cast<std::byte>(value);
        };
        writeByte(0, operationCode);
        writeByte(1, sessionTagValue);
        writeByte(2, segmentOrdinal);
        writeByte(3, outerBlockId);
        writeByte(4, static_cast<std::uint8_t>(payloadBytes.size()));
        writeByte(5, observationOrdinal.has_value() ? static_cast<std::uint8_t>(*observationOrdinal) : 0U);
        writeByte(6, observationOrdinal.has_value() ? 1U : 0U);
        writeByte(
            7,
            static_cast<std::uint8_t>(payloadBytes.size()));
        for (std::size_t payloadIndex = 0; payloadIndex < payloadBytes.size(); payloadIndex++)
        {
            operationSeed[operationSeedSize + kOperationRecordBytes + payloadIndex] =
                payloadBytes[payloadIndex];
        }
        operationSeedSize += kOperationRecordBytes + payloadBytes.size();
        return true;
    };

    const std::array<std::byte, 4> firstBlockPayload{
        std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD}};
    const std::array<std::byte, 4> secondBlockPayload{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};

    if (!pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy) ||
        !pbprotocol::SerializeResumeRecord(resumePayload, resumeSeed) ||
        !appendRecord(0U, 1U, 0U, 0U, firstBlockPayload, 1ULL) ||
        !appendRecord(0U, 1U, 0U, 1U, secondBlockPayload, 2ULL) ||
        !appendRecord(1U, 1U, 0U, 0U, {}, 9ULL) ||
        !appendRecord(2U, 1U, 0U, 0U, {}, std::nullopt))
    {
        std::cerr << "FUZZ_SETUP_FAILED\n";
        return 2;
    }

    std::uint64_t randomState = initialSeed == 0 ? kDefaultSeed : initialSeed;
    std::array<std::byte, kMaximumGeneratedInputBytes> input{};
    for (std::uint64_t iteration = 0; iteration < iterations; iteration++)
    {
        std::size_t inputSize = 0;
        switch (NextRandom(randomState) % 3ULL)
        {
        case 0:
            CopySeed(resumeSeed, input, inputSize);
            break;
        case 1:
        {
            const auto operationSpan =
                std::span<const std::byte>(operationSeed).first(operationSeedSize);
            std::copy(operationSpan.begin(), operationSpan.end(), input.begin());
            inputSize = operationSeedSize;
            break;
        }
        default:
            inputSize = static_cast<std::size_t>(
                NextRandom(randomState) % (input.size() + 1ULL));
            for (std::size_t byteIndex = 0; byteIndex < inputSize; byteIndex++)
            {
                input[byteIndex] = static_cast<std::byte>(
                    NextRandom(randomState) & 0xFFULL);
            }
            break;
        }

        if (inputSize != 0)
        {
            const std::size_t mutationCount = static_cast<std::size_t>(
                NextRandom(randomState) % 9ULL);
            for (std::size_t mutationIndex = 0;
                 mutationIndex < mutationCount;
                 mutationIndex++)
            {
                const std::size_t byteIndex = static_cast<std::size_t>(
                    NextRandom(randomState) % inputSize);
                input[byteIndex] ^= static_cast<std::byte>(
                    NextRandom(randomState) & 0xFFULL);
            }
        }

        ExerciseInput(std::span<const std::byte>(input).first(inputSize));
    }

    std::cout << "FUZZ_COMPLETED iterations=" << iterations
              << " seed=" << initialSeed << '\n';
    return 0;
}

int ReplayInputFile(const std::string_view inputPath)
{
    std::ifstream inputFile(std::string(inputPath), std::ios::binary);
    if (!inputFile)
    {
        std::cerr << "CORPUS_REPLAY_OPEN_FAILED path=" << inputPath << '\n';
        return 2;
    }

    std::array<char, kMaximumGeneratedInputBytes> input{};
    inputFile.read(
        input.data(),
        static_cast<std::streamsize>(input.size()));
    const std::streamsize inputSize = inputFile.gcount();
    if (inputFile.bad())
    {
        std::cerr << "CORPUS_REPLAY_READ_FAILED path=" << inputPath << '\n';
        return 2;
    }

    if (inputSize == static_cast<std::streamsize>(input.size()))
    {
        char extraByte = 0;
        inputFile.read(&extraByte, 1);
        if (inputFile.gcount() != 0)
        {
            std::cerr << "CORPUS_REPLAY_INPUT_TOO_LARGE path="
                      << inputPath << '\n';
            return 2;
        }
        if (inputFile.bad())
        {
            std::cerr << "CORPUS_REPLAY_READ_FAILED path=" << inputPath << '\n';
            return 2;
        }
    }

    const std::size_t inputByteCount = static_cast<std::size_t>(inputSize);
    ExerciseInput(std::as_bytes(std::span(input).first(inputByteCount)));
    std::cout << "CORPUS_REPLAY_VALIDATED path=" << inputPath
              << " bytes=" << inputByteCount << '\n';
    return 0;
}

#endif

} // namespace

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
    if (argumentCount == 3 &&
        std::string_view(arguments[1]) == "--input")
    {
        return ReplayInputFile(arguments[2]);
    }

    std::uint64_t iterations = kDefaultIterations;
    std::uint64_t seed = kDefaultSeed;
    if (argumentCount > 1 && !ParseUint64(arguments[1], iterations))
    {
        std::cerr << "usage: PBProtocolOrphanResourceFuzz [iterations] [seed]\n"
                     "       PBProtocolOrphanResourceFuzz --input <corpus-file>\n";
        return 2;
    }
    if (argumentCount > 2 && !ParseUint64(arguments[2], seed))
    {
        std::cerr << "usage: PBProtocolOrphanResourceFuzz [iterations] [seed]\n"
                     "       PBProtocolOrphanResourceFuzz --input <corpus-file>\n";
        return 2;
    }
    if (argumentCount > 3 || iterations == 0)
    {
        std::cerr << "usage: PBProtocolOrphanResourceFuzz [iterations] [seed]\n"
                     "       PBProtocolOrphanResourceFuzz --input <corpus-file>\n";
        return 2;
    }

    return RunMutationLoop(iterations, seed);
}

#endif
