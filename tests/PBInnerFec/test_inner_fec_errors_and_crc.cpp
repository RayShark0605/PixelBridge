#include "inner_fec_test_helpers.h"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace pbinnertectest;
using namespace pbinnerfec;

constexpr std::int16_t kChannelLlrMagnitude = 8192;
constexpr std::uint8_t kOutputSentinel = 0xA5;

const InnerFecProfileId kProfileIds[] = {
    kInnerFecProfileIdRobust, kInnerFecProfileIdBalanced,
    kInnerFecProfileIdFast,
};

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

// Encodes info, builds the hard channel LLR at kChannelLlrMagnitude, and
// flips flipCount distinct deterministic bit positions (drawn from the
// caller's PRNG after the information bytes, mirroring the pinned
// reference sweep).
[[nodiscard]] bool MakeFlippedChannel(
    const InnerFecProfileId profileId,
    SplitMix64& random,
    const std::uint32_t flipCount,
    std::vector<std::byte>& infoOut,
    std::vector<std::int16_t>& llrOut)
{
    const InnerFecProfile* profile =
        GetInnerFecProfile(profileId);
    if (profile == nullptr)
    {
        return false;
    }
    infoOut = MakeInfoBytes(profile->GetInfoByteCount(), random);
    const std::vector<std::byte> codeword =
        EncodeCodewordOrDie(profileId, infoOut);
    llrOut = MakeCleanLlr(codeword, kChannelLlrMagnitude);
    const std::vector<std::uint32_t> flippedBits =
        MakeDistinctBitIndices(random, flipCount, profile->nBits);
    FlipLlr(llrOut, flippedBits);
    return true;
}

} // namespace

TEST_CASE(
    "InnerFecErrors small random flips are always corrected to the original information",
    "[innerfec][errors][correction]")
{
    // Core invariance gate: any *successful* decode of a corrupted channel
    // must reproduce the original information bits exactly. Success means a
    // valid codeword; a different valid codeword has different information,
    // so this comparison catches any silent wrong-codeword convergence.
    const std::uint32_t kFlipCounts[] = {1u, 3u, 5u, 10u, 20u, 50u};
    const std::uint32_t kSeeds[] = {0x511u, 0x512u, 0x513u};
    for (const InnerFecProfileId profileId : kProfileIds)
    {
        auto decoderResult = QcLdpcDecoder::Create(profileId);
        REQUIRE(decoderResult);
        QcLdpcDecoder decoder = std::move(decoderResult).Value();
        for (const std::uint32_t flipCount : kFlipCounts)
        {
            for (const std::uint32_t seed : kSeeds)
            {
                SplitMix64 random(seed);
                std::vector<std::byte> info;
                std::vector<std::int16_t> llr;
                REQUIRE(MakeFlippedChannel(profileId, random,
                    flipCount, info, llr));
                std::vector<std::byte> output = MakeSentinelOutput();
                InnerFecDecodeOptions options;
                const auto result =
                    decoder.Decode(llr, options, output);
                REQUIRE(result);
                const std::span<const std::byte> recoveredInfo =
                    std::span<const std::byte>(
                        output.data(), info.size());
                REQUIRE(recoveredInfo.size() == info.size());
                CHECK(std::equal(
                    recoveredInfo.begin(), recoveredInfo.end(),
                    info.begin()));
            }
        }
    }
}

TEST_CASE(
    "InnerFecErrors over-capacity random flips fail closed with the output untouched",
    "[innerfec][errors][failure]")
{
    // Pinned over-capacity regime: 800 deterministic random flips at the
    // pinned channel magnitude exceed the reference decoder's correction
    // capability for every profile (deterministic; the pinned reference
    // sweep failed all three profiles on these exact seeds).
    for (const InnerFecProfileId profileId : kProfileIds)
    {
        auto decoderResult = QcLdpcDecoder::Create(profileId);
        REQUIRE(decoderResult);
        QcLdpcDecoder decoder = std::move(decoderResult).Value();
        SplitMix64 random(0x511u);
        std::vector<std::byte> info;
        std::vector<std::int16_t> llr;
        REQUIRE(MakeFlippedChannel(profileId, random, 800u,
            info, llr));
        std::vector<std::byte> output = MakeSentinelOutput();
        InnerFecDecodeOptions options;
        const auto result = decoder.Decode(llr, options, output);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            InnerFecErrorCode::SyndromeFailure);
        CHECK(result.Error().detail == options.maxIterations);
        CHECK(IsAllSentinel(output));
    }
}

