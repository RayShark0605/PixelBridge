#pragma once

#include <string>

namespace pbcore {

// Implementation metadata of the current build.
// Intentionally not part of the wire protocol: none of these values is ever
// serialized into PixelBridge transport data.
struct BuildInfo
{
    std::string productName;
    std::string version;
};

// Returns the build metadata for the current PixelBridge build.
BuildInfo GetBuildInfo();

} // namespace pbcore