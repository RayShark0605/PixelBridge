#include "pbprotocol/blake3_digest.h"

#include <blake3.h>

#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

namespace pbprotocol {

namespace {

template <typename StorageType>
[[nodiscard]] auto GetBlake3Hasher(StorageType& storage) noexcept
{
    using HasherType = std::conditional_t<
        std::is_const_v<StorageType>,
        const blake3_hasher,
        blake3_hasher>;
    return std::launder(reinterpret_cast<HasherType*>(storage.data()));
}

} // namespace

Blake3Hasher::Blake3Hasher() noexcept
{
    static_assert(sizeof(blake3_hasher) <= kHasherStorageByteCount);
    static_assert(alignof(blake3_hasher) <= alignof(std::max_align_t));
    static_assert(std::is_nothrow_default_constructible_v<blake3_hasher>);
    static_assert(std::is_nothrow_destructible_v<blake3_hasher>);

    blake3_hasher* const hasher = std::construct_at(
        reinterpret_cast<blake3_hasher*>(hasherStorage_.data()));
    blake3_hasher_init(hasher);
}

Blake3Hasher::~Blake3Hasher() noexcept
{
    std::destroy_at(GetBlake3Hasher(hasherStorage_));
}

void Blake3Hasher::Update(const std::span<const std::byte> data) noexcept
{
    if (data.empty())
    {
        return;
    }

    blake3_hasher_update(
        GetBlake3Hasher(hasherStorage_),
        data.data(),
        data.size());
}

std::array<std::byte, kDigestBytes> Blake3Hasher::Finalize() const noexcept
{
    std::array<std::byte, kDigestBytes> digest{};
    blake3_hasher_finalize(
        GetBlake3Hasher(hasherStorage_),
        reinterpret_cast<std::uint8_t*>(digest.data()),
        digest.size());
    return digest;
}

std::array<std::byte, kDigestBytes> ComputeBlake3Digest(
    const std::span<const std::byte> data) noexcept
{
    Blake3Hasher hasher;
    hasher.Update(data);
    return hasher.Finalize();
}

} // namespace pbprotocol
