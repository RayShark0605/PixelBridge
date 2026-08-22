#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace pbprotocol {

// Streaming CRC-32C (Castagnoli) as used by iSCSI/SMB/Ext4 and the
// PixelBridge transport layer. Parameters: width 32, polynomial
// 0x1EDC6F41 (reflected form 0x82F63B78), init 0xFFFFFFFF, refin/refout
// true, xorout 0xFFFFFFFF. Transport CRC only: it is not a cryptographic
// integrity or authentication mechanism.
class Crc32c
{
public:
    // Appends data to the running stream. Empty updates are no-ops.
    void Update(const std::span<const std::byte> data) noexcept;

    // Returns the CRC of everything updated so far without consuming state,
    // so Finalize() is repeatable and further Update() calls continue the
    // same stream (equivalent to one-shot over the concatenation).
    [[nodiscard]] std::uint32_t Finalize() const noexcept;

private:
    // Running reflected CRC state before final xorout.
    std::uint32_t crc_ = 0xFFFFFFFFu;
};

// One-shot convenience wrapper around Crc32c.
[[nodiscard]] std::uint32_t ComputeCrc32c(
    const std::span<const std::byte> data) noexcept;

} // namespace pbprotocol