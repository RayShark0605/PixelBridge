#include "pbprotocol/checked_integer.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

using pbprotocol::CheckedAddUint64WithinLimit;
using pbprotocol::CheckedAddUnsigned;
using pbprotocol::ProtocolError;
using pbprotocol::CheckedMultiplyUnsigned;
using pbprotocol::CheckedNarrowUnsigned;
using pbprotocol::ProtocolErrorCode;
using pbprotocol::ProtocolResult;
using pbprotocol::SaturatingAddUnsigned;
using pbprotocol::SaturatingIncrementUnsigned;

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

} // namespace

TEST_CASE("CheckedAddUnsigned handles the max boundary exactly",
          "[pbprotocol][checked-integer][add]")
{
    const auto atMax = CheckedAddUnsigned<std::uint64_t>(kMax, 0);
    REQUIRE(atMax.HasValue());
    REQUIRE(atMax.Value() == kMax);

    const auto beyondMax = CheckedAddUnsigned<std::uint64_t>(kMax, 1);
    REQUIRE_FALSE(beyondMax);
    REQUIRE(
        beyondMax.Error() ==
        ProtocolError{ProtocolErrorCode::LengthOverflow, 0});
}

TEST_CASE("CheckedMultiplyUnsigned handles zero and max boundaries",
          "[pbprotocol][checked-integer][mul]")
{
    const auto zeroTimesMax = CheckedMultiplyUnsigned<std::uint64_t>(0, kMax);
    REQUIRE(zeroTimesMax.HasValue());
    REQUIRE(zeroTimesMax.Value() == 0);

    const auto maxTimesMax = CheckedMultiplyUnsigned<std::uint64_t>(kMax, kMax);
    REQUIRE_FALSE(maxTimesMax);
    REQUIRE(maxTimesMax.Error().code == ProtocolErrorCode::LengthOverflow);

    // Exact fit: 2^16 * (2^16 - 1) = 2^32 - 2^16 = 0xFFFF0000 fits in uint32.
    const auto exactFit = CheckedMultiplyUnsigned<std::uint32_t>(
        65536u, 65535u);
    REQUIRE(exactFit.HasValue());
    REQUIRE(exactFit.Value() == 0xFFFF0000u);

    // One step past the exact fit overflows uint32.
    const auto overflow = CheckedMultiplyUnsigned<std::uint32_t>(
        65536u, 65536u);
    REQUIRE_FALSE(overflow);
    REQUIRE(overflow.Error().code == ProtocolErrorCode::LengthOverflow);
}

TEST_CASE("CheckedNarrowUnsigned accepts exact fit and rejects one above",
          "[pbprotocol][checked-integer][narrow]")
{
    const auto exactFit = CheckedNarrowUnsigned<std::uint32_t>(0xFFFFFFFFULL, 9);
    REQUIRE(exactFit.HasValue());
    REQUIRE(exactFit.Value() == 0xFFFFFFFFu);

    const auto oneAbove = CheckedNarrowUnsigned<std::uint32_t>(0x100000000ULL, 11);
    REQUIRE_FALSE(oneAbove);
    REQUIRE(
        oneAbove.Error() ==
        ProtocolError{ProtocolErrorCode::LengthNarrowing, 11});
}

TEST_CASE("CheckedNarrowUnsigned covers narrow same-width and widening paths",
          "[pbprotocol][checked-integer][narrow]")
{
    const auto uint8ExactFit = CheckedNarrowUnsigned<std::uint8_t>(255U, 3);
    REQUIRE(uint8ExactFit);
    REQUIRE(uint8ExactFit.Value() == 255U);

    const auto uint8Overflow = CheckedNarrowUnsigned<std::uint8_t>(256U, 4);
    REQUIRE_FALSE(uint8Overflow);
    REQUIRE(
        uint8Overflow.Error() ==
        ProtocolError{ProtocolErrorCode::LengthNarrowing, 4});

    const auto sameWidth = CheckedNarrowUnsigned<std::uint64_t>(kMax, 5);
    REQUIRE(sameWidth);
    REQUIRE(sameWidth.Value() == kMax);

    const auto widening = CheckedNarrowUnsigned<std::uint64_t>(
        std::numeric_limits<std::uint32_t>::max(),
        6);
    REQUIRE(widening);
    REQUIRE(
        widening.Value() ==
        std::numeric_limits<std::uint32_t>::max());
}

