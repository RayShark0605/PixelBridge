#include "pbprotocol/protocol_version.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Protocol version matches the v1 baseline", "[pbprotocol][version]")
{
    const pbprotocol::ProtocolVersion protocolVersion = pbprotocol::GetProtocolVersion();

    // Protocol baseline: major 1 (PB-Bootstrap-1 / PB-Control-1), minor 0.
    REQUIRE(protocolVersion.major == 1);
    REQUIRE(protocolVersion.minor == 0);
}

TEST_CASE("Protocol Major mismatch fails closed", "[pbprotocol][version]")
{
    const auto classification = pbprotocol::ClassifyProtocolVersion(
        pbprotocol::ProtocolVersion{2, 0},
        17);

    REQUIRE_FALSE(classification);
    REQUIRE(
        classification.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::UnsupportedProtocolMajor,
            17});
}

TEST_CASE("Protocol Minor classification defers forward compatibility",
          "[pbprotocol][version]")
{
    const auto currentClassification = pbprotocol::ClassifyProtocolVersion(
        pbprotocol::ProtocolVersion{1, 0});
    REQUIRE(currentClassification);
    REQUIRE(
        currentClassification.Value() ==
        pbprotocol::ProtocolVersionClassification::CurrentOrOlderMinor);

    const auto newerClassification = pbprotocol::ClassifyProtocolVersion(
        pbprotocol::ProtocolVersion{1, 1});
    REQUIRE(newerClassification);
    REQUIRE(
        newerClassification.Value() ==
        pbprotocol::ProtocolVersionClassification::NewerMinorRequiresOptionalValidation);

    const auto olderClassification = pbprotocol::ClassifyProtocolVersionAgainst(
        pbprotocol::ProtocolVersion{7, 3},
        pbprotocol::ProtocolVersion{7, 4});
    REQUIRE(olderClassification);
    REQUIRE(
        olderClassification.Value() ==
        pbprotocol::ProtocolVersionClassification::CurrentOrOlderMinor);
}
