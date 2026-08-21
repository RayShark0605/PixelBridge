#include "pbcore/build_info.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("Build info exposes the CMake-injected project version", "[pbcore][build-info]")
{
    const pbcore::BuildInfo buildInfo = pbcore::GetBuildInfo();

    REQUIRE_FALSE(buildInfo.productName.empty());
    REQUIRE(buildInfo.version == std::string(PB_CORE_VERSION_STRING));
}