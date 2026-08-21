#include "pbcore/build_info.h"

namespace pbcore {

namespace {

// Injected by CMake from the top-level project() version.
const char* const productName = "PixelBridge";
const char* const version = PB_CORE_VERSION_STRING;

} // namespace

BuildInfo GetBuildInfo()
{
    const BuildInfo buildInfo{productName, version};
    return buildInfo;
}

} // namespace pbcore