#include "pbprotocol/protocol_result.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

TEST_CASE("Segment completion diagnostic is append only", "[pbprotocol][result][phase0]")
{
    REQUIRE(static_cast<std::uint8_t>(pbprotocol::ProtocolErrorCode::ResumeStateIoFailure) == 51);
    REQUIRE(static_cast<std::uint8_t>(pbprotocol::ProtocolErrorCode::SegmentRecoveryIncomplete) == 52);
}

TEST_CASE("Protocol status cannot represent a successful failure",
          "[pbprotocol][result][invariant]")
{
    const pbprotocol::ProtocolStatus success = pbprotocol::ProtocolStatus::Success();
    REQUIRE(success);
    REQUIRE(success.Error().code == pbprotocol::ProtocolErrorCode::None);

    const pbprotocol::ProtocolStatus invalidFailure =
        pbprotocol::ProtocolStatus::Failure(pbprotocol::ProtocolErrorCode::None, 37);
    REQUIRE_FALSE(invalidFailure);
    REQUIRE(
        invalidFailure.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
            37});

    const pbprotocol::ProtocolStatus ordinaryFailure =
        pbprotocol::ProtocolStatus::Failure(
            pbprotocol::ProtocolErrorCode::TruncatedInput,
            11);
    REQUIRE_FALSE(ordinaryFailure);
    REQUIRE(
        ordinaryFailure.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::TruncatedInput,
            11});
}

TEST_CASE("Protocol result failures always carry a non-success error",
          "[pbprotocol][result][invariant]")
{
    const auto success = pbprotocol::ProtocolResult<int>::Success(23);
    REQUIRE(success);
    REQUIRE(success.Value() == 23);
    REQUIRE(success.Error().code == pbprotocol::ProtocolErrorCode::None);

    const auto invalidFailure = pbprotocol::ProtocolResult<int>::Failure(
        pbprotocol::ProtocolErrorCode::None,
        41);
    REQUIRE_FALSE(invalidFailure);
    REQUIRE(
        invalidFailure.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InternalInvariantViolation,
            41});

    const auto ordinaryFailure = pbprotocol::ProtocolResult<int>::Failure(
        pbprotocol::ProtocolErrorCode::LengthOverflow,
        17);
    REQUIRE_FALSE(ordinaryFailure);
    REQUIRE(
        ordinaryFailure.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::LengthOverflow,
            17});
    REQUIRE_THROWS_AS(
        ordinaryFailure.Value(),
        std::bad_optional_access);
}
