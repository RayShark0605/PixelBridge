#include "pbouterfec/direct_repeat.h"
#include "pbouterfec/wirehair_v2.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/descriptor_codec.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

constexpr std::uint32_t kOuterBlockBytes = 1024;
constexpr std::uint64_t kDefaultIterations = 100;
constexpr std::uint64_t kMaximumIterations = 10000;
constexpr std::array<std::size_t, 8> kMessageSizes{
    1,
    kOuterBlockBytes,
    kOuterBlockBytes + 1U,
    2U * kOuterBlockBytes,
    2U * kOuterBlockBytes + 1U,
    4U * kOuterBlockBytes,
    8U * kOuterBlockBytes,
    16U * kOuterBlockBytes};

struct BenchmarkResult
{
    double elapsedSeconds = 0;
    std::uint64_t maximumReservedBytes = 0;
};

[[nodiscard]] bool ParseIterations(
    const std::string_view text,
    std::uint64_t& iterations) noexcept
{
    const auto parseResult = std::from_chars(
        text.data(), text.data() + text.size(), iterations);
    return parseResult.ec == std::errc{} &&
        parseResult.ptr == text.data() + text.size() &&
        iterations > 0 && iterations <= kMaximumIterations;
}

[[nodiscard]] std::vector<std::byte> MakeMessage(
    const std::size_t messageBytes)
{
    std::vector<std::byte> message(messageBytes);
    for (std::size_t byteIndex = 0;
        byteIndex < message.size();
        byteIndex++)
    {
        message[byteIndex] = static_cast<std::byte>(
            static_cast<std::uint8_t>(byteIndex * 73U + 19U));
    }
    return message;
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeDirectDescriptor(
    const std::span<const std::byte> message)
{
    pbprotocol::SegmentDescriptor descriptor{};
    descriptor.encodedSize = message.size();
    descriptor.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    descriptor.outerBlockBytes = kOuterBlockBytes;
    descriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::ComputeBlake3Digest(message)};
    return descriptor;
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeWirehairDescriptor(
    const std::span<const std::byte> message,
    const pbprotocol::WirehairV2SerializedProfile& serializedProfile)
{
    pbprotocol::SegmentDescriptor descriptor{};
    descriptor.encodedSize = message.size();
    descriptor.outerFecMode = pbprotocol::OuterFecMode::WirehairV2;
    descriptor.outerBlockBytes = kOuterBlockBytes;
    descriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::ComputeBlake3Digest(message)};
    descriptor.wirehairV2SerializedProfile = serializedProfile;
    return descriptor;
}

[[nodiscard]] pbprotocol::ReceiverResourcePolicy MakeResourcePolicy() noexcept
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxDirectRepeatBlockCount = 64;
    return resourcePolicy;
}

[[nodiscard]] bool RunDirectBenchmark(
    const std::span<const std::byte> message,
    const std::uint64_t iterations,
    BenchmarkResult& benchmarkResult)
{
    const pbprotocol::SegmentDescriptor descriptor =
        MakeDirectDescriptor(message);
    auto managerResult =
        pbouterfec::OuterFecDecoderResourceManager::Create(
            MakeResourcePolicy());
    if (!managerResult)
    {
        return false;
    }
    pbouterfec::OuterFecDecoderResourceManager resourceManager =
        std::move(managerResult).Value();

    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t iteration = 0;
        iteration < iterations;
        iteration++)
    {
        auto encoderResult = pbouterfec::DirectRepeatEncoder::Recreate(
            message, descriptor);
        auto decoderResult = pbouterfec::DirectRepeatDecoder::Create(
            descriptor, kOuterBlockBytes, resourceManager);
        if (!encoderResult || !decoderResult)
        {
            return false;
        }
        pbouterfec::DirectRepeatEncoder encoder =
            std::move(encoderResult).Value();
        pbouterfec::DirectRepeatDecoder decoder =
            std::move(decoderResult).Value();
        benchmarkResult.maximumReservedBytes = std::max(
            benchmarkResult.maximumReservedBytes,
            resourceManager.GetReservedDecoderBytes());

        for (std::uint64_t blockIndex = 0;
            blockIndex < encoder.GetBlockCount();
            blockIndex++)
        {
            std::vector<std::byte> paddedPayload(kOuterBlockBytes);
            const std::uint32_t outerBlockId =
                static_cast<std::uint32_t>(blockIndex);
            const auto encodeResult = encoder.EncodeBlock(
                outerBlockId, paddedPayload);
            if (!encodeResult)
            {
                return false;
            }
            const auto decodeResult = decoder.DecodeBlock(
                outerBlockId,
                encodeResult.Value(),
                paddedPayload);
            if (!decodeResult)
            {
                return false;
            }
        }

        std::vector<std::byte> recovered(message.size());
        const auto recoverResult = decoder.Recover(recovered);
        if (!recoverResult || !std::equal(
                recovered.begin(), recovered.end(), message.begin()))
        {
            return false;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    benchmarkResult.elapsedSeconds =
        std::chrono::duration<double>(end - start).count();
    return resourceManager.GetActiveDecoderCount() == 0 &&
        resourceManager.GetReservedDecoderBytes() == 0;
}

[[nodiscard]] bool RunWirehairBenchmark(
    const std::span<const std::byte> message,
    const std::uint64_t iterations,
    BenchmarkResult& benchmarkResult)
{
    auto initialEncoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, kOuterBlockBytes);
    if (!initialEncoderResult)
    {
        return false;
    }
    const pbprotocol::SegmentDescriptor descriptor =
        MakeWirehairDescriptor(
            message,
            initialEncoderResult.Value().GetSerializedProfile());

    auto managerResult =
        pbouterfec::OuterFecDecoderResourceManager::Create(
            MakeResourcePolicy());
    if (!managerResult)
    {
        return false;
    }
    pbouterfec::OuterFecDecoderResourceManager resourceManager =
        std::move(managerResult).Value();

    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t iteration = 0;
        iteration < iterations;
        iteration++)
    {
        auto encoderResult = pbouterfec::WirehairV2Encoder::Recreate(
            message, descriptor);
        auto decoderResult = pbouterfec::WirehairV2Decoder::Create(
            descriptor, resourceManager);
        if (!encoderResult || !decoderResult)
        {
            return false;
        }
        pbouterfec::WirehairV2Encoder encoder =
            std::move(encoderResult).Value();
        pbouterfec::WirehairV2Decoder decoder =
            std::move(decoderResult).Value();
        benchmarkResult.maximumReservedBytes = std::max(
            benchmarkResult.maximumReservedBytes,
            resourceManager.GetReservedDecoderBytes());

        const std::uint32_t blockCount = encoder.GetBlockCount();
        for (std::uint32_t outerBlockId = 0;
            outerBlockId < blockCount;
            outerBlockId++)
        {
            std::vector<std::byte> payload(kOuterBlockBytes);
            const auto encodeResult = encoder.EncodeBlock(
                outerBlockId, payload);
            if (!encodeResult)
            {
                return false;
            }
            payload.resize(encodeResult.Value());
            const auto decodeResult = decoder.DecodeBlock(
                outerBlockId, payload);
            if (!decodeResult)
            {
                return false;
            }
        }

        std::vector<std::byte> recovered(message.size());
        const auto recoverResult = decoder.Recover(recovered);
        if (!recoverResult || !std::equal(
                recovered.begin(), recovered.end(), message.begin()))
        {
            return false;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    benchmarkResult.elapsedSeconds =
        std::chrono::duration<double>(end - start).count();
    return resourceManager.GetActiveDecoderCount() == 0 &&
        resourceManager.GetReservedDecoderBytes() == 0;
}

[[nodiscard]] double GetMicrosecondsPerIteration(
    const BenchmarkResult& benchmarkResult,
    const std::uint64_t iterations) noexcept
{
    return benchmarkResult.elapsedSeconds * 1000000.0 /
        static_cast<double>(iterations);
}

} // namespace

