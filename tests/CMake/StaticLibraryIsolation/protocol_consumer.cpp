#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/control_fragment_codec.h"
#include "pbprotocol/control_plane_receiver.h"
#include "pbprotocol/protocol_version.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

static_assert(std::is_nothrow_default_constructible_v<pbprotocol::Blake3Hasher>);
static_assert(!std::is_move_constructible_v<pbprotocol::Blake3Hasher>);

int main()
{
    const pbprotocol::ProtocolVersion protocolVersion =
        pbprotocol::GetProtocolVersion();
    if (protocolVersion.major != 1 || protocolVersion.minor != 0)
    {
        return 1;
    }

    const std::array<std::byte, 3> input{
        std::byte{0x00},
        std::byte{0x01},
        std::byte{0x02}};
    pbprotocol::Blake3Hasher hasher;
    hasher.Update(input);
    if (hasher.Finalize() != pbprotocol::ComputeBlake3Digest(input))
    {
        return 2;
    }

    const pbprotocol::BootstrapRecord bootstrapRecord{
        pbprotocol::kBootstrapVersion,
        protocolVersion,
        1,
        1,
        pbprotocol::SessionTag{2},
        3,
        4,
        0};
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bootstrapBytes{};
    if (!pbprotocol::SerializeBootstrapRecord(
            bootstrapRecord,
            bootstrapBytes))
    {
        return 3;
    }
    const auto parsedBootstrap = pbprotocol::ParseBootstrapRecord(
        bootstrapBytes);
    if (!parsedBootstrap || parsedBootstrap.Value() != bootstrapRecord)
    {
        return 4;
    }

    const pbprotocol::ControlRecordView controlRecord{
        pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor,
        5,
        bootstrapRecord.sessionTag,
        {}};
    std::array<std::byte, pbprotocol::kMinimumControlRecordBytes> controlBytes{};
    if (!pbprotocol::SerializeControlRecord(controlRecord, controlBytes))
    {
        return 5;
    }
    const auto parsedControl = pbprotocol::ParseControlRecord(controlBytes);
    if (!parsedControl || !parsedControl.Value().payload.empty())
    {
        return 6;
    }

    const auto fragmentCount = pbprotocol::GetControlFragmentCount(
        controlBytes,
        static_cast<std::uint16_t>(controlBytes.size()));
    if (!fragmentCount || fragmentCount.Value() != 1)
    {
        return 7;
    }
    const auto fragment = pbprotocol::GetControlFragment(
        1,
        controlBytes,
        0,
        static_cast<std::uint16_t>(controlBytes.size()));
    if (!fragment)
    {
        return 8;
    }
    std::array<std::byte, 54> fragmentBytes{};
    if (!pbprotocol::SerializeControlFragment(
            fragment.Value(),
            fragmentBytes) ||
        !pbprotocol::ParseControlFragment(fragmentBytes))
    {
        return 9;
    }

    auto receiverResult = pbprotocol::ControlPlaneReceiver::Create(
        pbprotocol::GetDefaultReceiverResourcePolicy());
    if (!receiverResult)
    {
        return 10;
    }
    auto receiver = std::move(receiverResult).Value();
    const auto admissionResult = receiver.ReceiveControlRecord(controlBytes);
    if (admissionResult ||
        admissionResult.Error().code !=
            pbprotocol::ProtocolErrorCode::TruncatedInput)
    {
        return 11;
    }

    return 0;
}
