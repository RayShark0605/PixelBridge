#include "pbprotocol/blake3_digest.h"

#include <blake3.h>

#include <cstdint>
#include <memory>

namespace pbprotocol {

struct Blake3Hasher::Impl
{
    explicit Impl()
    {
        blake3_hasher_init(&hasher);
    }

    blake3_hasher hasher{};
};

Blake3Hasher::Blake3Hasher() : impl_(std::make_unique<Impl>()) {}

Blake3Hasher::~Blake3Hasher() = default;

void Blake3Hasher::Update(const std::span<const std::byte> data) noexcept
{
    if (data.empty())
    {
        return;
    }

    blake3_hasher_update(&impl_->hasher, data.data(), data.size());
}

std::array<std::byte, kDigestBytes> Blake3Hasher::Finalize() const noexcept
{
    std::array<std::byte, kDigestBytes> digest{};
    blake3_hasher_finalize(
        &impl_->hasher,
        reinterpret_cast<std::uint8_t*>(digest.data()),
        digest.size());
    return digest;
}

std::array<std::byte, kDigestBytes> ComputeBlake3Digest(
    const std::span<const std::byte> data)
{
    Blake3Hasher hasher;
    hasher.Update(data);
    return hasher.Finalize();
}

} // namespace pbprotocol