TEST_CASE(
    "InnerFecErrors correction depth depends on the profile (pinned contrast)",
    "[innerfec][errors][contrast]")
{
    // Robust (K=10800) corrects 500 deterministic random flips on the
    // pinned seed; Fast (K=13320) does not correct 300. Both sides of this
    // contrast are pinned deterministic reference behavior.
    auto robustDecoder =
        QcLdpcDecoder::Create(kInnerFecProfileIdRobust);
    REQUIRE(robustDecoder);
    QcLdpcDecoder robust = std::move(robustDecoder).Value();
    SplitMix64 robustRandom(0x511u);
    std::vector<std::byte> robustInfo;
    std::vector<std::int16_t> robustLlr;
    REQUIRE(MakeFlippedChannel(kInnerFecProfileIdRobust,
        robustRandom, 500u, robustInfo, robustLlr));
    std::vector<std::byte> robustOutput = MakeSentinelOutput();
    InnerFecDecodeOptions options;
    const auto robustResult =
        robust.Decode(robustLlr, options, robustOutput);
    REQUIRE(robustResult);
    CHECK(std::equal(robustInfo.begin(), robustInfo.end(),
        robustOutput.begin()));

    auto fastDecoder =
        QcLdpcDecoder::Create(kInnerFecProfileIdFast);
    REQUIRE(fastDecoder);
    QcLdpcDecoder fast = std::move(fastDecoder).Value();
    SplitMix64 fastRandom(0x511u);
    std::vector<std::byte> fastInfo;
    std::vector<std::int16_t> fastLlr;
    REQUIRE(MakeFlippedChannel(kInnerFecProfileIdFast,
        fastRandom, 300u, fastInfo, fastLlr));
    std::vector<std::byte> fastOutput = MakeSentinelOutput();
    const auto fastResult = fast.Decode(fastLlr, options, fastOutput);
    CHECK_FALSE(fastResult);
    CHECK(fastResult.Error().code ==
        InnerFecErrorCode::SyndromeFailure);
    CHECK(IsAllSentinel(fastOutput));
}

TEST_CASE(
    "InnerFecErrors a different valid codeword decodes as itself (CRC is the gate)",
    "[innerfec][errors][valid-codeword]")
{
    // The codec layer cannot distinguish "the sender's codeword" from any
    // other valid codeword: a perfectly clean channel carrying codeword B
    // must decode to B. The transport CRC of the information bytes is what
    // lets the caller accept or reject (design document section 14.3).
    const InnerFecProfileId profileId = kInnerFecProfileIdBalanced;
    const InnerFecProfile* profile =
        GetInnerFecProfile(profileId);
    REQUIRE(profile != nullptr);
    const std::uint32_t infoBytes = profile->GetInfoByteCount();

    const std::vector<std::byte> infoA =
        MakePatternInfoBits(profile->kBits, 0xC0FFEEu);
    std::vector<std::byte> infoB(infoBytes);
    for (std::size_t byteIndex = 0; byteIndex < infoB.size();
        byteIndex++)
    {
        infoB[byteIndex] =
            std::byte{std::uint8_t(std::to_integer<std::uint8_t>(infoA[byteIndex])
                ^ 0xFFu)};
    }
    const std::vector<std::byte> codewordA =
        EncodeCodewordOrDie(profileId, infoA);
    const std::vector<std::byte> codewordB =
        EncodeCodewordOrDie(profileId, infoB);
    CHECK(codewordA != codewordB);
    CHECK(pbprotocol::ComputeCrc32c(infoA) !=
        pbprotocol::ComputeCrc32c(infoB));

    auto decoderResult = QcLdpcDecoder::Create(profileId);
    REQUIRE(decoderResult);
    QcLdpcDecoder decoder = std::move(decoderResult).Value();
    const std::vector<std::int16_t> llrB =
        MakeCleanLlr(codewordB, 16384);
    std::vector<std::byte> output = MakeSentinelOutput();
    InnerFecDecodeOptions options;
    const auto result = decoder.Decode(llrB, options, output);
    REQUIRE(result);
    CHECK(output == codewordB);
    CHECK(std::equal(infoB.begin(), infoB.end(), output.begin()));
    CHECK(pbprotocol::ComputeCrc32c(
        std::span<const std::byte>(output.data(), infoBytes)) ==
        pbprotocol::ComputeCrc32c(infoB));
}

