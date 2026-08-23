#include "pbouterfec/wirehair_v2.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>
#include <wirehair/wirehair.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

static_assert(!std::is_copy_constructible_v<pbouterfec::WirehairV2Encoder>);
static_assert(!std::is_copy_assignable_v<pbouterfec::WirehairV2Encoder>);
static_assert(std::is_nothrow_move_constructible_v<pbouterfec::WirehairV2Encoder>);
static_assert(std::is_nothrow_move_assignable_v<pbouterfec::WirehairV2Encoder>);
static_assert(!std::is_copy_constructible_v<pbouterfec::WirehairV2Decoder>);
static_assert(!std::is_copy_assignable_v<pbouterfec::WirehairV2Decoder>);
static_assert(std::is_nothrow_move_constructible_v<pbouterfec::WirehairV2Decoder>);
static_assert(std::is_nothrow_move_assignable_v<pbouterfec::WirehairV2Decoder>);

constexpr std::uint32_t kBlockBytes = 16;
constexpr std::size_t kMessageBytes = 117;

[[nodiscard]] std::vector<std::byte> MakeMessage(
    const std::size_t byteCount)
{
    std::vector<std::byte> message(byteCount);
    for (std::size_t byteIndex = 0; byteIndex < message.size(); byteIndex++)
    {
        message[byteIndex] = static_cast<std::byte>(
            static_cast<std::uint8_t>(byteIndex * 73U + 11U));
    }
    return message;
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeDescriptor(
    const std::span<const std::byte> message,
    const std::uint32_t outerBlockBytes,
    const pbprotocol::WirehairV2SerializedProfile& serializedProfile)
{
    pbprotocol::SegmentDescriptor descriptor{};
    descriptor.encodedSize = static_cast<std::uint64_t>(message.size());
    descriptor.outerFecMode = pbprotocol::OuterFecMode::WirehairV2;
    descriptor.outerBlockBytes = outerBlockBytes;
    descriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::ComputeBlake3Digest(message)};
    descriptor.wirehairV2SerializedProfile = serializedProfile;
    return descriptor;
}

[[nodiscard]] std::vector<std::byte> MakeEncodedBlock(
    pbouterfec::WirehairV2Encoder& encoder,
    const std::uint32_t outerBlockId)
{
    std::vector<std::byte> payload(encoder.GetOuterBlockBytes());
    const auto encodeResult = encoder.EncodeBlock(outerBlockId, payload);
    REQUIRE(encodeResult);
    payload.resize(encodeResult.Value());
    return payload;
}

void RequireRecoveryEquals(
    pbouterfec::WirehairV2Decoder& decoder,
    const std::span<const std::byte> expected)
{
    std::vector<std::byte> recovered(expected.size());
    const auto recoverResult = decoder.Recover(recovered);
    REQUIRE(recoverResult);
    REQUIRE(recoverResult.Value() == expected.size());
    REQUIRE(std::equal(
        recovered.begin(), recovered.end(), expected.begin(), expected.end()));
}

[[nodiscard]] constexpr std::byte Byte(const std::uint8_t value) noexcept
{
    return static_cast<std::byte>(value);
}

const std::array<std::byte, 32> kExpectedProfile117x16{
    Byte(0x57), Byte(0x48), Byte(0x56), Byte(0x32),
    Byte(0x01), Byte(0x00), Byte(0x20), Byte(0x00),
    Byte(0xC9), Byte(0xF9), Byte(0xF4), Byte(0x47),
    Byte(0xBB), Byte(0x5B), Byte(0x29), Byte(0x4B),
    Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x10), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00)};

} // namespace

