#pragma once

#include "pbprotocol/protocol_result.h"
#include "pbprotocol/protocol_types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace pbprotocol {

enum class DescriptorWireMaturity : std::uint8_t
{
    FormalProtocol1Schema1
};

inline constexpr DescriptorWireMaturity kDescriptorWireMaturity =
    DescriptorWireMaturity::FormalProtocol1Schema1;
inline constexpr std::uint16_t kDescriptorSchemaVersion = 1;
inline constexpr std::size_t kDescriptorSchemaVersionOffset = 0;
inline constexpr std::size_t kDescriptorSchemaVersionBytes = 2;
inline constexpr std::size_t kDescriptorHeaderBytesOffset = 2;
inline constexpr std::size_t kDescriptorHeaderBytesFieldBytes = 2;
inline constexpr std::size_t kDescriptorTotalBytesOffset = 4;
inline constexpr std::size_t kDescriptorTotalBytesFieldBytes = 4;
inline constexpr std::size_t kDescriptorSchemaPrefixBytes = 8;
inline constexpr std::size_t kDescriptorCrcBytes = 4;
inline constexpr std::size_t kDescriptorTlvTypeOffset = 0;
inline constexpr std::size_t kDescriptorTlvTypeBytes = 2;
inline constexpr std::size_t kDescriptorTlvFlagsOffset = 2;
inline constexpr std::size_t kDescriptorTlvFlagsBytes = 2;
inline constexpr std::size_t kDescriptorTlvValueBytesOffset = 4;
inline constexpr std::size_t kDescriptorTlvValueBytesFieldBytes = 4;
inline constexpr std::size_t kDescriptorTlvHeaderBytes = 8;
inline constexpr std::uint16_t kDescriptorTlvOptionalFlag = 0x0001U;
inline constexpr std::uint16_t kDescriptorTlvKnownFlags = kDescriptorTlvOptionalFlag;
inline constexpr std::uint64_t kSessionMandatoryFeatureMask = 0x00000000FFFFFFFFULL;
inline constexpr std::uint64_t kSessionOptionalFeatureMask = 0xFFFFFFFF00000000ULL;
inline constexpr std::size_t kSessionDescriptorHeaderBytes = 70;
inline constexpr std::size_t kMinimumSessionDescriptorPayloadBytes =
    kSessionDescriptorHeaderBytes + kDescriptorCrcBytes;
inline constexpr std::size_t kMaximumFileNameUtf8Bytes = 1020;
// Size of the canonical "payload.bin" fixture used by existing fixed-buffer
// tests and tools. Formal SessionDescriptor records are variable length;
// product code must call GetSerializedSize instead of assuming this value.
inline constexpr std::size_t kSessionDescriptorPayloadBytes = 85;
// Control records must still fit one 1,350-byte Inner-FEC information block
// after the PB-Control-1 envelope. The formal descriptor budget remains well
// below that limit while leaving bounded room for optional TLVs.
inline constexpr std::size_t kMaximumDescriptorPayloadBytes = 1300;
inline constexpr std::uint16_t kSegmentDescriptorHeaderBytes = 128;
inline constexpr std::uint16_t kFinalManifestHeaderBytes = 80;
inline constexpr std::size_t kDirectRepeatSegmentDescriptorPayloadBytes = 132;
inline constexpr std::size_t kWirehairV2SegmentDescriptorPayloadBytes = 164;
inline constexpr std::size_t kFinalManifestPayloadBytes = 84;

inline constexpr std::size_t kFormalWireSessionDescriptorProtocolMajorOffset = 8;
inline constexpr std::size_t kFormalWireSessionDescriptorProtocolMajorBytes = 2;
inline constexpr std::size_t kFormalWireSessionDescriptorProtocolMinorOffset = 10;
inline constexpr std::size_t kFormalWireSessionDescriptorProtocolMinorBytes = 2;
inline constexpr std::size_t kFormalWireSessionDescriptorSessionIdOffset = 12;
inline constexpr std::size_t kFormalWireSessionDescriptorSessionIdBytes = 16;
inline constexpr std::size_t kFormalWireSessionDescriptorVisualProfileIdOffset = 28;
inline constexpr std::size_t kFormalWireSessionDescriptorVisualProfileIdBytes = 8;
inline constexpr std::size_t kFormalWireSessionDescriptorOriginalFileSizeOffset = 36;
inline constexpr std::size_t kFormalWireSessionDescriptorOriginalFileSizeBytes = 8;
inline constexpr std::size_t kFormalWireSessionDescriptorSourceSegmentTargetBytesOffset = 44;
inline constexpr std::size_t kFormalWireSessionDescriptorSourceSegmentTargetBytesFieldBytes = 4;
inline constexpr std::size_t kFormalWireSessionDescriptorSegmentCountOffset = 48;
inline constexpr std::size_t kFormalWireSessionDescriptorSegmentCountBytes = 8;
inline constexpr std::size_t kFormalWireSessionDescriptorCompressionPolicyOffset = 56;
inline constexpr std::size_t kFormalWireSessionDescriptorCompressionPolicyBytes = 1;
inline constexpr std::size_t kFormalWireSessionDescriptorDigestAlgorithmOffset = 57;
inline constexpr std::size_t kFormalWireSessionDescriptorDigestAlgorithmBytes = 1;
inline constexpr std::size_t kFormalWireSessionDescriptorReservedOffset = 58;
inline constexpr std::size_t kFormalWireSessionDescriptorReservedBytes = 2;
inline constexpr std::size_t kFormalWireSessionDescriptorFeatureFlagsOffset = 60;
inline constexpr std::size_t kFormalWireSessionDescriptorFeatureFlagsBytes = 8;
inline constexpr std::size_t kFormalWireSessionDescriptorFileNameUtf8BytesOffset = 68;
inline constexpr std::size_t kFormalWireSessionDescriptorFileNameUtf8BytesFieldBytes = 2;
inline constexpr std::size_t kFormalWireSessionDescriptorFileNameUtf8Offset = 70;

