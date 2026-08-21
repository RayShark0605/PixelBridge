#include "pbprotocol/protocol_version.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Protocol version matches the skeleton baseline", "[pbprotocol][version]")
{
    const pbprotocol::ProtocolVersion protocolVersion = pbprotocol::GetProtocolVersion();

    // Skeleton baseline: major 1 (PB-Bootstrap-1 / PB-Control-1), minor 0.
    REQUIRE(protocolVersion.major == 1);
    REQUIRE(protocolVersion.minor == 0);
}