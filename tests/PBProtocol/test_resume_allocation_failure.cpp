#include "pbprotocol/resume_state.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace
{

thread_local bool allocationFailureEnabled = false;
thread_local bool allocationFailureObserved = false;
thread_local std::size_t allocationsBeforeFailure = 0;

[[nodiscard]] void* Allocate(const std::size_t byteCount)
{
    if (allocationFailureEnabled)
    {
        if (allocationsBeforeFailure == 0)
        {
            allocationFailureEnabled = false;
            allocationFailureObserved = true;
            throw std::bad_alloc{};
        }
        allocationsBeforeFailure--;
    }
    void* const allocation = std::malloc(byteCount == 0 ? 1 : byteCount);
    if (allocation == nullptr)
    {
        throw std::bad_alloc{};
    }
    return allocation;
}

class AllocationFailureScope final
{
public:
    explicit AllocationFailureScope(const std::size_t allocationIndex) noexcept
    {
        allocationsBeforeFailure = allocationIndex;
        allocationFailureObserved = false;
        allocationFailureEnabled = true;
    }
    ~AllocationFailureScope()
    {
        allocationFailureEnabled = false;
    }
    AllocationFailureScope(const AllocationFailureScope&) = delete;
    AllocationFailureScope& operator=(const AllocationFailureScope&) = delete;
};

[[nodiscard]] pbprotocol::ResumeCompletedSegmentRecord MakeCompleted(const std::uint64_t ordinal)
{
    pbprotocol::ResumeCompletedSegmentRecord record;
    record.segmentOrdinal = ordinal;
    record.rawOffset = ordinal * 64U;
    record.rawSize = 64;
    return record;
}

[[nodiscard]] pbprotocol::ResumeActiveWirehairCacheRecord MakeWirehair()
{
    pbprotocol::ResumeActiveWirehairCacheRecord record;
    record.segmentOrdinal = 1;
    record.entries.push_back({0, std::vector<std::byte>(32, std::byte{0x53})});
    return record;
}

[[nodiscard]] pbprotocol::ResumeActiveDirectRepeatRecord MakeDirectRepeat()
{
    pbprotocol::ResumeActiveDirectRepeatRecord record;
    record.segmentOrdinal = 1;
    record.directBlockCount = 2;
    record.entries.push_back({0, 32, std::vector<std::byte>(32, std::byte{0x62})});
    return record;
}

[[nodiscard]] std::vector<std::byte> CopyDocument(const pbprotocol::ResumeStateBuilder& builder)
{
    const auto bytes = builder.GetDocument();
    return {bytes.begin(), bytes.end()};
}

template <typename AppendFunction>
void VerifyAppendAllocationFailures(AppendFunction append)
{
    const auto policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    auto expectedResult = pbprotocol::ResumeStateBuilder::Create(policy);
    REQUIRE(expectedResult);
    auto expectedBuilder = std::move(expectedResult).Value();
    REQUIRE(expectedBuilder.AppendCompletedSegment(MakeCompleted(0)));
    const auto initialDocument = CopyDocument(expectedBuilder);
    REQUIRE(append(expectedBuilder));
    const auto expectedDocument = CopyDocument(expectedBuilder);

    bool reachedSuccessfulEnd = false;
    for (std::size_t allocationIndex = 0; allocationIndex < 64; allocationIndex++)
    {
        auto createResult = pbprotocol::ResumeStateBuilder::Create(policy);
        REQUIRE(createResult);
        auto builder = std::move(createResult).Value();
        REQUIRE(builder.AppendCompletedSegment(MakeCompleted(0)));
        auto status = pbprotocol::ProtocolStatus::Success();
        bool exceptionEscaped = false;
        {
            const AllocationFailureScope failure(allocationIndex);
            try
            {
                status = append(builder);
            }
            catch (const std::bad_alloc&)
            {
                exceptionEscaped = true;
            }
        }
        INFO("Allocation index: " << allocationIndex);
        REQUIRE_FALSE(exceptionEscaped);
        if (!allocationFailureObserved)
        {
            REQUIRE(status);
            REQUIRE(CopyDocument(builder) == expectedDocument);
            reachedSuccessfulEnd = true;
            break;
        }
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().code == pbprotocol::ProtocolErrorCode::ResourceExhausted);
        REQUIRE(CopyDocument(builder) == initialDocument);
        REQUIRE(append(builder));
        REQUIRE(CopyDocument(builder) == expectedDocument);
        const auto duplicateStatus = append(builder);
        REQUIRE_FALSE(duplicateStatus);
        REQUIRE(duplicateStatus.Error().code == pbprotocol::ProtocolErrorCode::ResumeRecordConflict);
        REQUIRE(CopyDocument(builder) == expectedDocument);
    }
    REQUIRE(reachedSuccessfulEnd);
}

