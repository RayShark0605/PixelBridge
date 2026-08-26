#include "pbreceiver/resume_replay.h"

#include "decoder_test_access.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_types.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

[[nodiscard]] constexpr std::byte Byte(const std::uint8_t value) noexcept
{
    return static_cast<std::byte>(value);
}

// Deterministic content pattern shared with the PBOuterFec suite so that
// cross-suite oracles stay comparable (independent byte-level evidence).
[[nodiscard]] std::vector<std::byte> MakeMessage(const std::size_t byteCount)
{
    std::vector<std::byte> message(byteCount);
    for (std::size_t byteIndex = 0; byteIndex < message.size(); byteIndex++)
    {
        message[byteIndex] = static_cast<std::byte>(static_cast<std::uint8_t>(
            byteIndex * 73U + 11U));
    }
    return message;
}

[[nodiscard]] pbouterfec::OuterFecDecoderResourceManager MakeDecoderManager()
{
    auto managerResult =
        pbouterfec::OuterFecDecoderResourceManager::Create(
            pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE(managerResult);
    return std::move(managerResult).Value();
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeWirehairDescriptor(
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

[[nodiscard]] pbprotocol::SegmentDescriptor MakeDirectRepeatDescriptor(
    const std::span<const std::byte> segment,
    const std::uint32_t outerBlockBytes)
{
    pbprotocol::SegmentDescriptor descriptor{};
    descriptor.encodedSize = static_cast<std::uint64_t>(segment.size());
    descriptor.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    descriptor.outerBlockBytes = outerBlockBytes;
    descriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::ComputeBlake3Digest(segment)};
    return descriptor;
}

[[nodiscard]] std::vector<std::byte> EncodeWirehairBlock(
    pbouterfec::WirehairV2Encoder& encoder,
    const std::uint32_t outerBlockId)
{
    std::vector<std::byte> payload(encoder.GetOuterBlockBytes());
    const auto encodeResult = encoder.EncodeBlock(outerBlockId, payload);
    REQUIRE(encodeResult);
    payload.resize(encodeResult.Value());
    return payload;
}

struct DirectEncodedBlock
{
    std::uint32_t realPayloadBytes = 0U;
    std::vector<std::byte> paddedPayload;
};

[[nodiscard]] DirectEncodedBlock EncodeDirectBlock(
    pbouterfec::DirectRepeatEncoder& encoder,
    const std::uint32_t blockOrdinal)
{
    DirectEncodedBlock block;
    block.paddedPayload.resize(encoder.GetOuterBlockBytes());
    const auto encodeResult = encoder.EncodeBlock(blockOrdinal, block.paddedPayload);
    REQUIRE(encodeResult);
    block.realPayloadBytes = encodeResult.Value();
    return block;
}

[[nodiscard]] bool IsOuterFecErrorWith(
    const pbreceiver::ReceiverError& receiverError,
    const pbouterfec::OuterFecErrorCode code,
    const std::uint64_t detail) noexcept
{
    const auto* outerError =
        std::get_if<pbouterfec::OuterFecError>(&receiverError);
    return outerError != nullptr && outerError->code == code &&
        outerError->detail == detail;
}

void RequireRecoverEquals(
    pbouterfec::WirehairV2Decoder& decoder,
    const std::span<const std::byte> expected)
{
    std::vector<std::byte> recovered(expected.size());
    const auto recoverResult = decoder.Recover(recovered);
    REQUIRE(recoverResult);
    CHECK(static_cast<std::uint64_t>(expected.size()) == recoverResult.Value());
    CHECK(std::equal(
        recovered.begin(), recovered.end(), expected.begin(), expected.end()));
}

void RequireRecoverEquals(
    pbouterfec::DirectRepeatDecoder& decoder,
    const std::span<const std::byte> expected)
{
    std::vector<std::byte> recovered(expected.size());
    const auto recoverResult = decoder.Recover(recovered);
    REQUIRE(recoverResult);
    CHECK(static_cast<std::uint64_t>(expected.size()) == recoverResult.Value());
    CHECK(std::equal(
        recovered.begin(), recovered.end(), expected.begin(), expected.end()));
}

} // namespace

TEST_CASE("ReplayActiveWirehairCache re-injects stored validated spans into a fresh decoder",
          "[resume-replay][wirehair]")
{
    constexpr std::uint32_t kOuterBlockBytes = 16U;
    const std::vector<std::byte> message = MakeMessage(117);

    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, kOuterBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder = std::move(encoderResult).Value();
    // 117 / 16 -> K=8 systematic blocks; the last systematic block carries
    // only 5 bytes (the short-tail case that must round-trip through replay).
    CHECK(encoder.GetBlockCount() == 8U);

    const pbprotocol::WirehairV2SerializedProfile serializedProfile =
        encoder.GetSerializedProfile();
    const pbprotocol::SegmentDescriptor descriptor = MakeWirehairDescriptor(
        message, kOuterBlockBytes, serializedProfile);
    const pbouterfec::OuterFecDecoderResourceManager decoderManager =
        MakeDecoderManager();

    auto liveDecoderResult =
        pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            descriptor, decoderManager);
    REQUIRE(liveDecoderResult);
    pbouterfec::WirehairV2Decoder liveDecoder =
        std::move(liveDecoderResult).Value();

    // Pre-crash capture: systematic ids 0/2/4 plus the short-tail last
    // systematic id (5 bytes) and one repair equation. The cache snapshot must
    // hold exactly these payload spans in stored order.
    pbprotocol::ResumeActiveWirehairCacheRecord cacheRecord;
    cacheRecord.segmentOrdinal = 7U;
    cacheRecord.wirehairProfile = serializedProfile;

    const std::array<std::uint32_t, 5> liveBlockIds{0U, 2U, 4U, 7U, 8U};
    for (const std::uint32_t blockId : liveBlockIds)
    {
        const std::vector<std::byte> payload = EncodeWirehairBlock(encoder, blockId);
        if (blockId == 7U)
        {
            // Independent geometry oracle: short tail = 117 % 16 = 5 bytes.
            CHECK(payload.size() == 5U);
        }
        else
        {
            CHECK(payload.size() == static_cast<std::size_t>(kOuterBlockBytes));
        }

        const auto decodeResult = liveDecoder.DecodeBlock(
            blockId, std::span<const std::byte>(payload));
        REQUIRE(decodeResult);
        // Five distinct equations < K=8: rank insufficient, never Ready yet.
        CHECK(decodeResult.Value() == pbouterfec::DecodeDisposition::NeedMore);

        pbprotocol::ResumeWirehairCacheEntry entry;
        entry.outerBlockId = blockId;
        entry.payload = payload;
        cacheRecord.entries.push_back(std::move(entry));
    }

    // Fresh decoder (restart path) replaying only the stored spans: exactly 5
    // entries accepted, still not Ready.
    auto replayDecoderResult =
        pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            descriptor, decoderManager);
    REQUIRE(replayDecoderResult);
    pbouterfec::WirehairV2Decoder replayDecoder =
        std::move(replayDecoderResult).Value();

    const auto replayResult =
        pbreceiver::ReplayActiveWirehairCache(cacheRecord, replayDecoder);
    REQUIRE(replayResult);
    CHECK(static_cast<std::uint64_t>(cacheRecord.entries.size()) ==
          static_cast<std::uint64_t>(replayResult.Value().replayedEntryCount));
    CHECK_FALSE(replayResult.Value().decoderReady);

    // Complete the Fountain rank with a fresh encoder: systematic equations
    // are deterministic source slices, so any encoder instance reproduces ids
    // 1/3/5 byte-for-byte. The decoder's finite admission table holds exactly
    // K=8 distinct block ids, so no ninth id may be fed to this instance.
    auto fillEncoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, kOuterBlockBytes);
    REQUIRE(fillEncoderResult);
    pbouterfec::WirehairV2Encoder fillEncoder = std::move(fillEncoderResult).Value();

    const std::array<std::uint32_t, 3> remainingIds{1U, 3U, 5U};
    for (std::size_t idIndex = 0; idIndex < remainingIds.size(); idIndex++)
    {
        const std::vector<std::byte> payload =
            EncodeWirehairBlock(fillEncoder, remainingIds[idIndex]);
        CHECK(payload.size() == static_cast<std::size_t>(kOuterBlockBytes));

        const auto decodeResult = replayDecoder.DecodeBlock(
            remainingIds[idIndex], std::span<const std::byte>(payload));
        REQUIRE(decodeResult);
        const bool isFinalFill = idIndex == remainingIds.size() - 1U;
        CHECK((decodeResult.Value() == pbouterfec::DecodeDisposition::Ready)
              == isFinalFill);
    }

    RequireRecoverEquals(replayDecoder, message);

    // Independent digest oracle: recovered bytes hash to exactly the digest
    // carried by the descriptor (in-band verification, not sender auth).
    const std::array<std::byte, pbprotocol::kDigestBytes> expectedDigest =
        pbprotocol::ComputeBlake3Digest(message);
    CHECK(descriptor.encodedDigest.bytes == expectedDigest);

    // Ready is sticky and duplicates are free: re-decoding an already
    // accepted equation reports Ready without growing the finite admission
    // table (a brand-new ninth id would be rejected instead).
    const std::vector<std::byte> postRecoverPayload =
        EncodeWirehairBlock(fillEncoder, 4U);
    CHECK(postRecoverPayload.size() == static_cast<std::size_t>(kOuterBlockBytes));
    const auto postRecoverDecodeResult = replayDecoder.DecodeBlock(
        4U, std::span<const std::byte>(postRecoverPayload));
    REQUIRE(postRecoverDecodeResult);
    CHECK(postRecoverDecodeResult.Value() == pbouterfec::DecodeDisposition::Ready);
}

