#include "pbprotocol/feature_flags.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

TEST_CASE("Feature flag validation separates known and skippable optional flags",
          "[pbprotocol][features]")
{
    const pbprotocol::FeatureFlags<std::uint64_t> flags{
        0x0001ULL,
        0x000AULL};
    const auto validation = pbprotocol::ValidateFeatureFlags(
        flags,
        std::uint64_t{0x0003ULL},
        24);

    REQUIRE(validation);
    REQUIRE(validation.Value().knownMandatory == 0x0001ULL);
    REQUIRE(validation.Value().knownOptional == 0x0002ULL);
    REQUIRE(validation.Value().unknownOptional == 0x0008ULL);
}

TEST_CASE("Unknown mandatory feature fails closed", "[pbprotocol][features]")
{
    const pbprotocol::FeatureFlags<std::uint32_t> flags{
        0x00000008U,
        0x00000000U};
    const auto validation = pbprotocol::ValidateFeatureFlags(
        flags,
        std::uint32_t{0x00000003U},
        9);

    REQUIRE_FALSE(validation);
    REQUIRE(
        validation.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::UnknownMandatoryFeature,
            9});
}

TEST_CASE("Feature classification overlap is rejected", "[pbprotocol][features]")
{
    const pbprotocol::FeatureFlags<std::uint16_t> flags{
        0x0002U,
        0x0002U};
    const auto validation = pbprotocol::ValidateFeatureFlags(
        flags,
        std::uint16_t{0x0003U},
        31);

    REQUIRE_FALSE(validation);
    REQUIRE(
        validation.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::ConflictingFeatureFlags,
            31});
}