TEST_CASE(
    "InnerFecCrc transport CRC32C integration over the decode pipeline",
    "[innerfec][errors][crc]")
{
    const InnerFecProfileId profileId = kInnerFecProfileIdFast;
    const InnerFecProfile* profile =
        GetInnerFecProfile(profileId);
    REQUIRE(profile != nullptr);
    const std::uint32_t infoBytes = profile->GetInfoByteCount();

    auto decoderResult = QcLdpcDecoder::Create(profileId);
    REQUIRE(decoderResult);
    QcLdpcDecoder decoder = std::move(decoderResult).Value();
    InnerFecDecodeOptions options;

    // Success path: three deterministic flips at the pinned channel
    // magnitude; the recovered information CRC matches the original.
    {
        SplitMix64 random(0x5EEDu);
        std::vector<std::byte> info;
        std::vector<std::int16_t> llr;
        REQUIRE(MakeFlippedChannel(profileId, random, 3u,
            info, llr));
        const std::vector<std::byte> encoded =
            EncodeCodewordOrDie(profileId, info);
        std::vector<std::byte> output = MakeSentinelOutput();
        const auto result = decoder.Decode(llr, options, output);
        REQUIRE(result);
        CHECK(output == encoded);
        CHECK(pbprotocol::ComputeCrc32c(
            std::span<const std::byte>(output.data(), infoBytes)) ==
            pbprotocol::ComputeCrc32c(info));
    }

    // Failure path: over-capacity corruption publishes no output; the
    // caller therefore never computes a CRC over garbage and the sentinel
    // output proves nothing was written.
    {
        SplitMix64 random(0x511u);
        std::vector<std::byte> info;
        std::vector<std::int16_t> llr;
        REQUIRE(MakeFlippedChannel(profileId, random, 800u,
            info, llr));
        std::vector<std::byte> output = MakeSentinelOutput();
        const auto result = decoder.Decode(llr, options, output);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            InnerFecErrorCode::SyndromeFailure);
        CHECK(IsAllSentinel(output));
    }

    // Low-confidence boundary (pinned): with |LLR| = 5 (far below the
    // default offset 2048) no min-sum message ever exceeds the offset, so
    // the decoder returns the channel hard decision. A clean low-confidence
    // channel of a valid codeword still "succeeds" - success at low
    // confidence is not proof of the sender's intent; the CRC gate stays
    // with the caller.
    {
        SplitMix64 random(0x5EEDu);
        std::vector<std::byte> info;
        std::vector<std::int16_t> llr;
        REQUIRE(MakeFlippedChannel(profileId, random, 0u,
            info, llr));
        for (std::size_t index = 0; index < llr.size(); index++)
        {
            llr[index] = llr[index] > 0 ? 5 : -5;
        }
        std::vector<std::byte> output = MakeSentinelOutput();
        const auto result = decoder.Decode(llr, options, output);
        REQUIRE(result);
        CHECK(result.Value().iterationsUsed == 1);
        CHECK(pbprotocol::ComputeCrc32c(
            std::span<const std::byte>(output.data(), infoBytes)) ==
            pbprotocol::ComputeCrc32c(info));
    }

    // Low-confidence corrupted channel (pinned): the same magnitude with
    // two flipped bits hard-decodes a non-codeword and fails closed after
    // all iterations (the messages never move, so no rescue is possible).
    {
        SplitMix64 random(0x5EEDu);
        std::vector<std::byte> info;
        std::vector<std::int16_t> llr;
        REQUIRE(MakeFlippedChannel(profileId, random, 2u,
            info, llr));
        for (std::size_t index = 0; index < llr.size(); index++)
        {
            llr[index] = llr[index] > 0 ? 5 : -5;
        }
        std::vector<std::byte> output = MakeSentinelOutput();
        const auto result = decoder.Decode(llr, options, output);
        CHECK_FALSE(result);
        CHECK(result.Error().code ==
            InnerFecErrorCode::SyndromeFailure);
        CHECK(result.Error().detail == options.maxIterations);
        CHECK(IsAllSentinel(output));
    }

    // Absolute low-confidence boundary (pinned): |LLR| = 1, the smallest
    // nonzero magnitude, of a clean valid codeword. Messages never move
    // (1 < offset), the channel hard decision is the codeword itself, so
    // the first pass succeeds with the original information.
    {
        SplitMix64 random(0x5EEDu);
        std::vector<std::byte> info;
        std::vector<std::int16_t> llr;
        REQUIRE(MakeFlippedChannel(profileId, random, 0u,
            info, llr));
        for (std::size_t index = 0; index < llr.size(); index++)
        {
            llr[index] = llr[index] > 0 ? 1 : -1;
        }
        std::vector<std::byte> output = MakeSentinelOutput();
        const auto result = decoder.Decode(llr, options, output);
        REQUIRE(result);
        CHECK(result.Value().iterationsUsed == 1);
        CHECK(pbprotocol::ComputeCrc32c(
            std::span<const std::byte>(output.data(), infoBytes)) ==
            pbprotocol::ComputeCrc32c(info));
    }
}