int main(const int argumentCount, char* arguments[])
{
    std::uint64_t iterations = kDefaultIterations;
    if (argumentCount > 2 ||
        (argumentCount == 2 &&
            !ParseIterations(arguments[1], iterations)))
    {
        std::cerr << "usage: PBOuterFecDirectRepeatBenchmark "
                     "[iterations:1..10000]\n";
        return 2;
    }

    std::cout << "BENCHMARK_STARTED\n"
              << "iterations=" << iterations << '\n'
              << "outer_block_bytes=" << kOuterBlockBytes << '\n'
              << "default_direct_repeat_efficiency_block_count="
              << pbouterfec::kDefaultDirectRepeatEfficiencyBlockCount << '\n'
              << "columns=message_bytes,block_count,selected_mode,"
                 "direct_us,wirehair_us,direct_reserved,wirehair_reserved\n";

    for (const std::size_t messageBytes : kMessageSizes)
    {
        const std::vector<std::byte> message = MakeMessage(messageBytes);
        const auto countResult = pbprotocol::GetDirectRepeatBlockCount(
            messageBytes, kOuterBlockBytes);
        const auto modeResult = pbouterfec::ChooseOuterFecMode(
            messageBytes, kOuterBlockBytes);
        if (!countResult || !modeResult)
        {
            std::cerr << "BENCHMARK_SELECTION_FAILED message_bytes="
                      << messageBytes << '\n';
            return 1;
        }

        BenchmarkResult directResult;
        if (!RunDirectBenchmark(message, iterations, directResult))
        {
            std::cerr << "BENCHMARK_DIRECT_FAILED message_bytes="
                      << messageBytes << '\n';
            return 1;
        }

        std::cout << std::fixed << std::setprecision(3)
                  << messageBytes << ','
                  << countResult.Value() << ','
                  << (modeResult.Value() ==
                        pbprotocol::OuterFecMode::DirectRepeat
                        ? "DirectRepeat"
                        : "WirehairV2") << ','
                  << GetMicrosecondsPerIteration(directResult, iterations)
                  << ',';

        if (countResult.Value() <
            pbouterfec::kWirehairV2MinimumBlockCount)
        {
            std::cout << "NA," << directResult.maximumReservedBytes
                      << ",NA\n";
            continue;
        }

        BenchmarkResult wirehairResult;
        if (!RunWirehairBenchmark(message, iterations, wirehairResult))
        {
            std::cerr << "BENCHMARK_WIREHAIR_FAILED message_bytes="
                      << messageBytes << '\n';
            return 1;
        }
        std::cout << GetMicrosecondsPerIteration(wirehairResult, iterations)
                  << ',' << directResult.maximumReservedBytes
                  << ',' << wirehairResult.maximumReservedBytes << '\n';
    }

    std::cout << "BENCHMARK_COMPLETED\n";
    return 0;
}
