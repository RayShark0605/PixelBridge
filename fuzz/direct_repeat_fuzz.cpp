#include "pbouterfec/direct_repeat.h"

#include "pbprotocol/blake3_digest.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

constexpr std::size_t kMaximumMessageBytes = 128;
constexpr std::uint32_t kMaximumOuterBlockBytes = 32;
constexpr std::size_t kMaximumOperations = 32;
constexpr std::size_t kMaximumGeneratedInputBytes = 512;

struct CanonicalBlock
{
    std::uint32_t payloadBytes = 0;
    std::vector<std::byte> paddedPayload;
};

struct DirectRepeatOracle
{
    std::vector<std::optional<std::vector<std::byte>>> acceptedPayloads;
    std::size_t receivedBlockCount = 0;
    bool ready = false;
    bool terminal = false;
};

[[noreturn]] void FailInvariant()
{
    std::abort();
}

[[nodiscard]] std::uint8_t GetByte(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    if (input.empty())
    {
        return static_cast<std::uint8_t>(offset * 29U + 7U);
    }
    return std::to_integer<std::uint8_t>(input[offset % input.size()]);
}

[[nodiscard]] std::uint32_t ReadUint32(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t byteIndex = 0; byteIndex < sizeof(value); byteIndex++)
    {
        value |= static_cast<std::uint32_t>(GetByte(
            input, offset + byteIndex)) <<
            static_cast<unsigned int>(byteIndex * 8U);
    }
    return value;
}

[[nodiscard]] std::vector<std::byte> MakeMessage(
    const std::span<const std::byte> input)
{
    const std::size_t messageBytes = 1U +
        (GetByte(input, 0) % kMaximumMessageBytes);
    std::vector<std::byte> message(messageBytes);
    for (std::size_t byteIndex = 0;
        byteIndex < message.size();
        byteIndex++)
    {
        message[byteIndex] = static_cast<std::byte>(
            GetByte(input, byteIndex + 3U) ^
            static_cast<std::uint8_t>(byteIndex * 73U + 19U));
    }
    return message;
}

[[nodiscard]] pbprotocol::ReceiverResourcePolicy MakeResourcePolicy() noexcept
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxEncodedSegmentBytes = kMaximumMessageBytes;
    resourcePolicy.maxOuterBlockBytes = kMaximumOuterBlockBytes;
    resourcePolicy.maxDirectRepeatBlockCount = kMaximumMessageBytes;
    resourcePolicy.maxActiveOuterFecDecoders = 1;
    resourcePolicy.maxOuterFecDecoderBytes = 1024ULL * 1024ULL;
    resourcePolicy.maxTotalOuterFecDecoderBytes =
        resourcePolicy.maxOuterFecDecoderBytes;
    return resourcePolicy;
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeDescriptor(
    const std::span<const std::byte> message,
    const std::uint32_t outerBlockBytes)
{
    pbprotocol::SegmentDescriptor descriptor{};
    descriptor.encodedSize = message.size();
    descriptor.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    descriptor.outerBlockBytes = outerBlockBytes;
    descriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::ComputeBlake3Digest(message)};
    return descriptor;
}

[[nodiscard]] CanonicalBlock EncodeCanonicalBlock(
    pbouterfec::DirectRepeatEncoder& encoder,
    const std::uint32_t outerBlockId)
{
    CanonicalBlock block;
    block.paddedPayload.resize(encoder.GetOuterBlockBytes());
    const auto encodeResult = encoder.EncodeBlock(
        outerBlockId, block.paddedPayload);
    if (!encodeResult)
    {
        FailInvariant();
    }
    block.payloadBytes = encodeResult.Value();
    return block;
}

[[nodiscard]] std::uint32_t GetExpectedPayloadBytes(
    const std::size_t encodedSize,
    const std::uint32_t outerBlockBytes,
    const std::uint32_t outerBlockId) noexcept
{
    const std::size_t blockOffset =
        static_cast<std::size_t>(outerBlockId) * outerBlockBytes;
    const std::size_t remainingBytes = encodedSize - blockOffset;
    return static_cast<std::uint32_t>(std::min(
        remainingBytes,
        static_cast<std::size_t>(outerBlockBytes)));
}

