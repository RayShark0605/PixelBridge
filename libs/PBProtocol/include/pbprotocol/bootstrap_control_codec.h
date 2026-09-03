#pragma once

#include "pbprotocol/protocol_result.h"
#include "pbprotocol/protocol_types.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>

namespace pbprotocol {

inline constexpr std::array<std::byte, 4> kBootstrapRecordMagic{
    std::byte{0x50},
    std::byte{0x42},
    std::byte{0x52},
    std::byte{0x47}};
inline constexpr std::array<std::byte, 4> kControlRecordMagic{
    std::byte{0x50},
    std::byte{0x42},
    std::byte{0x43},
    std::byte{0x52}};

inline constexpr std::uint8_t kBootstrapVersion = 1;
inline constexpr std::uint8_t kControlVersion = 1;
inline constexpr std::size_t kBootstrapRecordBytes = 44;
inline constexpr std::size_t kControlRecordPrefixBytes = 26;
inline constexpr std::size_t kControlRecordCrcBytes = 4;
inline constexpr std::size_t kMinimumControlRecordBytes =
    kControlRecordPrefixBytes + kControlRecordCrcBytes;
inline constexpr std::size_t kMaximumControlRecordBytes = 64U * 1024U;
inline constexpr std::size_t kMaximumControlPayloadBytes =
    kMaximumControlRecordBytes - kMinimumControlRecordBytes;

struct BootstrapRecord
{
    std::uint8_t bootstrapVersion = kBootstrapVersion;
    ProtocolVersion protocolVersion{};
    std::uint8_t visualLayoutVersion = 0;
    std::uint64_t visualProfileId = 0;
    SessionTag sessionTag{};
    std::uint64_t frameSequence = 0;
    std::uint32_t controlEpoch = 0;
    std::uint32_t flags = 0;

    bool operator==(const BootstrapRecord&) const = default;
};

enum class ControlRecordType : std::uint8_t
{
    SessionDescriptor = 1,
    SegmentDescriptor = 2,
    FinalManifest = 3
};

// The payload view borrows the caller-owned input. It is valid only while that
// input storage remains alive and unchanged.
struct ControlRecordView
{
    std::uint8_t controlVersion = kControlVersion;
    ControlRecordType recordType = static_cast<ControlRecordType>(0);
    std::uint64_t controlSequence = 0;
    SessionTag sessionTag{};
    std::span<const std::byte> payload;
};

[[nodiscard]] constexpr std::size_t GetSerializedSize(
    const BootstrapRecord&) noexcept
{
    return kBootstrapRecordBytes;
}

[[nodiscard]] ProtocolResult<std::size_t> GetSerializedSize(
    const ControlRecordView& record) noexcept;

[[nodiscard]] ProtocolStatus SerializeBootstrapRecord(
    const BootstrapRecord& record,
    std::span<std::byte> output) noexcept;

[[nodiscard]] ProtocolResult<BootstrapRecord> ParseBootstrapRecord(
    std::span<const std::byte> input) noexcept;

[[nodiscard]] ProtocolStatus SerializeControlRecord(
    const ControlRecordView& record,
    std::span<std::byte> output) noexcept;

// This validates only the PB-Control-1 envelope. Production receiver ingress
// must use ControlPlaneReceiver so descriptor policy, typed dispatch,
// SessionTag cross-check, and immutable binding cannot be skipped.
[[nodiscard]] ProtocolResult<ControlRecordView> ParseControlRecord(
    std::span<const std::byte> input) noexcept;

// Copies one complete PB-Control-1 record into a fixed-size Inner-FEC
// information block and zero-fills the unused tail. This does not alter or
// replace the Control envelope CRC: the complete record is parsed before any
// output is written. The input and output spans must not overlap.
[[nodiscard]] ProtocolStatus FrameControlRecordIntoInfoBlock(
    std::span<const std::byte> serializedRecord,
    std::size_t infoSize,
    std::span<std::byte> outInfoBlock) noexcept;

// Recovers one complete PB-Control-1 record from a fixed-size information
// block and verifies that every byte after RecordBytes is canonical zero
// padding. The returned view borrows infoBlock.
[[nodiscard]] ProtocolResult<std::span<const std::byte>>
    ExtractControlRecordFromInfoBlock(
        std::span<const std::byte> infoBlock) noexcept;

template <typename InputRange>
    requires(
        !std::ranges::borrowed_range<InputRange> &&
        std::constructible_from<std::span<const std::byte>, InputRange&&>)
[[nodiscard]] ProtocolResult<ControlRecordView> ParseControlRecord(
    InputRange&&) = delete;

} // namespace pbprotocol
