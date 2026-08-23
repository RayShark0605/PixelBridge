#include "pbcompression/segment_compression.h"
#include "pbcompression/segment_decompression.h"

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

constexpr std::size_t kControlBytes = 24;
constexpr std::size_t kMaximumPayloadBytes = 128 * 1024;

[[nodiscard]] std::uint8_t GetControlByte(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    if (offset >= input.size())
    {
        return 0;
    }
    return std::to_integer<std::uint8_t>(input[offset]);
}

[[nodiscard]] std::uint64_t ReadUint64(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t byteIndex = 0; byteIndex < sizeof(value); byteIndex++)
    {
        const std::size_t inputOffset = offset + byteIndex;
        if (inputOffset >= input.size())
        {
            break;
        }
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(input[inputOffset]))
            << (byteIndex * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t GetBeyondVectorMaximum() noexcept
{
    const std::vector<std::byte> emptyBytes;
    const std::uint64_t vectorMaximum =
        static_cast<std::uint64_t>(emptyBytes.max_size());
    if (vectorMaximum == std::numeric_limits<std::uint64_t>::max())
    {
        return vectorMaximum;
    }
    return vectorMaximum + 1;
}

[[nodiscard]] std::uint64_t SelectSize(
    const std::uint8_t selector,
    const std::uint64_t actualSize,
    const std::uint64_t explicitValue) noexcept
{
    switch (selector % 8U)
    {
        case 0:
            return actualSize;
        case 1:
            return actualSize == 0 ? 0 : actualSize - 1;
        case 2:
            return actualSize == std::numeric_limits<std::uint64_t>::max()
                ? actualSize
                : actualSize + 1;
        case 3:
            return 0;
        case 4:
            return 1;
        case 5:
            return explicitValue % kMaximumPayloadBytes;
        case 6:
            return std::numeric_limits<std::uint64_t>::max() - 1;
        default:
            return GetBeyondVectorMaximum();
    }
}

[[nodiscard]] std::uint64_t SelectLimit(
    const std::uint8_t selector,
    const std::uint64_t relatedSize,
    const std::uint64_t explicitValue) noexcept
{
    switch (selector % 8U)
    {
        case 0:
            return 64 * 1024;
        case 1:
            return relatedSize;
        case 2:
            return relatedSize == 0 ? 0 : relatedSize - 1;
        case 3:
            return relatedSize >= kMaximumPayloadBytes
                ? relatedSize
                : relatedSize + 1;
        case 4:
            return 0;
        case 5:
            return std::min<std::uint64_t>(
                explicitValue, kMaximumPayloadBytes);
        case 6:
            return std::numeric_limits<std::uint64_t>::max() - 1;
        default:
            return GetBeyondVectorMaximum();
    }
}

[[nodiscard]] std::uint32_t SelectWindowLog(
    const std::uint8_t selector) noexcept
{
    constexpr std::array<std::uint32_t, 8> windowLogs{
        0, 9, 10, 17, 22, 23, 30, 32};
    return windowLogs[selector % windowLogs.size()];
}

[[nodiscard]] pbprotocol::CompressionCodec SelectCodec(
    const std::uint8_t selector) noexcept
{
    switch (selector % 4U)
    {
        case 0:
            return pbprotocol::CompressionCodec::Raw;
        case 1:
            return pbprotocol::CompressionCodec::Zstandard;
        case 2:
            return static_cast<pbprotocol::CompressionCodec>(0);
        default:
            return static_cast<pbprotocol::CompressionCodec>(0xFFU);
    }
}

void ExerciseStreamingDecompression(
    const std::span<const std::byte> frameBytes,
    const std::uint64_t expectedEncodedSize,
    const std::uint64_t expectedRawSize,
    const pbcompression::DecompressionLimits& limits,
    const std::uint8_t partitionSelector,
    const std::uint8_t sequenceSelector)
{
    auto decompressorResult = pbcompression::SegmentDecompressor::Create(
        limits, expectedEncodedSize, expectedRawSize);
    if (!decompressorResult)
    {
        return;
    }
    pbcompression::SegmentDecompressor decompressor =
        std::move(decompressorResult).Value();

    const std::uint8_t sequence = sequenceSelector % 5U;
    if (sequence == 1U)
    {
        static_cast<void>(decompressor.Finish());
        static_cast<void>(decompressor.Update(frameBytes));
        static_cast<void>(decompressor.Finish());
        return;
    }

    std::size_t chunkBytes = frameBytes.size();
    switch (partitionSelector % 4U)
    {
        case 0:
            chunkBytes = 1;
            break;
        case 1:
            chunkBytes = pbcompression::kZstdFrameHeaderMaximumBytes;
            break;
        case 2:
            chunkBytes = frameBytes.empty()
                ? 1
                : std::max<std::size_t>(1, frameBytes.size() / 2);
            break;
        default:
            chunkBytes = std::max<std::size_t>(
                1, static_cast<std::size_t>(partitionSelector));
            break;
    }

    std::size_t position = 0;
    while (position < frameBytes.size())
    {
        const std::size_t currentBytes = std::min(
            chunkBytes, frameBytes.size() - position);
        const pbcompression::CompressionStatus updateStatus =
            decompressor.Update(frameBytes.subspan(position, currentBytes));
        position += currentBytes;
        if (!updateStatus)
        {
            static_cast<void>(decompressor.Update(
                frameBytes.subspan(position)));
            static_cast<void>(decompressor.Finish());
            return;
        }
        if (sequence == 2U && position < frameBytes.size())
        {
            static_cast<void>(decompressor.Finish());
        }
    }

    if (sequence == 3U)
    {
        const std::array<std::byte, 1> trailingByte{std::byte{0xA5}};
        static_cast<void>(decompressor.Update(trailingByte));
    }
    if (sequence == 4U)
    {
        static_cast<void>(decompressor.Update(
            std::span<const std::byte>{}));
    }
    const auto finishResult = decompressor.Finish();
    if (finishResult
        && finishResult.Value().size() != expectedRawSize)
    {
        std::abort();
    }
    static_cast<void>(decompressor.Finish());
}

void ExerciseCompression(const std::span<const std::byte> rawBytes,
                         const std::span<const std::byte> input)
{
    pbcompression::CompressionSettings settings;
    constexpr std::array<int, 6> compressionLevels{0, 1, 3, 9, 22, 23};
    settings.compressionLevel =
        compressionLevels[GetControlByte(input, 7)
            % compressionLevels.size()];
    settings.maxWindowLog = SelectWindowLog(GetControlByte(input, 8));
    settings.maxOutputBytes = SelectLimit(
        GetControlByte(input, 9),
        static_cast<std::uint64_t>(rawBytes.size()),
        ReadUint64(input, 16));
    settings.framingMarginBytes = ReadUint64(input, 8);

    const auto encodedResult = pbcompression::CompressSegment(
        rawBytes, settings);
    if (!encodedResult)
    {
        return;
    }

    const pbcompression::EncodedSegment& encodedSegment =
        encodedResult.Value();
    pbcompression::DecompressionLimits limits;
    limits.maxInputBytes = encodedSegment.bytes.size();
    limits.maxOutputBytes = rawBytes.size();
    limits.maxWindowLog = 31;
    const auto decodedResult = pbcompression::DecompressSegment(
        encodedSegment.codec,
        std::span<const std::byte>(encodedSegment.bytes),
        encodedSegment.bytes.size(),
        rawBytes.size(),
        limits);
    if (!decodedResult
        || decodedResult.Value().size() != rawBytes.size()
        || !std::equal(
            decodedResult.Value().begin(),
            decodedResult.Value().end(),
            rawBytes.begin()))
    {
        std::abort();
    }
}

void ExerciseInput(const std::span<const std::byte> input)
{
    const std::size_t payloadOffset =
        std::min(kControlBytes, input.size());
    const std::size_t payloadBytes = std::min(
        input.size() - payloadOffset, kMaximumPayloadBytes);
    const std::span<const std::byte> frameBytes =
        input.subspan(payloadOffset, payloadBytes);
    const std::uint64_t actualEncodedSize =
        static_cast<std::uint64_t>(frameBytes.size());
    const std::uint64_t explicitValue = ReadUint64(input, 16);
    const std::uint64_t expectedEncodedSize = SelectSize(
        GetControlByte(input, 1), actualEncodedSize, explicitValue);
    const std::uint64_t rawSizeHint =
        ReadUint64(input, 8) % kMaximumPayloadBytes;
    const std::uint64_t expectedRawSize = SelectSize(
        GetControlByte(input, 2), rawSizeHint, explicitValue);

    pbcompression::DecompressionLimits limits;
    limits.maxInputBytes = SelectLimit(
        GetControlByte(input, 3), expectedEncodedSize, explicitValue);
    limits.maxOutputBytes = SelectLimit(
        GetControlByte(input, 4), expectedRawSize, explicitValue);
    limits.maxWindowLog = SelectWindowLog(GetControlByte(input, 5));

    const pbprotocol::CompressionCodec codec =
        SelectCodec(GetControlByte(input, 0));
    const auto decodedResult = pbcompression::DecompressSegment(
        codec,
        frameBytes,
        expectedEncodedSize,
        expectedRawSize,
        limits);
    if (decodedResult)
    {
        if (decodedResult.Value().size() != expectedRawSize)
        {
            std::abort();
        }
        if (codec == pbprotocol::CompressionCodec::Raw
            && (decodedResult.Value().size() != frameBytes.size()
                || !std::equal(
                    decodedResult.Value().begin(),
                    decodedResult.Value().end(),
                    frameBytes.begin())))
        {
            std::abort();
        }
    }

    if (codec == pbprotocol::CompressionCodec::Zstandard)
    {
        ExerciseStreamingDecompression(
            frameBytes,
            expectedEncodedSize,
            expectedRawSize,
            limits,
            GetControlByte(input, 6),
            GetControlByte(input, 7));
    }

    ExerciseCompression(frameBytes, input);
}

#if !defined(PB_USE_LIBFUZZER)

constexpr std::size_t kMaximumReplayBytes =
    kControlBytes + kMaximumPayloadBytes;
constexpr std::size_t kMaximumGeneratedInputBytes = 2048;
constexpr std::uint64_t kDefaultIterations = 100000;
constexpr std::uint64_t kDefaultSeed = 0xC04D5EED5A7D1234ULL;

constexpr std::array<std::byte, 47> kKnownAnswerFrame{
    std::byte{0x28}, std::byte{0xB5}, std::byte{0x2F}, std::byte{0xFD},
    std::byte{0x64}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0D},
    std::byte{0x01}, std::byte{0x00}, std::byte{0xD0}, std::byte{0x41},
    std::byte{0x42}, std::byte{0x43}, std::byte{0x44}, std::byte{0x45},
    std::byte{0x46}, std::byte{0x47}, std::byte{0x48}, std::byte{0x49},
    std::byte{0x4A}, std::byte{0x4B}, std::byte{0x4C}, std::byte{0x4D},
    std::byte{0x4E}, std::byte{0x4F}, std::byte{0x50}, std::byte{0x51},
    std::byte{0x52}, std::byte{0x53}, std::byte{0x54}, std::byte{0x55},
    std::byte{0x56}, std::byte{0x57}, std::byte{0x58}, std::byte{0x59},
    std::byte{0x5A}, std::byte{0x01}, std::byte{0x00}, std::byte{0x8E},
    std::byte{0x9B}, std::byte{0x9A}, std::byte{0x63}, std::byte{0xED},
    std::byte{0x3D}, std::byte{0x39}, std::byte{0xDB}};

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
    return parseResult.ec == std::errc{}
        && parseResult.ptr == text.data() + text.size();
}