TEST_CASE("InnerFecSyndrome rejects malformed inputs",
    "[innerfec][errors][syndrome]")
{
    // The syndrome entry point validates the frozen identity and the exact
    // codeword size before any bit access (same contract as the encoder and
    // decoder entry points); pinned here so a future reordering of the
    // checks cannot regress the fail-closed behavior.
    for (const InnerFecProfileId profileId : kProfileIds)
    {
        const InnerFecProfile* profile = GetInnerFecProfile(profileId);
        REQUIRE(profile != nullptr);
        const std::uint32_t codewordBytes =
            profile->GetCodewordByteCount();
        std::vector<std::byte> codeword(codewordBytes);

        const auto unknown =
            ComputeQcLdpcSyndrome(0x1234ULL, codeword);
        CHECK_FALSE(unknown);
        CHECK(unknown.Error().code ==
            InnerFecErrorCode::UnknownProfileId);
        CHECK(unknown.Error().detail == 0x1234ULL);

        const auto shortResult =
            ComputeQcLdpcSyndrome(profileId,
                std::span<const std::byte>(
                    codeword.data(), codewordBytes - 1u));
        CHECK_FALSE(shortResult);
        CHECK(shortResult.Error().code ==
            InnerFecErrorCode::InvalidInput);
        CHECK(shortResult.Error().detail == codewordBytes);

        const auto longResult =
            ComputeQcLdpcSyndrome(profileId,
                std::span<const std::byte>(
                    codeword.data(), codewordBytes + 1u));
        CHECK_FALSE(longResult);
        CHECK(longResult.Error().code ==
            InnerFecErrorCode::InvalidInput);

        // A correctly sized all-zero codeword is accepted by the entry
        // point (zero is a valid codeword for every profile).
        const auto valid =
            ComputeQcLdpcSyndrome(profileId, codeword);
        CHECK(valid);
        CHECK(valid.Value());
    }
}
