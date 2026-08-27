#include "pbcore/build_info.h"
#include "pbprotocol/protocol_version.h"

#include <iostream>

#ifdef _WIN32
int RunDataWindowCommand(int argumentCount, wchar_t* arguments[]);
int wmain(const int argumentCount, wchar_t* arguments[])
#else
int main()
#endif
{
#ifdef _WIN32
    if (argumentCount > 1)
    {
        return RunDataWindowCommand(argumentCount, arguments);
    }
#endif
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