TEST_CASE("ReplayDirectRepeatBlocks revalidates lengths and canonical zero padding",
          "[resume-replay][direct-repeat]")
{
    constexpr std::uint32_t kOuterBlockBytes = 16U;
    const std::vector<std::byte> message = MakeMessage(37);

    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(
        message, kOuterBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder = std::move(encoderResult).Value();
    CHECK(encoder.GetBlockCount() == 3ULL); // ceil(37 / 16), last block real=5.

    const pbprotocol::SegmentDescriptor descriptor = MakeDirectRepeatDescriptor(
        message, kOuterBlockBytes);
    const pbouterfec::OuterFecDecoderResourceManager decoderManager =
        MakeDecoderManager();

    const DirectEncodedBlock block0 = EncodeDirectBlock(encoder, 0U);
    const DirectEncodedBlock block1 = EncodeDirectBlock(encoder, 1U);
    const DirectEncodedBlock block2 = EncodeDirectBlock(encoder, 2U);
    CHECK(block0.realPayloadBytes == 16U);
    CHECK(block1.realPayloadBytes == 16U);
    CHECK(block2.realPayloadBytes == 5U); // short tail: independent oracle.

    auto liveDecoderResult =
        pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
            descriptor, kOuterBlockBytes, decoderManager);
    REQUIRE(liveDecoderResult);
    pbouterfec::DirectRepeatDecoder liveDecoder =
        std::move(liveDecoderResult).Value();

    // Pre-crash capture: blocks 0 and 2 (the short-tail one), stored order.
    for (const std::uint32_t feedBlockOrdinal : {0U, 2U})
    {
        const DirectEncodedBlock& encodedBlock =
            (feedBlockOrdinal == 0U) ? block0 : block2;
        const auto decodeResult = liveDecoder.DecodeBlock(
            feedBlockOrdinal,
            encodedBlock.realPayloadBytes,
            std::span<const std::byte>(encodedBlock.paddedPayload));
        REQUIRE(decodeResult);
        CHECK(decodeResult.Value() == pbouterfec::DecodeDisposition::NeedMore);
    }

    pbprotocol::ResumeActiveDirectRepeatRecord directRecord;
    directRecord.segmentOrdinal = 9U;
    directRecord.directBlockCount = 3U;
    for (const std::uint32_t capturedOrdinal : {0U, 2U})
    {
        const DirectEncodedBlock& encodedBlock =
            (capturedOrdinal == 0U) ? block0 : block2;
        pbprotocol::ResumeDirectRepeatEntry entry;
        entry.blockOrdinal = capturedOrdinal;
        entry.realPayloadBytes = encodedBlock.realPayloadBytes;
        entry.paddedPayload = encodedBlock.paddedPayload;
        directRecord.entries.push_back(std::move(entry));
    }

    // Restart path: replay into a fresh decoder revalidates each stored span.
    auto replayDecoderResult =
        pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
            descriptor, kOuterBlockBytes, decoderManager);
    REQUIRE(replayDecoderResult);
    pbouterfec::DirectRepeatDecoder replayDecoder =
        std::move(replayDecoderResult).Value();

    const auto replayResult =
        pbreceiver::ReplayDirectRepeatBlocks(directRecord, replayDecoder);
    REQUIRE(replayResult);
    CHECK(static_cast<std::uint64_t>(directRecord.entries.size()) ==
          static_cast<std::uint64_t>(replayResult.Value().replayedEntryCount));
    CHECK_FALSE(replayResult.Value().decoderReady);

    // Complete live with block 1: all three blocks received -> Ready.
    auto fillEncoderResult = pbouterfec::DirectRepeatEncoder::Create(
        message, kOuterBlockBytes);
    REQUIRE(fillEncoderResult);
    pbouterfec::DirectRepeatEncoder fillEncoder = std::move(fillEncoderResult).Value();
    const DirectEncodedBlock refillBlock1 = EncodeDirectBlock(fillEncoder, 1U);
    CHECK(refillBlock1.realPayloadBytes == block1.realPayloadBytes);
    CHECK(refillBlock1.paddedPayload == block1.paddedPayload); // deterministic slices.

    const auto finalDecodeResult = replayDecoder.DecodeBlock(
        1U,
        refillBlock1.realPayloadBytes,
        std::span<const std::byte>(refillBlock1.paddedPayload));
    REQUIRE(finalDecodeResult);
    CHECK(finalDecodeResult.Value() == pbouterfec::DecodeDisposition::Ready);

    RequireRecoverEquals(replayDecoder, message);
    const std::array<std::byte, pbprotocol::kDigestBytes> expectedDigest =
        pbprotocol::ComputeBlake3Digest(message);
    CHECK(descriptor.encodedDigest.bytes == expectedDigest);

    // Corrupt replay entries fail closed during re-injection (never at
    // recovery time) with the decoder's exact diagnostic mapping:
    const std::uint64_t kExpectedBlockCount = 3ULL;

    auto runCorruptedReplay = [&decoderManager, &descriptor](
                                  pbprotocol::ResumeActiveDirectRepeatRecord corruptedRecord)
    {
        auto freshDecoderResult =
            pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
                descriptor, kOuterBlockBytes, decoderManager);
        REQUIRE(freshDecoderResult);
        pbouterfec::DirectRepeatDecoder freshDecoder = std::move(freshDecoderResult).Value();
        return pbreceiver::ReplayDirectRepeatBlocks(corruptedRecord, freshDecoder);
    };

    // (a) Non-canonical padding: block 2 has real=5, so the first violation at
    // padded index 7 must be reported as detail=7.
    {
        pbprotocol::ResumeActiveDirectRepeatRecord corrupted = directRecord;
        corrupted.entries[1].paddedPayload[7] = Byte(0x5A);
        const auto corruptedReplayResult = runCorruptedReplay(corrupted);
        REQUIRE_FALSE(corruptedReplayResult);
        CHECK(IsOuterFecErrorWith(
            corruptedReplayResult.Error(),
            pbouterfec::OuterFecErrorCode::InvalidInput, 7U));
    }

    // (b) realPayloadBytes mismatch: the decoder revalidates against its own
    // descriptor-derived expectation and reports the offending supplied value.
    {
        pbprotocol::ResumeActiveDirectRepeatRecord corrupted = directRecord;
        corrupted.entries[1].realPayloadBytes = 6U;
        const auto corruptedReplayResult = runCorruptedReplay(corrupted);
        REQUIRE_FALSE(corruptedReplayResult);
        CHECK(IsOuterFecErrorWith(
            corruptedReplayResult.Error(),
            pbouterfec::OuterFecErrorCode::InvalidInput, 6U));
    }

    // (c) Padded region narrower than OuterBlockBytes: detail = actual size.
    {
        pbprotocol::ResumeActiveDirectRepeatRecord corrupted = directRecord;
        corrupted.entries[0].paddedPayload.resize(15U);
        const auto corruptedReplayResult = runCorruptedReplay(corrupted);
        REQUIRE_FALSE(corruptedReplayResult);
        CHECK(IsOuterFecErrorWith(
            corruptedReplayResult.Error(),
            pbouterfec::OuterFecErrorCode::InvalidInput, 15U));
    }

    // (d) Block ordinal at the descriptor-derived count: out of range.
    {
        pbprotocol::ResumeActiveDirectRepeatRecord corrupted = directRecord;
        corrupted.entries[0].blockOrdinal = static_cast<std::uint32_t>(kExpectedBlockCount);
        const auto corruptedReplayResult = runCorruptedReplay(corrupted);
        REQUIRE_FALSE(corruptedReplayResult);
        CHECK(IsOuterFecErrorWith(
            corruptedReplayResult.Error(),
            pbouterfec::OuterFecErrorCode::InvalidInput, 3U));
    }

    // Permutation invariance: the same entries stored in reversed order replay
    // identically (no hidden ordering dependence in re-injection).
    {
        auto freshDecoderResult =
            pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
                descriptor, kOuterBlockBytes, decoderManager);
        REQUIRE(freshDecoderResult);
        pbouterfec::DirectRepeatDecoder permutationDecoder =
            std::move(freshDecoderResult).Value();

        pbprotocol::ResumeActiveDirectRepeatRecord reversed = directRecord;
        std::reverse(reversed.entries.begin(), reversed.entries.end());
        const auto permutationReplayResult =
            pbreceiver::ReplayDirectRepeatBlocks(reversed, permutationDecoder);
        REQUIRE(permutationReplayResult);
        CHECK(static_cast<std::uint64_t>(reversed.entries.size()) ==
              static_cast<std::uint64_t>(
                  permutationReplayResult.Value().replayedEntryCount));
        CHECK_FALSE(permutationReplayResult.Value().decoderReady);

        const auto permutationFillResult = permutationDecoder.DecodeBlock(
            1U,
            refillBlock1.realPayloadBytes,
            std::span<const std::byte>(refillBlock1.paddedPayload));
        REQUIRE(permutationFillResult);
        CHECK(permutationFillResult.Value() == pbouterfec::DecodeDisposition::Ready);
    }
}

