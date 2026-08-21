#include <pbcore/build_info.h>
#include <pbprotocol/protocol_version.h>

#include <iostream>

int main()
{
    const pbcore::BuildInfo buildInfo = pbcore::GetBuildInfo();
    const pbprotocol::ProtocolVersion protocolVersion = pbprotocol::GetProtocolVersion();

    // Deterministic banner: also proves that both core libraries are linked.
    std::cout << "PixelBridgeEncoder "
              << buildInfo.version
              << " (protocol "
              << protocolVersion.major << "." << protocolVersion.minor
              << ")"
              << std::endl;
    return 0;
}