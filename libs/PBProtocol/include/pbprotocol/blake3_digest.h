#pragma once

#include "pbprotocol/protocol_types.h"

#include <array>
#include <cstddef>
#include <span>

namespace pbprotocol {

// Streaming BLAKE3 with fixed 32-byte (BLAKE3-256) output. Wraps the pinned
// vcpkg blake3 implementation; this header intentionally does not expose
// <blake3.h>, so the library linkage stays PRIVATE. This unkeyed digest provides
// integrity only. It is not a MAC and does not authenticate the sender.
class Blake3Hasher
{
public:
    static constexpr std::size_t kDigestByteCount = kDigestBytes;

    // Construction initializes an empty allocation-free BLAKE3 stream.
    Blake3Hasher() noexcept;
    ~Blake3Hasher() noexcept;

    Blake3Hasher(const Blake3Hasher&) = delete;
    Blake3Hasher& operator=(const Blake3Hasher&) = delete;
    Blake3Hasher(Blake3Hasher&&) = delete;
    Blake3Hasher& operator=(Blake3Hasher&&) = delete;

    // Appends data to the running stream. Empty updates are no-ops. The
    // pinned BLAKE3 C API performs no allocation here, so this cannot throw.
    void Update(const std::span<const std::byte> data) noexcept;

    // Returns the digest of everything updated so far without consuming
    // state: Finalize() is repeatable and further Update() calls continue
    // the same stream (equivalent to one-shot over the concatenation).
    [[nodiscard]] std::array<std::byte, kDigestBytes> Finalize() const noexcept;

private:
    // Pinned BLAKE3 1.8.5 currently requires less than this capacity. The
    // implementation statically verifies both size and alignment before using
    // the storage, so a dependency ABI change fails at build time.
    static constexpr std::size_t kHasherStorageByteCount = 2048;
    alignas(std::max_align_t)
        std::array<std::byte, kHasherStorageByteCount> hasherStorage_{};
};

// One-shot convenience wrapper around Blake3Hasher.
[[nodiscard]] std::array<std::byte, kDigestBytes> ComputeBlake3Digest(
    const std::span<const std::byte> data) noexcept;

} // namespace pbprotocol