TEST_CASE("Checked add and multiply preserve narrow-type boundaries and offsets",
          "[pbprotocol][checked-integer][add][mul]")
{
    const auto uint8AtMax = CheckedAddUnsigned<std::uint8_t>(254U, 1U, 8);
    REQUIRE(uint8AtMax);
    REQUIRE(uint8AtMax.Value() == 255U);

    const auto uint8Overflow = CheckedAddUnsigned<std::uint8_t>(255U, 1U, 9);
    REQUIRE_FALSE(uint8Overflow);
    REQUIRE(
        uint8Overflow.Error() ==
        ProtocolError{ProtocolErrorCode::LengthOverflow, 9});

    const auto zeroProduct = CheckedMultiplyUnsigned<std::uint32_t>(
        0U,
        std::numeric_limits<std::uint32_t>::max(),
        10);
    REQUIRE(zeroProduct);
    REQUIRE(zeroProduct.Value() == 0U);

    const auto multiplicationOverflow =
        CheckedMultiplyUnsigned<std::uint32_t>(65536U, 65536U, 11);
    REQUIRE_FALSE(multiplicationOverflow);
    REQUIRE(
        multiplicationOverflow.Error() ==
        ProtocolError{ProtocolErrorCode::LengthOverflow, 11});
}

TEST_CASE("CheckedAddWithinLimit enforces overflow before the limit",
          "[pbprotocol][checked-integer][range]")
{
    // sum == limit passes with the exact bound.
    const auto atLimit = CheckedAddUint64WithinLimit(5, 3, 8);
    REQUIRE(atLimit.HasValue());
    REQUIRE(atLimit.Value() == 8);

    // sum == limit + 1 is a limit violation, not an overflow.
    const auto overLimit = CheckedAddUint64WithinLimit(5, 4, 8);
    REQUIRE_FALSE(overLimit);
    REQUIRE(
        overLimit.Error() ==
        ProtocolError{ProtocolErrorCode::LengthLimitExceeded, 0});

    // Overflow must win even though the wrapped sum (0) would be <= limit.
    const auto overflowWins = CheckedAddUint64WithinLimit(kMax, 1, 0);
    REQUIRE_FALSE(overflowWins);
    REQUIRE(overflowWins.Error().code == ProtocolErrorCode::LengthOverflow);

    // errorOffset is passed through on both failure classes.
    const auto overLimitWithOffset = CheckedAddUint64WithinLimit(5, 4, 8, 42);
    REQUIRE_FALSE(overLimitWithOffset);
    REQUIRE(
        overLimitWithOffset.Error() ==
        ProtocolError{ProtocolErrorCode::LengthLimitExceeded, 42});

    const auto overflowWithOffset = CheckedAddUint64WithinLimit(kMax, 1, 0, 77);
    REQUIRE_FALSE(overflowWithOffset);
    REQUIRE(
        overflowWithOffset.Error() ==
        ProtocolError{ProtocolErrorCode::LengthOverflow, 77});
}

TEST_CASE("CheckedAddWithinLimit works for narrower unsigned types",
          "[pbprotocol][checked-integer][range]")
{
    // sum == limit passes with the exact bound in uint32.
    const auto atLimit = pbprotocol::CheckedAddWithinLimit<std::uint32_t>(
        0x100u, 0xFFu, 0x1FFu);
    REQUIRE(atLimit.HasValue());
    REQUIRE(atLimit.Value() == 0x1FFu);

    // sum == limit + 1 is a limit violation in uint32.
    const auto overLimit = pbprotocol::CheckedAddWithinLimit<std::uint32_t>(
        0x100u, 0x100u, 0x1FFu);
    REQUIRE_FALSE(overLimit);
    REQUIRE(overLimit.Error().code == ProtocolErrorCode::LengthLimitExceeded);

    // Wrapping uint32 sum must report overflow even against a huge limit.
    const auto overflowWins = pbprotocol::CheckedAddWithinLimit<std::uint32_t>(
        0xFFFFFFFFu, 1u, 0xFFFFFFFFu);
    REQUIRE_FALSE(overflowWins);
    REQUIRE(overflowWins.Error().code == ProtocolErrorCode::LengthOverflow);
}

TEST_CASE("Telemetry arithmetic saturates instead of wrapping",
          "[pbprotocol][checked-integer][telemetry]")
{
    std::uint64_t counter = kMax - 1ULL;
    SaturatingIncrementUnsigned(counter);
    REQUIRE(counter == kMax);
    SaturatingIncrementUnsigned(counter);
    REQUIRE(counter == kMax);

    REQUIRE(SaturatingAddUnsigned<std::uint64_t>(kMax - 4ULL, 4ULL) ==
        kMax);
    REQUIRE(SaturatingAddUnsigned<std::uint64_t>(kMax - 4ULL, 5ULL) ==
        kMax);
    REQUIRE(SaturatingAddUnsigned<std::uint32_t>(10U, 20U) == 30U);
}
