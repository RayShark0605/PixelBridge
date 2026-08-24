#include "pbouterfec/wirehair_v2.h"

#include "decoder_test_access.h"

#include "pbprotocol/blake3_digest.h"

#include <algorithm>
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

namespace {

constexpr std::uint64_t kMessageBytes = 256ULL * 1024ULL;
constexpr std::uint32_t kOuterBlockBytes = 1024;
constexpr std::uint64_t kDefaultIterations = 5;
constexpr std::uint64_t kMaximumIterations = 1000;

[[nodiscard]] bool ParseIterations(
    const std::string_view text,
    std::uint64_t& iterations) noexcept
{
    const auto parseResult = std::from_chars(
        text.data(), text.data() + text.size(), iterations);
    return parseResult.ec == std::errc{} &&
        parseResult.ptr == text.data() + text.size() &&
        iterations > 0 &&
        iterations <= kMaximumIterations;
}

[[nodiscard]] std::vector<std::byte> MakeMessage()
{
    std::vector<std::byte> message(
        static_cast<std::size_t>(kMessageBytes));
    for (std::size_t byteIndex = 0;
        byteIndex < message.size();
        byteIndex++)
    {
        message[byteIndex] = static_cast<std::byte>(
            static_cast<std::uint8_t>(byteIndex * 73U + 19U));
    }
    return message;
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeDescriptor(
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

} // namespace

int main(const int argumentCount, char* arguments[])
{
    std::uint64_t iterations = kDefaultIterations;
    if (argumentCount > 2 ||
        (argumentCount == 2 &&
            !ParseIterations(arguments[1], iterations)))
    {
        std::cerr << "usage: PBOuterFecWirehairV2Benchmark "
                     "[iterations:1..1000]\n";
        return 2;
    }

    const std::vector<std::byte> message = MakeMessage();
    auto initialEncoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, kOuterBlockBytes);
    if (!initialEncoderResult)
    {
        std::cerr << "BENCHMARK_SETUP_ENCODER_FAILED error="
                  << static_cast<unsigned int>(
                        initialEncoderResult.Error().code)
                  << " detail=" << initialEncoderResult.Error().detail << '\n';
        return 1;
    }
    const pbprotocol::WirehairV2SerializedProfile serializedProfile =
        initialEncoderResult.Value().GetSerializedProfile();
    const std::uint32_t blockCount =
        initialEncoderResult.Value().GetBlockCount();
    const pbprotocol::SegmentDescriptor descriptor = MakeDescriptor(
        message, serializedProfile);

    auto managerResult =
        pbouterfec::WirehairV2DecoderResourceManager::Create(
            pbprotocol::GetDefaultReceiverResourcePolicy());
    if (!managerResult)
    {
        std::cerr << "BENCHMARK_SETUP_POLICY_FAILED error="
                  << static_cast<unsigned int>(managerResult.Error().code)
                  << " detail=" << managerResult.Error().detail << '\n';
        return 1;
    }
    pbouterfec::WirehairV2DecoderResourceManager resourceManager =
        std::move(managerResult).Value();

    std::uint64_t maximumReservedBytes = 0;
    const auto benchmarkStart = std::chrono::steady_clock::now();
    for (std::uint64_t iteration = 0;
        iteration < iterations;
        iteration++)
    {
        auto encoderResult = pbouterfec::WirehairV2Encoder::Recreate(
            message, descriptor);
        auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            descriptor, resourceManager);
        if (!encoderResult || !decoderResult)
        {
            std::cerr << "BENCHMARK_CODEC_CREATE_FAILED iteration="
                      << iteration << '\n';
            return 1;
        }
        pbouterfec::WirehairV2Encoder encoder =
            std::move(encoderResult).Value();
        pbouterfec::WirehairV2Decoder decoder =
            std::move(decoderResult).Value();
        maximumReservedBytes = std::max(
            maximumReservedBytes,
            resourceManager.GetReservedDecoderBytes());

        for (std::uint32_t outerBlockId = 0;
            outerBlockId < blockCount;
            outerBlockId++)
        {
            std::vector<std::byte> payload(kOuterBlockBytes);
            const auto encodeResult = encoder.EncodeBlock(
                outerBlockId, payload);
            if (!encodeResult)
            {
                std::cerr << "BENCHMARK_ENCODE_FAILED iteration="
                          << iteration << " block_id=" << outerBlockId << '\n';
                return 1;
            }
            payload.resize(encodeResult.Value());
            const auto decodeResult = decoder.DecodeBlock(
                outerBlockId, payload);
            if (!decodeResult ||
                (outerBlockId + 1U == blockCount &&
                    decodeResult.Value() !=
                        pbouterfec::DecodeDisposition::Ready))
            {
                std::cerr << "BENCHMARK_DECODE_FAILED iteration="
                          << iteration << " block_id=" << outerBlockId << '\n';
                return 1;
            }
        }

        std::vector<std::byte> recovered(message.size());
        const auto recoverResult = decoder.Recover(recovered);
        if (!recoverResult || recovered != message)
        {
            std::cerr << "BENCHMARK_RECOVERY_FAILED iteration="
                      << iteration << '\n';
            return 1;
        }
    }
    const auto benchmarkEnd = std::chrono::steady_clock::now();

    if (resourceManager.GetActiveDecoderCount() != 0 ||
        resourceManager.GetReservedDecoderBytes() != 0)
    {
        std::cerr << "BENCHMARK_RESERVATION_LEAK\n";
        return 1;
    }

    const double elapsedSeconds = std::chrono::duration<double>(
        benchmarkEnd - benchmarkStart).count();
    const double processedMebibytes =
        static_cast<double>(kMessageBytes * iterations) /
        (1024.0 * 1024.0);
    const double mebibytesPerSecond = elapsedSeconds == 0.0
        ? 0.0
        : processedMebibytes / elapsedSeconds;

    std::cout << std::fixed << std::setprecision(6)
              << "BENCHMARK_COMPLETED\n"
              << "iterations=" << iterations << '\n'
              << "message_bytes=" << kMessageBytes << '\n'
              << "outer_block_bytes=" << kOuterBlockBytes << '\n'
              << "block_count=" << blockCount << '\n'
              << "maximum_reserved_decoder_bytes="
              << maximumReservedBytes << '\n'
              << "elapsed_seconds=" << elapsedSeconds << '\n'
              << "roundtrip_mebibytes_per_second="
              << mebibytesPerSecond << '\n';
    return 0;
}
