#include "pbprotocol/descriptor_binding.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/session_registry.h"

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
constexpr std::uint64_t kDefaultSeed = 0xBADC0FFEE0DDF00DULL;

[[nodiscard]] pbprotocol::ReceiverResourcePolicy MakeFuzzResourcePolicy() noexcept
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxAcceptedFileBytes = 1024ULL * 1024ULL;
    resourcePolicy.maxSegmentCount = 64;
    resourcePolicy.maxRawSegmentBytes = 64ULL * 1024ULL;
    resourcePolicy.maxEncodedSegmentBytes = 64ULL * 1024ULL;
    resourcePolicy.maxOuterBlockBytes = 4096;
    resourcePolicy.maxDescriptorStateBytes = 1024ULL * 1024ULL;
    resourcePolicy.maxConcurrentSessions = 2;
    resourcePolicy.maxTotalDescriptorStateBytes = 2ULL * 1024ULL * 1024ULL;
    return resourcePolicy;
}

[[nodiscard]] pbprotocol::SessionDescriptor MakeFixedSessionDescriptor() noexcept
{
    pbprotocol::SessionId sessionId{};
    for (std::size_t byteIndex = 0; byteIndex < sessionId.bytes.size(); byteIndex++)
    {
        sessionId.bytes[byteIndex] = static_cast<std::byte>(byteIndex);
    }

    return pbprotocol::SessionDescriptor{
        pbprotocol::GetProtocolVersion(),
        sessionId,
        64,
        8,
        pbprotocol::DigestAlgorithm::Blake3_256};
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeGeneratedSegmentDescriptor(
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const std::uint64_t segmentOrdinal,
    const std::uint64_t rawOffset,
    const std::uint64_t rawSize,
    const std::uint32_t outerBlockBytes) noexcept
{
    const pbprotocol::RawDigest rawDigest{};
    return pbprotocol::SegmentDescriptor{
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId),
        segmentOrdinal,
        rawOffset,
        rawSize,
        rawSize,
        pbprotocol::CompressionCodec::Raw,
        pbprotocol::OuterFecMode::DirectRepeat,
        outerBlockBytes,
        rawDigest,
        pbprotocol::EncodedDigest{rawDigest.bytes},
        std::nullopt};
}

void ExerciseDescriptorState(
    const std::span<const std::byte> input,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    auto stateResult = pbprotocol::DescriptorBindingState::Create(
        sessionDescriptor,
        resourcePolicy);
    if (!stateResult)
    {
        return;
    }
    auto state = std::move(stateResult).Value();

    constexpr std::size_t bytesPerOperation = 4;
    constexpr std::size_t maximumOperations = 16;
    const std::size_t operationCount = std::min(
        input.size() / bytesPerOperation,
        maximumOperations);
    for (std::size_t operationIndex = 0;
         operationIndex < operationCount;
         operationIndex++)
    {
        const std::size_t inputOffset = operationIndex * bytesPerOperation;
        const std::uint64_t segmentOrdinal =
            std::to_integer<std::uint8_t>(input[inputOffset]) % 8ULL;
        const std::uint64_t rawOffset =
            std::to_integer<std::uint8_t>(input[inputOffset + 1]) % 64ULL;
        const std::uint64_t rawSize = 1ULL +
            (std::to_integer<std::uint8_t>(input[inputOffset + 2]) % 16ULL);
        const std::uint32_t outerBlockBytes = 1U +
            (std::to_integer<std::uint8_t>(input[inputOffset + 3]) % 16U);
        const pbprotocol::SegmentDescriptor descriptor =
            MakeGeneratedSegmentDescriptor(
                sessionDescriptor,
                segmentOrdinal,
                rawOffset,
                rawSize,
                outerBlockBytes);
        static_cast<void>(state.BindSegmentDescriptor(descriptor));
        if (state.HasTerminalError())
        {
            break;
        }
    }

    static_cast<void>(state.ValidateCompleteSegmentMap());
}

