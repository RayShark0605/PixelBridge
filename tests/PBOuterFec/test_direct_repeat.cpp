#include "pbouterfec/direct_repeat.h"
#include "pbouterfec/wirehair_v2.h"

#include "decoder_test_access.h"

#include "pbcompression/segment_compression.h"
#include "pbcompression/segment_decompression.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_version.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

static_assert(!std::is_copy_constructible_v<
    pbouterfec::DirectRepeatEncoder>);
static_assert(!std::is_copy_assignable_v<
    pbouterfec::DirectRepeatEncoder>);
static_assert(std::is_nothrow_move_constructible_v<
    pbouterfec::DirectRepeatEncoder>);
static_assert(std::is_nothrow_move_assignable_v<
    pbouterfec::DirectRepeatEncoder>);
static_assert(!std::is_copy_constructible_v<
    pbouterfec::DirectRepeatDecoder>);
static_assert(!std::is_copy_assignable_v<
    pbouterfec::DirectRepeatDecoder>);
static_assert(std::is_nothrow_move_constructible_v<
    pbouterfec::DirectRepeatDecoder>);
static_assert(std::is_nothrow_move_assignable_v<
    pbouterfec::DirectRepeatDecoder>);
static_assert(std::is_same_v<
    pbouterfec::WirehairV2DecoderResourceManager,
    pbouterfec::OuterFecDecoderResourceManager>);
static_assert(static_cast<std::uint8_t>(
    pbouterfec::OuterFecErrorCode::InvalidState) == 5U);
static_assert(static_cast<std::uint8_t>(
    pbouterfec::OuterFecErrorCode::UnsupportedPlatform) == 18U);

[[nodiscard]] constexpr std::byte Byte(
    const std::uint8_t value) noexcept
{
    return static_cast<std::byte>(value);
}

[[nodiscard]] std::vector<std::byte> MakeMessage(
    const std::size_t byteCount)
{
    std::vector<std::byte> message(byteCount);
    for (std::size_t byteIndex = 0; byteIndex < message.size(); byteIndex++)
    {
        message[byteIndex] = Byte(static_cast<std::uint8_t>(
            byteIndex * 73U + 11U));
    }
    return message;
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeDirectDescriptor(
    const std::span<const std::byte> encodedSegment,
    const std::uint32_t outerBlockBytes)
{
    pbprotocol::SegmentDescriptor descriptor{};
    descriptor.encodedSize = static_cast<std::uint64_t>(
        encodedSegment.size());
    descriptor.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    descriptor.outerBlockBytes = outerBlockBytes;
    descriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::ComputeBlake3Digest(encodedSegment)};
    descriptor.wirehairV2SerializedProfile = std::nullopt;
    return descriptor;
}

[[nodiscard]] pbprotocol::SessionDescriptor MakeSessionDescriptor(
    const std::uint64_t originalFileSize,
    const std::uint64_t segmentCount)
{
    pbprotocol::SessionId sessionId{};
    for (std::size_t byteIndex = 0;
        byteIndex < sessionId.bytes.size();
        byteIndex++)
    {
        sessionId.bytes[byteIndex] = Byte(static_cast<std::uint8_t>(
            0x31U + byteIndex));
    }

    return pbprotocol::SessionDescriptor{
        pbprotocol::GetProtocolVersion(),
        sessionId,
        originalFileSize,
        segmentCount,
        pbprotocol::DigestAlgorithm::Blake3_256};
}

[[nodiscard]] pbouterfec::OuterFecDecoderResourceManager
MakeResourceManager(
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy())
{
    auto managerResult =
        pbouterfec::OuterFecDecoderResourceManager::Create(resourcePolicy);
    REQUIRE(managerResult);
    return std::move(managerResult).Value();
}

struct DirectBlock
{
    std::uint32_t payloadBytes = 0;
    std::vector<std::byte> paddedPayload;
};

[[nodiscard]] DirectBlock EncodeDirectBlock(
    pbouterfec::DirectRepeatEncoder& encoder,
    const std::uint32_t outerBlockId)
{
    DirectBlock block;
    block.paddedPayload.resize(encoder.GetOuterBlockBytes());
    const auto encodeResult = encoder.EncodeBlock(
        outerBlockId, block.paddedPayload);
    REQUIRE(encodeResult);
    block.payloadBytes = encodeResult.Value();
    return block;
}

[[nodiscard]] pbouterfec::DirectRepeatDecoder MakeDirectDecoder(
    const pbprotocol::SegmentDescriptor& descriptor,
    const pbouterfec::OuterFecDecoderResourceManager& resourceManager)
{
    auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
        descriptor, descriptor.outerBlockBytes, resourceManager);
    REQUIRE(decoderResult);
    return std::move(decoderResult).Value();
}

void RequireRecoveredEquals(
    pbouterfec::DirectRepeatDecoder& decoder,
    const std::span<const std::byte> expected)
{
    std::vector<std::byte> output(expected.size());
    const auto recoverResult = decoder.Recover(output);
    REQUIRE(recoverResult);
    REQUIRE(recoverResult.Value() == expected.size());
    REQUIRE(std::equal(
        output.begin(), output.end(), expected.begin(), expected.end()));
}

} // namespace

TEST_CASE("DirectRepeat mode selection applies frozen dimensions and efficiency gate",
          "[direct-repeat][mode][boundary]")
{
    const auto zeroResult = pbouterfec::ChooseOuterFecMode(0, 16);
    REQUIRE(zeroResult);
    REQUIRE(zeroResult.Value() == pbprotocol::OuterFecMode::DirectRepeat);

    const auto oneResult = pbouterfec::ChooseOuterFecMode(16, 16);
    REQUIRE(oneResult);
    REQUIRE(oneResult.Value() == pbprotocol::OuterFecMode::DirectRepeat);

    const auto twoResult = pbouterfec::ChooseOuterFecMode(17, 16);
    REQUIRE(twoResult);
    REQUIRE(twoResult.Value() == pbprotocol::OuterFecMode::DirectRepeat);

    const auto threeResult = pbouterfec::ChooseOuterFecMode(33, 16);
    REQUIRE(threeResult);
    REQUIRE(threeResult.Value() == pbprotocol::OuterFecMode::WirehairV2);

    pbouterfec::OuterFecModeSelectionPolicy tunedPolicy{};
    tunedPolicy.maximumEfficientDirectRepeatBlockCount = 3;
    const auto tunedThreeResult = pbouterfec::ChooseOuterFecMode(
        33, 16, tunedPolicy);
    REQUIRE(tunedThreeResult);
    REQUIRE(tunedThreeResult.Value() ==
        pbprotocol::OuterFecMode::DirectRepeat);
    const auto tunedFourResult = pbouterfec::ChooseOuterFecMode(
        49, 16, tunedPolicy);
    REQUIRE(tunedFourResult);
    REQUIRE(tunedFourResult.Value() ==
        pbprotocol::OuterFecMode::WirehairV2);

    const auto maximumWirehairResult = pbouterfec::ChooseOuterFecMode(
        64000, 1);
    REQUIRE(maximumWirehairResult);
    REQUIRE(maximumWirehairResult.Value() ==
        pbprotocol::OuterFecMode::WirehairV2);

    const auto excessiveWirehairResult = pbouterfec::ChooseOuterFecMode(
        64001, 1);
    REQUIRE_FALSE(excessiveWirehairResult);
    REQUIRE(excessiveWirehairResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidDimensions);
    REQUIRE(excessiveWirehairResult.Error().detail == 64001);

    pbouterfec::OuterFecModeSelectionPolicy zeroPolicy{};
    zeroPolicy.maximumEfficientDirectRepeatBlockCount = 0;
    REQUIRE_FALSE(pbouterfec::ChooseOuterFecMode(1, 16, zeroPolicy));

    pbouterfec::OuterFecModeSelectionPolicy excessivePolicy{};
    excessivePolicy.maximumEfficientDirectRepeatBlockCount = 64001;
    REQUIRE_FALSE(pbouterfec::ChooseOuterFecMode(
        1, 16, excessivePolicy));

    const auto ordinalSpaceResult = pbprotocol::GetDirectRepeatBlockCount(
        static_cast<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max()) + 1ULL,
        1);
    REQUIRE(ordinalSpaceResult);
    REQUIRE(ordinalSpaceResult.Value() ==
        static_cast<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max()) + 1ULL);

    const auto arithmeticOverflowResult =
        pbouterfec::ChooseOuterFecMode(
            std::numeric_limits<std::uint64_t>::max(), 1);
    REQUIRE_FALSE(arithmeticOverflowResult);
    REQUIRE(arithmeticOverflowResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidDimensions);

    const auto zeroBlockResult = pbouterfec::ChooseOuterFecMode(0, 0);
    REQUIRE_FALSE(zeroBlockResult);
    REQUIRE(zeroBlockResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidDimensions);

    const auto oversizedBlockResult = pbouterfec::ChooseOuterFecMode(
        1, pbprotocol::kMaximumTransportPayloadBytes + 1U);
    REQUIRE_FALSE(oversizedBlockResult);
    REQUIRE(oversizedBlockResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidDimensions);
}