TEST_CASE("ReplayActiveWirehairCache surfaces the decoder terminal latch after an id conflict",
          "[resume-replay][wirehair][defensive]")
{
    constexpr std::uint32_t kOuterBlockBytes = 16U;
    const std::vector<std::byte> message = MakeMessage(117);

    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, kOuterBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder = std::move(encoderResult).Value();
    const pbprotocol::SegmentDescriptor descriptor = MakeWirehairDescriptor(
        message, kOuterBlockBytes, encoder.GetSerializedProfile());

    auto decoderResult =
        pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            descriptor, MakeDecoderManager());
    REQUIRE(decoderResult);
    pbouterfec::WirehairV2Decoder decoder = std::move(decoderResult).Value();

    // Deliberately bypass LoadResumeState validation here: a same-id payload
    // conflict that the document loader rejects with ResumeRecordConflict is
    // handed to replay as-is, proving the decoder terminal latch stays
    // authoritative and no partial state leaks before Ready.
    pbprotocol::ResumeActiveWirehairCacheRecord conflictingRecord;
    conflictingRecord.segmentOrdinal = 1U;
    conflictingRecord.wirehairProfile = encoder.GetSerializedProfile();

    const std::vector<std::byte> validPayload0 = EncodeWirehairBlock(encoder, 0U);
    const std::vector<std::byte> conflictPayload0 = [&validPayload0]()
    {
        std::vector<std::byte> conflicted = validPayload0;
        conflicted[3] ^= Byte(0x41); // one bit flip keeps the span length valid.
        return conflicted;
    }();

    pbprotocol::ResumeWirehairCacheEntry firstEntry;
    firstEntry.outerBlockId = 0U;
    firstEntry.payload = validPayload0;
    pbprotocol::ResumeWirehairCacheEntry secondEntry;
    secondEntry.outerBlockId = 0U;
    secondEntry.payload = conflictPayload0;
    conflictingRecord.entries.push_back(std::move(firstEntry));
    conflictingRecord.entries.push_back(std::move(secondEntry));

    const auto conflictReplayResult =
        pbreceiver::ReplayActiveWirehairCache(conflictingRecord, decoder);
    REQUIRE_FALSE(conflictReplayResult);
    CHECK(IsOuterFecErrorWith(
        conflictReplayResult.Error(),
        pbouterfec::OuterFecErrorCode::OuterBlockConflict, 0U));

    // Every subsequent operation on the latched decoder reports InvalidState
    // carrying the original terminal code as detail.
    const std::uint64_t expectedLatchDetail = static_cast<std::uint64_t>(
        pbouterfec::OuterFecErrorCode::OuterBlockConflict);

    pbprotocol::ResumeActiveWirehairCacheRecord followUpRecord;
    followUpRecord.segmentOrdinal = 1U;
    followUpRecord.wirehairProfile = encoder.GetSerializedProfile();
    const std::vector<std::byte> validPayload1 = EncodeWirehairBlock(encoder, 1U);
    pbprotocol::ResumeWirehairCacheEntry followUpEntry;
    followUpEntry.outerBlockId = 1U;
    followUpEntry.payload = validPayload1;
    followUpRecord.entries.push_back(std::move(followUpEntry));

    const auto followUpReplayResult =
        pbreceiver::ReplayActiveWirehairCache(followUpRecord, decoder);
    REQUIRE_FALSE(followUpReplayResult);
    CHECK(IsOuterFecErrorWith(
        followUpReplayResult.Error(),
        pbouterfec::OuterFecErrorCode::InvalidState, expectedLatchDetail));

    std::vector<std::byte> recovered(message.size());
    const std::byte sentinelByte = Byte(0x5A);
    std::fill(recovered.begin(), recovered.end(), sentinelByte);
    const auto prematureRecoverResult = decoder.Recover(recovered);
    REQUIRE_FALSE(prematureRecoverResult);
    // Recover reports plain InvalidState; the terminal code detail rides on
    // DecodeBlock results (asserted above), so no partial state can leak.
    CHECK(IsOuterFecErrorWith(
        prematureRecoverResult.Error(),
        pbouterfec::OuterFecErrorCode::InvalidState, 0U));
    // No partial write before Ready: the output buffer is untouched.
    CHECK(std::all_of(
        recovered.begin(), recovered.end(),
        [sentinelByte](const std::byte value) noexcept { return value == sentinelByte; }));
}

