#include "pbouterfec/wirehair_v2.h"

#include "pbprotocol/blake3_digest.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaximumMessageBytes = 129;
constexpr std::size_t kMaximumOperations = 32;

[[nodiscard]] std::uint8_t GetByte(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    if (input.empty())
    {
        return static_cast<std::uint8_t>(offset);
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
    const std::size_t messageBytes = 2U +
        (GetByte(input, 0) % (kMaximumMessageBytes - 1U));
    std::vector<std::byte> message(messageBytes);
    for (std::size_t byteIndex = 0;
        byteIndex < message.size();
        byteIndex++)
    {
        message[byteIndex] = static_cast<std::byte>(
            GetByte(input, byteIndex + 2U) ^
            static_cast<std::uint8_t>(byteIndex * 73U + 19U));
    }
    return message;
}

[[nodiscard]] pbprotocol::ReceiverResourcePolicy MakeResourcePolicy() noexcept
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxEncodedSegmentBytes = 4096;
    resourcePolicy.maxOuterBlockBytes = 4096;
    resourcePolicy.maxActiveOuterFecDecoders = 1;
    resourcePolicy.maxOuterFecDecoderBytes = 16ULL * 1024ULL * 1024ULL;
    resourcePolicy.maxTotalOuterFecDecoderBytes =
        resourcePolicy.maxOuterFecDecoderBytes;
    return resourcePolicy;
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeDescriptor(
    const std::span<const std::byte> message,
    const std::uint32_t outerBlockBytes,
    const pbprotocol::WirehairV2SerializedProfile& serializedProfile)
{
    pbprotocol::SegmentDescriptor descriptor{};
    descriptor.encodedSize = message.size();
    descriptor.outerFecMode = pbprotocol::OuterFecMode::WirehairV2;
    descriptor.outerBlockBytes = outerBlockBytes;
    descriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::ComputeBlake3Digest(message)};
    descriptor.wirehairV2SerializedProfile = serializedProfile;
    return descriptor;
}

