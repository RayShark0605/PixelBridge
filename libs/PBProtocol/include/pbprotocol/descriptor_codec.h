#pragma once

#include "pbprotocol/protocol_result.h"
#include "pbprotocol/protocol_types.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace pbprotocol {

enum class DescriptorWireMaturity : std::uint8_t
{
    Phase0Provisional
};

// These exact payloads are Phase-0 implementation vectors. They are not the
// complete formal v1 records because SessionVisualProfileId and the remaining
// architecture-required bindings have not been assigned final wire fields.
inline constexpr DescriptorWireMaturity kDescriptorWireMaturity =
    DescriptorWireMaturity::Phase0Provisional;
inline constexpr std::size_t kSessionDescriptorPayloadBytes = 37;
inline constexpr std::size_t kDirectRepeatSegmentDescriptorPayloadBytes = 110;
inline constexpr std::size_t kWirehairV2SegmentDescriptorPayloadBytes = 142;
inline constexpr std::size_t kFinalManifestPayloadBytes = 65;

inline constexpr std::uint64_t kWirehairV2CertifiedProfileId =
    0x4B295BBB47F4F9C9ULL;
inline constexpr std::uint64_t kWirehairV2MinimumBlockCount = 2;
inline constexpr std::uint64_t kWirehairV2MaximumBlockCount = 64000;

[[nodiscard]] ProtocolStatus ValidateReceiverResourcePolicy(
    const ReceiverResourcePolicy& resourcePolicy) noexcept;

// The overloads without a policy validate wire/descriptor structure only.
// Receiver entry points must use the overloads that also accept a local policy.
[[nodiscard]] ProtocolStatus ValidateSessionDescriptor(
    const SessionDescriptor& descriptor) noexcept;

[[nodiscard]] ProtocolStatus ValidateSessionDescriptor(
    const SessionDescriptor& descriptor,
    const ReceiverResourcePolicy& resourcePolicy) noexcept;

[[nodiscard]] ProtocolStatus ValidateWirehairV2SerializedProfile(
    const WirehairV2SerializedProfile& profile,
    std::uint64_t expectedMessageBytes,
    std::uint32_t expectedBlockBytes,
    std::size_t fieldOffset = 0) noexcept;

[[nodiscard]] ProtocolResult<std::uint64_t> GetDirectRepeatBlockCount(
    std::uint64_t encodedSize,
    std::uint32_t outerBlockBytes,
    std::size_t fieldOffset = 0) noexcept;

[[nodiscard]] ProtocolStatus ValidateSegmentDescriptor(
    const SegmentDescriptor& descriptor,
    const SessionDescriptor& sessionDescriptor) noexcept;

[[nodiscard]] ProtocolStatus ValidateSegmentDescriptor(
    const SegmentDescriptor& descriptor,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy) noexcept;

[[nodiscard]] ProtocolStatus ValidateFinalManifest(
    const FinalManifest& finalManifest,
    const SessionDescriptor& sessionDescriptor) noexcept;

[[nodiscard]] ProtocolStatus ValidateFinalManifest(
    const FinalManifest& finalManifest,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy) noexcept;

[[nodiscard]] constexpr std::size_t GetSerializedSize(
    const SessionDescriptor&) noexcept
{
    return kSessionDescriptorPayloadBytes;
}

[[nodiscard]] ProtocolResult<std::size_t> GetSerializedSize(
    const SegmentDescriptor& descriptor) noexcept;

[[nodiscard]] constexpr std::size_t GetSerializedSize(
    const FinalManifest&) noexcept
{
    return kFinalManifestPayloadBytes;
}

[[nodiscard]] ProtocolStatus SerializeSessionDescriptor(
    const SessionDescriptor& descriptor,
    std::span<std::byte> output) noexcept;

[[nodiscard]] ProtocolStatus SerializeSessionDescriptor(
    const SessionDescriptor& descriptor,
    const ReceiverResourcePolicy& resourcePolicy,
    std::span<std::byte> output) noexcept;

[[nodiscard]] ProtocolStatus SerializeSegmentDescriptor(
    const SegmentDescriptor& descriptor,
    const SessionDescriptor& sessionDescriptor,
    std::span<std::byte> output) noexcept;

[[nodiscard]] ProtocolStatus SerializeSegmentDescriptor(
    const SegmentDescriptor& descriptor,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy,
    std::span<std::byte> output) noexcept;

[[nodiscard]] ProtocolStatus SerializeFinalManifest(
    const FinalManifest& finalManifest,
    const SessionDescriptor& sessionDescriptor,
    std::span<std::byte> output) noexcept;

[[nodiscard]] ProtocolStatus SerializeFinalManifest(
    const FinalManifest& finalManifest,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy,
    std::span<std::byte> output) noexcept;

[[nodiscard]] ProtocolResult<SessionDescriptor> ParseSessionDescriptor(
    std::span<const std::byte> input,
    const ReceiverResourcePolicy& resourcePolicy) noexcept;

[[nodiscard]] ProtocolResult<SegmentDescriptor> ParseSegmentDescriptor(
    std::span<const std::byte> input,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy) noexcept;

[[nodiscard]] ProtocolResult<FinalManifest> ParseFinalManifest(
    std::span<const std::byte> input,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy) noexcept;

} // namespace pbprotocol
