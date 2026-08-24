#include "inner_fec_test_helpers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace pbinnertectest;
using namespace pbinnerfec;

constexpr std::int16_t kCleanLlrMagnitude = 16384;
constexpr std::int16_t kErrorLlrMagnitude = 8192;
constexpr std::uint8_t kOutputSentinel = 0xA5;

[[nodiscard]] std::vector<std::byte> MakeSentinelOutput()
{
    return std::vector<std::byte>(kDvbS2ShortFrameCodewordByteCount,
        std::byte{kOutputSentinel});
}

bool IsAllSentinel(const std::span<const std::byte> data)
{
    for (const std::byte value : data)
    {
        if (value != std::byte{kOutputSentinel})
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<std::byte> MakeRandomInfo(
    const InnerFecProfileId profileId, const std::uint32_t seed)
{
    const InnerFecProfile* profile = GetInnerFecProfile(profileId);
    SplitMix64 random(seed);
    return MakeInfoBytes(profile->GetInfoByteCount(), random);
}

} // namespace

TEST_CASE(
    "InnerFecDecoder clean channel recodes exactly and terminates after one pass",
    "[innerfec][decoder][clean]")
{
    const InnerFecProfileId kProfileIds[] = {
        kInnerFecProfileIdRobust, kInnerFecProfileIdBalanced,
        kInnerFecProfileIdFast,
    };
    for (const InnerFecProfileId profileId : kProfileIds)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(profileId);
        REQUIRE(profile != nullptr);
        auto decoderResult = QcLdpcDecoder::Create(profileId);
        REQUIRE(decoderResult);
        QcLdpcDecoder decoder = std::move(decoderResult).Value();
        CHECK(decoder.GetProfile() == profile);

        const std::vector<std::byte> info =
            MakeRandomInfo(profileId, 0xC0D1E5u);
        const std::vector<std::byte> codeword =
            EncodeCodewordOrDie(profileId, info);
        const std::vector<std::int16_t> llr =
            MakeCleanLlr(codeword, kCleanLlrMagnitude);
        std::vector<std::byte> output = MakeSentinelOutput();

        InnerFecDecodeOptions options; // documented defaults
        const auto result = decoder.Decode(llr, options, output);
        REQUIRE(result);
        CHECK(result.Value().iterationsUsed == 1);
        CHECK(result.Value().syndromePassedIteration == 1);
        CHECK(output == codeword);
        CHECK(std::equal(info.begin(), info.end(), output.begin()));
    }
}

TEST_CASE(
    "InnerFecDecoder all-inverted channel fails closed with the output untouched",
    "[innerfec][decoder][inverted]")
{
    const InnerFecProfileId kProfileIds[] = {
        kInnerFecProfileIdRobust, kInnerFecProfileIdBalanced,
        kInnerFecProfileIdFast,
    };
    for (const InnerFecProfileId profileId : kProfileIds)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(profileId);
        REQUIRE(profile != nullptr);
        auto decoderResult = QcLdpcDecoder::Create(profileId);
        REQUIRE(decoderResult);
        QcLdpcDecoder decoder = std::move(decoderResult).Value();

        const std::vector<std::byte> info =
            MakeRandomInfo(profileId, 0xC0D1E5u);
        const std::vector<std::byte> codeword =
            EncodeCodewordOrDie(profileId, info);
        const std::vector<std::int16_t> cleanLlr =
            MakeCleanLlr(codeword, kCleanLlrMagnitude);
        std::vector<std::int16_t> invertedLlr = cleanLlr;
        for (std::size_t index = 0; index < invertedLlr.size(); index++)
        {
            invertedLlr[index] = -invertedLlr[index];
        }
        std::vector<std::byte> output = MakeSentinelOutput();

        InnerFecDecodeOptions options;
        const auto result =
            decoder.Decode(invertedLlr, options, output);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            InnerFecErrorCode::SyndromeFailure);
        CHECK(result.Error().detail == options.maxIterations);
        CHECK(IsAllSentinel(output));
    }
}

TEST_CASE(
    "InnerFecDecoder all-zero LLR hard-decodes the valid all-zero codeword",
    "[innerfec][decoder][zero-llr]")
{
    // Pinned low-confidence boundary semantics: zero LLR means zero
    // confidence; the hard decision is the all-zero vector, which is a
    // valid codeword, so decoding "succeeds" in one pass. Success at this
    // confidence proves nothing about the sender; the transport CRC of the
    // information bytes is the caller's final gate.
    const InnerFecProfileId kProfileIds[] = {
        kInnerFecProfileIdRobust, kInnerFecProfileIdBalanced,
        kInnerFecProfileIdFast,
    };
    for (const InnerFecProfileId profileId : kProfileIds)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(profileId);
        REQUIRE(profile != nullptr);
        auto decoderResult = QcLdpcDecoder::Create(profileId);
        REQUIRE(decoderResult);
        QcLdpcDecoder decoder = std::move(decoderResult).Value();

        const std::vector<std::int16_t> llr(profile->nBits, 0);
        std::vector<std::byte> output(MakeSentinelOutput());

        InnerFecDecodeOptions options;
        const auto result = decoder.Decode(llr, options, output);
        REQUIRE(result);
        CHECK(result.Value().iterationsUsed == 1);
        for (const std::byte value : output)
        {
            CHECK(value == std::byte{0});
        }
    }
}

