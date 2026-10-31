#include "sender_rate_prior.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

TEST_CASE("Rate prior rejects reversed zero and unbounded rates without changing defaults", "[rate-prior]")
{
    REQUIRE(pbapp::ValidateSenderRatePrior({200, 280}));
    REQUIRE(pbapp::ValidateSenderRatePrior({1, 1}));
    REQUIRE(pbapp::ValidateSenderRatePrior({1048576, 1048576}));
    REQUIRE_FALSE(pbapp::ValidateSenderRatePrior({0, 280}));
    REQUIRE_FALSE(pbapp::ValidateSenderRatePrior({281, 280}));
    REQUIRE_FALSE(pbapp::ValidateSenderRatePrior({1, 1048577}));
}

TEST_CASE("Rate prior maps raw bytes not encoded work and gives older visits less service", "[rate-prior]")
{
    // t=100 s: [20000, 28000] KiB, independent of the Segment's K.
    const auto old = pbapp::CalculateSenderRatePriorDecision({200, 280}, 100000000000ULL,
        0, 1024000, 102400000, 1600, true, 0);
    REQUIRE(old);
    REQUIRE(old->lowerRawBytes == 20480000);
    REQUIRE(old->upperRawBytes == 28672000);
    REQUIRE(old->region == pbapp::SenderRatePriorRegion::BeforeFrontier);
    REQUIRE(old->visitEquationBudget == 2000);
    const auto near = pbapp::CalculateSenderRatePriorDecision({200, 280}, 100000000000ULL,
        20480000, 1024000, 102400000, 1600, true, 0);
    REQUIRE(near);
    REQUIRE(near->region == pbapp::SenderRatePriorRegion::NearFrontier);
    REQUIRE(near->visitEquationBudget == 2800);
    const auto future = pbapp::CalculateSenderRatePriorDecision({200, 280}, 100000000000ULL,
        30720000, 1024000, 102400000, 1600, true, 0);
    REQUIRE(future);
    REQUIRE(future->region == pbapp::SenderRatePriorRegion::AheadOfFrontier);
    REQUIRE(future->visitEquationBudget == 3200);
    const auto compressed = pbapp::CalculateSenderRatePriorDecision({200, 280}, 100000000000ULL,
        20480000, 1024000, 102400000, 400, false, 1);
    REQUIRE(compressed);
    REQUIRE(compressed->lowerRawBytes == near->lowerRawBytes);
    REQUIRE(compressed->visitEquationBudget == 200);
}

TEST_CASE("Old repairs remain nonzero and deterministic safety sweeps cannot starve early gaps", "[rate-prior]")
{
    const auto old = pbapp::CalculateSenderRatePriorDecision({200, 280}, 100000000000ULL,
        0, 1024000, 102400000, 1600, false, 1);
    REQUIRE(old);
    REQUIRE(old->visitEquationBudget == 100);
    const auto small = pbapp::CalculateSenderRatePriorDecision({200, 280}, 100000000000ULL,
        0, 1024000, 102400000, 1, false, 2);
    REQUIRE(small);
    REQUIRE(small->visitEquationBudget == 16);
    for (const std::uint64_t pass : {3ULL, 7ULL, 11ULL})
    {
        const auto safety = pbapp::CalculateSenderRatePriorDecision({200, 280}, 100000000000ULL,
            0, 1024000, 102400000, 1600, false, pass);
        REQUIRE(safety);
        REQUIRE(safety->region == pbapp::SenderRatePriorRegion::SafetyRepair);
        REQUIRE(safety->visitEquationBudget == 2000);
    }
    const auto beyondEof = pbapp::CalculateSenderRatePriorDecision({200, 280}, 1000000000000ULL,
        0, 1024000, 102400000, 1600, false, 4);
    REQUIRE(beyondEof);
    REQUIRE(beyondEof->lowerRawBytes == 102400000);
    REQUIRE(beyondEof->upperRawBytes == 102400000);
    REQUIRE(beyondEof->region == pbapp::SenderRatePriorRegion::SafetyRepair);
    REQUIRE(beyondEof->visitEquationBudget == 2000);
}

TEST_CASE("Rate prior saturates large frontiers and rejects malformed segment ranges", "[rate-prior]")
{
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    const auto saturated = pbapp::CalculateSenderRatePriorDecision({1048576, 1048576}, maximum,
        maximum - 1, 1, maximum, 64000, false, 1);
    REQUIRE(saturated);
    REQUIRE(saturated->lowerRawBytes == maximum);
    REQUIRE(saturated->upperRawBytes == maximum);
    REQUIRE_FALSE(pbapp::CalculateSenderRatePriorDecision({0, 280}, 0, 0, 1, 1, 1, true, 0));
    REQUIRE_FALSE(pbapp::CalculateSenderRatePriorDecision({200, 280}, 0, maximum, 1, maximum, 1, true, 0));
    REQUIRE_FALSE(pbapp::CalculateSenderRatePriorDecision({200, 280}, 0, 0, 0, 0, 1, true, 0));
    REQUIRE_FALSE(pbapp::CalculateSenderRatePriorDecision({200, 280}, 0, 0, 1, 1, 0, true, 0));
    REQUIRE_FALSE(pbapp::CalculateSenderRatePriorDecision({200, 280}, 0, 0, 1, 1, 64001, true, 0));
}

TEST_CASE("Rate prior clock starts on commit and excludes long local suspensions", "[rate-prior]")
{
    pbapp::SenderRatePriorClock clock;
    REQUIRE(clock.GetActiveNanoseconds() == 0);
    REQUIRE(clock.CommitFrame(800000000000ULL));
    REQUIRE(clock.GetActiveNanoseconds() == 0);
    REQUIRE(clock.CommitFrame(800500000000ULL));
    REQUIRE(clock.GetActiveNanoseconds() == 500000000);
    REQUIRE_FALSE(clock.CommitFrame(800400000000ULL));
    REQUIRE(clock.GetActiveNanoseconds() == 500000000);
    REQUIRE(clock.CommitFrame(860500000000ULL));
    REQUIRE(clock.GetActiveNanoseconds() == 1500000000);
    REQUIRE(clock.GetClampedGapCount() == 1);
    REQUIRE(clock.CommitFrame(860500000000ULL));
    REQUIRE(clock.GetActiveNanoseconds() == 1500000000);
}