[[nodiscard]] bool HasCanonicalPadding(
    const std::span<const std::byte> paddedPayload,
    const std::uint32_t expectedPayloadBytes) noexcept
{
    for (std::size_t paddingIndex = expectedPayloadBytes;
        paddingIndex < paddedPayload.size();
        paddingIndex++)
    {
        if (paddedPayload[paddingIndex] != std::byte{0})
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<std::byte> ReassembleOracle(
    const DirectRepeatOracle& oracle,
    const std::size_t encodedSize,
    const std::uint32_t outerBlockBytes)
{
    std::vector<std::byte> encodedBytes(encodedSize);
    for (std::size_t blockIndex = 0;
        blockIndex < oracle.acceptedPayloads.size();
        blockIndex++)
    {
        if (!oracle.acceptedPayloads[blockIndex].has_value())
        {
            FailInvariant();
        }
        const std::vector<std::byte>& payload =
            oracle.acceptedPayloads[blockIndex].value();
        const std::size_t blockOffset = blockIndex * outerBlockBytes;
        std::copy(
            payload.begin(),
            payload.end(),
            encodedBytes.begin() + blockOffset);
    }
    return encodedBytes;
}

void CheckDecodeOperation(
    pbouterfec::DirectRepeatDecoder& decoder,
    DirectRepeatOracle& oracle,
    const std::span<const std::byte> expectedEncodedBytes,
    const std::uint32_t outerBlockBytes,
    const std::uint32_t outerBlockId,
    const std::uint32_t payloadBytes,
    const std::span<const std::byte> paddedPayload)
{
    const auto decodeResult = decoder.DecodeBlock(
        outerBlockId, payloadBytes, paddedPayload);
    if (oracle.terminal)
    {
        if (decodeResult || decodeResult.Error().code !=
            pbouterfec::OuterFecErrorCode::InvalidState)
        {
            FailInvariant();
        }
        return;
    }

    const bool validOrdinal =
        static_cast<std::uint64_t>(outerBlockId) <
        oracle.acceptedPayloads.size();
    const bool validRegionSize = paddedPayload.size() == outerBlockBytes;
    std::uint32_t expectedPayloadBytes = 0;
    bool canonicalPadding = false;
    if (validOrdinal && validRegionSize)
    {
        expectedPayloadBytes = GetExpectedPayloadBytes(
            expectedEncodedBytes.size(), outerBlockBytes, outerBlockId);
        canonicalPadding = HasCanonicalPadding(
            paddedPayload, expectedPayloadBytes);
    }
    const bool structurallyValid = validOrdinal && validRegionSize &&
        payloadBytes == expectedPayloadBytes && canonicalPadding;
    if (!structurallyValid)
    {
        if (decodeResult || decodeResult.Error().code !=
            pbouterfec::OuterFecErrorCode::InvalidInput)
        {
            FailInvariant();
        }
        oracle.terminal = true;
        return;
    }

    const std::vector<std::byte> realPayload(
        paddedPayload.begin(),
        paddedPayload.begin() + expectedPayloadBytes);
    std::optional<std::vector<std::byte>>& acceptedPayload =
        oracle.acceptedPayloads[outerBlockId];
    if (acceptedPayload.has_value())
    {
        if (acceptedPayload.value() != realPayload)
        {
            if (decodeResult || decodeResult.Error().code !=
                pbouterfec::OuterFecErrorCode::OuterBlockConflict)
            {
                FailInvariant();
            }
            oracle.terminal = true;
            return;
        }
        if (!decodeResult || decodeResult.Value() !=
            (oracle.ready
                ? pbouterfec::DecodeDisposition::Ready
                : pbouterfec::DecodeDisposition::NeedMore))
        {
            FailInvariant();
        }
        return;
    }

    acceptedPayload = realPayload;
    oracle.receivedBlockCount++;
    if (oracle.receivedBlockCount != oracle.acceptedPayloads.size())
    {
        if (!decodeResult || decodeResult.Value() !=
            pbouterfec::DecodeDisposition::NeedMore)
        {
            FailInvariant();
        }
        return;
    }

    const std::vector<std::byte> reassembled = ReassembleOracle(
        oracle, expectedEncodedBytes.size(), outerBlockBytes);
    if (!std::equal(
            reassembled.begin(),
            reassembled.end(),
            expectedEncodedBytes.begin(),
            expectedEncodedBytes.end()))
    {
        if (decodeResult || decodeResult.Error().code !=
            pbouterfec::OuterFecErrorCode::EncodedDigestMismatch)
        {
            FailInvariant();
        }
        oracle.terminal = true;
        return;
    }

    if (!decodeResult || decodeResult.Value() !=
        pbouterfec::DecodeDisposition::Ready)
    {
        FailInvariant();
    }
    oracle.ready = true;
}

void ExerciseCanonicalRoundTrip(
    pbouterfec::DirectRepeatEncoder& encoder,
    const pbprotocol::SegmentDescriptor& descriptor,
    const std::span<const std::byte> message,
    const pbouterfec::OuterFecDecoderResourceManager& resourceManager)
{
    auto decoderResult = pbouterfec::DirectRepeatDecoder::Create(
        descriptor, descriptor.outerBlockBytes, resourceManager);
    if (!decoderResult)
    {
        FailInvariant();
    }
    pbouterfec::DirectRepeatDecoder decoder =
        std::move(decoderResult).Value();

    std::vector<std::byte> prematureOutput(message.size(), std::byte{0x5A});
    const std::vector<std::byte> originalPrematureOutput = prematureOutput;
    if (decoder.Recover(prematureOutput) ||
        prematureOutput != originalPrematureOutput)
    {
        FailInvariant();
    }

    const std::uint64_t blockCount = encoder.GetBlockCount();
    DirectRepeatOracle oracle;
    oracle.acceptedPayloads.resize(static_cast<std::size_t>(blockCount));
    for (std::uint64_t reverseIndex = blockCount;
        reverseIndex > 0;
        reverseIndex--)
    {
        const std::uint32_t outerBlockId = static_cast<std::uint32_t>(
            reverseIndex - 1ULL);
        const CanonicalBlock block = EncodeCanonicalBlock(
            encoder, outerBlockId);
        CheckDecodeOperation(
            decoder,
            oracle,
            message,
            descriptor.outerBlockBytes,
            outerBlockId,
            block.payloadBytes,
            block.paddedPayload);
        if (reverseIndex == blockCount)
        {
            CheckDecodeOperation(
                decoder,
                oracle,
                message,
                descriptor.outerBlockBytes,
                outerBlockId,
                block.payloadBytes,
                block.paddedPayload);
        }
    }
    if (!oracle.ready || oracle.terminal)
    {
        FailInvariant();
    }

    const CanonicalBlock firstBlock = EncodeCanonicalBlock(encoder, 0);
    CheckDecodeOperation(
        decoder,
        oracle,
        message,
        descriptor.outerBlockBytes,
        0,
        firstBlock.payloadBytes,
        firstBlock.paddedPayload);

    std::vector<std::byte> recovered(message.size() + 3U, std::byte{0xA5});
    const auto recoverResult = decoder.Recover(recovered);
    if (!recoverResult || recoverResult.Value() != message.size() ||
        !std::equal(
            message.begin(), message.end(), recovered.begin()) ||
        !std::all_of(
            recovered.begin() + message.size(),
            recovered.end(),
            [](const std::byte value)
            {
                return value == std::byte{0xA5};
            }))
    {
        FailInvariant();
    }
}

void ExerciseMutatedSequence(
    const std::span<const std::byte> input,
    pbouterfec::DirectRepeatEncoder& encoder,
    const pbprotocol::SegmentDescriptor& descriptor,
    const std::span<const std::byte> message,
    const pbouterfec::OuterFecDecoderResourceManager& resourceManager)
{
    auto decoderResult = pbouterfec::DirectRepeatDecoder::Create(
        descriptor, descriptor.outerBlockBytes, resourceManager);
    if (!decoderResult)
    {
        FailInvariant();
    }
    pbouterfec::DirectRepeatDecoder decoder =
        std::move(decoderResult).Value();

    DirectRepeatOracle oracle;
    oracle.acceptedPayloads.resize(
        static_cast<std::size_t>(encoder.GetBlockCount()));
    const std::size_t operationCount = 1U +
        (GetByte(input, 2) % kMaximumOperations);
    for (std::size_t operationIndex = 0;
        operationIndex < operationCount && !oracle.terminal;
        operationIndex++)
    {
        const std::size_t controlOffset = 11U + operationIndex * 11U;
        std::uint32_t outerBlockId = ReadUint32(input, controlOffset);
        switch (GetByte(input, controlOffset + 4U) % 6U)
        {
        case 0:
            outerBlockId = static_cast<std::uint32_t>(
                operationIndex % oracle.acceptedPayloads.size());
            break;
        case 1:
            outerBlockId = static_cast<std::uint32_t>(
                oracle.acceptedPayloads.size() - 1U);
            break;
        case 2:
            outerBlockId = static_cast<std::uint32_t>(
                oracle.acceptedPayloads.size());
            break;
        case 3:
            outerBlockId = std::numeric_limits<std::uint32_t>::max();
            break;
        case 4:
            outerBlockId %= static_cast<std::uint32_t>(
                oracle.acceptedPayloads.size());
            break;
        default:
            outerBlockId = 0;
            break;
        }

        const bool validOrdinal =
            static_cast<std::uint64_t>(outerBlockId) <
            oracle.acceptedPayloads.size();
        CanonicalBlock block;
        if (validOrdinal)
        {
            block = EncodeCanonicalBlock(encoder, outerBlockId);
        }
        else
        {
            block.payloadBytes = descriptor.outerBlockBytes;
            block.paddedPayload.resize(
                descriptor.outerBlockBytes, std::byte{0});
        }

        const std::uint8_t contentFlags = GetByte(
            input, controlOffset + 5U);
        if ((contentFlags & 1U) != 0 &&
            validOrdinal && block.payloadBytes > 0)
        {
            const std::size_t payloadIndex =
                GetByte(input, controlOffset + 6U) % block.payloadBytes;
            block.paddedPayload[payloadIndex] ^= std::byte{0x01};
        }
        if ((contentFlags & 2U) != 0 &&
            validOrdinal &&
            block.payloadBytes < block.paddedPayload.size())
        {
            const std::size_t paddingBytes =
                block.paddedPayload.size() - block.payloadBytes;
            const std::size_t paddingIndex = block.payloadBytes +
                (GetByte(input, controlOffset + 7U) % paddingBytes);
            block.paddedPayload[paddingIndex] ^= std::byte{0x80};
        }

        std::uint32_t payloadBytes = block.payloadBytes;
        switch (GetByte(input, controlOffset + 8U) % 4U)
        {
        case 1:
            payloadBytes = payloadBytes == 0 ? 0 : payloadBytes - 1U;
            break;
        case 2:
            payloadBytes++;
            break;
        case 3:
            payloadBytes = ReadUint32(input, controlOffset + 9U);
            break;
        default:
            break;
        }

        switch (GetByte(input, controlOffset + 10U) % 4U)
        {
        case 1:
            block.paddedPayload.resize(
                block.paddedPayload.size() - 1U);
            break;
        case 2:
            block.paddedPayload.push_back(std::byte{0});
            break;
        default:
            break;
        }

        CheckDecodeOperation(
            decoder,
            oracle,
            message,
            descriptor.outerBlockBytes,
            outerBlockId,
            payloadBytes,
            block.paddedPayload);
        if (!oracle.terminal && (contentFlags & 4U) != 0)
        {
            CheckDecodeOperation(
                decoder,
                oracle,
                message,
                descriptor.outerBlockBytes,
                outerBlockId,
                payloadBytes,
                block.paddedPayload);
        }
        if (!oracle.terminal && (contentFlags & 8U) != 0 &&
            validOrdinal && block.payloadBytes > 0 &&
            block.paddedPayload.size() == descriptor.outerBlockBytes &&
            payloadBytes == block.payloadBytes &&
            HasCanonicalPadding(block.paddedPayload, block.payloadBytes))
        {
            std::vector<std::byte> conflict = block.paddedPayload;
            conflict[0] ^= std::byte{0x40};
            CheckDecodeOperation(
                decoder,
                oracle,
                message,
                descriptor.outerBlockBytes,
                outerBlockId,
                payloadBytes,
                conflict);
        }
    }

    if (!oracle.terminal && !oracle.ready)
    {
        for (std::size_t blockIndex = 0;
            blockIndex < oracle.acceptedPayloads.size() &&
                !oracle.terminal && !oracle.ready;
            blockIndex++)
        {
            if (oracle.acceptedPayloads[blockIndex].has_value())
            {
                continue;
            }
            const std::uint32_t outerBlockId = static_cast<std::uint32_t>(
                blockIndex);
            const CanonicalBlock block = EncodeCanonicalBlock(
                encoder, outerBlockId);
            CheckDecodeOperation(
                decoder,
                oracle,
                message,
                descriptor.outerBlockBytes,
                outerBlockId,
                block.payloadBytes,
                block.paddedPayload);
        }
    }

    if (oracle.terminal)
    {
        const CanonicalBlock block = EncodeCanonicalBlock(encoder, 0);
        CheckDecodeOperation(
            decoder,
            oracle,
            message,
            descriptor.outerBlockBytes,
            0,
            block.payloadBytes,
            block.paddedPayload);
        std::vector<std::byte> output(message.size(), std::byte{0x5A});
        if (decoder.Recover(output))
        {
            FailInvariant();
        }
        return;
    }

    if (!oracle.ready)
    {
        FailInvariant();
    }
    std::vector<std::byte> recovered(message.size());
    const auto recoverResult = decoder.Recover(recovered);
    if (!recoverResult || recovered !=
        std::vector<std::byte>(message.begin(), message.end()))
    {
        FailInvariant();
    }
}

void ExerciseInput(const std::span<const std::byte> input)
{
    const std::uint32_t outerBlockBytes = 1U +
        (GetByte(input, 1) % kMaximumOuterBlockBytes);

    const std::span<const std::byte> emptyMessage;
    auto emptyEncoderResult = pbouterfec::DirectRepeatEncoder::Create(
        emptyMessage, outerBlockBytes);
    if (!emptyEncoderResult ||
        emptyEncoderResult.Value().GetBlockCount() != 0)
    {
        FailInvariant();
    }
    std::vector<std::byte> unchangedOutput(
        outerBlockBytes, std::byte{0xA5});
    const std::vector<std::byte> originalOutput = unchangedOutput;
    if (emptyEncoderResult.Value().EncodeBlock(0, unchangedOutput) ||
        unchangedOutput != originalOutput)
    {
        FailInvariant();
    }

    const std::vector<std::byte> message = MakeMessage(input);
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(
        message, outerBlockBytes);
    if (!encoderResult)
    {
        FailInvariant();
    }
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    const pbprotocol::SegmentDescriptor descriptor = MakeDescriptor(
        message, outerBlockBytes);

    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        MakeResourcePolicy();
    auto managerResult =
        pbouterfec::OuterFecDecoderResourceManager::Create(resourcePolicy);
    if (!managerResult)
    {
        FailInvariant();
    }
    pbouterfec::OuterFecDecoderResourceManager resourceManager =
        std::move(managerResult).Value();

    {
        const std::uint32_t mismatchedBlockBytes =
            outerBlockBytes == kMaximumOuterBlockBytes
                ? outerBlockBytes - 1U
                : outerBlockBytes + 1U;
        const auto mismatchResult = pbouterfec::DirectRepeatDecoder::Create(
            descriptor, mismatchedBlockBytes, resourceManager);
        if (mismatchResult || mismatchResult.Error().code !=
            pbouterfec::OuterFecErrorCode::OuterBlockBytesMismatch ||
            resourceManager.GetActiveDecoderCount() != 0 ||
            resourceManager.GetReservedDecoderBytes() != 0)
        {
            FailInvariant();
        }
    }

    std::vector<std::byte> recreationBytes = message;
    recreationBytes[GetByte(input, 4) % recreationBytes.size()] ^=
        std::byte{0x01};
    if (pbouterfec::DirectRepeatEncoder::Recreate(
            recreationBytes, descriptor))
    {
        FailInvariant();
    }

    {
        ExerciseCanonicalRoundTrip(
            encoder, descriptor, message, resourceManager);
    }
    if (resourceManager.GetActiveDecoderCount() != 0 ||
        resourceManager.GetReservedDecoderBytes() != 0)
    {
        FailInvariant();
    }

    {
        ExerciseMutatedSequence(
            input, encoder, descriptor, message, resourceManager);
    }
    if (resourceManager.GetActiveDecoderCount() != 0 ||
        resourceManager.GetReservedDecoderBytes() != 0)
    {
        FailInvariant();
    }
}

#if !defined(PB_USE_LIBFUZZER)

constexpr std::uint64_t kDefaultIterations = 2000;
constexpr std::uint64_t kDefaultSeed = 0x4452505446555A5AULL;

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
        text.data(), text.data() + text.size(), value);
    return parseResult.ec == std::errc{} &&
        parseResult.ptr == text.data() + text.size();
}