TEST_CASE("DirectRepeat zero-byte path emits no blocks or segment descriptor",
          "[direct-repeat][zero-byte][descriptor]")
{
    const auto blockCountResult = pbprotocol::GetDirectRepeatBlockCount(
        0, 4096);
    REQUIRE(blockCountResult);
    REQUIRE(blockCountResult.Value() == 0);

    const std::span<const std::byte> emptySegment;
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(
        emptySegment, 4096);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    REQUIRE(encoder.GetEncodedSize() == 0);
    REQUIRE(encoder.GetBlockCount() == 0);
    REQUIRE(encoder.GetOuterBlockBytes() == 4096);

    std::array<std::byte, 8> unchangedOutput{};
    std::fill(
        unchangedOutput.begin(), unchangedOutput.end(), Byte(0xA5));
    const auto encodeResult = encoder.EncodeBlock(0, unchangedOutput);
    REQUIRE_FALSE(encodeResult);
    REQUIRE(encodeResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidInput);
    REQUIRE(std::all_of(
        unchangedOutput.begin(), unchangedOutput.end(),
        [](const std::byte value)
        {
            return value == Byte(0xA5);
        }));

    const pbprotocol::SessionDescriptor emptySession =
        MakeSessionDescriptor(0, 0);
    REQUIRE(pbprotocol::ValidateSessionDescriptor(emptySession));
    REQUIRE(pbprotocol::GetEmptyBlake3WholeFileDigest().bytes ==
        pbprotocol::ComputeBlake3Digest(emptySegment));

    const pbprotocol::SessionDescriptor nonemptySession =
        MakeSessionDescriptor(1, 1);
    pbprotocol::SegmentDescriptor invalidZeroDescriptor{};
    invalidZeroDescriptor.sessionTag = pbprotocol::DeriveSessionTag(
        nonemptySession.sessionId);
    invalidZeroDescriptor.rawSize = 1;
    invalidZeroDescriptor.encodedSize = 0;
    invalidZeroDescriptor.compressionCodec =
        pbprotocol::CompressionCodec::Zstandard;
    invalidZeroDescriptor.outerFecMode =
        pbprotocol::OuterFecMode::DirectRepeat;
    invalidZeroDescriptor.outerBlockBytes = 4096;
    const auto descriptorStatus = pbprotocol::ValidateSegmentDescriptor(
        invalidZeroDescriptor, nonemptySession);
    REQUIRE_FALSE(descriptorStatus);
    REQUIRE(descriptorStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::InvalidDescriptor);

    const pbprotocol::SegmentDescriptor zeroDescriptor =
        MakeDirectDescriptor(emptySegment, 4096);
    auto resourceManager = MakeResourceManager();
    const auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
        zeroDescriptor, zeroDescriptor.outerBlockBytes, resourceManager);
    REQUIRE_FALSE(decoderResult);
    REQUIRE(decoderResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidDescriptor);
    REQUIRE(resourceManager.GetActiveDecoderCount() == 0);
}

TEST_CASE("Default two-block efficiency gate completes DirectRepeat end to end",
          "[direct-repeat][mode][integration][tail]")
{
    constexpr std::uint32_t outerBlockBytes = 16;
    const std::vector<std::byte> message = MakeMessage(
        outerBlockBytes + 1U);
    const auto modeResult = pbouterfec::ChooseOuterFecMode(
        message.size(), outerBlockBytes);
    REQUIRE(modeResult);
    REQUIRE(modeResult.Value() == pbprotocol::OuterFecMode::DirectRepeat);

    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(
        message, outerBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    REQUIRE(encoder.GetBlockCount() == 2);

    const DirectBlock firstBlock = EncodeDirectBlock(encoder, 0);
    const DirectBlock tailBlock = EncodeDirectBlock(encoder, 1);
    REQUIRE(firstBlock.payloadBytes == outerBlockBytes);
    REQUIRE(tailBlock.payloadBytes == 1);
    REQUIRE(std::all_of(
        tailBlock.paddedPayload.begin() + 1,
        tailBlock.paddedPayload.end(),
        [](const std::byte value)
        {
            return value == Byte(0);
        }));

    const pbprotocol::SegmentDescriptor descriptor = MakeDirectDescriptor(
        message, outerBlockBytes);
    auto resourceManager = MakeResourceManager();
    pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
        descriptor, resourceManager);
    const auto tailResult = decoder.DecodeBlock(
        1, tailBlock.payloadBytes, tailBlock.paddedPayload);
    REQUIRE(tailResult);
    REQUIRE(tailResult.Value() == pbouterfec::DecodeDisposition::NeedMore);
    const auto firstResult = decoder.DecodeBlock(
        0, firstBlock.payloadBytes, firstBlock.paddedPayload);
    REQUIRE(firstResult);
    REQUIRE(firstResult.Value() == pbouterfec::DecodeDisposition::Ready);
    RequireRecoveredEquals(decoder, message);
}

TEST_CASE("DirectRepeat encoder emits canonical deterministic tail blocks",
          "[direct-repeat][encoder][golden]")
{
    const std::array<std::byte, 1> oneByteMessage{Byte(0xAB)};
    auto oneByteEncoderResult = pbouterfec::DirectRepeatEncoder::Create(
        oneByteMessage, 4);
    REQUIRE(oneByteEncoderResult);
    pbouterfec::DirectRepeatEncoder oneByteEncoder =
        std::move(oneByteEncoderResult).Value();
    REQUIRE(oneByteEncoder.GetBlockCount() == 1);
    const DirectBlock oneByteBlock = EncodeDirectBlock(oneByteEncoder, 0);
    REQUIRE(oneByteBlock.payloadBytes == 1);
    REQUIRE(oneByteBlock.paddedPayload == std::vector<std::byte>{
        Byte(0xAB), Byte(0x00), Byte(0x00), Byte(0x00)});

    const std::array<std::byte, 6> message{
        Byte(0x01), Byte(0x02), Byte(0x03),
        Byte(0x04), Byte(0x05), Byte(0x06)};
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(message, 4);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    REQUIRE(encoder.GetEncodedSize() == 6);
    REQUIRE(encoder.GetBlockCount() == 2);

    std::array<std::byte, 7> tailOutput{};
    std::fill(tailOutput.begin(), tailOutput.end(), Byte(0xA5));
    const auto tailResult = encoder.EncodeBlock(1, tailOutput);
    REQUIRE(tailResult);
    REQUIRE(tailResult.Value() == 2);
    const std::array<std::byte, 7> expectedTail{
        Byte(0x05), Byte(0x06), Byte(0x00), Byte(0x00),
        Byte(0xA5), Byte(0xA5), Byte(0xA5)};
    REQUIRE(tailOutput == expectedTail);

    std::array<std::byte, 7> repeatedTail{};
    std::fill(repeatedTail.begin(), repeatedTail.end(), Byte(0x3C));
    const auto repeatedResult = encoder.EncodeBlock(1, repeatedTail);
    REQUIRE(repeatedResult);
    REQUIRE(repeatedResult.Value() == tailResult.Value());
    REQUIRE(std::equal(
        repeatedTail.begin(), repeatedTail.begin() + 4,
        tailOutput.begin(), tailOutput.begin() + 4));
    REQUIRE(std::all_of(
        repeatedTail.begin() + 4, repeatedTail.end(),
        [](const std::byte value)
        {
            return value == Byte(0x3C);
        }));

    const DirectBlock firstBlock = EncodeDirectBlock(encoder, 0);
    REQUIRE(firstBlock.payloadBytes == 4);
    REQUIRE(firstBlock.paddedPayload == std::vector<std::byte>{
        Byte(0x01), Byte(0x02), Byte(0x03), Byte(0x04)});
}

