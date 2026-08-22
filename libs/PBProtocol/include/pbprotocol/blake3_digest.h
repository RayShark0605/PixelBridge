#pragma once

#include "pbprotocol/protocol_types.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace pbprotocol {

// Streaming BLAKE3 with fixed 32-byte (BLAKE3-256) output. Wraps the pinned
// vcpkg blake3 implementation; this header intentionally does not expose
// <blake3.h>, so the library linkage stays PRIVATE. The hasher is owned by a
// single thread, non-copyable, and movable: moves transfer ownership of the
// underlying state.
class Blake3Hasher
{
public:
    static constexpr std::size_t kDigestByteCount = kDigestBytes;

    // Construction initializes an empty BLAKE3 stream. May throw
    // std::bad_alloc because the implementation state is heap-allocated to
    // keep <blake3.h> out of this header.
    Blake3Hasher();
    ~Blake3Hasher();

    Blake3Hasher(const Blake3Hasher&) = delete;
    Blake3Hasher& operator=(const Blake3Hasher&) = delete;
    Blake3Hasher(Blake3Hasher&&) noexcept = default;
    Blake3Hasher& operator=(Blake3Hasher&&) noexcept = default;

    // Appends data to the running stream. Empty updates are no-ops. The
    // pinned BLAKE3 C API performs no allocation here, so this cannot throw.
    void Update(const std::span<const std::byte> data) noexcept;

    // Returns the digest of everything updated so far without consuming
    // state: Finalize() is repeatable and further Update() calls continue
    // the same stream (equivalent to one-shot over the concatenation).
    [[nodiscard]] std::array<std::byte, kDigestBytes> Finalize() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One-shot convenience wrapper around Blake3Hasher.
[[nodiscard]] std::array<std::byte, kDigestBytes> ComputeBlake3Digest(
    const std::span<const std::byte> data);

} // namespace pbprotocol