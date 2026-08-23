#include "pbcompression/compression_error.h"
#include "pbcompression/segment_compression.h"
#include "pbcompression/segment_decompression.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_types.h"
#include "pbprotocol/protocol_version.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace
{

using pbcompression::CompressionErrorCode;
using pbcompression::CompressionSettings;
using pbcompression::CompressionStatus;
using pbcompression::DecompressionLimits;
using pbcompression::SegmentCompressor;
using pbcompression::SegmentDecompressor;

[[nodiscard]] std::vector<std::byte> MakePatternBytes(
    const std::size_t count,
    const std::uint8_t base)
{
    std::vector<std::byte> bytes(count);
    for (std::size_t byteIndex = 0; byteIndex < count; byteIndex++)
    {
        bytes[byteIndex] =
            static_cast<std::byte>(base + byteIndex % 26U);
    }
    return bytes;
}

[[nodiscard]] std::uint64_t SplitMix64(std::uint64_t& state)
{
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

[[nodiscard]] std::vector<std::byte> MakeRandomBytes(
    const std::size_t count,
    const std::uint64_t seed)
{
    std::uint64_t state = seed;
    std::vector<std::byte> bytes(count);
    for (std::size_t byteIndex = 0; byteIndex < count; byteIndex++)
    {
        const std::uint64_t value = SplitMix64(state);
        bytes[byteIndex] = static_cast<std::byte>(value & 0xFFULL);
    }
    return bytes;
}

// Synthetic container resembling ZIP/MP4 payloads: a short repeated header
// followed by a high entropy block, repeated until the requested size.
[[nodiscard]] std::vector<std::byte> MakeContainerLikeBytes(
    const std::size_t count)
{
    constexpr std::size_t headerBytes = 16;
    constexpr std::size_t entropyBytes = 256;

    std::uint64_t state = 0xC0FFEEC0FFEE0001ULL;
    std::vector<std::byte> bytes;
    bytes.reserve(count);
    std::uint64_t blockIndex = 0;
    while (bytes.size() < count)
    {
        const std::uint8_t headerValue =
            static_cast<std::uint8_t>(0x5AU ^ (blockIndex & 0x0FULL));
        for (std::size_t byteIndex = 0; byteIndex < headerBytes; byteIndex++)
        {
            bytes.push_back(static_cast<std::byte>(headerValue));
        }
        for (std::size_t byteIndex = 0; byteIndex < entropyBytes; byteIndex++)
        {
            const std::uint64_t value = SplitMix64(state);
            bytes.push_back(static_cast<std::byte>(value & 0xFFULL));
        }
    }
    bytes.resize(count);
    return bytes;
}

// 4MiB of zeros with a 256B marker at offset 0 and again at 2MiB so a
// long match is available when the encoder is allowed a wide window.
[[nodiscard]] std::vector<std::byte> MakeWideWindowInput()
{
    constexpr std::size_t totalBytes = 4 * 1024 * 1024;
    constexpr std::size_t markerBytes = 256;
    constexpr std::size_t markerOffset = 2 * 1024 * 1024;
    std::vector<std::byte> bytes(totalBytes, std::byte{0});
    for (std::size_t markerIndex = 0; markerIndex < markerBytes; markerIndex++)
    {
        const std::byte markerValue =
            static_cast<std::byte>(0x41U + markerIndex % 26U);
        bytes[markerIndex] = markerValue;
        bytes[markerOffset + markerIndex] = markerValue;
    }
    return bytes;
}

[[nodiscard]] std::string ToHex(const std::span<const std::byte> bytes)
{
    static const char kHexDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (const std::byte value : bytes)
    {
        const std::uint8_t rawValue = static_cast<std::uint8_t>(value);
        hex.push_back(kHexDigits[rawValue >> 4]);
        hex.push_back(kHexDigits[rawValue & 0x0FU]);
    }
    return hex;
}

[[nodiscard]] bool HasFrameMagic(const std::span<const std::byte> bytes)
{
    if (bytes.size() < 4)
    {
        return false;
    }
    return static_cast<std::uint8_t>(bytes[0]) == 0x28U
        && static_cast<std::uint8_t>(bytes[1]) == 0xB5U
        && static_cast<std::uint8_t>(bytes[2]) == 0x2FU
        && static_cast<std::uint8_t>(bytes[3]) == 0xFDU;
}

// Independent recomputation of the 12.1 RAW fallback rule without calling
// ChooseSegmentCodec: compressed + margin >= raw (checked add, overflow
// counts as Raw) keeps the segment Raw.
[[nodiscard]] pbprotocol::CompressionCodec ExpectedSegmentCodec(
    const std::uint64_t compressedBytes,
    const std::uint64_t rawBytes,
    const std::uint64_t framingMarginBytes)
{
    if (compressedBytes >= std::numeric_limits<std::uint64_t>::max()
        - framingMarginBytes)
    {
        return pbprotocol::CompressionCodec::Raw;
    }
    if (compressedBytes + framingMarginBytes >= rawBytes)
    {
        return pbprotocol::CompressionCodec::Raw;
    }
    return pbprotocol::CompressionCodec::Zstandard;
}

// REQUIRE is only legal in void functions, so helpers report through out
// parameters and leave all assertions inside void bodies.
void VerifyRoundTrip(
    const std::vector<std::byte>& input,
    const pbprotocol::CompressionCodec codec,
    const std::vector<std::byte>& encoded)
{
    const DecompressionLimits limits;
    const auto decodedResult = pbcompression::DecompressSegment(
        codec,
        std::span<const std::byte>(encoded),
        input.size(),
        limits);
    REQUIRE(decodedResult.HasValue());
    REQUIRE(decodedResult.Value().size() == input.size());
    REQUIRE(std::equal(
        decodedResult.Value().begin(),
        decodedResult.Value().end(),
        input.begin()));
}

void CompressFrame(
    const std::vector<std::byte>& input,
    const CompressionSettings& settings,
    std::vector<std::byte>& frameOut)
{
    const auto encodedResult = pbcompression::CompressSegment(
        std::span<const std::byte>(input), settings);
    REQUIRE(encodedResult.HasValue());
    REQUIRE(encodedResult.Value().codec
        == pbprotocol::CompressionCodec::Zstandard);
    frameOut = encodedResult.Value().bytes;
}

void CompressInChunks(
    const std::vector<std::byte>& input,
    const CompressionSettings& settings,
    const std::vector<std::size_t>& chunkSizes,
    std::vector<std::byte>& frameOut)
{
    auto compressorResult = SegmentCompressor::Create(
        settings, input.size());
    REQUIRE(compressorResult.HasValue());
    SegmentCompressor compressor = std::move(compressorResult).Value();

    std::size_t position = 0;
    for (const std::size_t chunkSize : chunkSizes)
    {
        const std::span<const std::byte> chunk(
            input.data() + position, chunkSize);
        const CompressionStatus updateStatus = compressor.Update(chunk);
        REQUIRE(updateStatus.HasValue());
        position += chunkSize;
    }
    REQUIRE(position == input.size());

    const auto frameResult = compressor.Finish();
    REQUIRE(frameResult.HasValue());
    frameOut = frameResult.Value();
}

} // namespace

TEST_CASE("ZeroByteSegmentStaysRawAndRoundTrips",
          "[pbcompression][segment][edge]")
{
    const CompressionSettings settings;
    const auto encodedResult = pbcompression::CompressSegment(
        std::span<const std::byte>{}, settings);
    REQUIRE(encodedResult.HasValue());
    REQUIRE(encodedResult.Value().codec
        == pbprotocol::CompressionCodec::Raw);
    REQUIRE(encodedResult.Value().bytes.empty());

    const DecompressionLimits limits;
    const auto decodedResult = pbcompression::DecompressSegment(
        pbprotocol::CompressionCodec::Raw,
        std::span<const std::byte>{},
        0,
        limits);
    REQUIRE(decodedResult.HasValue());
    REQUIRE(decodedResult.Value().empty());
}

TEST_CASE("OneByteSegmentFallsBackToRawAndStreamingFrameWorks",
          "[pbcompression][segment][edge]")
{
    const std::vector<std::byte> input{static_cast<std::byte>(0x42)};
    const CompressionSettings settings;
    const auto encodedResult = pbcompression::CompressSegment(
        std::span<const std::byte>(input), settings);
    REQUIRE(encodedResult.HasValue());
    REQUIRE(encodedResult.Value().codec
        == pbprotocol::CompressionCodec::Raw);
    REQUIRE(encodedResult.Value().bytes == input);
    VerifyRoundTrip(
        input, pbprotocol::CompressionCodec::Raw,
        encodedResult.Value().bytes);

    // The streaming compressor must also be usable for tiny segments,
    // bypassing the 12.1 codec selection that keeps them Raw.
    auto tinyCompressorResult = SegmentCompressor::Create(
        settings, input.size());
    REQUIRE(tinyCompressorResult.HasValue());
    SegmentCompressor tinyCompressor =
        std::move(tinyCompressorResult).Value();
    REQUIRE(tinyCompressor.Update(
        std::span<const std::byte>(input)).HasValue());
    const auto tinyFrameResult = tinyCompressor.Finish();
    REQUIRE(tinyFrameResult.HasValue());
    std::vector<std::byte> frame = std::move(tinyFrameResult).Value();
    REQUIRE(HasFrameMagic(frame));
    VerifyRoundTrip(
        input, pbprotocol::CompressionCodec::Zstandard, frame);
}

TEST_CASE("CompressiblePatternUsesZstandardAndMarginForcesRaw",
          "[pbcompression][segment][fallback]")
{
    constexpr std::size_t rawBytes = 64 * 1024;
    const std::vector<std::byte> input = MakePatternBytes(rawBytes, 0x41U);
    const CompressionSettings settings;
    const auto encodedResult = pbcompression::CompressSegment(
        std::span<const std::byte>(input), settings);
    REQUIRE(encodedResult.HasValue());
    REQUIRE(encodedResult.Value().codec
        == pbprotocol::CompressionCodec::Zstandard);
    REQUIRE(encodedResult.Value().bytes.size() < rawBytes);
    VerifyRoundTrip(
        input, pbprotocol::CompressionCodec::Zstandard,
        encodedResult.Value().bytes);

    CompressionSettings marginSettings = settings;
    marginSettings.framingMarginBytes = rawBytes;
    const auto marginResult = pbcompression::CompressSegment(
        std::span<const std::byte>(input), marginSettings);
    REQUIRE(marginResult.HasValue());
    REQUIRE(marginResult.Value().codec
        == pbprotocol::CompressionCodec::Raw);
    REQUIRE(marginResult.Value().bytes == input);
    VerifyRoundTrip(
        input, pbprotocol::CompressionCodec::Raw,
        marginResult.Value().bytes);
}

TEST_CASE("IncompressibleRandomFallsBackToRaw",
          "[pbcompression][segment][fallback]")
{
    constexpr std::size_t rawBytes = 256 * 1024;
    const std::vector<std::byte> input =
        MakeRandomBytes(rawBytes, 0x9E3779B97F4A7C15ULL);
    const CompressionSettings settings;
    const auto encodedResult = pbcompression::CompressSegment(
        std::span<const std::byte>(input), settings);
    REQUIRE(encodedResult.HasValue());
    REQUIRE(encodedResult.Value().codec
        == pbprotocol::CompressionCodec::Raw);
    REQUIRE(encodedResult.Value().bytes == input);
    VerifyRoundTrip(
        input, pbprotocol::CompressionCodec::Raw,
        encodedResult.Value().bytes);
}

TEST_CASE("ContainerLikeSegmentMatchesIndependentRuleRecomputation",
          "[pbcompression][segment][fallback]")
{
    constexpr std::size_t rawBytes = 512 * 1024;
    constexpr std::uint64_t framingMargin = 128;
    const std::vector<std::byte> input = MakeContainerLikeBytes(rawBytes);

    CompressionSettings settings;
    settings.framingMarginBytes = framingMargin;
    const auto encodedResult = pbcompression::CompressSegment(
        std::span<const std::byte>(input), settings);
    REQUIRE(encodedResult.HasValue());

    const auto expectedCodec = ExpectedSegmentCodec(
        static_cast<std::uint64_t>(encodedResult.Value().bytes.size()),
        static_cast<std::uint64_t>(rawBytes),
        framingMargin);
    REQUIRE(encodedResult.Value().codec == expectedCodec);
    REQUIRE(expectedCodec == pbprotocol::CompressionCodec::Zstandard);
    VerifyRoundTrip(
        input, encodedResult.Value().codec, encodedResult.Value().bytes);
}

TEST_CASE("ChunkedStreamingProducesIdenticalFrames",
          "[pbcompression][streaming][equivalence]")
{
    const CompressionSettings settings;

    const std::vector<std::byte> smallInput = MakePatternBytes(13, 0x41U);
    std::vector<std::byte> smallChunked;
    std::vector<std::byte> smallOneShot;
    CompressInChunks(smallInput, settings, {1, 2, 3, 7}, smallChunked);
    CompressInChunks(smallInput, settings, {13}, smallOneShot);
    REQUIRE(smallChunked == smallOneShot);
    REQUIRE(HasFrameMagic(smallChunked));
    VerifyRoundTrip(
        smallInput, pbprotocol::CompressionCodec::Zstandard, smallChunked);

    const std::vector<std::byte> blockInput = MakeContainerLikeBytes(4096);
    std::vector<std::byte> blockChunked;
    std::vector<std::byte> blockOneShot;
    CompressInChunks(
        blockInput, settings, {1024, 1024, 1024, 1024}, blockChunked);
    CompressInChunks(blockInput, settings, {4096}, blockOneShot);
    REQUIRE(blockChunked == blockOneShot);
    VerifyRoundTrip(
        blockInput, pbprotocol::CompressionCodec::Zstandard, blockChunked);
}

TEST_CASE("TruncatedFramesFailWithIncompleteFrame",
          "[pbcompression][segment][malformed]")
{
    constexpr std::size_t rawBytes = 64 * 1024;
    const std::vector<std::byte> input = MakeContainerLikeBytes(rawBytes);
    const CompressionSettings settings;
    std::vector<std::byte> frame;
    CompressFrame(input, settings, frame);
    REQUIRE(frame.size() > 16);

    const DecompressionLimits limits;
    const std::uint64_t expectedSize = rawBytes;
    const std::vector<std::size_t> truncations{
        frame.size() - 1, frame.size() - 8, frame.size() / 2};
    for (const std::size_t truncatedSize : truncations)
    {
        const std::span<const std::byte> truncated(
            frame.data(), truncatedSize);
        const auto decodedResult = pbcompression::DecompressSegment(
            pbprotocol::CompressionCodec::Zstandard,
            truncated,
            expectedSize,
            limits);
        REQUIRE_FALSE(decodedResult.HasValue());
        REQUIRE(decodedResult.Error().code
            == CompressionErrorCode::IncompleteFrame);
    }
}

TEST_CASE("WindowLogBoundsFailClosedOnBothSides",
          "[pbcompression][segment][limits]")
{
    CompressionSettings encoderSettings;
    encoderSettings.maxWindowLog = 0;
    REQUIRE_FALSE(SegmentCompressor::Create(encoderSettings, 0).HasValue());
    REQUIRE(SegmentCompressor::Create(encoderSettings, 0).Error().code
        == CompressionErrorCode::InvalidMaxWindowLog);
    encoderSettings.maxWindowLog = 9;
    REQUIRE(SegmentCompressor::Create(encoderSettings, 0).Error().code
        == CompressionErrorCode::InvalidMaxWindowLog);
    encoderSettings.maxWindowLog = 32;
    REQUIRE(SegmentCompressor::Create(encoderSettings, 0).Error().code
        == CompressionErrorCode::InvalidMaxWindowLog);

    DecompressionLimits decoderLimits;
    decoderLimits.maxWindowLog = 0;
    REQUIRE_FALSE(SegmentDecompressor::Create(decoderLimits, 0).HasValue());
    REQUIRE(SegmentDecompressor::Create(decoderLimits, 0).Error().code
        == CompressionErrorCode::InvalidMaxWindowLog);
    decoderLimits.maxWindowLog = 32;
    REQUIRE(SegmentDecompressor::Create(decoderLimits, 0).Error().code
        == CompressionErrorCode::InvalidMaxWindowLog);

    // A frame compressed with a wide window must be rejected by a decoder
    // whose window limit is too small.
    CompressionSettings wideSettings;
    wideSettings.maxWindowLog = 30;
    const std::vector<std::byte> input = MakeWideWindowInput();
    std::vector<std::byte> frame;
    CompressFrame(input, wideSettings, frame);

    DecompressionLimits narrowLimits;
    narrowLimits.maxWindowLog = 10;
    const auto decodedResult = pbcompression::DecompressSegment(
        pbprotocol::CompressionCodec::Zstandard,
        std::span<const std::byte>(frame),
        input.size(),
        narrowLimits);
    REQUIRE_FALSE(decodedResult.HasValue());
    REQUIRE(decodedResult.Error().code
        == CompressionErrorCode::OutputLimitExceeded);
}

TEST_CASE("WrongExpectedRawSizeFailsWithExactErrors",
          "[pbcompression][segment][rawsize]")
{
    constexpr std::size_t rawBytes = 16 * 1024;
    const std::vector<std::byte> input = MakePatternBytes(rawBytes, 0x41U);
    const CompressionSettings settings;
    std::vector<std::byte> frame;
    CompressFrame(input, settings, frame);

    const DecompressionLimits limits;

    const auto tooLarge = pbcompression::DecompressSegment(
        pbprotocol::CompressionCodec::Zstandard,
        std::span<const std::byte>(frame),
        rawBytes + 1,
        limits);
    REQUIRE_FALSE(tooLarge.HasValue());
    REQUIRE(tooLarge.Error().code
        == CompressionErrorCode::RawSizeMismatch);

    const auto tooSmall = pbcompression::DecompressSegment(
        pbprotocol::CompressionCodec::Zstandard,
        std::span<const std::byte>(frame),
        rawBytes - 1,
        limits);
    REQUIRE_FALSE(tooSmall.HasValue());
    REQUIRE(tooSmall.Error().code
        == CompressionErrorCode::OutputLimitExceeded);

    const auto rawMismatch = pbcompression::DecompressSegment(
        pbprotocol::CompressionCodec::Raw,
        std::span<const std::byte>(frame),
        frame.size() + 1,
        limits);
    REQUIRE_FALSE(rawMismatch.HasValue());
    REQUIRE(rawMismatch.Error().code
        == CompressionErrorCode::RawSizeMismatch);
}

TEST_CASE("InvalidEncoderSettingsFailClosed",
          "[pbcompression][segment][limits]")
{
    CompressionSettings levelTooLow;
    levelTooLow.compressionLevel = 0;
    REQUIRE(SegmentCompressor::Create(levelTooLow, 0).Error().code
        == CompressionErrorCode::InvalidCompressionLevel);
    CompressionSettings levelTooHigh;
    levelTooHigh.compressionLevel = 23;
    REQUIRE(SegmentCompressor::Create(levelTooHigh, 0).Error().code
        == CompressionErrorCode::InvalidCompressionLevel);

    CompressionSettings windowTooLow;
    windowTooLow.maxWindowLog = 9;
    REQUIRE(SegmentCompressor::Create(windowTooLow, 0).Error().code
        == CompressionErrorCode::InvalidMaxWindowLog);
    CompressionSettings windowTooHigh;
    windowTooHigh.maxWindowLog = 32;
    REQUIRE(SegmentCompressor::Create(windowTooHigh, 0).Error().code
        == CompressionErrorCode::InvalidMaxWindowLog);

    CompressionSettings outputZero;
    outputZero.maxOutputBytes = 0;
    REQUIRE(SegmentCompressor::Create(outputZero, 0).Error().code
        == CompressionErrorCode::InvalidMaxOutputBytes);
    CompressionSettings outputMax;
    outputMax.maxOutputBytes =
        std::numeric_limits<std::uint64_t>::max();
    REQUIRE(SegmentCompressor::Create(outputMax, 0).Error().code
        == CompressionErrorCode::InvalidMaxOutputBytes);

    // A frame larger than the output budget fails with OutputLimitExceeded.
    const std::vector<std::byte> input = MakeContainerLikeBytes(512 * 1024);
    CompressionSettings tightSettings;
    tightSettings.maxOutputBytes = 1024;
    const auto encodedResult = pbcompression::CompressSegment(
        std::span<const std::byte>(input), tightSettings);
    REQUIRE_FALSE(encodedResult.HasValue());
    REQUIRE(encodedResult.Error().code
        == CompressionErrorCode::OutputLimitExceeded);
}

TEST_CASE("DecompressionBombFailsBeforeAllocation",
          "[pbcompression][segment][limits]")
{
    constexpr std::size_t rawBytes = 1024 * 1024;
    const std::vector<std::byte> input(rawBytes, std::byte{0});
    const CompressionSettings settings;
    std::vector<std::byte> frame;
    CompressFrame(input, settings, frame);
    REQUIRE(frame.size() < 64 * 1024);

    DecompressionLimits bombLimits;
    bombLimits.maxOutputBytes = 64 * 1024;
    const auto createResult = SegmentDecompressor::Create(
        bombLimits, rawBytes);
    REQUIRE_FALSE(createResult.HasValue());
    REQUIRE(createResult.Error().code
        == CompressionErrorCode::OutputLimitExceeded);

    const auto decodedResult = pbcompression::DecompressSegment(
        pbprotocol::CompressionCodec::Zstandard,
        std::span<const std::byte>(frame),
        rawBytes,
        bombLimits);
    REQUIRE_FALSE(decodedResult.HasValue());
    REQUIRE(decodedResult.Error().code
        == CompressionErrorCode::OutputLimitExceeded);
}

TEST_CASE("CorruptedAndTrailingFramesFailWithExactErrors",
          "[pbcompression][segment][malformed]")
{
    const std::vector<std::byte> input = MakeContainerLikeBytes(256 * 1024);
    const CompressionSettings settings;
    const DecompressionLimits limits;

    std::vector<std::byte> corruptedFrame;
    CompressFrame(input, settings, corruptedFrame);
    const std::size_t corruptPosition = corruptedFrame.size() / 2;
    const std::uint8_t flippedValue =
        static_cast<std::uint8_t>(corruptedFrame[corruptPosition]) ^ 0x01U;
    corruptedFrame[corruptPosition] = static_cast<std::byte>(flippedValue);
    const auto corruptedResult = pbcompression::DecompressSegment(
        pbprotocol::CompressionCodec::Zstandard,
        std::span<const std::byte>(corruptedFrame),
        input.size(),
        limits);
    REQUIRE_FALSE(corruptedResult.HasValue());
    REQUIRE(corruptedResult.Error().code
        == CompressionErrorCode::CorruptedFrame);

    std::vector<std::byte> trailingFrame;
    CompressFrame(input, settings, trailingFrame);
    trailingFrame.push_back(static_cast<std::byte>(0xAA));
    trailingFrame.push_back(static_cast<std::byte>(0xBB));
    trailingFrame.push_back(static_cast<std::byte>(0xCC));
    trailingFrame.push_back(static_cast<std::byte>(0xDD));
    const auto trailingResult = pbcompression::DecompressSegment(
        pbprotocol::CompressionCodec::Zstandard,
        std::span<const std::byte>(trailingFrame),
        input.size(),
        limits);
    REQUIRE_FALSE(trailingResult.HasValue());
    REQUIRE(trailingResult.Error().code
        == CompressionErrorCode::TrailingInput);
}

TEST_CASE("TerminalStatesLatchAndRepeatErrors",
          "[pbcompression][segment][state]")
{
    const std::vector<std::byte> input = MakePatternBytes(64, 0x41U);
    const CompressionSettings settings;
    const DecompressionLimits limits;
    const std::span<const std::byte> emptySpan;

    // After a successful Finish, further calls are InvalidState.
    auto compressorResult = SegmentCompressor::Create(
        settings, input.size());
    REQUIRE(compressorResult.HasValue());
    SegmentCompressor compressor = std::move(compressorResult).Value();
    REQUIRE(compressor.Update(
        std::span<const std::byte>(input)).HasValue());
    REQUIRE(compressor.Finish().HasValue());
    REQUIRE(compressor.Update(emptySpan).Error().code
        == CompressionErrorCode::InvalidState);
    const auto lateFinish = compressor.Finish();
    REQUIRE_FALSE(lateFinish.HasValue());
    REQUIRE(lateFinish.Error().code
        == CompressionErrorCode::InvalidState);

    std::vector<std::byte> frame;
    CompressFrame(input, settings, frame);
    auto decompressorResult = SegmentDecompressor::Create(
        limits, input.size());
    REQUIRE(decompressorResult.HasValue());
    SegmentDecompressor decompressor =
        std::move(decompressorResult).Value();
    REQUIRE(decompressor.Update(
        std::span<const std::byte>(frame)).HasValue());
    REQUIRE(decompressor.Finish().HasValue());
    REQUIRE(decompressor.Update(emptySpan).Error().code
        == CompressionErrorCode::InvalidState);
    const auto lateDecompressFinish = decompressor.Finish();
    REQUIRE_FALSE(lateDecompressFinish.HasValue());
    REQUIRE(lateDecompressFinish.Error().code
        == CompressionErrorCode::InvalidState);

    // A failed compressor latches the first error and repeats it.
    CompressionSettings smallSettings;
    smallSettings.maxOutputBytes = 64;
    auto smallCompressorResult = SegmentCompressor::Create(
        smallSettings, 0);
    REQUIRE(smallCompressorResult.HasValue());
    SegmentCompressor smallCompressor =
        std::move(smallCompressorResult).Value();
    const std::vector<std::byte> randomInput =
        MakeRandomBytes(256 * 1024, 0x1234567890ABCDEFULL);
    const CompressionStatus firstUpdate = smallCompressor.Update(
        std::span<const std::byte>(randomInput));
    REQUIRE_FALSE(firstUpdate.HasValue());
    REQUIRE(firstUpdate.Error().code
        == CompressionErrorCode::OutputLimitExceeded);
    const CompressionStatus secondUpdate = smallCompressor.Update(
        std::span<const std::byte>(randomInput));
    REQUIRE(secondUpdate.Error() == firstUpdate.Error());
    const auto stalledFinish = smallCompressor.Finish();
    REQUIRE_FALSE(stalledFinish.HasValue());
    REQUIRE(stalledFinish.Error() == firstUpdate.Error());
}

TEST_CASE("KnownAnswerVectorPinsZstdFrameBytes",
          "[pbcompression][golden]")
{
    // Pinned to the manifest zstd 1.5.7 behavior for the default settings.
    // If the pinned zstd revision changes, this vector must be re-derived
    // and reviewed instead of silently accepted.
    const std::vector<std::byte> input = MakePatternBytes(256, 0x41U);
    const CompressionSettings settings;
    const auto encodedResult = pbcompression::CompressSegment(
        std::span<const std::byte>(input), settings);
    REQUIRE(encodedResult.HasValue());
    REQUIRE(encodedResult.Value().codec
        == pbprotocol::CompressionCodec::Zstandard);

    const std::string actualHex = ToHex(encodedResult.Value().bytes);
    INFO("frame size: " + std::to_string(encodedResult.Value().bytes.size()));
    INFO("frame hex: " + actualHex);

    const std::string expectedHex =
        "28b52ffd6400000d0100d04142434445464748494a4b4c4d"
        "4e4f505152535455565758595a01008e9b9a63ed3d39db";
    const std::uint64_t expectedSize = 47;
    REQUIRE(encodedResult.Value().bytes.size() == expectedSize);
    REQUIRE(actualHex == expectedHex);
}

TEST_CASE("EncodedSegmentBindsToValidDescriptors",
          "[pbcompression][integration][descriptor]")
{
    constexpr std::size_t rawBytes = 4 * 1024 * 1024;
    const std::vector<std::byte> input = MakePatternBytes(rawBytes, 0x41U);

    const CompressionSettings settings;
    std::vector<std::byte> frame;
    CompressInChunks(
        input,
        settings,
        {1024 * 1024, 1024 * 1024, 1024 * 1024, 1024 * 1024},
        frame);
    REQUIRE(HasFrameMagic(frame));
    VerifyRoundTrip(
        input, pbprotocol::CompressionCodec::Zstandard, frame);

    pbprotocol::SessionId sessionId;
    for (std::size_t byteIndex = 0; byteIndex < sessionId.bytes.size();
         byteIndex++)
    {
        sessionId.bytes[byteIndex] =
            static_cast<std::byte>(0x5EU + byteIndex);
    }

    const pbprotocol::SessionDescriptor sessionDescriptor{
        pbprotocol::GetProtocolVersion(),
        sessionId,
        input.size(),
        1,
        pbprotocol::DigestAlgorithm::Blake3_256};
    REQUIRE(pbprotocol::ValidateSessionDescriptor(sessionDescriptor)
        .HasValue());

    pbprotocol::SegmentDescriptor segmentDescriptor;
    segmentDescriptor.sessionTag =
        pbprotocol::DeriveSessionTag(sessionId);
    segmentDescriptor.segmentOrdinal = 0;
    segmentDescriptor.rawOffset = 0;
    segmentDescriptor.rawSize = input.size();
    segmentDescriptor.encodedSize = frame.size();
    segmentDescriptor.compressionCodec =
        pbprotocol::CompressionCodec::Zstandard;
    segmentDescriptor.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    segmentDescriptor.outerBlockBytes = 4096;
    segmentDescriptor.rawDigest.bytes = pbprotocol::ComputeBlake3Digest(
        std::span<const std::byte>(input));
    segmentDescriptor.encodedDigest.bytes = pbprotocol::ComputeBlake3Digest(
        std::span<const std::byte>(frame));
    segmentDescriptor.wirehairV2SerializedProfile = std::nullopt;
    REQUIRE(pbprotocol::ValidateSegmentDescriptor(
        segmentDescriptor, sessionDescriptor).HasValue());

    // Raw segments must close the encodedSize/encodedDigest rule loop.
    const std::vector<std::byte> rawInput{static_cast<std::byte>(0x77)};
    const auto rawDigestBytes = pbprotocol::ComputeBlake3Digest(
        std::span<const std::byte>(rawInput));
    pbprotocol::SegmentDescriptor rawSegmentDescriptor;
    rawSegmentDescriptor.sessionTag =
        pbprotocol::DeriveSessionTag(sessionId);
    rawSegmentDescriptor.segmentOrdinal = 0;
    rawSegmentDescriptor.rawOffset = 0;
    rawSegmentDescriptor.rawSize = 1;
    rawSegmentDescriptor.encodedSize = 1;
    rawSegmentDescriptor.compressionCodec =
        pbprotocol::CompressionCodec::Raw;
    rawSegmentDescriptor.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    rawSegmentDescriptor.outerBlockBytes = 16;
    rawSegmentDescriptor.rawDigest.bytes = rawDigestBytes;
    rawSegmentDescriptor.encodedDigest.bytes = rawDigestBytes;
    rawSegmentDescriptor.wirehairV2SerializedProfile = std::nullopt;
    REQUIRE(pbprotocol::ValidateSegmentDescriptor(
        rawSegmentDescriptor, sessionDescriptor).HasValue());
}