TEST_CASE("InnerFecDecoder normalized min-sum (scale 3/4) clean round trip",
    "[innerfec][decoder][scale]")
{
    const InnerFecProfileId kProfileIds[] = {
        kInnerFecProfileIdRobust, kInnerFecProfileIdBalanced,
        kInnerFecProfileIdFast,
    };
    for (const InnerFecProfileId profileId : kProfileIds)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(profileId);
        REQUIRE(profile != nullptr);
        auto decoderResult = QcLdpcDecoder::Create(profileId);
        REQUIRE(decoderResult);
        QcLdpcDecoder decoder = std::move(decoderResult).Value();

        const std::vector<std::byte> info =
            MakeRandomInfo(profileId, 0xC0D1E5u);
        const std::vector<std::byte> codeword =
            EncodeCodewordOrDie(profileId, info);
        const std::vector<std::int16_t> llr =
            MakeCleanLlr(codeword, kCleanLlrMagnitude);
        std::vector<std::byte> output = MakeSentinelOutput();

        InnerFecDecodeOptions options;
        options.scaleNum = 3;
        options.scaleDen = 4;
        const auto result = decoder.Decode(llr, options, output);
        REQUIRE(result);
        CHECK(result.Value().iterationsUsed == 1);
        CHECK(output == codeword);
    }
}

TEST_CASE("InnerFecDecoder single flipped bit is corrected in one pass",
    "[innerfec][decoder][single-flip]")
{
    // A single error bit flips the parity of its incident rows; one layered
    // pass of min-sum evidence moves the flipped variable back (its rows all
    // blame exactly one variable) and the syndrome passes, even when the
    // caller allows only a single iteration.
    const InnerFecProfileId kProfileIds[] = {
        kInnerFecProfileIdRobust, kInnerFecProfileIdBalanced,
        kInnerFecProfileIdFast,
    };
    for (const InnerFecProfileId profileId : kProfileIds)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(profileId);
        REQUIRE(profile != nullptr);
        auto decoderResult = QcLdpcDecoder::Create(profileId);
        REQUIRE(decoderResult);
        QcLdpcDecoder decoder = std::move(decoderResult).Value();

        SplitMix64 random(0x511u);
        const std::vector<std::byte> info =
            MakeInfoBytes(profile->GetInfoByteCount(), random);
        const std::vector<std::byte> codeword =
            EncodeCodewordOrDie(profileId, info);
        std::vector<std::int16_t> llr =
            MakeCleanLlr(codeword, kErrorLlrMagnitude);
        const std::uint32_t flippedBit =
            static_cast<std::uint32_t>(
                random.Next() % profile->nBits);
        llr[flippedBit] = -llr[flippedBit];
        std::vector<std::byte> output = MakeSentinelOutput();

        InnerFecDecodeOptions options;
        options.maxIterations = 1;
        const auto result = decoder.Decode(llr, options, output);
        REQUIRE(result);
        CHECK(result.Value().iterationsUsed == 1);
        CHECK(output == codeword);
        CHECK(std::equal(info.begin(), info.end(), output.begin()));
    }
}