inline constexpr std::size_t kFormalWireSegmentDescriptorSessionTagOffset = 8;
inline constexpr std::size_t kFormalWireSegmentDescriptorSessionTagBytes = 8;
inline constexpr std::size_t kFormalWireSegmentDescriptorOrdinalOffset = 16;
inline constexpr std::size_t kFormalWireSegmentDescriptorOrdinalBytes = 8;
inline constexpr std::size_t kFormalWireSegmentDescriptorRawOffsetOffset = 24;
inline constexpr std::size_t kFormalWireSegmentDescriptorRawOffsetBytes = 8;
inline constexpr std::size_t kFormalWireSegmentDescriptorRawSizeOffset = 32;
inline constexpr std::size_t kFormalWireSegmentDescriptorRawSizeBytes = 8;
inline constexpr std::size_t kFormalWireSegmentDescriptorEncodedSizeOffset = 40;
inline constexpr std::size_t kFormalWireSegmentDescriptorEncodedSizeBytes = 8;
inline constexpr std::size_t kFormalWireSegmentDescriptorCompressionCodecOffset = 48;
inline constexpr std::size_t kFormalWireSegmentDescriptorCompressionCodecBytes = 1;
inline constexpr std::size_t kFormalWireSegmentDescriptorOuterFecModeOffset = 49;
inline constexpr std::size_t kFormalWireSegmentDescriptorOuterFecModeBytes = 1;
inline constexpr std::size_t kFormalWireSegmentDescriptorReservedOffset = 50;
inline constexpr std::size_t kFormalWireSegmentDescriptorReservedBytes = 2;
inline constexpr std::size_t kFormalWireSegmentDescriptorOuterBlockBytesOffset = 52;
inline constexpr std::size_t kFormalWireSegmentDescriptorOuterBlockBytesFieldBytes = 4;
inline constexpr std::size_t kFormalWireSegmentDescriptorRawDigestOffset = 56;
inline constexpr std::size_t kFormalWireSegmentDescriptorRawDigestBytes = 32;
inline constexpr std::size_t kFormalWireSegmentDescriptorEncodedDigestOffset = 88;
inline constexpr std::size_t kFormalWireSegmentDescriptorEncodedDigestBytes = 32;
inline constexpr std::size_t kFormalWireSegmentDescriptorFlagsOffset = 120;
inline constexpr std::size_t kFormalWireSegmentDescriptorFlagsBytes = 8;
inline constexpr std::size_t kFormalWireSegmentDescriptorWirehairProfileOffset = 128;
inline constexpr std::size_t kFormalWireSegmentDescriptorWirehairProfileBytes = 32;

inline constexpr std::size_t kFormalWireFinalManifestSessionIdOffset = 8;
inline constexpr std::size_t kFormalWireFinalManifestSessionIdBytes = 16;
inline constexpr std::size_t kFormalWireFinalManifestOriginalFileSizeOffset = 24;
inline constexpr std::size_t kFormalWireFinalManifestOriginalFileSizeBytes = 8;
inline constexpr std::size_t kFormalWireFinalManifestSegmentCountOffset = 32;
inline constexpr std::size_t kFormalWireFinalManifestSegmentCountBytes = 8;
inline constexpr std::size_t kFormalWireFinalManifestWholeFileDigestOffset = 40;
inline constexpr std::size_t kFormalWireFinalManifestWholeFileDigestBytes = 32;
inline constexpr std::size_t kFormalWireFinalManifestDigestAlgorithmOffset = 72;
inline constexpr std::size_t kFormalWireFinalManifestDigestAlgorithmBytes = 1;
inline constexpr std::size_t kFormalWireFinalManifestReservedOffset = 73;
inline constexpr std::size_t kFormalWireFinalManifestReservedBytes = 7;

inline constexpr std::uint64_t kWirehairV2CertifiedProfileId =
    0x4B295BBB47F4F9C9ULL;
inline constexpr std::uint64_t kWirehairV2MinimumBlockCount = 2;
inline constexpr std::uint64_t kWirehairV2MaximumBlockCount = 64000;

[[nodiscard]] ProtocolStatus ValidateReceiverResourcePolicy(
    const ReceiverResourcePolicy& resourcePolicy) noexcept;

[[nodiscard]] ProtocolStatus ValidateFileNameUtf8(
    std::string_view fileNameUtf8,
    std::size_t fieldOffset = 0) noexcept;

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

[[nodiscard]] ProtocolResult<std::size_t> GetSerializedSize(
    const SessionDescriptor& descriptor) noexcept;

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
