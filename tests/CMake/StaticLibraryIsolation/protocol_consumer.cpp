#include "pbprotocol/blake3_digest.h"
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

    return 0;
}