TEST_CASE("DirectRepeat encoder failures do not modify caller output",
          "[direct-repeat][encoder][error]")
{
    std::vector<std::byte> message = MakeMessage(5);
    const std::vector<std::byte> originalMessage = message;
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(message, 4);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    message[0] ^= Byte(0xFF);
    const DirectBlock stableBlock = EncodeDirectBlock(encoder, 0);
    REQUIRE(std::equal(
        stableBlock.paddedPayload.begin(),
        stableBlock.paddedPayload.end(),
        originalMessage.begin(),
        originalMessage.begin() + 4));

    std::array<std::byte, 3> shortOutput{};
    std::fill(shortOutput.begin(), shortOutput.end(), Byte(0x7D));
    const auto shortResult = encoder.EncodeBlock(0, shortOutput);
    REQUIRE_FALSE(shortResult);
    REQUIRE(shortResult.Error().code ==
        pbouterfec::OuterFecErrorCode::BufferTooSmall);
    REQUIRE(shortResult.Error().detail == 4);
    REQUIRE(std::all_of(
        shortOutput.begin(), shortOutput.end(),
        [](const std::byte value)
        {
            return value == Byte(0x7D);
        }));

    std::array<std::byte, 4> invalidIdOutput{};
    std::fill(
        invalidIdOutput.begin(), invalidIdOutput.end(), Byte(0x6E));
    const auto invalidIdResult = encoder.EncodeBlock(2, invalidIdOutput);
    REQUIRE_FALSE(invalidIdResult);
    REQUIRE(invalidIdResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidInput);
    REQUIRE(std::all_of(
        invalidIdOutput.begin(), invalidIdOutput.end(),
        [](const std::byte value)
        {
            return value == Byte(0x6E);
        }));

    const auto zeroBlockResult = pbouterfec::DirectRepeatEncoder::Create(
        message, 0);
    REQUIRE_FALSE(zeroBlockResult);
    REQUIRE(zeroBlockResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidDimensions);

    const auto oversizedBlockResult =
        pbouterfec::DirectRepeatEncoder::Create(
            message,
            pbprotocol::kMaximumTransportPayloadBytes + 1U);
    REQUIRE_FALSE(oversizedBlockResult);
    REQUIRE(oversizedBlockResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidDimensions);

    const std::array<std::byte, 1> maximumBlockMessage{Byte(0x42)};
    auto maximumBlockResult = pbouterfec::DirectRepeatEncoder::Create(
        maximumBlockMessage,
        pbprotocol::kMaximumTransportPayloadBytes);
    REQUIRE(maximumBlockResult);
    pbouterfec::DirectRepeatEncoder maximumBlockEncoder =
        std::move(maximumBlockResult).Value();
    std::vector<std::byte> maximumBlockOutput(
        pbprotocol::kMaximumTransportPayloadBytes, Byte(0xA5));
    const auto maximumEncodeResult = maximumBlockEncoder.EncodeBlock(
        0, maximumBlockOutput);
    REQUIRE(maximumEncodeResult);
    REQUIRE(maximumEncodeResult.Value() == 1);
    REQUIRE(maximumBlockOutput.front() == Byte(0x42));
    REQUIRE(std::all_of(
        maximumBlockOutput.begin() + 1,
        maximumBlockOutput.end(),
        [](const std::byte value)
        {
            return value == Byte(0);
        }));

    pbouterfec::DirectRepeatEncoder movedEncoder = std::move(encoder);
    const auto movedFromResult = encoder.EncodeBlock(0, invalidIdOutput);
    REQUIRE_FALSE(movedFromResult);
    REQUIRE(movedFromResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidState);
    REQUIRE(EncodeDirectBlock(movedEncoder, 0).payloadBytes == 4);
}

TEST_CASE("DirectRepeat recreation binds exact descriptor and digest",
          "[direct-repeat][encoder][carousel][digest]")
{
    const std::vector<std::byte> message = MakeMessage(17);
    const pbprotocol::SegmentDescriptor descriptor =
        MakeDirectDescriptor(message, 16);

    auto recreatedResult = pbouterfec::DirectRepeatEncoder::Recreate(
        message, descriptor);
    REQUIRE(recreatedResult);
    pbouterfec::DirectRepeatEncoder recreated =
        std::move(recreatedResult).Value();
    REQUIRE(recreated.GetBlockCount() == 2);
    REQUIRE(EncodeDirectBlock(recreated, 1).paddedPayload ==
        std::vector<std::byte>{
            message.back(), Byte(0), Byte(0), Byte(0),
            Byte(0), Byte(0), Byte(0), Byte(0),
            Byte(0), Byte(0), Byte(0), Byte(0),
            Byte(0), Byte(0), Byte(0), Byte(0)});

    std::vector<std::byte> changedMessage = message;
    changedMessage[3] ^= Byte(0x80);
    const auto digestMismatchResult =
        pbouterfec::DirectRepeatEncoder::Recreate(
            changedMessage, descriptor);
    REQUIRE_FALSE(digestMismatchResult);
    REQUIRE(digestMismatchResult.Error().code ==
        pbouterfec::OuterFecErrorCode::EncodedDigestMismatch);

    const auto sizeMismatchResult =
        pbouterfec::DirectRepeatEncoder::Recreate(
            std::span<const std::byte>(message).first(16), descriptor);
    REQUIRE_FALSE(sizeMismatchResult);
    REQUIRE(sizeMismatchResult.Error().code ==
        pbouterfec::OuterFecErrorCode::EncodedSizeMismatch);

    pbprotocol::SegmentDescriptor wirehairDescriptor = descriptor;
    wirehairDescriptor.outerFecMode = pbprotocol::OuterFecMode::WirehairV2;
    const auto wrongModeResult = pbouterfec::DirectRepeatEncoder::Recreate(
        message, wirehairDescriptor);
    REQUIRE_FALSE(wrongModeResult);
    REQUIRE(wrongModeResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidDescriptor);

    pbprotocol::SegmentDescriptor profiledDescriptor = descriptor;
    profiledDescriptor.wirehairV2SerializedProfile =
        pbprotocol::WirehairV2SerializedProfile{};
    const auto profileResult = pbouterfec::DirectRepeatEncoder::Recreate(
        message, profiledDescriptor);
    REQUIRE_FALSE(profileResult);
    REQUIRE(profileResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidDescriptor);
}

