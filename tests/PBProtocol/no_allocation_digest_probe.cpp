#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/protocol_types.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace {

thread_local bool rejectAllocations = false;

[[nodiscard]] void* AllocateUnaligned(const std::size_t byteCount)
{
    if (rejectAllocations)
    {
        throw std::bad_alloc{};
    }

    const std::size_t allocationByteCount = byteCount == 0 ? 1 : byteCount;
    void* const allocation = std::malloc(allocationByteCount);
    if (allocation == nullptr)
    {
        throw std::bad_alloc{};
    }
    return allocation;
}

[[nodiscard]] void* AllocateAligned(
    const std::size_t byteCount,
    const std::size_t alignment)
{
    if (rejectAllocations)
    {
        throw std::bad_alloc{};
    }

    const std::size_t allocationByteCount = byteCount == 0 ? 1 : byteCount;
    void* const allocation = _aligned_malloc(allocationByteCount, alignment);
    if (allocation == nullptr)
    {
        throw std::bad_alloc{};
    }
    return allocation;
}

} // namespace

void* operator new(const std::size_t byteCount)
{
    return AllocateUnaligned(byteCount);
}

void* operator new[](const std::size_t byteCount)
{
    return AllocateUnaligned(byteCount);
}

void* operator new(
    const std::size_t byteCount,
    const std::align_val_t alignment)
{
    return AllocateAligned(byteCount, static_cast<std::size_t>(alignment));
}

void* operator new[](
    const std::size_t byteCount,
    const std::align_val_t alignment)
{
    return AllocateAligned(byteCount, static_cast<std::size_t>(alignment));
}

void operator delete(void* const allocation) noexcept
{
    std::free(allocation);
}

void operator delete[](void* const allocation) noexcept
{
    std::free(allocation);
}

void operator delete(
    void* const allocation,
    const std::size_t) noexcept
{
    std::free(allocation);
}

void operator delete[](
    void* const allocation,
    const std::size_t) noexcept
{
    std::free(allocation);
}

void operator delete(
    void* const allocation,
    const std::align_val_t) noexcept
{
    _aligned_free(allocation);
}

void operator delete[](
    void* const allocation,
    const std::align_val_t) noexcept
{
    _aligned_free(allocation);
}

void operator delete(
    void* const allocation,
    const std::size_t,
    const std::align_val_t) noexcept
{
    _aligned_free(allocation);
}

void operator delete[](
    void* const allocation,
    const std::size_t,
    const std::align_val_t) noexcept
{
    _aligned_free(allocation);
}

int main()
{
    std::array<std::byte, 65> input{};
    for (std::size_t byteIndex = 0; byteIndex < input.size(); byteIndex++)
    {
        input[byteIndex] = static_cast<std::byte>(byteIndex % 251U);
    }

    pbprotocol::SessionId sessionId{};
    for (std::size_t byteIndex = 0;
         byteIndex < sessionId.bytes.size();
         byteIndex++)
    {
        sessionId.bytes[byteIndex] = static_cast<std::byte>(byteIndex);
    }

    bool resultsMatch = false;
    rejectAllocations = true;
    {
        pbprotocol::Blake3Hasher hasher;
        hasher.Update(std::span<const std::byte>(input).first<32>());
        hasher.Update(std::span<const std::byte>(input).subspan(32));
        const auto streamingDigest = hasher.Finalize();
        const auto oneShotDigest = pbprotocol::ComputeBlake3Digest(input);
        const auto emptyDigest = pbprotocol::ComputeBlake3Digest({});
        const pbprotocol::WholeFileDigest emptyWholeFileDigest =
            pbprotocol::GetEmptyBlake3WholeFileDigest();
        const pbprotocol::SessionTag sessionTag =
            pbprotocol::DeriveSessionTag(sessionId);

        resultsMatch = streamingDigest == oneShotDigest &&
            emptyDigest == emptyWholeFileDigest.bytes &&
            sessionTag.value == 0x81DF204BD997BAD0ULL;
    }
    rejectAllocations = false;

    return resultsMatch ? 0 : 1;
}
