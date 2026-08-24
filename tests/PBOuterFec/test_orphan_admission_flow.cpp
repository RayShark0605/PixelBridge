#include "pbouterfec/direct_repeat.h"

#include "decoder_test_access.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/descriptor_binding.h"
#include "pbprotocol/orphan_transport_block_cache.h"
#include "pbprotocol/protocol_types.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

[[nodiscard]] constexpr std::byte Byte(const std::uint8_t value) noexcept
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

[[nodiscard]] pbprotocol::SessionDescriptor MakeFlowSession(
    const std::uint64_t originalFileSize) noexcept
{
    pbprotocol::SessionId sessionId{};
    for (std::size_t byteIndex = 0; byteIndex < sessionId.bytes.size();
         byteIndex++)
    {
        sessionId.bytes[byteIndex] = Byte(static_cast<std::uint8_t>(
            0x41U + byteIndex));
    }

    return pbprotocol::SessionDescriptor{
        pbprotocol::GetProtocolVersion(),
        sessionId,
        originalFileSize,
        1,
        pbprotocol::DigestAlgorithm::Blake3_256};
}

struct EncodedBlock
{
    std::uint32_t payloadBytes = 0;
    std::vector<std::byte> paddedPayload;
};

[[nodiscard]] std::vector<EncodedBlock> EncodeAllBlocks(
    const std::span<const std::byte> message,
    const std::uint32_t outerBlockBytes)
{
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(
        message,
        outerBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();

    std::vector<EncodedBlock> blocks;
    for (std::uint64_t blockIndex = 0;
         blockIndex < encoder.GetBlockCount();
         blockIndex++)
    {
        EncodedBlock block;
        block.paddedPayload.resize(outerBlockBytes);
        const auto encodeResult = encoder.EncodeBlock(
            static_cast<std::uint32_t>(blockIndex),
            block.paddedPayload);
        REQUIRE(encodeResult);
        block.payloadBytes = encodeResult.Value();
        blocks.push_back(std::move(block));
    }
    return blocks;
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeFlowSegmentDescriptor(
    const pbprotocol::SessionDescriptor& session,
    const std::span<const std::byte> message,
    const std::uint32_t outerBlockBytes) noexcept
{
    const auto digest = pbprotocol::ComputeBlake3Digest(message);
    return pbprotocol::SegmentDescriptor{
        pbprotocol::DeriveSessionTag(session.sessionId),
        0,
        0,
        static_cast<std::uint64_t>(message.size()),
        static_cast<std::uint64_t>(message.size()),
        pbprotocol::CompressionCodec::Raw,
        pbprotocol::OuterFecMode::DirectRepeat,
        outerBlockBytes,
        pbprotocol::RawDigest{digest},
        pbprotocol::EncodedDigest{digest},
        std::nullopt};
}

[[nodiscard]] pbouterfec::OuterFecDecoderResourceManager MakeFlowManager(
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    auto managerResult =
        pbouterfec::OuterFecDecoderResourceManager::Create(resourcePolicy);
    REQUIRE(managerResult);
    return std::move(managerResult).Value();
}

TEST_CASE("Orphan data blocks never create decoders before descriptor binding",
          "[orphan][admission-flow][integration]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    const std::vector<std::byte> message = MakeMessage(20);
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const pbprotocol::SessionDescriptor session = MakeFlowSession(
        static_cast<std::uint64_t>(message.size()));

    // 20 bytes at 8-byte blocks: payloads of 8, 8, and 4.
    const std::vector<EncodedBlock> blocks = EncodeAllBlocks(message, outerBlockBytes);
    REQUIRE(blocks.size() == 3U);
    REQUIRE(blocks[0].payloadBytes == 8U);
    REQUIRE(blocks[1].payloadBytes == 8U);
    REQUIRE(blocks[2].payloadBytes == 4U);

    const pbouterfec::OuterFecDecoderResourceManager resourceManager =
        MakeFlowManager(resourcePolicy);
    auto cacheResult = pbprotocol::OrphanTransportBlockCache::Create(
        resourcePolicy);
    REQUIRE(cacheResult);
    pbprotocol::OrphanTransportBlockCache orphanCache =
        std::move(cacheResult).Value();

    const pbprotocol::SessionTag sessionTag =
        pbprotocol::DeriveSessionTag(session.sessionId);
    // The descriptor is not bound yet: every block must land in the bounded
    // orphan cache (or be dropped), never in a decoder.
    REQUIRE(orphanCache.Admit(
        sessionTag,
        0,
        0,
        static_cast<std::uint16_t>(blocks[0].payloadBytes),
        blocks[0].paddedPayload,
        5));
    REQUIRE(orphanCache.Admit(
        sessionTag,
        0,
        1,
        static_cast<std::uint16_t>(blocks[1].payloadBytes),
        blocks[1].paddedPayload,
        6));
    REQUIRE(orphanCache.Admit(
        sessionTag,
        0,
        2,
        static_cast<std::uint16_t>(blocks[2].payloadBytes),
        blocks[2].paddedPayload,
        7));

    // Core invariant: unknown-descriptor data created no decoder and reserved
    // no decoder bytes.
    REQUIRE(resourceManager.GetActiveDecoderCount() == 0);
    REQUIRE(resourceManager.GetReservedDecoderBytes() == 0);
    REQUIRE(orphanCache.GetAdmittedBlockCount() == 3U);
    REQUIRE(orphanCache.GetDroppedBlockCount() == 0U);
    REQUIRE(orphanCache.GetCachedBlockCount() == 3U);
    REQUIRE(orphanCache.GetCachedBytes() == 24U);

    const pbprotocol::SegmentDescriptor descriptor =
        MakeFlowSegmentDescriptor(session, message, outerBlockBytes);
    auto stateResult = pbprotocol::DescriptorBindingState::Create(
        session,
        resourcePolicy);
    REQUIRE(stateResult);
    pbprotocol::DescriptorBindingState bindingState =
        std::move(stateResult).Value();
    const auto bindResult = bindingState.BindSegmentDescriptor(descriptor);
    REQUIRE(bindResult);
    REQUIRE(bindResult.Value() ==
        pbprotocol::DescriptorBindDisposition::Inserted);

    // Drain after binding: arrival order plus the DescriptorWaitTime metric.
    const auto drainResult = orphanCache.Drain(sessionTag, 0, 9);
    REQUIRE(drainResult);
    const pbprotocol::OrphanTransportBlockDrain& drain =
        drainResult.Value();
    REQUIRE(drain.entries.size() == blocks.size());
    for (std::size_t blockIndex = 0; blockIndex < drain.entries.size();
         blockIndex++)
    {
        REQUIRE(drain.entries[blockIndex].outerBlockId ==
            static_cast<std::uint32_t>(blockIndex));
        REQUIRE(drain.entries[blockIndex].declaredPayloadBytes ==
            blocks[blockIndex].payloadBytes);
        REQUIRE(drain.entries[blockIndex].paddedPayload ==
            blocks[blockIndex].paddedPayload);
    }
    REQUIRE(drain.waitObservations.has_value());
    REQUIRE(*drain.waitObservations == 4U);
    REQUIRE(orphanCache.GetCachedBlockCount() == 0U);
    REQUIRE(orphanCache.GetCachedBytes() == 0U);

    // Only now is a decoder created, and it consumes the drained bytes.
    auto decoderResult = pbouterfec::test::DecoderTestAccess::CreateDirectRepeatDecoder(
        descriptor,
        outerBlockBytes,
        resourceManager);
    REQUIRE(decoderResult);
    pbouterfec::DirectRepeatDecoder decoder =
        std::move(decoderResult).Value();
    REQUIRE(resourceManager.GetActiveDecoderCount() == 1U);

    for (std::size_t blockIndex = 0; blockIndex < drain.entries.size();
         blockIndex++)
    {
        const auto decodeResult = decoder.DecodeBlock(
            drain.entries[blockIndex].outerBlockId,
            drain.entries[blockIndex].declaredPayloadBytes,
            drain.entries[blockIndex].paddedPayload);
        REQUIRE(decodeResult);
        const pbouterfec::DecodeDisposition expectedDisposition =
            blockIndex + 1U < drain.entries.size()
                ? pbouterfec::DecodeDisposition::NeedMore
                : pbouterfec::DecodeDisposition::Ready;
        REQUIRE(decodeResult.Value() == expectedDisposition);
    }

    std::vector<std::byte> recovered(message.size());
    const auto recoverResult = decoder.Recover(recovered);
    REQUIRE(recoverResult);
    REQUIRE(recoverResult.Value() == message.size());
    REQUIRE(recovered == message);
    REQUIRE(resourceManager.GetQuotaExceededCount() == 0U);
}

TEST_CASE("Orphan quota overflow drops the incoming block without eviction",
          "[orphan][admission-flow][quota]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    const std::vector<std::byte> message = MakeMessage(20);
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    // Two full blocks fit exactly (16 bytes); the third must be dropped.
    resourcePolicy.maxOrphanTransportBytes = 16;
    const pbprotocol::SessionDescriptor session = MakeFlowSession(
        static_cast<std::uint64_t>(message.size()));

    const std::vector<EncodedBlock> blocks = EncodeAllBlocks(message, outerBlockBytes);
    REQUIRE(blocks.size() == 3U);

    const pbouterfec::OuterFecDecoderResourceManager resourceManager =
        MakeFlowManager(resourcePolicy);
    auto cacheResult = pbprotocol::OrphanTransportBlockCache::Create(
        resourcePolicy);
    REQUIRE(cacheResult);
    pbprotocol::OrphanTransportBlockCache orphanCache =
        std::move(cacheResult).Value();

    const pbprotocol::SessionTag sessionTag =
        pbprotocol::DeriveSessionTag(session.sessionId);
    REQUIRE(orphanCache.Admit(
        sessionTag,
        0,
        0,
        static_cast<std::uint16_t>(blocks[0].payloadBytes),
        blocks[0].paddedPayload,
        1));
    REQUIRE(orphanCache.Admit(
        sessionTag,
        0,
        1,
        static_cast<std::uint16_t>(blocks[1].payloadBytes),
        blocks[1].paddedPayload,
        2));

    const auto overflowResult = orphanCache.Admit(
        sessionTag,
        0,
        2,
        static_cast<std::uint16_t>(blocks[2].payloadBytes),
        blocks[2].paddedPayload,
        3);
    REQUIRE_FALSE(overflowResult);
    REQUIRE(overflowResult.Error().code ==
        pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);

    // The dropped block is the only loss: cached blocks are never evicted and
    // no decoder state exists at all.
    REQUIRE(orphanCache.GetDroppedBlockCount() == 1U);
    REQUIRE(orphanCache.GetCachedBlockCount() == 2U);
    REQUIRE(orphanCache.GetCachedBytes() == 16U);
    REQUIRE(resourceManager.GetActiveDecoderCount() == 0U);
    REQUIRE(resourceManager.GetReservedDecoderBytes() == 0U);

    const auto drainResult = orphanCache.Drain(sessionTag, 0, 4);
    REQUIRE(drainResult);
    REQUIRE(drainResult.Value().entries.size() == 2U);
}

} // namespace
