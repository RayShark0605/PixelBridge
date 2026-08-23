#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/protocol_version.h"

#include <array>
#include <cstddef>
#include <type_traits>

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

    return 0;
}