template <typename AppendFunction>
void VerifyBudgetBeforeAllocation(AppendFunction append)
{
    for (const bool withExistingRecord : std::array{false, true})
    {
        auto policy = pbprotocol::GetDefaultReceiverResourcePolicy();
        policy.maxResumeBytes = withExistingRecord ? 128U : 64U;
        auto createResult = pbprotocol::ResumeStateBuilder::Create(policy);
        REQUIRE(createResult);
        auto builder = std::move(createResult).Value();
        if (withExistingRecord)
        {
            REQUIRE(builder.AppendCompletedSegment(MakeCompleted(0)));
        }
        const auto initialDocument = CopyDocument(builder);
        auto status = pbprotocol::ProtocolStatus::Success();
        {
            const AllocationFailureScope failure(0);
            status = append(builder);
        }
        REQUIRE_FALSE(allocationFailureObserved);
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().code == pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
        REQUIRE(CopyDocument(builder) == initialDocument);
    }
}

} // namespace

// Resume vectors and their entries have default alignment. These overrides
// intercept their allocations without replacing any product allocator.
void* operator new(const std::size_t byteCount)
{
    return Allocate(byteCount);
}

void* operator new[](const std::size_t byteCount)
{
    return Allocate(byteCount);
}

void operator delete(void* const allocation) noexcept
{
    std::free(allocation);
}

void operator delete[](void* const allocation) noexcept
{
    std::free(allocation);
}

void operator delete(void* const allocation, const std::size_t) noexcept
{
    std::free(allocation);
}

void operator delete[](void* const allocation, const std::size_t) noexcept
{
    std::free(allocation);
}

TEST_CASE("Every resume builder allocation failure preserves bytes and key admission", "[pbprotocol][resume][allocation]")
{
    const auto completed = MakeCompleted(1);
    const auto wirehair = MakeWirehair();
    const auto directRepeat = MakeDirectRepeat();
    VerifyAppendAllocationFailures([&completed](pbprotocol::ResumeStateBuilder& builder)
    {
        return builder.AppendCompletedSegment(completed);
    });
    VerifyAppendAllocationFailures([&wirehair](pbprotocol::ResumeStateBuilder& builder)
    {
        return builder.AppendActiveWirehairCache(wirehair);
    });
    VerifyAppendAllocationFailures([&directRepeat](pbprotocol::ResumeStateBuilder& builder)
    {
        return builder.AppendDirectRepeatReceivedBlocks(directRepeat);
    });
}

TEST_CASE("Resume cache body allocation follows the complete document budget gate", "[pbprotocol][resume][allocation]")
{
    const auto wirehair = MakeWirehair();
    const auto directRepeat = MakeDirectRepeat();
    VerifyBudgetBeforeAllocation([&wirehair](pbprotocol::ResumeStateBuilder& builder)
    {
        return builder.AppendActiveWirehairCache(wirehair);
    });
    VerifyBudgetBeforeAllocation([&directRepeat](pbprotocol::ResumeStateBuilder& builder)
    {
        return builder.AppendDirectRepeatReceivedBlocks(directRepeat);
    });
}

TEST_CASE("Resume loading reports allocation failures instead of throwing from a payload copy", "[pbprotocol][resume][allocation]")
{
    const auto policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    auto createResult = pbprotocol::ResumeStateBuilder::Create(policy);
    REQUIRE(createResult);
    auto builder = std::move(createResult).Value();
    REQUIRE(builder.AppendCompletedSegment(MakeCompleted(0)));
    auto wirehair = MakeWirehair();
    wirehair.entries.push_back({1, std::vector<std::byte>(32, std::byte{0x74})});
    REQUIRE(builder.AppendActiveWirehairCache(wirehair));
    auto directRepeat = MakeDirectRepeat();
    directRepeat.segmentOrdinal = 2;
    directRepeat.entries.push_back({1, 32, std::vector<std::byte>(32, std::byte{0x85})});
    REQUIRE(builder.AppendDirectRepeatReceivedBlocks(directRepeat));
    const auto expectedResult = pbprotocol::LoadResumeState(builder.GetDocument(), policy);
    REQUIRE(expectedResult);

    bool reachedSuccessfulEnd = false;
    for (std::size_t allocationIndex = 0; allocationIndex < 64; allocationIndex++)
    {
        auto result = pbprotocol::ProtocolResult<pbprotocol::LoadedResumeState>::Failure(
            pbprotocol::ProtocolErrorCode::InternalInvariantViolation, 0);
        bool exceptionEscaped = false;
        {
            const AllocationFailureScope failure(allocationIndex);
            try
            {
                result = pbprotocol::LoadResumeState(builder.GetDocument(), policy);
            }
            catch (const std::bad_alloc&)
            {
                exceptionEscaped = true;
            }
        }
        INFO("Allocation index: " << allocationIndex);
        REQUIRE_FALSE(exceptionEscaped);
        if (result)
        {
            REQUIRE(result.Value() == expectedResult.Value());
        }
        else
        {
            REQUIRE(allocationFailureObserved);
            REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::ResourceExhausted);
        }
        if (!allocationFailureObserved)
        {
            reachedSuccessfulEnd = true;
            break;
        }
    }
    REQUIRE(reachedSuccessfulEnd);
}