TEST_CASE("DirectRepeat decoder reassembles out of order and retries recovery",
          "[direct-repeat][decoder][reassembly]")
{
    const std::vector<std::byte> message = MakeMessage(10);
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(message, 4);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    const DirectBlock block0 = EncodeDirectBlock(encoder, 0);
    const DirectBlock block1 = EncodeDirectBlock(encoder, 1);
    const DirectBlock block2 = EncodeDirectBlock(encoder, 2);

    const pbprotocol::SegmentDescriptor descriptor =
        MakeDirectDescriptor(message, 4);
    auto resourceManager = MakeResourceManager();
    pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
        descriptor, resourceManager);

    std::vector<std::byte> prematureOutput(message.size(), Byte(0xA1));
    const auto prematureRecoverResult = decoder.Recover(prematureOutput);
    REQUIRE_FALSE(prematureRecoverResult);
    REQUIRE(prematureRecoverResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidState);

    const auto tailResult = decoder.DecodeBlock(
        2, block2.payloadBytes, block2.paddedPayload);
    REQUIRE(tailResult);
    REQUIRE(tailResult.Value() == pbouterfec::DecodeDisposition::NeedMore);
    const auto duplicateTailResult = decoder.DecodeBlock(
        2, block2.payloadBytes, block2.paddedPayload);
    REQUIRE(duplicateTailResult);
    REQUIRE(duplicateTailResult.Value() ==
        pbouterfec::DecodeDisposition::NeedMore);

    const auto firstResult = decoder.DecodeBlock(
        0, block0.payloadBytes, block0.paddedPayload);
    REQUIRE(firstResult);
    REQUIRE(firstResult.Value() == pbouterfec::DecodeDisposition::NeedMore);
    const auto finalResult = decoder.DecodeBlock(
        1, block1.payloadBytes, block1.paddedPayload);
    REQUIRE(finalResult);
    REQUIRE(finalResult.Value() == pbouterfec::DecodeDisposition::Ready);

    const auto completedDuplicateResult = decoder.DecodeBlock(
        0, block0.payloadBytes, block0.paddedPayload);
    REQUIRE(completedDuplicateResult);
    REQUIRE(completedDuplicateResult.Value() ==
        pbouterfec::DecodeDisposition::Ready);

    std::vector<std::byte> shortOutput(message.size() - 1, Byte(0x5A));
    const auto shortRecoverResult = decoder.Recover(shortOutput);
    REQUIRE_FALSE(shortRecoverResult);
    REQUIRE(shortRecoverResult.Error().code ==
        pbouterfec::OuterFecErrorCode::BufferTooSmall);
    REQUIRE(shortRecoverResult.Error().detail == message.size());
    REQUIRE(std::all_of(
        shortOutput.begin(), shortOutput.end(),
        [](const std::byte value)
        {
            return value == Byte(0x5A);
        }));
    RequireRecoveredEquals(decoder, message);
}

TEST_CASE("DirectRepeat conflicting duplicate terminates before completion",
          "[direct-repeat][decoder][duplicate][conflict]")
{
    const std::vector<std::byte> message = MakeMessage(9);
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(message, 4);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    DirectBlock firstBlock = EncodeDirectBlock(encoder, 0);

    const pbprotocol::SegmentDescriptor descriptor =
        MakeDirectDescriptor(message, 4);
    auto resourceManager = MakeResourceManager();
    pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
        descriptor, resourceManager);
    REQUIRE(decoder.DecodeBlock(
        0, firstBlock.payloadBytes, firstBlock.paddedPayload));

    firstBlock.paddedPayload[0] ^= Byte(0x01);
    const auto conflictResult = decoder.DecodeBlock(
        0, firstBlock.payloadBytes, firstBlock.paddedPayload);
    REQUIRE_FALSE(conflictResult);
    REQUIRE(conflictResult.Error().code ==
        pbouterfec::OuterFecErrorCode::OuterBlockConflict);
    REQUIRE(conflictResult.Error().detail == 0);

    firstBlock.paddedPayload[0] ^= Byte(0x01);
    const auto postConflictResult = decoder.DecodeBlock(
        0, firstBlock.payloadBytes, firstBlock.paddedPayload);
    REQUIRE_FALSE(postConflictResult);
    REQUIRE(postConflictResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidState);
    std::vector<std::byte> output(message.size());
    REQUIRE_FALSE(decoder.Recover(output));
}

TEST_CASE("DirectRepeat conflicting duplicate terminates after completion",
          "[direct-repeat][decoder][duplicate][conflict][ready]")
{
    const std::vector<std::byte> message = MakeMessage(5);
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(message, 4);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    const DirectBlock firstBlock = EncodeDirectBlock(encoder, 0);
    DirectBlock tailBlock = EncodeDirectBlock(encoder, 1);

    const pbprotocol::SegmentDescriptor descriptor =
        MakeDirectDescriptor(message, 4);
    auto resourceManager = MakeResourceManager();
    pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
        descriptor, resourceManager);
    REQUIRE(decoder.DecodeBlock(
        0, firstBlock.payloadBytes, firstBlock.paddedPayload));
    const auto readyResult = decoder.DecodeBlock(
        1, tailBlock.payloadBytes, tailBlock.paddedPayload);
    REQUIRE(readyResult);
    REQUIRE(readyResult.Value() == pbouterfec::DecodeDisposition::Ready);
    RequireRecoveredEquals(decoder, message);

    tailBlock.paddedPayload[0] ^= Byte(0x40);
    const auto conflictResult = decoder.DecodeBlock(
        1, tailBlock.payloadBytes, tailBlock.paddedPayload);
    REQUIRE_FALSE(conflictResult);
    REQUIRE(conflictResult.Error().code ==
        pbouterfec::OuterFecErrorCode::OuterBlockConflict);
    std::vector<std::byte> output(message.size());
    const auto recoverAfterConflictResult = decoder.Recover(output);
    REQUIRE_FALSE(recoverAfterConflictResult);
    REQUIRE(recoverAfterConflictResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidState);
}

TEST_CASE("DirectRepeat decoder rejects malformed block shape and padding",
          "[direct-repeat][decoder][malformed][padding]")
{
    const std::vector<std::byte> message = MakeMessage(5);
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(message, 4);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    const DirectBlock firstBlock = EncodeDirectBlock(encoder, 0);
    const DirectBlock tailBlock = EncodeDirectBlock(encoder, 1);
    const pbprotocol::SegmentDescriptor descriptor =
        MakeDirectDescriptor(message, 4);
    auto resourceManager = MakeResourceManager();

    SECTION("ordinal")
    {
        pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
            descriptor, resourceManager);
        const auto result = decoder.DecodeBlock(
            2, tailBlock.payloadBytes, tailBlock.paddedPayload);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code ==
            pbouterfec::OuterFecErrorCode::InvalidInput);
    }
    SECTION("non-tail payload length")
    {
        pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
            descriptor, resourceManager);
        const auto result = decoder.DecodeBlock(
            0, firstBlock.payloadBytes - 1, firstBlock.paddedPayload);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code ==
            pbouterfec::OuterFecErrorCode::InvalidInput);
    }
    SECTION("non-tail payload length too large")
    {
        pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
            descriptor, resourceManager);
        const auto result = decoder.DecodeBlock(
            0, firstBlock.payloadBytes + 1, firstBlock.paddedPayload);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code ==
            pbouterfec::OuterFecErrorCode::InvalidInput);
    }
    SECTION("tail payload length")
    {
        pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
            descriptor, resourceManager);
        const auto result = decoder.DecodeBlock(
            1, tailBlock.payloadBytes + 1, tailBlock.paddedPayload);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code ==
            pbouterfec::OuterFecErrorCode::InvalidInput);
    }
    SECTION("tail payload length too small")
    {
        pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
            descriptor, resourceManager);
        const auto result = decoder.DecodeBlock(
            1, tailBlock.payloadBytes - 1, tailBlock.paddedPayload);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code ==
            pbouterfec::OuterFecErrorCode::InvalidInput);
    }
    SECTION("truncated fixed payload region")
    {
        pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
            descriptor, resourceManager);
        const auto result = decoder.DecodeBlock(
            1,
            tailBlock.payloadBytes,
            std::span<const std::byte>(tailBlock.paddedPayload).first(3));
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code ==
            pbouterfec::OuterFecErrorCode::InvalidInput);
    }
    SECTION("oversized fixed payload region")
    {
        pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
            descriptor, resourceManager);
        std::vector<std::byte> oversizedPayload = tailBlock.paddedPayload;
        oversizedPayload.push_back(Byte(0));
        const auto result = decoder.DecodeBlock(
            1, tailBlock.payloadBytes, oversizedPayload);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code ==
            pbouterfec::OuterFecErrorCode::InvalidInput);
    }
    SECTION("nonzero canonical padding")
    {
        for (std::size_t paddingIndex = tailBlock.payloadBytes;
            paddingIndex < tailBlock.paddedPayload.size();
            paddingIndex++)
        {
            pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
                descriptor, resourceManager);
            std::vector<std::byte> noncanonicalPayload =
                tailBlock.paddedPayload;
            noncanonicalPayload[paddingIndex] = Byte(0x80);
            const auto result = decoder.DecodeBlock(
                1, tailBlock.payloadBytes, noncanonicalPayload);
            REQUIRE_FALSE(result);
            REQUIRE(result.Error().code ==
                pbouterfec::OuterFecErrorCode::InvalidInput);
            REQUIRE(result.Error().detail == paddingIndex);
            const auto afterFailureResult = decoder.DecodeBlock(
                1, tailBlock.payloadBytes, tailBlock.paddedPayload);
            REQUIRE_FALSE(afterFailureResult);
            REQUIRE(afterFailureResult.Error().code ==
                pbouterfec::OuterFecErrorCode::InvalidState);
        }
    }
    SECTION("nonzero canonical padding after completion is terminal")
    {
        pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
            descriptor, resourceManager);
        REQUIRE(decoder.DecodeBlock(
            0, firstBlock.payloadBytes, firstBlock.paddedPayload));
        const auto readyResult = decoder.DecodeBlock(
            1, tailBlock.payloadBytes, tailBlock.paddedPayload);
        REQUIRE(readyResult);
        REQUIRE(readyResult.Value() ==
            pbouterfec::DecodeDisposition::Ready);

        std::vector<std::byte> noncanonicalPayload =
            tailBlock.paddedPayload;
        noncanonicalPayload[tailBlock.payloadBytes] = Byte(0x01);
        const auto paddingResult = decoder.DecodeBlock(
            1, tailBlock.payloadBytes, noncanonicalPayload);
        REQUIRE_FALSE(paddingResult);
        REQUIRE(paddingResult.Error().code ==
            pbouterfec::OuterFecErrorCode::InvalidInput);
        std::vector<std::byte> output(message.size());
        REQUIRE_FALSE(decoder.Recover(output));
    }
}

