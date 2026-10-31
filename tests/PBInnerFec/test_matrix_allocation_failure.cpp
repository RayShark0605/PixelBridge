#include "pbinnerfec/inner_fec_profile.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace
{

thread_local bool rejectAllocations = false;
thread_local bool allocationRejected = false;

[[nodiscard]] void* Allocate(const std::size_t byteCount)
{
    if (rejectAllocations)
    {
        allocationRejected = true;
        throw std::bad_alloc{};
    }
    void* const allocation = std::malloc(byteCount == 0 ? 1 : byteCount);
    if (allocation == nullptr)
    {
        throw std::bad_alloc{};
    }
    return allocation;
}

} // namespace

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

TEST_CASE("Matrix serialization fails closed without terminating on allocation failure", "[innerfec][matrix][allocation]")
{
    for (const auto profileId : std::array{pbinnerfec::kInnerFecProfileIdRobust,
        pbinnerfec::kInnerFecProfileIdBalanced, pbinnerfec::kInnerFecProfileIdFast})
    {
        const auto* const profile = pbinnerfec::GetInnerFecProfile(profileId);
        REQUIRE(profile != nullptr);
        std::array<std::byte, pbinnerfec::kMaxInnerFecMatrixSerializationBytes> storage{};
        allocationRejected = false;
        rejectAllocations = true;
        const auto failed = pbinnerfec::SerializeInnerFecMatrix(*profile);
        const auto serialized = pbinnerfec::SerializeInnerFecMatrixInto(*profile, storage);
        const auto digest = pbinnerfec::ComputeInnerFecMatrixDigest(*profile);
        rejectAllocations = false;
        REQUIRE(allocationRejected);
        REQUIRE(failed.empty());
        REQUIRE_FALSE(serialized.empty());
        REQUIRE(digest == profile->matrixDigest);
        const auto recovered = pbinnerfec::SerializeInnerFecMatrix(*profile);
        REQUIRE(std::ranges::equal(recovered, serialized));
    }
}