TEST_CASE("Wirehair V2 canonical descriptor and repair packet match independent goldens")
{
    const std::vector<std::byte> message117 = MakeMessage(kMessageBytes);
    auto encoder117Result = pbouterfec::WirehairV2Encoder::Create(
        message117, kBlockBytes);
    REQUIRE(encoder117Result);
    pbouterfec::WirehairV2Encoder encoder117 =
        std::move(encoder117Result).Value();

    const pbprotocol::WirehairV2SerializedProfile serializedProfile =
        encoder117.GetSerializedProfile();
    REQUIRE(serializedProfile.bytes == kExpectedProfile117x16);
    REQUIRE(pbprotocol::ValidateWirehairV2SerializedProfile(
        serializedProfile, kMessageBytes, kBlockBytes));
    REQUIRE(wirehair_v2_profile_validate(
        serializedProfile.bytes.data(),
        static_cast<std::uint32_t>(serializedProfile.bytes.size()))
        == WirehairV2_Success);

    WirehairV2Profile upstreamProfile{};
    REQUIRE(wirehair_v2_profile_deserialize(
        serializedProfile.bytes.data(),
        static_cast<std::uint32_t>(serializedProfile.bytes.size()),
        &upstreamProfile) == WirehairV2_Success);
    REQUIRE(upstreamProfile.profile_id
        == pbouterfec::kWirehairV2CertifiedProfileId);
    REQUIRE(upstreamProfile.message_bytes == kMessageBytes);
    REQUIRE(upstreamProfile.block_bytes == kBlockBytes);

    const std::vector<std::byte> officialMessage = MakeMessage(128);
    auto officialEncoderResult = pbouterfec::WirehairV2Encoder::Create(
        officialMessage, kBlockBytes);
    REQUIRE(officialEncoderResult);
    pbouterfec::WirehairV2Encoder officialEncoder =
        std::move(officialEncoderResult).Value();
    const std::vector<std::byte> repair =
        MakeEncodedBlock(officialEncoder, 12345);
    const std::array<std::byte, 16> expectedRepair{
        Byte(0xE0), Byte(0x0C), Byte(0x23), Byte(0x1B),
        Byte(0x7C), Byte(0x47), Byte(0xFF), Byte(0xD4),
        Byte(0x80), Byte(0x17), Byte(0x47), Byte(0x48),
        Byte(0x26), Byte(0xCB), Byte(0x88), Byte(0xB4)};
    REQUIRE(std::equal(
        repair.begin(), repair.end(), expectedRepair.begin(), expectedRepair.end()));
}

TEST_CASE("Wirehair V2 systematic blocks recover the exact segment")
{
    const std::vector<std::byte> message = MakeMessage(kMessageBytes);
    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, kBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder =
        std::move(encoderResult).Value();
    REQUIRE(encoder.GetBlockCount() == 8);

    const pbprotocol::SegmentDescriptor descriptor = MakeDescriptor(
        message, kBlockBytes, encoder.GetSerializedProfile());
    auto decoderResult = pbouterfec::WirehairV2Decoder::Create(descriptor);
    REQUIRE(decoderResult);
    pbouterfec::WirehairV2Decoder decoder =
        std::move(decoderResult).Value();

    for (std::uint32_t blockId = 0;
        blockId < encoder.GetBlockCount();
        blockId++)
    {
        const std::vector<std::byte> payload =
            MakeEncodedBlock(encoder, blockId);
        const std::size_t expectedBytes =
            blockId + 1U == encoder.GetBlockCount() ? 5U : kBlockBytes;
        REQUIRE(payload.size() == expectedBytes);

        const auto decodeResult = decoder.DecodeBlock(blockId, payload);
        REQUIRE(decodeResult);
        REQUIRE(decodeResult.Value() ==
            (blockId + 1U == encoder.GetBlockCount()
                ? pbouterfec::DecodeDisposition::Ready
                : pbouterfec::DecodeDisposition::NeedMore));
    }

    RequireRecoveryEquals(decoder, message);
}