void ExerciseInput(const std::span<const std::byte> input)
{
    const std::vector<std::byte> message = MakeMessage(input);
    const std::uint32_t outerBlockBytes = 1U +
        (GetByte(input, 1) %
            static_cast<std::uint32_t>(message.size() - 1U));
    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, outerBlockBytes);
    if (!encoderResult)
    {
        return;
    }
    pbouterfec::WirehairV2Encoder encoder =
        std::move(encoderResult).Value();
    pbprotocol::SegmentDescriptor descriptor = MakeDescriptor(
        message, outerBlockBytes, encoder.GetSerializedProfile());

    const std::uint8_t descriptorSelector = GetByte(input, 3) % 8U;
    if (descriptorSelector == 1U)
    {
        descriptor.encodedSize++;
    }
    else if (descriptorSelector == 2U)
    {
        descriptor.outerBlockBytes++;
    }
    else if (descriptorSelector == 3U)
    {
        const std::size_t profileOffset =
            GetByte(input, 4) % pbprotocol::kWirehairV2SerializedProfileBytes;
        descriptor.wirehairV2SerializedProfile->bytes[profileOffset] ^=
            static_cast<std::byte>(GetByte(input, 5) | 1U);
    }

    auto managerResult =
        pbouterfec::WirehairV2DecoderResourceManager::Create(
            MakeResourcePolicy());
    if (!managerResult)
    {
        std::abort();
    }
    pbouterfec::WirehairV2DecoderResourceManager resourceManager =
        std::move(managerResult).Value();

    const bool useChangedRecreationBytes =
        (GetByte(input, 6) & 1U) != 0 && !message.empty();
    std::vector<std::byte> recreationBytes = message;
    if (useChangedRecreationBytes)
    {
        recreationBytes[GetByte(input, 7) % recreationBytes.size()] ^=
            std::byte{0x01};
    }
    const auto recreateResult = pbouterfec::WirehairV2Encoder::Recreate(
        recreationBytes, descriptor);
    if (useChangedRecreationBytes && recreateResult)
    {
        std::abort();
    }

    {
        auto decoderResult = pbouterfec::WirehairV2Decoder::Create(
            descriptor, resourceManager);
        if (!decoderResult)
        {
            if (resourceManager.GetActiveDecoderCount() != 0 ||
                resourceManager.GetReservedDecoderBytes() != 0)
            {
                std::abort();
            }
            return;
        }
        pbouterfec::WirehairV2Decoder decoder =
            std::move(decoderResult).Value();
        if (resourceManager.GetActiveDecoderCount() != 1 ||
            resourceManager.GetReservedDecoderBytes() == 0)
        {
            std::abort();
        }

        bool allAcceptedPayloadsAreEncoded =
            descriptor.encodedSize == message.size() &&
            descriptor.outerBlockBytes == outerBlockBytes &&
            descriptor.wirehairV2SerializedProfile.value() ==
                encoder.GetSerializedProfile();
        const std::size_t operationCount = 1U +
            (GetByte(input, 8) % kMaximumOperations);
        for (std::size_t operationIndex = 0;
            operationIndex < operationCount;
            operationIndex++)
        {
            const std::size_t controlOffset = 9U + operationIndex * 7U;
            std::uint32_t outerBlockId = ReadUint32(input, controlOffset);
            switch (GetByte(input, controlOffset + 4U) % 4U)
            {
            case 0:
                outerBlockId = static_cast<std::uint32_t>(operationIndex);
                break;
            case 1:
                outerBlockId = encoder.GetBlockCount() - 1U;
                break;
            case 2:
                outerBlockId = encoder.GetBlockCount() +
                    static_cast<std::uint32_t>(operationIndex);
                break;
            default:
                break;
            }

            const std::uint8_t operationFlags = GetByte(
                input, controlOffset + 5U);
            std::vector<std::byte> payload;
            if ((operationFlags & 1U) == 0)
            {
                payload.resize(outerBlockBytes);
                const auto encodeResult = encoder.EncodeBlock(
                    outerBlockId, payload);
                if (!encodeResult)
                {
                    break;
                }
                payload.resize(encodeResult.Value());
            }
            else
            {
                const std::size_t payloadBytes =
                    GetByte(input, controlOffset + 6U) %
                    (static_cast<std::size_t>(outerBlockBytes) + 1U);
                payload.resize(payloadBytes);
                for (std::size_t byteIndex = 0;
                    byteIndex < payload.size();
                    byteIndex++)
                {
                    payload[byteIndex] = static_cast<std::byte>(GetByte(
                        input, controlOffset + 7U + byteIndex));
                }
                allAcceptedPayloadsAreEncoded = false;
            }

            const auto decodeResult = decoder.DecodeBlock(
                outerBlockId, payload);
            if (!decodeResult)
            {
                static_cast<void>(decoder.DecodeBlock(
                    outerBlockId, payload));
                break;
            }

            if ((operationFlags & 2U) != 0)
            {
                const auto duplicateResult = decoder.DecodeBlock(
                    outerBlockId, payload);
                if (!duplicateResult ||
                    duplicateResult.Value() != decodeResult.Value())
                {
                    std::abort();
                }
            }
            if ((operationFlags & 4U) != 0 && !payload.empty())
            {
                std::vector<std::byte> conflict = payload;
                conflict[0] ^= std::byte{0x01};
                const auto conflictResult = decoder.DecodeBlock(
                    outerBlockId, conflict);
                if (conflictResult || conflictResult.Error().code !=
                    pbouterfec::OuterFecErrorCode::OuterBlockConflict)
                {
                    std::abort();
                }
                break;
            }

            if (decodeResult.Value() == pbouterfec::DecodeDisposition::Ready)
            {
                std::vector<std::byte> recovered(message.size());
                const auto recoverResult = decoder.Recover(recovered);
                if (!recoverResult ||
                    recoverResult.Value() != message.size() ||
                    (allAcceptedPayloadsAreEncoded && recovered != message))
                {
                    std::abort();
                }
                break;
            }
        }
    }

    if (resourceManager.GetActiveDecoderCount() != 0 ||
        resourceManager.GetReservedDecoderBytes() != 0)
    {
        std::abort();
    }
}

#if !defined(PB_USE_LIBFUZZER)

constexpr std::size_t kMaximumGeneratedInputBytes = 512;
constexpr std::uint64_t kDefaultIterations = 2000;
constexpr std::uint64_t kDefaultSeed = 0x5748563257524150ULL;

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
    std::uint64_t iterations = kDefaultIterations;
    std::uint64_t seed = kDefaultSeed;
    if (argumentCount > 1 && !ParseUint64(arguments[1], iterations))
    {
        std::cerr << "usage: PBOuterFecWirehairV2Fuzz "
                     "[iterations] [seed]\n";
        return 2;
    }
    if (argumentCount > 2 && !ParseUint64(arguments[2], seed))
    {
        std::cerr << "usage: PBOuterFecWirehairV2Fuzz "
                     "[iterations] [seed]\n";
        return 2;
    }
    if (argumentCount > 3 || iterations == 0)
    {
        std::cerr << "usage: PBOuterFecWirehairV2Fuzz "
                     "[iterations] [seed]\n";
        return 2;
    }
    return RunMutationLoop(iterations, seed);
}

#endif
