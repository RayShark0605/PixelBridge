#pragma once

#include "pbprotocol/bootstrap_control_codec.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>

namespace pbprotocol {

inline constexpr std::size_t kControlFragmentPrefixBytes = 20;
inline constexpr std::size_t kControlFragmentCrcBytes = 4;
inline constexpr std::size_t kMinimumControlFragmentBytes =
    kControlFragmentPrefixBytes + 1U + kControlFragmentCrcBytes;
inline constexpr std::size_t kMaximumControlFragmentPayloadBytes = UINT16_MAX;
inline constexpr std::size_t kMaximumControlFragmentBytes =
    kControlFragmentPrefixBytes + kMaximumControlFragmentPayloadBytes +
    kControlFragmentCrcBytes;

// The payload view borrows the caller-owned input or logical Control record.
// It is valid only while that storage remains alive and unchanged.
struct ControlFragmentView
{
    std::uint64_t controlRecordId = 0;
    std::uint16_t fragmentIndex = 0;
    std::uint16_t fragmentCount = 0;
    std::uint32_t totalRecordBytes = 0;
    std::uint16_t flags = 0;
    std::span<const std::byte> payload;
};

[[nodiscard]] ProtocolResult<std::size_t> GetSerializedSize(
    const ControlFragmentView& fragment) noexcept;

[[nodiscard]] ProtocolResult<std::uint16_t> GetControlFragmentCount(
    std::span<const std::byte> recordBytes,
    std::uint16_t maxFragmentPayloadBytes) noexcept;

[[nodiscard]] ProtocolResult<ControlFragmentView> GetControlFragment(
    std::uint64_t controlRecordId,
    std::span<const std::byte> recordBytes,
    std::uint16_t fragmentIndex,
    std::uint16_t maxFragmentPayloadBytes) noexcept;

template <typename InputRange>
    requires(
        !std::ranges::borrowed_range<InputRange> &&
        std::constructible_from<std::span<const std::byte>, InputRange&&>)
[[nodiscard]] ProtocolResult<ControlFragmentView> GetControlFragment(
    std::uint64_t,
    InputRange&&,
    std::uint16_t,
    std::uint16_t) = delete;

[[nodiscard]] ProtocolStatus SerializeControlFragment(
    const ControlFragmentView& fragment,
    std::span<std::byte> output) noexcept;

[[nodiscard]] ProtocolResult<ControlFragmentView> ParseControlFragment(
    std::span<const std::byte> input) noexcept;

template <typename InputRange>
    requires(
        !std::ranges::borrowed_range<InputRange> &&
        std::constructible_from<std::span<const std::byte>, InputRange&&>)
[[nodiscard]] ProtocolResult<ControlFragmentView> ParseControlFragment(
    InputRange&&) = delete;

} // namespace pbprotocol