TEST_CASE("DirectRepeat digest mismatch is terminal and blocks recovery",
          "[direct-repeat][decoder][digest][error]")
{
    const std::vector<std::byte> message = MakeMessage(7);
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(message, 4);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    const DirectBlock firstBlock = EncodeDirectBlock(encoder, 0);
    const DirectBlock tailBlock = EncodeDirectBlock(encoder, 1);

    pbprotocol::SegmentDescriptor descriptor = MakeDirectDescriptor(
        message, 4);
    descriptor.encodedDigest.bytes[0] ^= Byte(0x01);
    auto resourceManager = MakeResourceManager();
    pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
        descriptor, resourceManager);
    const auto firstResult = decoder.DecodeBlock(
        0, firstBlock.payloadBytes, firstBlock.paddedPayload);
    REQUIRE(firstResult);
    REQUIRE(firstResult.Value() == pbouterfec::DecodeDisposition::NeedMore);
    const auto digestResult = decoder.DecodeBlock(
        1, tailBlock.payloadBytes, tailBlock.paddedPayload);
    REQUIRE_FALSE(digestResult);
    REQUIRE(digestResult.Error().code ==
        pbouterfec::OuterFecErrorCode::EncodedDigestMismatch);

    std::vector<std::byte> output(message.size());
    const auto recoverResult = decoder.Recover(output);
    REQUIRE_FALSE(recoverResult);
    REQUIRE(recoverResult.Error().code ==
        pbouterfec::OuterFecErrorCode::InvalidState);
}

TEST_CASE("DirectRepeat decoder reservations enforce all shared quotas",
          "[direct-repeat][decoder][resource][quota]")
{
    const std::vector<std::byte> message = MakeMessage(65);
    const pbprotocol::SegmentDescriptor descriptor =
        MakeDirectDescriptor(message, 16);

    {
        auto profileManager = MakeResourceManager();
        const auto profileMismatchResult =
            pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
                descriptor, 15, profileManager);
        REQUIRE_FALSE(profileMismatchResult);
        REQUIRE(profileMismatchResult.Error().code ==
            pbouterfec::OuterFecErrorCode::OuterBlockBytesMismatch);
        REQUIRE(profileManager.GetActiveDecoderCount() == 0);
        REQUIRE(profileManager.GetReservedDecoderBytes() == 0);
        REQUIRE(profileManager.GetQuotaExceededCount() == 0);
    }

    {
        pbprotocol::ReceiverResourcePolicy directCountPolicy =
            pbprotocol::GetDefaultReceiverResourcePolicy();
        directCountPolicy.maxDirectRepeatBlockCount = 4;
        auto directCountManager = MakeResourceManager(directCountPolicy);
        const auto directCountResult =
            pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
                descriptor,
                descriptor.outerBlockBytes,
                directCountManager);
        REQUIRE_FALSE(directCountResult);
        REQUIRE(directCountResult.Error().code ==
            pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
        REQUIRE(directCountResult.Error().detail == 5);
        REQUIRE(directCountManager.GetActiveDecoderCount() == 0);
        REQUIRE(directCountManager.GetReservedDecoderBytes() == 0);
        REQUIRE(directCountManager.GetQuotaExceededCount() == 1);

        directCountPolicy.maxDirectRepeatBlockCount = 5;
        auto exactCountManager = MakeResourceManager(directCountPolicy);
        const auto exactCountResult =
            pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
                descriptor,
                descriptor.outerBlockBytes,
                exactCountManager);
        REQUIRE(exactCountResult);
        REQUIRE(exactCountManager.GetActiveDecoderCount() == 1);
        REQUIRE(exactCountManager.GetQuotaExceededCount() == 0);
    }

    {
        const pbprotocol::SegmentDescriptor oneByteBlockDescriptor =
            MakeDirectDescriptor(message, 1);
        auto defaultCountManager = MakeResourceManager();
        const auto defaultCountResult =
            pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
                oneByteBlockDescriptor,
                oneByteBlockDescriptor.outerBlockBytes,
                defaultCountManager);
        REQUIRE_FALSE(defaultCountResult);
        REQUIRE(defaultCountResult.Error().code ==
            pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
        REQUIRE(defaultCountResult.Error().detail == 65);
        REQUIRE(defaultCountManager.GetActiveDecoderCount() == 0);
        REQUIRE(defaultCountManager.GetReservedDecoderBytes() == 0);
        REQUIRE(defaultCountManager.GetQuotaExceededCount() == 1);
    }

    pbprotocol::ReceiverResourcePolicy encodedPolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    encodedPolicy.maxEncodedSegmentBytes = message.size() - 1;
    auto encodedManager = MakeResourceManager(encodedPolicy);
    const auto encodedLimitResult = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
        descriptor, descriptor.outerBlockBytes, encodedManager);
    REQUIRE_FALSE(encodedLimitResult);
    REQUIRE(encodedLimitResult.Error().code ==
        pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
    REQUIRE(encodedManager.GetActiveDecoderCount() == 0);
    REQUIRE(encodedManager.GetQuotaExceededCount() == 1);

    pbprotocol::ReceiverResourcePolicy blockPolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    blockPolicy.maxOuterBlockBytes = 15;
    auto blockManager = MakeResourceManager(blockPolicy);
    const auto blockLimitResult = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
        descriptor, descriptor.outerBlockBytes, blockManager);
    REQUIRE_FALSE(blockLimitResult);
    REQUIRE(blockLimitResult.Error().code ==
        pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
    REQUIRE(blockManager.GetActiveDecoderCount() == 0);
    REQUIRE(blockManager.GetQuotaExceededCount() == 1);

    std::uint64_t reservationBytes = 0;
    {
        auto probeManager = MakeResourceManager();
        {
            pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
                descriptor, probeManager);
            REQUIRE(probeManager.GetActiveDecoderCount() == 1);
            reservationBytes = probeManager.GetReservedDecoderBytes();
            REQUIRE(reservationBytes > message.size());

            pbouterfec::DirectRepeatDecoder movedDecoder =
                std::move(decoder);
            REQUIRE(probeManager.GetActiveDecoderCount() == 1);
            const auto movedFromResult = decoder.DecodeBlock(0, 16, {});
            REQUIRE_FALSE(movedFromResult);
            REQUIRE(movedFromResult.Error().code ==
                pbouterfec::OuterFecErrorCode::InvalidState);
        }
        REQUIRE(probeManager.GetActiveDecoderCount() == 0);
        REQUIRE(probeManager.GetReservedDecoderBytes() == 0);
    }

    {
        auto moveAssignmentManager = MakeResourceManager();
        {
            pbouterfec::DirectRepeatDecoder firstDecoder = MakeDirectDecoder(
                descriptor, moveAssignmentManager);
            pbouterfec::DirectRepeatDecoder secondDecoder = MakeDirectDecoder(
                descriptor, moveAssignmentManager);
            REQUIRE(moveAssignmentManager.GetActiveDecoderCount() == 2);
            secondDecoder = std::move(firstDecoder);
            REQUIRE(moveAssignmentManager.GetActiveDecoderCount() == 1);
        }
        REQUIRE(moveAssignmentManager.GetActiveDecoderCount() == 0);
        REQUIRE(moveAssignmentManager.GetReservedDecoderBytes() == 0);
    }

    pbprotocol::ReceiverResourcePolicy perDecoderPolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    perDecoderPolicy.maxOuterFecDecoderBytes = reservationBytes - 1;
    auto perDecoderManager = MakeResourceManager(perDecoderPolicy);
    const auto perDecoderResult = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
        descriptor, descriptor.outerBlockBytes, perDecoderManager);
    REQUIRE_FALSE(perDecoderResult);
    REQUIRE(perDecoderResult.Error().code ==
        pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
    REQUIRE(perDecoderManager.GetActiveDecoderCount() == 0);
    REQUIRE(perDecoderManager.GetQuotaExceededCount() == 1);

    pbprotocol::ReceiverResourcePolicy countPolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    countPolicy.maxActiveOuterFecDecoders = 1;
    auto countManager = MakeResourceManager(countPolicy);
    {
        pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
            descriptor, countManager);
        const auto secondResult = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
            descriptor, descriptor.outerBlockBytes, countManager);
        REQUIRE_FALSE(secondResult);
        REQUIRE(secondResult.Error().code ==
            pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
        REQUIRE(countManager.GetActiveDecoderCount() == 1);
        REQUIRE(countManager.GetQuotaExceededCount() == 1);
    }
    REQUIRE(countManager.GetActiveDecoderCount() == 0);

    pbprotocol::ReceiverResourcePolicy aggregatePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    aggregatePolicy.maxActiveOuterFecDecoders = 3;
    aggregatePolicy.maxOuterFecDecoderBytes = reservationBytes;
    aggregatePolicy.maxTotalOuterFecDecoderBytes = reservationBytes * 2;
    auto aggregateManager = MakeResourceManager(aggregatePolicy);
    {
        pbouterfec::DirectRepeatDecoder firstDecoder = MakeDirectDecoder(
            descriptor, aggregateManager);
        pbouterfec::DirectRepeatDecoder secondDecoder = MakeDirectDecoder(
            descriptor, aggregateManager);
        REQUIRE(aggregateManager.GetActiveDecoderCount() == 2);
        REQUIRE(aggregateManager.GetReservedDecoderBytes() ==
            reservationBytes * 2);
        const auto thirdResult = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
            descriptor, descriptor.outerBlockBytes, aggregateManager);
        REQUIRE_FALSE(thirdResult);
        REQUIRE(thirdResult.Error().code ==
            pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
    }
    REQUIRE(aggregateManager.GetActiveDecoderCount() == 0);
    REQUIRE(aggregateManager.GetReservedDecoderBytes() == 0);
    REQUIRE(aggregateManager.GetQuotaExceededCount() == 1);
}

