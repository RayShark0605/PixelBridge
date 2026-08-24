#include "pbreceiver/receiver_ingress.h"

#include "pbouterfec/direct_repeat.h"
#include "pbouterfec/outer_fec_decoder_resource.h"
#include "pbouterfec/wirehair_v2.h"
#include "pbprotocol/protocol_types.h"

#include <cstdint>

template <typename DescriptorType>
concept CanCreateDirectRepeatDecoder = requires(
    const DescriptorType& descriptor,
    pbouterfec::OuterFecDecoderResourceManager& resourceManager)
{
    pbouterfec::DirectRepeatDecoder::Create(
        descriptor,
        std::uint32_t{256},
        resourceManager);
};

template <typename DescriptorType>
concept CanCreateWirehairDecoder = requires(
    const DescriptorType& descriptor,
    pbouterfec::OuterFecDecoderResourceManager& resourceManager)
{
    pbouterfec::WirehairV2Decoder::Create(
        descriptor,
        std::uint32_t{256},
        resourceManager);
};

static_assert(CanCreateDirectRepeatDecoder<
    pbprotocol::BoundSegmentDescriptor>);
static_assert(CanCreateWirehairDecoder<pbprotocol::BoundSegmentDescriptor>);
static_assert(!CanCreateDirectRepeatDecoder<pbprotocol::SegmentDescriptor>);
static_assert(!CanCreateWirehairDecoder<pbprotocol::SegmentDescriptor>);

int main()
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    auto receiverResult = pbreceiver::ReceiverIngress::Create(
        resourcePolicy,
        256);
    if (!receiverResult)
    {
        return 1;
    }

    const pbreceiver::ReceiverResourceTelemetrySnapshot telemetry =
        receiverResult.Value().GetTelemetry();
    return telemetry.activeOuterFecDecoderCount == 0 ? 0 : 2;
}