TEST_CASE("Wirehair V2 repair-only decoding has a finite repair window")
{
    const std::vector<std::byte> message = MakeMessage(kMessageBytes);
    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, kBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder =
        std::move(encoderResult).Value();
    const pbprotocol::SegmentDescriptor descriptor = MakeDescriptor(
        message, kBlockBytes, encoder.GetSerializedProfile());
    auto decoderResult = pbouterfec::WirehairV2Decoder::Create(descriptor);
    REQUIRE(decoderResult);
    pbouterfec::WirehairV2Decoder decoder =
        std::move(decoderResult).Value();

    pbouterfec::DecodeDisposition disposition =
        pbouterfec::DecodeDisposition::NeedMore;
    const std::uint32_t firstRepairId = encoder.GetBlockCount();
    for (std::uint32_t blockId = firstRepairId;
        blockId < firstRepairId + 64U
        && disposition == pbouterfec::DecodeDisposition::NeedMore;
        blockId++)
    {
        const std::vector<std::byte> payload =
            MakeEncodedBlock(encoder, blockId);
        REQUIRE(payload.size() == kBlockBytes);
        const auto decodeResult = decoder.DecodeBlock(blockId, payload);
        REQUIRE(decodeResult);
        disposition = decodeResult.Value();
    }

    REQUIRE(disposition == pbouterfec::DecodeDisposition::Ready);
    RequireRecoveryEquals(decoder, message);
}

TEST_CASE("Wirehair V2 accepts a fixed out-of-order systematic and repair mix")
{
    const std::vector<std::byte> message = MakeMessage(kMessageBytes);
    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, kBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder =
        std::move(encoderResult).Value();
    const pbprotocol::SegmentDescriptor descriptor = MakeDescriptor(
        message, kBlockBytes, encoder.GetSerializedProfile());
    auto decoderResult = pbouterfec::WirehairV2Decoder::Create(descriptor);
    REQUIRE(decoderResult);
    pbouterfec::WirehairV2Decoder decoder =
        std::move(decoderResult).Value();

    const std::array<std::uint32_t, 10> permutation{
        8, 2, 9, 7, 0, 6, 1, 5, 3, 4};
    pbouterfec::DecodeDisposition disposition =
        pbouterfec::DecodeDisposition::NeedMore;
    for (const std::uint32_t blockId : permutation)
    {
        if (disposition == pbouterfec::DecodeDisposition::Ready)
        {
            break;
        }
        const std::vector<std::byte> payload =
            MakeEncodedBlock(encoder, blockId);
        const auto decodeResult = decoder.DecodeBlock(blockId, payload);
        REQUIRE(decodeResult);
        disposition = decodeResult.Value();
    }

    REQUIRE(disposition == pbouterfec::DecodeDisposition::Ready);
    RequireRecoveryEquals(decoder, message);
}

TEST_CASE("Wirehair V2 duplicate IDs are idempotent and conflicts are terminal")
{
    const std::vector<std::byte> message = MakeMessage(kMessageBytes);
    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, kBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder =
        std::move(encoderResult).Value();
    const pbprotocol::SegmentDescriptor descriptor = MakeDescriptor(
        message, kBlockBytes, encoder.GetSerializedProfile());
    auto decoderResult = pbouterfec::WirehairV2Decoder::Create(descriptor);
    REQUIRE(decoderResult);
    pbouterfec::WirehairV2Decoder decoder =
        std::move(decoderResult).Value();

    const std::vector<std::byte> payload = MakeEncodedBlock(encoder, 0);
    const auto firstResult = decoder.DecodeBlock(0, payload);
    REQUIRE(firstResult);
    REQUIRE(firstResult.Value() == pbouterfec::DecodeDisposition::NeedMore);
    const auto duplicateResult = decoder.DecodeBlock(0, payload);
    REQUIRE(duplicateResult);
    REQUIRE(duplicateResult.Value() == pbouterfec::DecodeDisposition::NeedMore);

    std::vector<std::byte> conflictingPayload = payload;
    conflictingPayload[3] ^= Byte(0x80);
    const auto conflictResult = decoder.DecodeBlock(0, conflictingPayload);
    REQUIRE_FALSE(conflictResult);
    REQUIRE(conflictResult.Error().code
        == pbouterfec::OuterFecErrorCode::InvalidInput);

    const auto afterConflict = decoder.DecodeBlock(1, payload);
    REQUIRE_FALSE(afterConflict);
    REQUIRE(afterConflict.Error().code
        == pbouterfec::OuterFecErrorCode::InvalidState);
}

