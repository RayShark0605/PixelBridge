#pragma once

#include "pbouterfec/direct_repeat.h"
#include "pbouterfec/wirehair_v2.h"
#include "outer_fec_decoder_resource_internal.h"

#include <cstdint>

namespace pbouterfec::test
{

// Non-installed seam for unit tests, fuzzers, and benchmarks that exercise
// descriptor-validation and quota failures directly. Production network input
// must use the BoundSegmentDescriptor factories exposed by the public headers.
class DecoderTestAccess
{
public:
    [[nodiscard]] static OuterFecResult<DirectRepeatDecoder>
    CreateDirectRepeatDecoder(
        const pbprotocol::SegmentDescriptor& segmentDescriptor,
        const std::uint32_t expectedOuterBlockBytes,
        const OuterFecDecoderResourceManager& resourceManager)
    {
        return DirectRepeatDecoder::CreateFromDescriptor(
            segmentDescriptor,
            expectedOuterBlockBytes,
            resourceManager);
    }

    [[nodiscard]] static OuterFecResult<WirehairV2Decoder>
    CreateWirehairV2Decoder(
        const pbprotocol::SegmentDescriptor& segmentDescriptor,
        const OuterFecDecoderResourceManager& resourceManager)
    {
        return WirehairV2Decoder::CreateFromDescriptor(
            segmentDescriptor,
            resourceManager);
    }

    static void SetQuotaExceededCount(
        OuterFecDecoderResourceManager& resourceManager,
        const std::uint64_t quotaExceededCount) noexcept
    {
        if (resourceManager.state_)
        {
            resourceManager.state_->quotaExceededCount.store(
                quotaExceededCount,
                std::memory_order_relaxed);
        }
    }
};

} // namespace pbouterfec::test
