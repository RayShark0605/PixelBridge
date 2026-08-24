#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/orphan_transport_block_cache.h"
#include "pbprotocol/output_reservation.h"
#include "pbprotocol/resume_state.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

constexpr std::size_t kMaximumGeneratedInputBytes = 256;
constexpr std::uint64_t kDefaultIterations = 100000;
constexpr std::uint64_t kDefaultSeed = 0xF00DBA5EBADCAFE1ULL;
// [op][sessionTag][segmentOrdinal][outerBlockId][payloadLength]
// [observationLow][hasObservationFlag][reservationSizeIndex]
constexpr std::size_t kOperationRecordBytes = 8;

[[nodiscard]] pbprotocol::ReceiverResourcePolicy MakeFuzzResourcePolicy() noexcept
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    // Small budgets make quota drops, conflicts, and single-block limit
    // rejections reachable within a few operations.
    resourcePolicy.maxOrphanTransportBytes = 256;
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

void ExerciseResumeRecords(
    const std::span<const std::byte> input,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    static_cast<void>(pbprotocol::ParseResumeRecord(input, resourcePolicy));
    if (input.empty())
    {
        return;
    }

    // A short prefix exercises the truncated-header and length-overflow paths.
    const std::size_t prefixSize = std::min<std::size_t>(4, input.size());
    static_cast<void>(pbprotocol::ParseResumeRecord(
        input.first(prefixSize), resourcePolicy));

    // One flipped byte keeps a valid record on the CRC-mismatch boundary.
    std::array<std::byte, kMaximumGeneratedInputBytes> mutated{};
    const std::size_t copySize = std::min(input.size(), mutated.size());
    for (std::size_t byteIndex = 0; byteIndex < copySize; byteIndex++)
    {
        mutated[byteIndex] = input[byteIndex];
    }
    mutated[0] ^= static_cast<std::byte>(0x01);
    static_cast<void>(pbprotocol::ParseResumeRecord(
        std::span<const std::byte>(mutated).first(copySize), resourcePolicy));

    // The budget gate is exercised on both sides of maxResumeBytes.
    static_cast<void>(pbprotocol::ValidateResumeStateBudget(
        static_cast<std::uint64_t>(input.size()) * 8ULL, resourcePolicy));
}

void ExerciseOrphanOperations(
    const std::span<const std::byte> input,
    pbprotocol::OrphanTransportBlockCache& cache,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::array<std::uint64_t, 8>& reservationBoundarySizes)
{
    for (std::size_t recordOffset = 0;
         recordOffset + kOperationRecordBytes <= input.size();
         recordOffset++)
    {
        const auto readByte = [&](const std::size_t index) noexcept -> std::uint8_t
        {
            return std::to_integer<std::uint8_t>(input[recordOffset + index]);
        };
        const std::uint64_t operationCode = readByte(0);
        const pbprotocol::SessionTag sessionTag{readByte(1)};
        const std::uint64_t segmentOrdinal = readByte(2);
        const std::uint32_t outerBlockId = readByte(3);
        // 0..32: zero-length payloads hit InvalidRecordSize, and the maximum
        // equals maxOuterBlockBytes so the single-block limit stays reachable.
        const std::size_t payloadLength = readByte(4) % 33U;
        if (recordOffset + kOperationRecordBytes + payloadLength > input.size())
        {
            // Incomplete record: skip it instead of fabricating payload bytes.
            continue;
        }
        const auto payloadSpan = input.subspan(
            recordOffset + kOperationRecordBytes, payloadLength);
        const bool hasObservation = (readByte(6) & 1U) != 0U;
        const std::optional<std::uint64_t> observationOrdinal =
            hasObservation
                ? std::make_optional(static_cast<std::uint64_t>(readByte(5)))
                : std::nullopt;

        switch (operationCode % 4ULL)
        {
        case 0:
            static_cast<void>(cache.Admit(
                sessionTag, segmentOrdinal, outerBlockId, payloadSpan, observationOrdinal));
            break;
        case 1:
            static_cast<void>(cache.Drain(sessionTag, segmentOrdinal, observationOrdinal));
            break;
        case 2:
            cache.ClearSession(sessionTag);
            break;
        default:
            // Stateless gate: the returned status is the observable event. The
            // record's final byte selects one of eight boundary sizes so every
            // entry of reservationBoundarySizes stays reachable by mutation.
            static_cast<void>(pbprotocol::EvaluateOutputReservation(
                reservationBoundarySizes[readByte(7) % 8ULL], resourcePolicy));
            break;
        }
    }
}

void ExerciseInput(const std::span<const std::byte> input)
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        MakeFuzzResourcePolicy();

    ExerciseResumeRecords(input, resourcePolicy);

    auto cacheResult = pbprotocol::OrphanTransportBlockCache::Create(
        resourcePolicy);
    if (!cacheResult)
    {
        // The fixed policy is validated in RunMutationLoop setup; a failure
        // here would indicate a regression in the factory itself.
        return;
    }
    auto cache = std::move(cacheResult).Value();

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
        input, cache, resourcePolicy, reservationBoundarySizes);
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
        writeByte(7, 0U);
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
    std::cout << "CORPUS_REPLAY_NO_CRASH path=" << inputPath
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