TEST_CASE("DirectRepeat allocation failure rolls back shared reservation",
          "[direct-repeat][decoder][resource][allocation]")
{
    constexpr std::uint64_t ordinalSpace =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max()) + 1ULL;
    constexpr std::uint32_t outerBlockBytes = 65535;
    constexpr std::uint64_t impossibleEncodedSize =
        ordinalSpace * outerBlockBytes;
    constexpr std::uint64_t bitmapBytes = ordinalSpace / 8ULL;
    constexpr std::uint64_t fixedAdmissionBytes = 4096;
    constexpr std::uint64_t reservationBytes =
        impossibleEncodedSize + bitmapBytes + fixedAdmissionBytes;

    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxEncodedSegmentBytes = impossibleEncodedSize;
    resourcePolicy.maxDirectRepeatBlockCount = ordinalSpace;
    resourcePolicy.maxOuterFecDecoderBytes = reservationBytes;
    resourcePolicy.maxTotalOuterFecDecoderBytes = reservationBytes;
    auto resourceManager = MakeResourceManager(resourcePolicy);

    pbprotocol::SegmentDescriptor descriptor{};
    descriptor.encodedSize = impossibleEncodedSize;
    descriptor.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    descriptor.outerBlockBytes = outerBlockBytes;
    const auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
        descriptor, descriptor.outerBlockBytes, resourceManager);
    REQUIRE_FALSE(decoderResult);
    REQUIRE(decoderResult.Error().code ==
        pbouterfec::OuterFecErrorCode::OutOfMemory);
    REQUIRE(resourceManager.GetActiveDecoderCount() == 0);
    REQUIRE(resourceManager.GetReservedDecoderBytes() == 0);
    REQUIRE(resourceManager.GetQuotaExceededCount() == 0);
}