TEST_CASE("Wirehair V2 Carousel recreation preserves descriptor and equations")
{
    const std::vector<std::byte> message = MakeMessage(kMessageBytes);
    pbprotocol::WirehairV2SerializedProfile savedProfile{};
    std::array<std::vector<std::byte>, 3> firstPassPayloads;
    const std::array<std::uint32_t, 3> blockIds{8, 9, 12345};

    {
        auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
            message, kBlockBytes);
        REQUIRE(encoderResult);
        pbouterfec::WirehairV2Encoder encoder =
            std::move(encoderResult).Value();
        savedProfile = encoder.GetSerializedProfile();
        for (std::size_t blockIndex = 0;
            blockIndex < blockIds.size();
            blockIndex++)
        {
            firstPassPayloads[blockIndex] =
                MakeEncodedBlock(encoder, blockIds[blockIndex]);
        }
    }

    const pbprotocol::SegmentDescriptor descriptor = MakeDescriptor(
        message, kBlockBytes, savedProfile);
    auto recreatedResult = pbouterfec::WirehairV2Encoder::Recreate(
        message, descriptor);
    REQUIRE(recreatedResult);
    pbouterfec::WirehairV2Encoder recreated =
        std::move(recreatedResult).Value();
    REQUIRE(recreated.GetSerializedProfile() == savedProfile);
    for (std::size_t blockIndex = 0;
        blockIndex < blockIds.size();
        blockIndex++)
    {
        REQUIRE(MakeEncodedBlock(recreated, blockIds[blockIndex])
            == firstPassPayloads[blockIndex]);
    }

    const std::vector<std::byte> nextRepair =
        MakeEncodedBlock(recreated, 12346);
    REQUIRE(nextRepair != firstPassPayloads.back());

    std::vector<std::byte> changedMessage = message;
    changedMessage[42] ^= Byte(0x01);
    const auto changedMessageResult =
        pbouterfec::WirehairV2Encoder::Recreate(
            changedMessage, descriptor);
    REQUIRE_FALSE(changedMessageResult);
    REQUIRE(changedMessageResult.Error().code
        == pbouterfec::OuterFecErrorCode::EncodedDigestMismatch);

    pbprotocol::SegmentDescriptor badMagic = descriptor;
    badMagic.wirehairV2SerializedProfile->bytes[0] = Byte(0x58);
    const auto badMagicResult = pbouterfec::WirehairV2Encoder::Recreate(
        message, badMagic);
    REQUIRE_FALSE(badMagicResult);
    REQUIRE(badMagicResult.Error().code
        == pbouterfec::OuterFecErrorCode::InvalidMagic);

    pbprotocol::SegmentDescriptor changedProfile = descriptor;
    changedProfile.wirehairV2SerializedProfile->bytes[8] = Byte(0x00);
    const auto changedProfileResult =
        pbouterfec::WirehairV2Encoder::Recreate(message, changedProfile);
    REQUIRE_FALSE(changedProfileResult);
    REQUIRE(changedProfileResult.Error().code
        == pbouterfec::OuterFecErrorCode::UnsupportedProfile);

    pbprotocol::SegmentDescriptor changedSeed = descriptor;
    changedSeed.wirehairV2SerializedProfile->bytes[28] = Byte(0x01);
    REQUIRE_FALSE(pbouterfec::WirehairV2Encoder::Recreate(
        message, changedSeed));
}

