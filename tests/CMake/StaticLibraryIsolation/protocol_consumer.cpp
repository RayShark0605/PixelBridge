#include "pbprotocol/protocol_version.h"

int main()
{
    const pbprotocol::ProtocolVersion protocolVersion =
        pbprotocol::GetProtocolVersion();
    if (protocolVersion.major != 1 || protocolVersion.minor != 0)
    {
        return 1;
    }

    return 0;
}