TEST_CASE("DirectRepeat resource manager serializes concurrent admission and release",
          "[direct-repeat][decoder][resource][concurrency]")
{
    const std::vector<std::byte> message = MakeMessage(3);
    const pbprotocol::SegmentDescriptor descriptor =
        MakeDirectDescriptor(message, 4);
    constexpr pbouterfec::OuterFecErrorCode quotaExceeded =
        pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded;

    SECTION("two simultaneous admissions cannot exceed an active cap of one")
    {
        constexpr std::size_t iterationCount = 32;
        for (std::size_t iteration = 0;
            iteration < iterationCount;
            iteration++)
        {
            pbprotocol::ReceiverResourcePolicy resourcePolicy =
                pbprotocol::GetDefaultReceiverResourcePolicy();
            resourcePolicy.maxActiveOuterFecDecoders = 1;
            auto resourceManager = MakeResourceManager(resourcePolicy);

            std::array<bool, 2> succeeded{};
            std::array<pbouterfec::OuterFecErrorCode, 2> errors{};
            std::barrier startBarrier(3);
            std::barrier admittedBarrier(3);
            std::barrier releaseBarrier(3);
            std::array<std::thread, 2> workers;
            for (std::size_t workerIndex = 0;
                workerIndex < workers.size();
                workerIndex++)
            {
                workers[workerIndex] = std::thread(
                    [&, workerIndex]()
                    {
                        startBarrier.arrive_and_wait();
                        auto decoderResult =
                            pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
                                descriptor,
                                descriptor.outerBlockBytes,
                                resourceManager);
                        succeeded[workerIndex] =
                            static_cast<bool>(decoderResult);
                        if (!decoderResult)
                        {
                            errors[workerIndex] = decoderResult.Error().code;
                        }
                        admittedBarrier.arrive_and_wait();
                        releaseBarrier.arrive_and_wait();
                    });
            }

            startBarrier.arrive_and_wait();
            admittedBarrier.arrive_and_wait();
            const std::uint64_t activeAtBarrier =
                resourceManager.GetActiveDecoderCount();
            const std::uint64_t reservedAtBarrier =
                resourceManager.GetReservedDecoderBytes();
            releaseBarrier.arrive_and_wait();
            for (std::thread& worker : workers)
            {
                worker.join();
            }

            const std::size_t successCount = static_cast<std::size_t>(
                succeeded[0]) + static_cast<std::size_t>(succeeded[1]);
            REQUIRE(successCount == 1);
            REQUIRE(activeAtBarrier == 1);
            REQUIRE(reservedAtBarrier > 0);
            for (std::size_t workerIndex = 0;
                workerIndex < workers.size();
                workerIndex++)
            {
                if (!succeeded[workerIndex])
                {
                    REQUIRE(errors[workerIndex] == quotaExceeded);
                }
            }
            REQUIRE(resourceManager.GetActiveDecoderCount() == 0);
            REQUIRE(resourceManager.GetReservedDecoderBytes() == 0);
        }
    }

    SECTION("four reservations can release concurrently exactly once")
    {
        pbprotocol::ReceiverResourcePolicy resourcePolicy =
            pbprotocol::GetDefaultReceiverResourcePolicy();
        resourcePolicy.maxActiveOuterFecDecoders = 4;
        auto resourceManager = MakeResourceManager(resourcePolicy);

        std::vector<pbouterfec::DirectRepeatDecoder> decoders;
        decoders.reserve(4);
        for (std::size_t decoderIndex = 0;
            decoderIndex < 4;
            decoderIndex++)
        {
            decoders.push_back(MakeDirectDecoder(
                descriptor, resourceManager));
        }
        REQUIRE(resourceManager.GetActiveDecoderCount() == 4);
        const std::uint64_t reservedBeforeRelease =
            resourceManager.GetReservedDecoderBytes();
        REQUIRE(reservedBeforeRelease > 0);

        std::barrier releaseBarrier(5);
        std::array<std::thread, 4> workers;
        for (std::size_t workerIndex = 0;
            workerIndex < workers.size();
            workerIndex++)
        {
            workers[workerIndex] = std::thread(
                [decoder = std::move(decoders[workerIndex]),
                 &releaseBarrier]() mutable
                {
                    releaseBarrier.arrive_and_wait();
                });
        }
        releaseBarrier.arrive_and_wait();
        for (std::thread& worker : workers)
        {
            worker.join();
        }

        REQUIRE(resourceManager.GetActiveDecoderCount() == 0);
        REQUIRE(resourceManager.GetReservedDecoderBytes() == 0);
    }

    SECTION("concurrent releases remain safe after manager shutdown")
    {
        std::barrier releaseBarrier(5);
        std::array<std::thread, 4> workers;
        std::uint64_t activeBeforeShutdown = 0;
        std::uint64_t reservedBeforeShutdown = 0;
        {
            pbprotocol::ReceiverResourcePolicy resourcePolicy =
                pbprotocol::GetDefaultReceiverResourcePolicy();
            resourcePolicy.maxActiveOuterFecDecoders = 4;
            auto resourceManager = MakeResourceManager(resourcePolicy);

            std::vector<pbouterfec::DirectRepeatDecoder> decoders;
            decoders.reserve(workers.size());
            for (std::size_t decoderIndex = 0;
                decoderIndex < workers.size();
                decoderIndex++)
            {
                decoders.push_back(MakeDirectDecoder(
                    descriptor, resourceManager));
            }
            for (std::size_t workerIndex = 0;
                workerIndex < workers.size();
                workerIndex++)
            {
                workers[workerIndex] = std::thread(
                    [decoder = std::move(decoders[workerIndex]),
                     &releaseBarrier]() mutable
                    {
                        releaseBarrier.arrive_and_wait();
                    });
            }
            activeBeforeShutdown =
                resourceManager.GetActiveDecoderCount();
            reservedBeforeShutdown =
                resourceManager.GetReservedDecoderBytes();
        }

        releaseBarrier.arrive_and_wait();
        for (std::thread& worker : workers)
        {
            worker.join();
        }
        REQUIRE(activeBeforeShutdown == 4);
        REQUIRE(reservedBeforeShutdown > 0);
    }
}

TEST_CASE("DirectRepeat reservation safely outlives resource manager",
          "[direct-repeat][decoder][resource][lifetime]")
{
    const std::vector<std::byte> message = MakeMessage(3);
    const pbprotocol::SegmentDescriptor descriptor =
        MakeDirectDescriptor(message, 4);
    std::optional<pbouterfec::DirectRepeatDecoder> decoder;
    {
        auto resourceManager = MakeResourceManager();
        auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
            descriptor, descriptor.outerBlockBytes, resourceManager);
        REQUIRE(decoderResult);
        decoder.emplace(std::move(decoderResult).Value());
        REQUIRE(resourceManager.GetActiveDecoderCount() == 1);
        REQUIRE(resourceManager.GetReservedDecoderBytes() > 0);
    }

    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(message, 4);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    const DirectBlock block = EncodeDirectBlock(encoder, 0);
    const auto decodeResult = decoder->DecodeBlock(
        0, block.payloadBytes, block.paddedPayload);
    REQUIRE(decodeResult);
    REQUIRE(decodeResult.Value() == pbouterfec::DecodeDisposition::Ready);
    RequireRecoveredEquals(*decoder, message);
    decoder.reset();
}

TEST_CASE("DirectRepeat and Wirehair share one receiver-wide quota",
          "[direct-repeat][wirehair][resource][integration]")
{
    const std::vector<std::byte> directMessage = MakeMessage(3);
    const pbprotocol::SegmentDescriptor directDescriptor =
        MakeDirectDescriptor(directMessage, 4);
    const std::vector<std::byte> wirehairMessage = MakeMessage(17);
    auto wirehairEncoderResult = pbouterfec::WirehairV2Encoder::Create(
        wirehairMessage, 16);
    REQUIRE(wirehairEncoderResult);
    pbouterfec::WirehairV2Encoder wirehairEncoder =
        std::move(wirehairEncoderResult).Value();
    pbprotocol::SegmentDescriptor wirehairDescriptor{};
    wirehairDescriptor.encodedSize = wirehairMessage.size();
    wirehairDescriptor.outerFecMode = pbprotocol::OuterFecMode::WirehairV2;
    wirehairDescriptor.outerBlockBytes = 16;
    wirehairDescriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::ComputeBlake3Digest(wirehairMessage)};
    wirehairDescriptor.wirehairV2SerializedProfile =
        wirehairEncoder.GetSerializedProfile();

    std::uint64_t directReservationBytes = 0;
    std::uint64_t wirehairReservationBytes = 0;
    {
        auto probeManager = MakeResourceManager();
        {
            pbouterfec::DirectRepeatDecoder directDecoder = MakeDirectDecoder(
                directDescriptor, probeManager);
            directReservationBytes =
                probeManager.GetReservedDecoderBytes();
        }
        {
            auto wirehairDecoderResult =
                pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
                    wirehairDescriptor, probeManager);
            REQUIRE(wirehairDecoderResult);
            pbouterfec::WirehairV2Decoder wirehairDecoder =
                std::move(wirehairDecoderResult).Value();
            wirehairReservationBytes =
                probeManager.GetReservedDecoderBytes();
        }
        REQUIRE(probeManager.GetActiveDecoderCount() == 0);
        REQUIRE(probeManager.GetReservedDecoderBytes() == 0);
    }
    REQUIRE(directReservationBytes > 0);
    REQUIRE(wirehairReservationBytes > 0);

    pbprotocol::ReceiverResourcePolicy activePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    activePolicy.maxActiveOuterFecDecoders = 1;
    auto activeManager = MakeResourceManager(activePolicy);
    {
        pbouterfec::DirectRepeatDecoder directDecoder = MakeDirectDecoder(
            directDescriptor, activeManager);
        const auto blockedWirehairResult =
            pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
                wirehairDescriptor, activeManager);
        REQUIRE_FALSE(blockedWirehairResult);
        REQUIRE(blockedWirehairResult.Error().code ==
            pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
    }
    {
        auto wirehairDecoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            wirehairDescriptor, activeManager);
        REQUIRE(wirehairDecoderResult);
        pbouterfec::WirehairV2Decoder wirehairDecoder =
            std::move(wirehairDecoderResult).Value();
        const auto blockedDirectResult =
            pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
                directDescriptor,
                directDescriptor.outerBlockBytes,
                activeManager);
        REQUIRE_FALSE(blockedDirectResult);
        REQUIRE(blockedDirectResult.Error().code ==
            pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
    }

    const std::uint64_t combinedReservationBytes =
        directReservationBytes + wirehairReservationBytes;
    pbprotocol::ReceiverResourcePolicy aggregatePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    aggregatePolicy.maxActiveOuterFecDecoders = 4;
    aggregatePolicy.maxOuterFecDecoderBytes = std::max(
        directReservationBytes, wirehairReservationBytes);
    aggregatePolicy.maxTotalOuterFecDecoderBytes =
        combinedReservationBytes;
    auto aggregateManager = MakeResourceManager(aggregatePolicy);
    {
        pbouterfec::DirectRepeatDecoder directDecoder = MakeDirectDecoder(
            directDescriptor, aggregateManager);
        auto wirehairDecoderResult = pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            wirehairDescriptor, aggregateManager);
        REQUIRE(wirehairDecoderResult);
        pbouterfec::WirehairV2Decoder wirehairDecoder =
            std::move(wirehairDecoderResult).Value();
        REQUIRE(aggregateManager.GetActiveDecoderCount() == 2);
        REQUIRE(aggregateManager.GetReservedDecoderBytes() ==
            combinedReservationBytes);

        const auto blockedDirectResult =
            pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
                directDescriptor,
                directDescriptor.outerBlockBytes,
                aggregateManager);
        REQUIRE_FALSE(blockedDirectResult);
        REQUIRE(blockedDirectResult.Error().code ==
            pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
        const auto blockedWirehairResult =
            pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
                wirehairDescriptor, aggregateManager);
        REQUIRE_FALSE(blockedWirehairResult);
        REQUIRE(blockedWirehairResult.Error().code ==
            pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
    }
    REQUIRE(aggregateManager.GetActiveDecoderCount() == 0);
    REQUIRE(aggregateManager.GetReservedDecoderBytes() == 0);
}