TEST_CASE("Wirehair V2 validates K and block dimensions before creation")
{
    const std::vector<std::byte> oneBlock = MakeMessage(kBlockBytes);
    const auto oneBlockResult = pbouterfec::WirehairV2Encoder::Create(
        oneBlock, kBlockBytes);
    REQUIRE_FALSE(oneBlockResult);
    REQUIRE(oneBlockResult.Error().code
        == pbouterfec::OuterFecErrorCode::InvalidDimensions);
    REQUIRE(oneBlockResult.Error().detail == 1);

    const std::vector<std::byte> tooManyBlocks = MakeMessage(64001);
    const auto tooManyResult = pbouterfec::WirehairV2Encoder::Create(
        tooManyBlocks, 1);
    REQUIRE_FALSE(tooManyResult);
    REQUIRE(tooManyResult.Error().code
        == pbouterfec::OuterFecErrorCode::InvalidDimensions);
    REQUIRE(tooManyResult.Error().detail == 64001);

    const std::vector<std::byte> twoBlocks = MakeMessage(17);
    const auto twoBlockResult = pbouterfec::WirehairV2Encoder::Create(
        twoBlocks, kBlockBytes);
    REQUIRE(twoBlockResult);

    const std::vector<std::byte> emptyMessage;
    REQUIRE_FALSE(pbouterfec::WirehairV2Encoder::Create(
        emptyMessage, kBlockBytes));
    REQUIRE_FALSE(pbouterfec::WirehairV2Encoder::Create(
        twoBlocks, 0));
    const auto hugeBlockResult = pbouterfec::WirehairV2Encoder::Create(
        twoBlocks, 0x80000000U);
    REQUIRE_FALSE(hugeBlockResult);
    REQUIRE(hugeBlockResult.Error().code
        == pbouterfec::OuterFecErrorCode::InvalidDimensions);
}

TEST_CASE("Wirehair V2 rejects malformed and mismatched descriptors")
{
    const std::vector<std::byte> message = MakeMessage(kMessageBytes);
    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, kBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder =
        std::move(encoderResult).Value();
    const pbprotocol::SegmentDescriptor validDescriptor = MakeDescriptor(
        message, kBlockBytes, encoder.GetSerializedProfile());

    struct MutationCase
    {
        std::size_t byteOffset = 0;
        std::byte value{};
        pbouterfec::OuterFecErrorCode expectedError =
            pbouterfec::OuterFecErrorCode::CodecError;
    };
    const std::array<MutationCase, 5> mutations{{
        {0, Byte(0x58), pbouterfec::OuterFecErrorCode::InvalidMagic},
        {4, Byte(0x02), pbouterfec::OuterFecErrorCode::UnsupportedVersion},
        {6, Byte(0x21), pbouterfec::OuterFecErrorCode::InvalidSize},
        {31, Byte(0x01), pbouterfec::OuterFecErrorCode::ReservedNonzero},
        {8, Byte(0x00), pbouterfec::OuterFecErrorCode::UnsupportedProfile}}};

    for (const MutationCase& mutation : mutations)
    {
        pbprotocol::SegmentDescriptor descriptor = validDescriptor;
        descriptor.wirehairV2SerializedProfile->bytes[mutation.byteOffset] =
            mutation.value;
        const auto decoderResult =
            pbouterfec::WirehairV2Decoder::Create(descriptor);
        REQUIRE_FALSE(decoderResult);
        REQUIRE(decoderResult.Error().code == mutation.expectedError);
    }

    pbprotocol::SegmentDescriptor messageMismatch = validDescriptor;
    messageMismatch.encodedSize++;
    const auto messageMismatchResult =
        pbouterfec::WirehairV2Decoder::Create(messageMismatch);
    REQUIRE_FALSE(messageMismatchResult);
    REQUIRE(messageMismatchResult.Error().code
        == pbouterfec::OuterFecErrorCode::EncodedSizeMismatch);

    pbprotocol::SegmentDescriptor blockMismatch = validDescriptor;
    blockMismatch.outerBlockBytes++;
    const auto blockMismatchResult =
        pbouterfec::WirehairV2Decoder::Create(blockMismatch);
    REQUIRE_FALSE(blockMismatchResult);
    REQUIRE(blockMismatchResult.Error().code
        == pbouterfec::OuterFecErrorCode::OuterBlockBytesMismatch);

    pbprotocol::SegmentDescriptor missingProfile = validDescriptor;
    missingProfile.wirehairV2SerializedProfile.reset();
    REQUIRE_FALSE(pbouterfec::WirehairV2Decoder::Create(missingProfile));

    pbprotocol::SegmentDescriptor wrongMode = validDescriptor;
    wrongMode.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    REQUIRE_FALSE(pbouterfec::WirehairV2Decoder::Create(wrongMode));
}

