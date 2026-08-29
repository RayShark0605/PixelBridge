#include "pbcapturenormalize/capture_types.h"

#include <catch2/catch_test_macros.hpp>
#include <intrin.h>
#include <array>
#include <limits>

using namespace pbcapturenormalize;

TEST_CASE("Capture QPC conversion is integer exact and failure is immutable")
{
    struct Example
    {
        std::int64_t ticks;
        std::int64_t frequency;
        std::int64_t expected;
    };
    constexpr std::array examples{
        Example{0, 1, 0}, Example{1, 3, 3333333}, Example{2, 3, 6666666}, Example{3, 3, 10000000},
        Example{10000001, 10000000, 10000001}, Example{9223372036854775807, 9223372036854775807, 10000000},
        Example{9223372036854775806, 9223372036854775807, 9999999},
        Example{9223372036854775807, 10000000, 9223372036854775807}};
    for (const auto& example : examples)
    {
        std::int64_t output = -7;
        REQUIRE(ConvertQpcTo100ns(example.ticks, example.frequency, output));
        REQUIRE(output == example.expected);
    }
    for (const auto& example : std::array{Example{-1, 1, 0}, Example{1, 0, 0}, Example{1, -1, 0},
                                        Example{9223372036854775807, 1, 0}, Example{9223372036854775807, 9999999, 0}})
    {
        std::int64_t output = 73;
        REQUIRE_FALSE(ConvertQpcTo100ns(example.ticks, example.frequency, output));
        REQUIRE(output == 73);
    }
}

TEST_CASE("Capture QPC conversion agrees with independent 128 bit arithmetic")
{
    std::uint64_t seed = 0x96d157643531cdea;
    const auto Next = [&seed]()
    {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed & static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    };
    for (std::size_t index = 0; index < 4096; index++)
    {
        const auto ticks = Next();
        const auto frequency = index % 2 == 0 ? Next() | 1u : (Next() % 10000000) + 1;
        std::uint64_t high = 0;
        const auto low = _umul128(ticks, 10000000, &high);
        std::uint64_t remainder = 0;
        const auto expected = high < frequency ? _udiv128(high, low, frequency, &remainder) : std::numeric_limits<std::uint64_t>::max();
        const bool valid = expected <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        std::int64_t output = -73;
        REQUIRE(ConvertQpcTo100ns(static_cast<std::int64_t>(ticks), static_cast<std::int64_t>(frequency), output) == valid);
        REQUIRE(output == (valid ? static_cast<std::int64_t>(expected) : -73));
    }
}

TEST_CASE("Effective capture time takes the earliest valid source and fails closed when none exists")
{
    struct Example
    {
        std::int64_t claimed100ns;
        std::int64_t arrivalQpc100ns;
        std::int64_t expected;
    };
    constexpr std::int64_t unmeasured = -1;
    constexpr std::array examples{
        // No usable time source at all: the caller must fail closed.
        Example{unmeasured, unmeasured, unmeasured},
        Example{-5, unmeasured, unmeasured},
        Example{unmeasured, -7, unmeasured},
        // A valid zero is a real time, not "unmeasured".
        Example{0, unmeasured, 0},
        Example{unmeasured, 0, 0},
        // Normal: a claim at or before the arrival is the capture instant.
        Example{100, 200, 100},
        Example{200, 200, 200},
        // An ahead-of-time claim (WGC SystemRelativeTime) is superseded by the arrival.
        Example{500, 200, 200},
        Example{9223372036854775807, 1, 1},
        Example{9223372036854775807, unmeasured, 9223372036854775807},
    };
    for (const auto& example : examples)
    {
        CAPTURE(example.claimed100ns, example.arrivalQpc100ns);
        REQUIRE(ResolveEffectiveCaptureTime100ns(example.claimed100ns, example.arrivalQpc100ns) == example.expected);
    }
}