TEST_CASE("InnerFecDecoder decode is deterministic for identical inputs",
    "[innerfec][decoder][determinism]")
{
    const InnerFecProfile* profile =
        GetInnerFecProfile(kInnerFecProfileIdBalanced);
    REQUIRE(profile != nullptr);
    auto firstDecoder = QcLdpcDecoder::Create(
        kInnerFecProfileIdBalanced);
    REQUIRE(firstDecoder);
    auto secondDecoder = QcLdpcDecoder::Create(
        kInnerFecProfileIdBalanced);
    REQUIRE(secondDecoder);
    QcLdpcDecoder decoderOne = std::move(firstDecoder).Value();
    QcLdpcDecoder decoderTwo = std::move(secondDecoder).Value();

    const std::vector<std::byte> info =
        MakeRandomInfo(kInnerFecProfileIdBalanced, 0xC0D1E5u);
    const std::vector<std::byte> codeword =
        EncodeCodewordOrDie(kInnerFecProfileIdBalanced, info);
    const std::vector<std::int16_t> llr =
        MakeCleanLlr(codeword, kCleanLlrMagnitude);
    InnerFecDecodeOptions options;

    std::vector<std::byte> outputOne(
        kDvbS2ShortFrameCodewordByteCount);
    std::vector<std::byte> outputTwo(
        kDvbS2ShortFrameCodewordByteCount);
    const auto firstResult = decoderOne.Decode(llr, options, outputOne);
    const auto secondResult = decoderTwo.Decode(llr, options, outputTwo);
    REQUIRE(firstResult);
    REQUIRE(secondResult);
    CHECK(outputOne == outputTwo);
    CHECK(outputOne == codeword);
    CHECK(firstResult.Value() == secondResult.Value());

    // A single instance is reusable: each call re-initializes its
    // workspace from the channel LLR, so repeated identical calls on the
    // same instance stay deterministic.
    std::vector<std::byte> outputRepeat(
        kDvbS2ShortFrameCodewordByteCount);
    const auto repeatResult =
        decoderOne.Decode(llr, options, outputRepeat);
    REQUIRE(repeatResult);
    CHECK(outputRepeat == outputOne);
    CHECK(repeatResult.Value() == firstResult.Value());

    // A wrong-sized output span is rejected before any write.
    std::vector<std::byte> shortOutput(
        kDvbS2ShortFrameCodewordByteCount - 1);
    const auto shortResult = decoderOne.Decode(llr, options, shortOutput);
    CHECK_FALSE(shortResult);
    CHECK(shortResult.Error().code ==
        InnerFecErrorCode::InvalidInput);
}

TEST_CASE("InnerFecDecoder option bounds are enforced",
    "[innerfec][decoder][options]")
{
    const InnerFecProfile* profile =
        GetInnerFecProfile(kInnerFecProfileIdFast);
    REQUIRE(profile != nullptr);
    auto decoderResult =
        QcLdpcDecoder::Create(kInnerFecProfileIdFast);
    REQUIRE(decoderResult);
    QcLdpcDecoder decoder = std::move(decoderResult).Value();

    const std::vector<std::byte> info =
        MakeRandomInfo(kInnerFecProfileIdFast, 0xC0D1E5u);
    const std::vector<std::byte> codeword =
        EncodeCodewordOrDie(kInnerFecProfileIdFast, info);
    const std::vector<std::int16_t> llr =
        MakeCleanLlr(codeword, kCleanLlrMagnitude);
    std::vector<std::byte> output(kDvbS2ShortFrameCodewordByteCount);

    InnerFecDecodeOptions options;
    options.maxIterations = 0;
    const auto zeroIterations = decoder.Decode(llr, options, output);
    CHECK_FALSE(zeroIterations);
    CHECK(zeroIterations.Error().code ==
        InnerFecErrorCode::InvalidOption);

    options.maxIterations = kQcLdpcMaxIterations + 1;
    const auto tooManyIterations = decoder.Decode(llr, options, output);
    CHECK_FALSE(tooManyIterations);
    CHECK(tooManyIterations.Error().code ==
        InnerFecErrorCode::InvalidOption);
    CHECK(tooManyIterations.Error().detail ==
        kQcLdpcMaxIterations + 1);

    options.maxIterations = kQcLdpcMaxIterations;
    options.syndromeCheckInterval = 0;
    const auto zeroInterval = decoder.Decode(llr, options, output);
    CHECK_FALSE(zeroInterval);
    CHECK(zeroInterval.Error().code ==
        InnerFecErrorCode::InvalidOption);

    options.syndromeCheckInterval =
        kQcLdpcMaxIterations + 1;
    const auto tooLargeInterval = decoder.Decode(llr, options, output);
    CHECK_FALSE(tooLargeInterval);
    CHECK(tooLargeInterval.Error().code ==
        InnerFecErrorCode::InvalidOption);
    // An interval equal to maxIterations is accepted.
    options.syndromeCheckInterval = kQcLdpcMaxIterations;
    CHECK(decoder.Decode(llr, options, output));

    options = InnerFecDecodeOptions{};
    options.offset = kQcLdpcMaxLlrOffset + 1;
    const auto tooLargeOffset = decoder.Decode(llr, options, output);
    CHECK_FALSE(tooLargeOffset);
    CHECK(tooLargeOffset.Error().code ==
        InnerFecErrorCode::InvalidOption);
    options.offset = kQcLdpcMaxLlrOffset;
    CHECK(decoder.Decode(llr, options, output));

    options = InnerFecDecodeOptions{};
    options.scaleDen = 0;
    const auto zeroDen = decoder.Decode(llr, options, output);
    CHECK_FALSE(zeroDen);
    CHECK(zeroDen.Error().code == InnerFecErrorCode::InvalidOption);

    options.scaleDen = kQcLdpcMaxLlrScale + 1;
    const auto tooLargeDen = decoder.Decode(llr, options, output);
    CHECK_FALSE(tooLargeDen);
    CHECK(tooLargeDen.Error().code ==
        InnerFecErrorCode::InvalidOption);

    options.scaleDen = kQcLdpcMaxLlrScale;
    options.scaleNum = kQcLdpcMaxLlrScale + 1;
    const auto amplifyingScale = decoder.Decode(llr, options, output);
    CHECK_FALSE(amplifyingScale);
    CHECK(amplifyingScale.Error().code ==
        InnerFecErrorCode::InvalidOption);
    // scaleNum == scaleDen (exact min-sum at the scale bound) is accepted.
    options.scaleNum = kQcLdpcMaxLlrScale;
    CHECK(decoder.Decode(llr, options, output));
}