void BuildKnownAnswerSeed(
    std::array<std::byte, kMaximumGeneratedInputBytes>& input,
    std::size_t& inputSize) noexcept
{
    input.fill(std::byte{0});
    input[0] = std::byte{1};
    input[1] = std::byte{0};
    input[2] = std::byte{0};
    input[3] = std::byte{1};
    input[4] = std::byte{0};
    input[5] = std::byte{5};
    input[6] = std::byte{0};
    input[8] = std::byte{0x00};
    input[9] = std::byte{0x01};
    std::copy(
        kKnownAnswerFrame.begin(),
        kKnownAnswerFrame.end(),
        input.begin() + static_cast<std::ptrdiff_t>(kControlBytes));
    inputSize = kControlBytes + kKnownAnswerFrame.size();
}

int RunMutationLoop(
    const std::uint64_t iterations,
    const std::uint64_t initialSeed)
{
    std::uint64_t randomState = initialSeed == 0
        ? kDefaultSeed
        : initialSeed;
    std::array<std::byte, kMaximumGeneratedInputBytes> input{};
    for (std::uint64_t iteration = 0; iteration < iterations; iteration++)
    {
        std::size_t inputSize = 0;
        if (NextRandom(randomState) % 3ULL != 0)
        {
            BuildKnownAnswerSeed(input, inputSize);
        }
        else
        {
            inputSize = static_cast<std::size_t>(
                NextRandom(randomState) % (input.size() + 1ULL));
            for (std::size_t byteIndex = 0;
                 byteIndex < inputSize;
                 byteIndex++)
            {
                input[byteIndex] = static_cast<std::byte>(
                    NextRandom(randomState) & 0xFFULL);
            }
        }

        if (inputSize != 0)
        {
            const std::size_t mutationCount = static_cast<std::size_t>(
                NextRandom(randomState) % 17ULL);
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

    std::array<char, kMaximumReplayBytes> input{};
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
        if (inputFile.gcount() != 0)
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
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--input")
    {
        return ReplayInputFile(arguments[2]);
    }

    std::uint64_t iterations = kDefaultIterations;
    std::uint64_t seed = kDefaultSeed;
    if (argumentCount > 1 && !ParseUint64(arguments[1], iterations))
    {
        std::cerr << "usage: PBCompressionZstdBoundaryFuzz "
                     "[iterations] [seed]\n"
                     "       PBCompressionZstdBoundaryFuzz "
                     "--input <corpus-file>\n";
        return 2;
    }
    if (argumentCount > 2 && !ParseUint64(arguments[2], seed))
    {
        std::cerr << "usage: PBCompressionZstdBoundaryFuzz "
                     "[iterations] [seed]\n"
                     "       PBCompressionZstdBoundaryFuzz "
                     "--input <corpus-file>\n";
        return 2;
    }
    if (argumentCount > 3 || iterations == 0)
    {
        std::cerr << "usage: PBCompressionZstdBoundaryFuzz "
                     "[iterations] [seed]\n"
                     "       PBCompressionZstdBoundaryFuzz "
                     "--input <corpus-file>\n";
        return 2;
    }
    return RunMutationLoop(iterations, seed);
}

#endif