int RunMutationLoop(
    const std::uint64_t iterations,
    const std::uint64_t initialSeed)
{
    std::uint64_t randomState = initialSeed == 0
        ? kDefaultSeed
        : initialSeed;
    std::array<std::byte, kMaximumGeneratedInputBytes> input{};
    for (std::uint64_t iteration = 0;
        iteration < iterations;
        iteration++)
    {
        const std::size_t inputSize = static_cast<std::size_t>(
            NextRandom(randomState) % (input.size() + 1ULL));
        for (std::size_t byteIndex = 0;
            byteIndex < inputSize;
            byteIndex++)
        {
            input[byteIndex] = static_cast<std::byte>(
                NextRandom(randomState) & 0xFFULL);
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
    inputFile.read(input.data(), static_cast<std::streamsize>(input.size()));
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
        if (inputFile.gcount() != 0 || inputFile.bad())
        {
            std::cerr << "CORPUS_REPLAY_INPUT_TOO_LARGE path="
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
        std::cerr << "usage: PBOuterFecDirectRepeatFuzz "
                     "[iterations] [seed]\n"
                     "       PBOuterFecDirectRepeatFuzz "
                     "--input <corpus-file>\n";
        return 2;
    }
    if (argumentCount > 2 && !ParseUint64(arguments[2], seed))
    {
        std::cerr << "usage: PBOuterFecDirectRepeatFuzz "
                     "[iterations] [seed]\n"
                     "       PBOuterFecDirectRepeatFuzz "
                     "--input <corpus-file>\n";
        return 2;
    }
    if (argumentCount > 3 || iterations == 0)
    {
        std::cerr << "usage: PBOuterFecDirectRepeatFuzz "
                     "[iterations] [seed]\n"
                     "       PBOuterFecDirectRepeatFuzz "
                     "--input <corpus-file>\n";
        return 2;
    }
    return RunMutationLoop(iterations, seed);
}

#endif
