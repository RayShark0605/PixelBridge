#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace pbprotocol::detail {

// Internal reference and dispatch diagnostics; not a public protocol API.
[[nodiscard]] std::uint32_t UpdateCrc32cPortable(std::uint32_t state, std::span<const std::byte> data) noexcept;
[[nodiscard]] bool IsCrc32cHardwareAvailable() noexcept;

} // namespace pbprotocol::detail
