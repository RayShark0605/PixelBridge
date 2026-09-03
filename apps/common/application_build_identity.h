#pragma once

#include "pbcore/build_info.h"
#include "pbprotocol/protocol_version.h"

#include <ostream>
#include <string_view>

namespace pbapp
{

inline void WriteApplicationBuildIdentity(std::ostream& stream,
                                          const std::string_view applicationName,
                                          const pbcore::BuildInfo& buildInfo,
                                          const pbprotocol::ProtocolVersion protocolVersion,
                                          const std::string_view gitCommit)
{
    stream << "{\"schema\":\"PixelBridge.ApplicationBuildIdentity.1\",\"applicationName\":\""
           << applicationName << "\",\"applicationVersion\":\"" << buildInfo.version
           << "\",\"protocolMajor\":" << protocolVersion.major << ",\"protocolMinor\":" << protocolVersion.minor
           << ",\"gitCommit\":\"" << gitCommit << "\"}" << std::endl;
}

} // namespace pbapp