TEST_CASE("Wirehair mixed profiles reject odd blocks and remain outside admission")
{
    const std::vector<std::byte> oddBlockMessage = MakeMessage(33);
    for (const std::uint64_t profileId : {
        pbouterfec::kWirehairV2MixedProfileId,
        pbouterfec::kWirehairV2MixedMix2ProfileId})
    {
        const auto oddResult = pbouterfec::WirehairV2Encoder::Create(
            oddBlockMessage, 3, profileId);
        REQUIRE_FALSE(oddResult);
        REQUIRE(oddResult.Error().code
            == pbouterfec::OuterFecErrorCode::InvalidDimensions);
    }

    const std::vector<std::byte> evenBlockMessage = MakeMessage(128);
    auto mixedEncoderResult = pbouterfec::WirehairV2Encoder::Create(
        evenBlockMessage,
        kBlockBytes,
        pbouterfec::kWirehairV2MixedProfileId);
    REQUIRE(mixedEncoderResult);
    pbouterfec::WirehairV2Encoder mixedEncoder =
        std::move(mixedEncoderResult).Value();
    const pbprotocol::SegmentDescriptor mixedDescriptor = MakeDescriptor(
        evenBlockMessage,
        kBlockBytes,
        mixedEncoder.GetSerializedProfile());
    const auto mixedDecoderResult =
        pbouterfec::WirehairV2Decoder::Create(mixedDescriptor);
    REQUIRE_FALSE(mixedDecoderResult);
    REQUIRE(mixedDecoderResult.Error().code
        == pbouterfec::OuterFecErrorCode::UnsupportedProfile);
}

TEST_CASE("Wirehair V2 BufferTooSmall paths are transactional and retriable")
{
    const std::vector<std::byte> message = MakeMessage(kMessageBytes);
    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, kBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder =
        std::move(encoderResult).Value();

    std::vector<std::byte> shortRepair(kBlockBytes - 1U, Byte(0xA5));
    const std::vector<std::byte> originalShortRepair = shortRepair;
    const auto shortRepairResult = encoder.EncodeBlock(
        encoder.GetBlockCount(), shortRepair);
    REQUIRE_FALSE(shortRepairResult);
    REQUIRE(shortRepairResult.Error().code
        == pbouterfec::OuterFecErrorCode::BufferTooSmall);
    REQUIRE(shortRepairResult.Error().detail == kBlockBytes);
    REQUIRE(shortRepair == originalShortRepair);

    std::vector<std::byte> shortFinal(4, Byte(0x5A));
    const std::vector<std::byte> originalShortFinal = shortFinal;
    const auto shortFinalResult = encoder.EncodeBlock(
        encoder.GetBlockCount() - 1U, shortFinal);
    REQUIRE_FALSE(shortFinalResult);
    REQUIRE(shortFinalResult.Error().detail == 5);
    REQUIRE(shortFinal == originalShortFinal);

    const pbprotocol::SegmentDescriptor descriptor = MakeDescriptor(
        message, kBlockBytes, encoder.GetSerializedProfile());
    auto decoderResult = pbouterfec::WirehairV2Decoder::Create(descriptor);
    REQUIRE(decoderResult);
    pbouterfec::WirehairV2Decoder decoder =
        std::move(decoderResult).Value();
    for (std::uint32_t blockId = 0;
        blockId < encoder.GetBlockCount();
        blockId++)
    {
        const std::vector<std::byte> payload =
            MakeEncodedBlock(encoder, blockId);
        REQUIRE(decoder.DecodeBlock(blockId, payload));
    }

    std::vector<std::byte> shortRecovery(message.size() - 1U, Byte(0xCC));
    const std::vector<std::byte> originalShortRecovery = shortRecovery;
    const auto shortRecoveryResult = decoder.Recover(shortRecovery);
    REQUIRE_FALSE(shortRecoveryResult);
    REQUIRE(shortRecoveryResult.Error().code
        == pbouterfec::OuterFecErrorCode::BufferTooSmall);
    REQUIRE(shortRecoveryResult.Error().detail == message.size());
    REQUIRE(shortRecovery == originalShortRecovery);

    RequireRecoveryEquals(decoder, message);
}