TEST_CASE("Highly compressed segment completes through DirectRepeat",
          "[direct-repeat][compression][integration]")
{
    const std::vector<std::byte> rawSegment(
        256 * 1024, Byte(0x41));
    const pbcompression::CompressionSettings compressionSettings;
    const auto compressionResult = pbcompression::CompressSegment(
        rawSegment, compressionSettings);
    REQUIRE(compressionResult);
    REQUIRE(compressionResult.Value().codec ==
        pbprotocol::CompressionCodec::Zstandard);
    const std::vector<std::byte>& encodedSegment =
        compressionResult.Value().bytes;
    constexpr std::uint32_t outerBlockBytes = 4096;
    REQUIRE(encodedSegment.size() < outerBlockBytes);

    const auto modeResult = pbouterfec::ChooseOuterFecMode(
        encodedSegment.size(), outerBlockBytes);
    REQUIRE(modeResult);
    REQUIRE(modeResult.Value() == pbprotocol::OuterFecMode::DirectRepeat);

    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(rawSegment.size(), 1);
    REQUIRE(pbprotocol::ValidateSessionDescriptor(sessionDescriptor));
    pbprotocol::SegmentDescriptor segmentDescriptor =
        MakeDirectDescriptor(encodedSegment, outerBlockBytes);
    segmentDescriptor.sessionTag = pbprotocol::DeriveSessionTag(
        sessionDescriptor.sessionId);
    segmentDescriptor.segmentOrdinal = 0;
    segmentDescriptor.rawOffset = 0;
    segmentDescriptor.rawSize = rawSegment.size();
    segmentDescriptor.compressionCodec =
        compressionResult.Value().codec;
    segmentDescriptor.rawDigest = pbprotocol::RawDigest{
        pbprotocol::ComputeBlake3Digest(rawSegment)};
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    REQUIRE(pbprotocol::ValidateSegmentDescriptor(
        segmentDescriptor, sessionDescriptor, resourcePolicy));

    auto encoderResult = pbouterfec::DirectRepeatEncoder::Recreate(
        encodedSegment, segmentDescriptor);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    REQUIRE(encoder.GetBlockCount() == 1);
    const DirectBlock block = EncodeDirectBlock(encoder, 0);
    REQUIRE(block.payloadBytes == encodedSegment.size());
    REQUIRE(std::all_of(
        block.paddedPayload.begin() + block.payloadBytes,
        block.paddedPayload.end(),
        [](const std::byte value)
        {
            return value == Byte(0);
        }));

    auto resourceManager = MakeResourceManager(resourcePolicy);
    pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
        segmentDescriptor, resourceManager);
    const auto readyResult = decoder.DecodeBlock(
        0, block.payloadBytes, block.paddedPayload);
    REQUIRE(readyResult);
    REQUIRE(readyResult.Value() == pbouterfec::DecodeDisposition::Ready);
    const auto duplicateResult = decoder.DecodeBlock(
        0, block.payloadBytes, block.paddedPayload);
    REQUIRE(duplicateResult);
    REQUIRE(duplicateResult.Value() == pbouterfec::DecodeDisposition::Ready);

    std::vector<std::byte> recoveredEncoded(encodedSegment.size());
    const auto recoverResult = decoder.Recover(recoveredEncoded);
    REQUIRE(recoverResult);
    REQUIRE(recoveredEncoded == encodedSegment);
    REQUIRE(pbprotocol::ComputeBlake3Digest(recoveredEncoded) ==
        segmentDescriptor.encodedDigest.bytes);

    const pbcompression::DecompressionLimits decompressionLimits =
        pbcompression::MakeDecompressionLimits(resourcePolicy);
    const auto decompressionResult = pbcompression::DecompressSegment(
        segmentDescriptor, recoveredEncoded, decompressionLimits);
    REQUIRE(decompressionResult);
    REQUIRE(decompressionResult.Value() == rawSegment);
    REQUIRE(pbprotocol::ComputeBlake3Digest(
        decompressionResult.Value()) == segmentDescriptor.rawDigest.bytes);
}

TEST_CASE("Outer FEC quota telemetry saturates instead of wrapping",
          "[pbouterfec][resource][telemetry]")
{
    const std::vector<std::byte> message = MakeMessage(2);
    const pbprotocol::SegmentDescriptor descriptor =
        MakeDirectDescriptor(message, 2);
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxEncodedSegmentBytes = 1;
    auto resourceManager = MakeResourceManager(resourcePolicy);
    constexpr std::uint64_t maximumCount =
        std::numeric_limits<std::uint64_t>::max();
    pbouterfec::test::DecoderTestAccess::SetQuotaExceededCount(
        resourceManager,
        maximumCount - 1ULL);

    const auto firstResult =
        pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
            descriptor,
            descriptor.outerBlockBytes,
            resourceManager);
    REQUIRE_FALSE(firstResult);
    REQUIRE(firstResult.Error().code ==
        pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded);
    REQUIRE(resourceManager.GetQuotaExceededCount() == maximumCount);

    const auto secondResult =
        pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
            descriptor,
            descriptor.outerBlockBytes,
            resourceManager);
    REQUIRE_FALSE(secondResult);
    REQUIRE(resourceManager.GetQuotaExceededCount() == maximumCount);
}

TEST_CASE("DirectRepeat decoder exposes the bound block count for replay cross-checks",
          "[direct-repeat][accessor][trust-boundary]")
{
    constexpr std::uint32_t outerBlockBytes = 16;
    const std::vector<std::byte> message = MakeMessage(outerBlockBytes + 1U);

    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(
        message, outerBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();
    REQUIRE(encoder.GetBlockCount() == 2);

    const pbprotocol::SegmentDescriptor descriptor = MakeDirectDescriptor(
        message, outerBlockBytes);
    auto resourceManager = MakeResourceManager();
    pbouterfec::DirectRepeatDecoder decoder = MakeDirectDecoder(
        descriptor, resourceManager);

    REQUIRE(decoder.GetBoundBlockCount() == 2);

    // An empty (moved-from) decoder reports 0.
    pbouterfec::DirectRepeatDecoder movedAway = std::move(decoder);
    (void)movedAway;
    REQUIRE(decoder.GetBoundBlockCount() == 0);
}
