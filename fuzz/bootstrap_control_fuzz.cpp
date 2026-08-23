#include "pbprotocol/bootstrap_control_codec.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::size_t kMaximumAcceptedFuzzInputBytes =
    pbprotocol::kMaximumControlRecordBytes + 1;
constexpr std::size_t kMaximumRandomInputBytes = 512;
constexpr std::uint64_t kDefaultIterations = 100000;
constexpr std::uint64_t kDefaultSeed = 0x50424354524C0001ULL;

constexpr std::array<std::byte, pbprotocol::kBootstrapRecordBytes>
    kBootstrapSeed{
        std::byte{0x50}, std::byte{0x42}, std::byte{0x52}, std::byte{0x47},
        std::byte{0x01}, std::byte{0x01}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
        std::byte{0xD0}, std::byte{0xBA}, std::byte{0x97}, std::byte{0xD9},
        std::byte{0x4B}, std::byte{0x20}, std::byte{0xDF}, std::byte{0x81},
        std::byte{0x18}, std::byte{0x17}, std::byte{0x16}, std::byte{0x15},
        std::byte{0x14}, std::byte{0x13}, std::byte{0x12}, std::byte{0x11},
        std::byte{0x24}, std::byte{0x23}, std::byte{0x22}, std::byte{0x21},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xEA}, std::byte{0xE1}, std::byte{0x88}, std::byte{0xD4}};

constexpr std::array<std::byte, 67> kControlSeed{
    std::byte{0x50}, std::byte{0x42}, std::byte{0x43}, std::byte{0x52},
    std::byte{0x01}, std::byte{0x01},
    std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
    std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
    std::byte{0xD0}, std::byte{0xBA}, std::byte{0x97}, std::byte{0xD9},
    std::byte{0x4B}, std::byte{0x20}, std::byte{0xDF}, std::byte{0x81},
    std::byte{0x43}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
    std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
    std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B},
    std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F},
    std::byte{0x75}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x01},
    std::byte{0xC8}, std::byte{0x83}, std::byte{0x38}, std::byte{0xA1}};

void ExerciseInput(const std::span<const std::byte> input)
{
    if (input.size() > kMaximumAcceptedFuzzInputBytes)
    {
        return;
    }

    const auto bootstrapResult = pbprotocol::ParseBootstrapRecord(input);
    if (bootstrapResult)
    {
        std::array<std::byte, pbprotocol::kBootstrapRecordBytes> reserialized{};
        if (!pbprotocol::SerializeBootstrapRecord(
                bootstrapResult.Value(),
                reserialized) ||
            !std::ranges::equal(reserialized, input))
        {
            std::abort();
        }
    }

    const auto controlResult = pbprotocol::ParseControlRecord(input);
    if (controlResult)
    {
        std::vector<std::byte> reserialized(input.size());
        if (!pbprotocol::SerializeControlRecord(
                controlResult.Value(),
                reserialized) ||
            !std::ranges::equal(reserialized, input))
        {
            std::abort();
        }
    }
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
    const std::span<std::byte> destination,
    std::size_t& destinationSize)
{
    std::copy(source.begin(), source.end(), destination.begin());
    destinationSize = source.size();
}

int RunMutationLoop(
    const std::uint64_t iterations,
    const std::uint64_t initialSeed)
{
    std::uint64_t randomState = initialSeed == 0 ? kDefaultSeed : initialSeed;
    std::vector<std::byte> input(kMaximumAcceptedFuzzInputBytes);

    for (std::uint64_t iteration = 0; iteration < iterations; iteration++)
    {
        std::size_t inputSize = 0;
        switch (NextRandom(randomState) % 5ULL)
        {
        case 0:
            CopySeed(kBootstrapSeed, input, inputSize);
            break;
        case 1:
            CopySeed(kControlSeed, input, inputSize);
            break;
        case 2:
            inputSize = static_cast<std::size_t>(
                NextRandom(randomState) % (kMaximumRandomInputBytes + 1ULL));
            for (std::size_t byteIndex = 0; byteIndex < inputSize; byteIndex++)
            {
                input[byteIndex] = static_cast<std::byte>(
                    NextRandom(randomState) & 0xFFULL);
            }
            break;
        case 3:
            inputSize = 0;
            break;
        default:
            inputSize = kMaximumAcceptedFuzzInputBytes;
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

    std::vector<char> input(kMaximumAcceptedFuzzInputBytes);
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
        std::cerr << "usage: PBProtocolBootstrapControlFuzz "
                     "[iterations] [seed]\n"
                     "       PBProtocolBootstrapControlFuzz "
                     "--input <corpus-file>\n";
        return 2;
    }
    if (argumentCount > 2 && !ParseUint64(arguments[2], seed))
    {
        std::cerr << "usage: PBProtocolBootstrapControlFuzz "
                     "[iterations] [seed]\n"
                     "       PBProtocolBootstrapControlFuzz "
                     "--input <corpus-file>\n";
        return 2;
    }
    if (argumentCount > 3 || iterations == 0)
    {
        std::cerr << "usage: PBProtocolBootstrapControlFuzz "
                     "[iterations] [seed]\n"
                     "       PBProtocolBootstrapControlFuzz "
                     "--input <corpus-file>\n";
        return 2;
    }

    return RunMutationLoop(iterations, seed);
}

#endif
