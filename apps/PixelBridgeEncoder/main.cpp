#include "pbcore/build_info.h"
#include "pbprotocol/protocol_version.h"

#include <iostream>
#include <string_view>

#ifdef _WIN32
int RunDataWindowCommand(int argumentCount, wchar_t* arguments[]);
#ifdef PB_ENABLE_APPLICATION_RUNTIME
int RunEncoderRuntimeCommand(int argumentCount, const wchar_t* const arguments[]);
#endif
#ifdef PB_ENABLE_QT_GUI
int RunEncoderGui(int argumentCount, wchar_t* arguments[]);
#endif
int wmain(const int argumentCount, wchar_t* arguments[])
#else
int main()
#endif
{
#ifdef _WIN32
    if (argumentCount > 1)
    {
        if (std::wstring_view(arguments[1]) == L"--version" && argumentCount == 2)
        {
            const pbcore::BuildInfo buildInfo = pbcore::GetBuildInfo();
            const pbprotocol::ProtocolVersion protocolVersion = pbprotocol::GetProtocolVersion();
            std::cout << "PixelBridgeEncoder " << buildInfo.version << " (protocol "
                      << protocolVersion.major << "." << protocolVersion.minor << ")" << std::endl;
            return 0;
        }
#ifdef PB_ENABLE_QT_GUI
        if ((std::wstring_view(arguments[1]) == L"--gui-smoke" && argumentCount == 2) ||
            (std::wstring_view(arguments[1]) == L"--gui-integration-smoke" && argumentCount == 3))
        {
            return RunEncoderGui(argumentCount, arguments);
        }
#endif
#ifdef PB_ENABLE_APPLICATION_RUNTIME
        if (std::wstring_view(arguments[1]) == L"--headless-broadcast")
        {
            return RunEncoderRuntimeCommand(argumentCount, arguments);
        }
#endif
        return RunDataWindowCommand(argumentCount, arguments);
    }
#ifdef PB_ENABLE_QT_GUI
    return RunEncoderGui(argumentCount, arguments);
#endif
#endif
#if !defined(_WIN32) || !defined(PB_ENABLE_QT_GUI)
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
#endif
}