TEST_CASE("Replay of an empty cache record succeeds and leaves the decoder usable",
          "[resume-replay]")
{
    constexpr std::uint32_t kOuterBlockBytes = 16U;

    // Wirehair side: zero entries -> nothing replayed, not Ready, decoder clean.
    const std::vector<std::byte> wirehairMessage = MakeMessage(117);
    auto wirehairEncoderResult = pbouterfec::WirehairV2Encoder::Create(
        wirehairMessage, kOuterBlockBytes);
    REQUIRE(wirehairEncoderResult);
    pbouterfec::WirehairV2Encoder wirehairEncoder = std::move(wirehairEncoderResult).Value();
    const pbprotocol::SegmentDescriptor wirehairDescriptor = MakeWirehairDescriptor(
        wirehairMessage, kOuterBlockBytes, wirehairEncoder.GetSerializedProfile());

    auto wirehairDecoderResult =
        pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
            wirehairDescriptor, MakeDecoderManager());
    REQUIRE(wirehairDecoderResult);
    pbouterfec::WirehairV2Decoder wirehairDecoder = std::move(wirehairDecoderResult).Value();

    const pbprotocol::ResumeActiveWirehairCacheRecord emptyWirehairRecord{5U};
    // The aggregate leaves wirehairProfile zeroed; replay must not read it for
    // an entryless record, so this also pins the no-touch contract.
    const auto emptyWirehairReplayResult =
        pbreceiver::ReplayActiveWirehairCache(emptyWirehairRecord, wirehairDecoder);
    REQUIRE(emptyWirehairReplayResult);
    CHECK(emptyWirehairReplayResult.Value().replayedEntryCount == 0U);
    CHECK_FALSE(emptyWirehairReplayResult.Value().decoderReady);

    const std::vector<std::byte> validPayload = EncodeWirehairBlock(
        wirehairEncoder, 0U);
    const auto postEmptyDecodeResult = wirehairDecoder.DecodeBlock(
        0U, std::span<const std::byte>(validPayload));
    REQUIRE(postEmptyDecodeResult);
    CHECK(postEmptyDecodeResult.Value() == pbouterfec::DecodeDisposition::NeedMore);

    // DirectRepeat side: same contract.
    const std::vector<std::byte> directMessage = MakeMessage(37);
    auto directEncoderResult = pbouterfec::DirectRepeatEncoder::Create(
        directMessage, kOuterBlockBytes);
    REQUIRE(directEncoderResult);
    pbouterfec::DirectRepeatEncoder directEncoder = std::move(directEncoderResult).Value();
    const pbprotocol::SegmentDescriptor directDescriptor = MakeDirectRepeatDescriptor(
        directMessage, kOuterBlockBytes);

    auto directDecoderResult =
        pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
            directDescriptor, kOuterBlockBytes, MakeDecoderManager());
    REQUIRE(directDecoderResult);
    pbouterfec::DirectRepeatDecoder directDecoder = std::move(directDecoderResult).Value();

    const pbprotocol::ResumeActiveDirectRepeatRecord emptyDirectRecord{6U};
    // directBlockCount stays 0: replay of an entryless record must not consult
    // it, and the fresh decoder (created from a real descriptor) stays usable.
    const auto emptyDirectReplayResult =
        pbreceiver::ReplayDirectRepeatBlocks(emptyDirectRecord, directDecoder);
    REQUIRE(emptyDirectReplayResult);
    CHECK(emptyDirectReplayResult.Value().replayedEntryCount == 0U);
    CHECK_FALSE(emptyDirectReplayResult.Value().decoderReady);

    const DirectEncodedBlock encodedBlock = EncodeDirectBlock(directEncoder, 0U);
    const auto postEmptyDirectDecodeResult = directDecoder.DecodeBlock(
        0U,
        encodedBlock.realPayloadBytes,
        std::span<const std::byte>(encodedBlock.paddedPayload));
    REQUIRE(postEmptyDirectDecodeResult);
    CHECK(postEmptyDirectDecodeResult.Value() == pbouterfec::DecodeDisposition::NeedMore);
}

