#include "pbprotocol/session_random.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pbprotocol::test {

class SessionRandomTestAccess
{
public:
    [[nodiscard]] static ProtocolResult<SessionId> GenerateWithRng(
        detail::SystemRngFunction rngFunction) noexcept
    {
        return detail::SystemCsprngSource::GenerateWithRng(rngFunction);
    }
};

} // namespace pbprotocol::test

namespace {

// Deterministic source: byte i becomes i + 1. Lets the test pin the exact
// 16-byte mapping (no reordering, no transformation).
[[nodiscard]] pbprotocol::ProtocolStatus DrawSequentialBytes(
    std::span<std::byte> buffer) noexcept
{
    for (std::size_t index = 0; index < buffer.size(); index++)
    {
        const std::uint8_t value = static_cast<std::uint8_t>(index + 1);
        buffer[index] = static_cast<std::byte>(value);
    }
    return pbprotocol::ProtocolStatus::Success();
}

// Simulated CSPRNG failure with a non-zero offset to verify propagation.
[[nodiscard]] pbprotocol::ProtocolStatus DrawFailingBytes(
    std::span<std::byte>) noexcept
{
    return pbprotocol::ProtocolStatus::Failure(
        pbprotocol::ProtocolErrorCode::CsprngFailure, 7);
}

} // namespace

TEST_CASE("GenerateRandomSessionId draws distinct ids from the OS CSPRNG",
          "[pbprotocol][session-random]")
{
    constexpr std::size_t kDrawCount = 32;
    std::vector<pbprotocol::SessionId> sessionIds;
    sessionIds.reserve(kDrawCount);

    for (std::size_t drawIndex = 0; drawIndex < kDrawCount; drawIndex++)
    {
        const auto result = pbprotocol::GenerateRandomSessionId();
        REQUIRE(result.HasValue());
        sessionIds.push_back(result.Value());
    }

    for (std::size_t leftIndex = 0; leftIndex < kDrawCount; leftIndex++)
    {
        for (std::size_t rightIndex = leftIndex + 1; rightIndex < kDrawCount;
             rightIndex++)
        {
            REQUIRE(sessionIds[leftIndex] != sessionIds[rightIndex]);
        }
    }
}

TEST_CASE("Injected deterministic RNG maps all 16 bytes without reordering",
          "[pbprotocol][session-random][seam]")
{
    const auto result = pbprotocol::test::SessionRandomTestAccess::GenerateWithRng(
        &DrawSequentialBytes);
    REQUIRE(result.HasValue());

    for (std::size_t index = 0; index < pbprotocol::kSessionIdBytes; index++)
    {
        const std::uint8_t expectedByte = static_cast<std::uint8_t>(index + 1);
        REQUIRE(
            std::to_integer<std::uint8_t>(result.Value().bytes[index]) ==
            expectedByte);
    }
}

TEST_CASE("Injected failing RNG propagates the CSPRNG error",
          "[pbprotocol][session-random][error-path]")
{
    const auto result = pbprotocol::test::SessionRandomTestAccess::GenerateWithRng(
        &DrawFailingBytes);
    REQUIRE_FALSE(result);
    REQUIRE(
        result.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::CsprngFailure, 7});
}

TEST_CASE("Null RNG source is an internal invariant violation",
          "[pbprotocol][session-random][error-path]")
{
    const auto result =
        pbprotocol::test::SessionRandomTestAccess::GenerateWithRng(nullptr);
    REQUIRE_FALSE(result);
    REQUIRE(
        result.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InternalInvariantViolation, 0});
}