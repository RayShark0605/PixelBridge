#include "pbcore/build_info.h"
#include "pbprotocol/protocol_version.h"

#include <iostream>
#include <string_view>

#ifdef _WIN32
int RunScreenRegionCommand(int argumentCount, wchar_t* arguments[]);
int RunCaptureBootstrapCommand(int argumentCount, wchar_t* arguments[]);
int wmain(const int argumentCount, wchar_t* arguments[])
#else
int main()
#endif
{
#ifdef _WIN32
    if (argumentCount > 1)
    {
        if (std::wstring_view(arguments[1]) == L"--capture-bootstrap" || std::wstring_view(arguments[1]) == L"--capture-desktop-levels")
        {
            return RunCaptureBootstrapCommand(argumentCount, arguments);
        }
        return RunScreenRegionCommand(argumentCount, arguments);
    }
#endif
    const pbcore::BuildInfo buildInfo = pbcore::GetBuildInfo();
    const pbprotocol::ProtocolVersion protocolVersion = pbprotocol::GetProtocolVersion();

    // Deterministic banner: also proves that both core libraries are linked.
    std::cout << "PixelBridgeDecoder "
              << buildInfo.version
              << " (protocol "
              << protocolVersion.major << "." << protocolVersion.minor
              << ")"
              << std::endl;
    return 0;
}