void ExerciseInput(const std::span<const std::byte> input)
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        MakeFuzzResourcePolicy();
    const pbprotocol::SessionDescriptor fixedSession =
        MakeFixedSessionDescriptor();

    const auto parsedSession = pbprotocol::ParseSessionDescriptor(
        input,
        resourcePolicy);
    const auto parsedSegment = pbprotocol::ParseSegmentDescriptor(
        input,
        fixedSession,
        resourcePolicy);
    const auto parsedManifest = pbprotocol::ParseFinalManifest(
        input,
        fixedSession,
        resourcePolicy);

    auto registryResult = pbprotocol::SessionRegistry::Create(resourcePolicy);
    if (registryResult)
    {
        auto registry = std::move(registryResult).Value();
        if (parsedSession)
        {
            static_cast<void>(registry.BindSessionDescriptor(
                parsedSession.Value()));
        }
        static_cast<void>(registry.BindSessionDescriptor(fixedSession));
        if (parsedSegment)
        {
            static_cast<void>(registry.BindSegmentDescriptor(
                parsedSegment.Value()));
        }
        if (parsedManifest)
        {
            static_cast<void>(registry.BindFinalManifest(
                parsedManifest.Value()));
        }
    }

    ExerciseDescriptorState(input, fixedSession, resourcePolicy);
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
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeFixedSessionDescriptor();
    const pbprotocol::SegmentDescriptor segmentDescriptor =
        MakeGeneratedSegmentDescriptor(sessionDescriptor, 0, 0, 8, 8);
    const pbprotocol::FinalManifest finalManifest{
        sessionDescriptor.sessionId,
        sessionDescriptor.originalFileSize,
        sessionDescriptor.segmentCount,
        pbprotocol::WholeFileDigest{},
        sessionDescriptor.digestAlgorithm};

    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes>
        sessionSeed{};
    std::array<
        std::byte,
        pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes> segmentSeed{};
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> manifestSeed{};
    if (!pbprotocol::SerializeSessionDescriptor(sessionDescriptor, sessionSeed) ||
        !pbprotocol::SerializeSegmentDescriptor(
            segmentDescriptor,
            sessionDescriptor,
            segmentSeed) ||
        !pbprotocol::SerializeFinalManifest(
            finalManifest,
            sessionDescriptor,
            manifestSeed) ||
        !pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy))
    {
        std::cerr << "FUZZ_SETUP_FAILED\n";
        return 2;
    }

    std::uint64_t randomState = initialSeed == 0 ? kDefaultSeed : initialSeed;
    std::array<std::byte, kMaximumGeneratedInputBytes> input{};
    for (std::uint64_t iteration = 0; iteration < iterations; iteration++)
    {
        std::size_t inputSize = 0;
        switch (NextRandom(randomState) % 4ULL)
        {
        case 0:
            CopySeed(sessionSeed, input, inputSize);
            break;
        case 1:
            CopySeed(segmentSeed, input, inputSize);
            break;
        case 2:
            CopySeed(manifestSeed, input, inputSize);
            break;
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
            std::cerr << "CORPUS_REPLAY_READ_FAILED path="
                      << inputPath << '\n';
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
        std::cerr << "usage: PBProtocolDescriptorResourceFuzz "
                     "[iterations] [seed]\n"
                     "       PBProtocolDescriptorResourceFuzz "
                     "--input <corpus-file>\n";
        return 2;
    }
    if (argumentCount > 2 && !ParseUint64(arguments[2], seed))
    {
        std::cerr << "usage: PBProtocolDescriptorResourceFuzz "
                     "[iterations] [seed]\n"
                     "       PBProtocolDescriptorResourceFuzz "
                     "--input <corpus-file>\n";
        return 2;
    }
    if (argumentCount > 3 || iterations == 0)
    {
        std::cerr << "usage: PBProtocolDescriptorResourceFuzz "
                     "[iterations] [seed]\n"
                     "       PBProtocolDescriptorResourceFuzz "
                     "--input <corpus-file>\n";
        return 2;
    }

    return RunMutationLoop(iterations, seed);
}

#endif