TEST_CASE("InnerFecDecoder rejects malformed llr and output spans",
    "[innerfec][decoder][errors]")
{
    const InnerFecProfile* profile =
        GetInnerFecProfile(kInnerFecProfileIdFast);
    REQUIRE(profile != nullptr);
    auto decoderResult =
        QcLdpcDecoder::Create(kInnerFecProfileIdFast);
    REQUIRE(decoderResult);
    QcLdpcDecoder decoder = std::move(decoderResult).Value();

    const std::vector<std::int16_t> shortLlr(profile->nBits - 1);
    std::vector<std::byte> output(kDvbS2ShortFrameCodewordByteCount);
    InnerFecDecodeOptions options;
    const auto shortLlrResult =
        decoder.Decode(shortLlr, options, output);
    CHECK_FALSE(shortLlrResult);
    CHECK(shortLlrResult.Error().code ==
        InnerFecErrorCode::InvalidInput);
    CHECK(shortLlrResult.Error().detail == profile->nBits);

    const std::vector<std::int16_t> longLlr(profile->nBits + 1);
    const auto longLlrResult =
        decoder.Decode(longLlr, options, output);
    CHECK_FALSE(longLlrResult);
    CHECK(longLlrResult.Error().code ==
        InnerFecErrorCode::InvalidInput);

    const std::vector<std::int16_t> llr(profile->nBits, 1);
    std::vector<std::byte> shortOutput(
        kDvbS2ShortFrameCodewordByteCount - 1);
    const auto shortOutputResult =
        decoder.Decode(llr, options, shortOutput);
    CHECK_FALSE(shortOutputResult);
    CHECK(shortOutputResult.Error().code ==
        InnerFecErrorCode::InvalidInput);

    std::vector<std::byte> longOutput(
        kDvbS2ShortFrameCodewordByteCount + 1);
    const auto longOutputResult =
        decoder.Decode(llr, options, longOutput);
    CHECK_FALSE(longOutputResult);
    CHECK(longOutputResult.Error().code ==
        InnerFecErrorCode::InvalidInput);
}

TEST_CASE(
    "InnerFecDecoder default-constructed and moved-from instances fail closed",
    "[innerfec][decoder][state]")
{
    const InnerFecProfile* profile =
        GetInnerFecProfile(kInnerFecProfileIdFast);
    REQUIRE(profile != nullptr);
    const std::vector<std::int16_t> llr(profile->nBits, 1);
    std::vector<std::byte> output(kDvbS2ShortFrameCodewordByteCount);
    InnerFecDecodeOptions options;

    QcLdpcDecoder empty;
    CHECK(empty.GetProfile() == nullptr);
    const auto emptyResult = empty.Decode(llr, options, output);
    CHECK_FALSE(emptyResult);
    CHECK(emptyResult.Error().code == InnerFecErrorCode::InvalidState);

    auto created = QcLdpcDecoder::Create(kInnerFecProfileIdFast);
    REQUIRE(created);
    QcLdpcDecoder movedFrom = std::move(created).Value();
    REQUIRE(movedFrom.GetProfile() != nullptr);
    QcLdpcDecoder movedTo = std::move(movedFrom);
    CHECK(movedFrom.GetProfile() == nullptr);
    CHECK(movedTo.GetProfile() != nullptr);
    const auto movedFromResult =
        movedFrom.Decode(llr, options, output);
    CHECK_FALSE(movedFromResult);
    CHECK(movedFromResult.Error().code ==
        InnerFecErrorCode::InvalidState);
    CHECK(movedTo.Decode(llr, options, output));

    // Create fails closed for unknown profiles.
    const auto unknown = QcLdpcDecoder::Create(0xABCDULL);
    CHECK_FALSE(unknown);
    CHECK(unknown.Error().code ==
        InnerFecErrorCode::UnknownProfileId);
    CHECK(unknown.Error().detail == 0xABCDULL);
}
