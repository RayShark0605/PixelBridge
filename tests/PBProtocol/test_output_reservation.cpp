#include "descriptor_test_helpers.h"

#include "pbprotocol/output_reservation.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

using pbprotocol::OutputReservationDecision;

[[nodiscard]] pbprotocol::ProtocolResult<OutputReservationDecision>
Evaluate(const std::uint64_t originalFileSize,
         const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    return pbprotocol::EvaluateOutputReservation(
        originalFileSize,
        resourcePolicy);
}

TEST_CASE("OutputReservation enforces accepted-file and prompt boundaries",
          "[pbprotocol][output-reservation]")
{
    SECTION("sizes at or below the prompt threshold auto-accept")
    {
        pbprotocol::ReceiverResourcePolicy resourcePolicy =
            pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxOutputPreallocationBytesWithoutPrompt = 1024;

        const auto zeroResult = Evaluate(0, resourcePolicy);
        REQUIRE(zeroResult);
        REQUIRE(zeroResult.Value() == OutputReservationDecision::AutoAccept);

        const auto belowResult = Evaluate(1023, resourcePolicy);
        REQUIRE(belowResult);
        REQUIRE(belowResult.Value() == OutputReservationDecision::AutoAccept);

        // The threshold itself is still prompt-free; only strictly larger
        // sizes require confirmation.
        const auto exactPromptResult = Evaluate(1024, resourcePolicy);
        REQUIRE(exactPromptResult);
        REQUIRE(exactPromptResult.Value() ==
            OutputReservationDecision::AutoAccept);
    }

    SECTION("sizes above the prompt threshold require confirmation")
    {
        pbprotocol::ReceiverResourcePolicy resourcePolicy =
            pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxOutputPreallocationBytesWithoutPrompt = 1024;

        const auto aboveResult = Evaluate(1025, resourcePolicy);
        REQUIRE(aboveResult);
        REQUIRE(aboveResult.Value() ==
            OutputReservationDecision::RequiresUserConfirmation);
    }

    SECTION("sizes at the accepted-file cap are still admitted")
    {
        pbprotocol::ReceiverResourcePolicy resourcePolicy =
            pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxAcceptedFileBytes = 4096;
        resourcePolicy.maxOutputPreallocationBytesWithoutPrompt = 1024;

        const auto exactCapResult = Evaluate(4096, resourcePolicy);
        REQUIRE(exactCapResult);
        REQUIRE(exactCapResult.Value() ==
            OutputReservationDecision::RequiresUserConfirmation);
    }

    SECTION("sizes above the accepted-file cap are denied")
    {
        pbprotocol::ReceiverResourcePolicy resourcePolicy =
            pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxAcceptedFileBytes = 4096;
        resourcePolicy.maxOutputPreallocationBytesWithoutPrompt = 1024;

        const auto overCapResult = Evaluate(4097, resourcePolicy);
        REQUIRE_FALSE(overCapResult);
        REQUIRE(overCapResult.Error().code ==
            pbprotocol::ProtocolErrorCode::OutputReservationDenied);
    }

    SECTION("invalid policy fails closed before any decision")
    {
        const pbprotocol::ReceiverResourcePolicy zeroedPolicy{};
        const auto result = Evaluate(0, zeroedPolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidResourcePolicy);
    }
}

TEST_CASE("OutputReservation default policy keeps the 4 GiB prompt boundary",
          "[pbprotocol][output-reservation][default]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    REQUIRE(resourcePolicy.maxOutputPreallocationBytesWithoutPrompt <=
        resourcePolicy.maxAcceptedFileBytes);

    constexpr std::uint64_t kDefaultPromptThreshold =
        4ULL * 1024ULL * 1024ULL * 1024ULL;
    const auto belowResult = Evaluate(
        kDefaultPromptThreshold - 1,
        resourcePolicy);
    REQUIRE(belowResult);
    REQUIRE(belowResult.Value() == OutputReservationDecision::AutoAccept);

    const auto aboveResult = Evaluate(
        kDefaultPromptThreshold + 1,
        resourcePolicy);
    REQUIRE(aboveResult);
    REQUIRE(aboveResult.Value() ==
        OutputReservationDecision::RequiresUserConfirmation);
}

} // namespace