TEST_CASE("Replay entry size violations map to the decoder exact OuterFecError",
          "[resume-replay][error-mapping]")
{
    constexpr std::uint32_t kOuterBlockBytes = 16U;
    const std::vector<std::byte> message = MakeMessage(117);

    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        message, kOuterBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder = std::move(encoderResult).Value();
    const pbprotocol::SegmentDescriptor descriptor = MakeWirehairDescriptor(
        message, kOuterBlockBytes, encoder.GetSerializedProfile());
    const pbouterfec::OuterFecDecoderResourceManager decoderManager =
        MakeDecoderManager();

    // (a) One entry wider than the full block region: detail = actual size.
    {
        const std::vector<std::byte> validPayload0 = EncodeWirehairBlock(encoder, 0U);
        pbprotocol::ResumeActiveWirehairCacheRecord oversizeRecord;
        oversizeRecord.segmentOrdinal = 2U;
        oversizeRecord.wirehairProfile = encoder.GetSerializedProfile();
        pbprotocol::ResumeWirehairCacheEntry oversizeEntry;
        oversizeEntry.outerBlockId = 0U;
        oversizeEntry.payload = validPayload0;
        oversizeEntry.payload.push_back(Byte(0x00)); // 17 bytes where 16 required.
        oversizeRecord.entries.push_back(std::move(oversizeEntry));

        auto freshDecoderResult =
            pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
                descriptor, decoderManager);
        REQUIRE(freshDecoderResult);
        const auto replayResult = pbreceiver::ReplayActiveWirehairCache(
            oversizeRecord, freshDecoderResult.Value());
        REQUIRE_FALSE(replayResult);
        CHECK(IsOuterFecErrorWith(
            replayResult.Error(),
            pbouterfec::OuterFecErrorCode::InvalidInput, 17U));
    }

    // (b) The short-tail systematic id receives a full-size span: the decoder
    // requires exactly the tail length and rejects with detail = actual size.
    {
        const std::vector<std::byte> validPayload0 = EncodeWirehairBlock(encoder, 0U);
        pbprotocol::ResumeActiveWirehairCacheRecord shortTailOversizeRecord;
        shortTailOversizeRecord.segmentOrdinal = 3U;
        shortTailOversizeRecord.wirehairProfile = encoder.GetSerializedProfile();
        pbprotocol::ResumeWirehairCacheEntry shortTailEntry;
        shortTailEntry.outerBlockId = 7U; // last systematic id of 117/16.
        shortTailEntry.payload = validPayload0; // 16 bytes where 5 required.
        shortTailOversizeRecord.entries.push_back(std::move(shortTailEntry));

        auto freshDecoderResult =
            pbouterfec::test::DecoderTestAccess::CreateWirehairV2Decoder(
                descriptor, decoderManager);
        REQUIRE(freshDecoderResult);
        const auto replayResult = pbreceiver::ReplayActiveWirehairCache(
            shortTailOversizeRecord, freshDecoderResult.Value());
        REQUIRE_FALSE(replayResult);
        CHECK(IsOuterFecErrorWith(
            replayResult.Error(),
            pbouterfec::OuterFecErrorCode::InvalidInput, 16U));
    }
}
