#include "pbprotocol/protocol_types.h"

#include "pbprotocol/blake3_digest.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace pbprotocol {

namespace {

constexpr std::string_view kSessionTagDomain = "PixelBridge SessionTag v1";

} // namespace

ReceiverResourcePolicy GetDefaultReceiverResourcePolicy() noexcept
{
    constexpr std::uint64_t kibibyte = 1024ULL;
    constexpr std::uint64_t mebibyte = 1024ULL * kibibyte;
    constexpr std::uint64_t gibibyte = 1024ULL * mebibyte;

    ReceiverResourcePolicy resourcePolicy{};
    resourcePolicy.maxAcceptedFileBytes = 500ULL * gibibyte;
    resourcePolicy.maxSegmentCount = 65536ULL;
    resourcePolicy.maxRawSegmentBytes = 16ULL * mebibyte;
    resourcePolicy.maxEncodedSegmentBytes = 32ULL * mebibyte;
    resourcePolicy.maxOuterBlockBytes = kMaximumTransportPayloadBytes;
    resourcePolicy.maxDescriptorStateBytes = 64ULL * mebibyte;
    resourcePolicy.maxConcurrentSessions = 4ULL;
    resourcePolicy.maxTotalDescriptorStateBytes = 256ULL * mebibyte;
    // DirectRepeat is a Tiny/Small path. The sender's default efficiency gate
    // is narrower, while this receiver-local ceiling leaves room for stricter
    // profile-specific tuning without admitting millions of one-byte blocks.
    resourcePolicy.maxDirectRepeatBlockCount = 64ULL;
    resourcePolicy.maxActiveOuterFecDecoders = 4ULL;
    resourcePolicy.maxOuterFecDecoderBytes = 512ULL * mebibyte;
    resourcePolicy.maxTotalOuterFecDecoderBytes = 1024ULL * mebibyte;
    resourcePolicy.maxControlRecordBytes = 64U * 1024U;
    resourcePolicy.maxConcurrentControlReassemblies = 8ULL;
    resourcePolicy.maxControlReassemblyBytes = 1ULL * mebibyte;
    resourcePolicy.maxControlFragmentsPerRecord = 4096ULL;
    resourcePolicy.maxControlReassemblyInactivityObservations = 16384ULL;
    resourcePolicy.maxOrphanTransportBytes = 4ULL * mebibyte;
    resourcePolicy.maxOrphanTransportBlocks = 64ULL;
    // 8 MiB == 2^23, bit-identical to the historical fixed window log of 23
    // used by MakeDecompressionLimits.
    resourcePolicy.maxZstdWindowBytes = 8ULL * mebibyte;
    resourcePolicy.maxResumeBytes = 256ULL * mebibyte;
    resourcePolicy.maxOutputPreallocationBytesWithoutPrompt = 4ULL * gibibyte;
    return resourcePolicy;
}

SessionTag DeriveSessionTag(const SessionId& sessionId) noexcept
{
    const auto domainBytes = std::as_bytes(std::span(kSessionTagDomain));
    Blake3Hasher hasher;
    hasher.Update(domainBytes);
    hasher.Update(sessionId.bytes);
    const std::array<std::byte, kDigestBytes> digest = hasher.Finalize();

    std::uint64_t tagValue = 0;
    for (std::size_t byteIndex = 0; byteIndex < sizeof(tagValue); byteIndex++)
    {
        tagValue |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(digest[byteIndex]))
            << static_cast<unsigned int>(byteIndex * 8U);
    }

    return SessionTag{tagValue};
}

WholeFileDigest GetEmptyBlake3WholeFileDigest() noexcept
{
    return WholeFileDigest{ComputeBlake3Digest({})};
}

} // namespace pbprotocol
