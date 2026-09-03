#include "pbprotocol/descriptor_codec.h"

#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/byte_io.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/crc32c.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pbprotocol {

namespace {

constexpr std::size_t kSessionProtocolMajorOffset = kFormalWireSessionDescriptorProtocolMajorOffset;
constexpr std::size_t kSessionProtocolMinorOffset = kFormalWireSessionDescriptorProtocolMinorOffset;
constexpr std::size_t kSessionIdOffset = kFormalWireSessionDescriptorSessionIdOffset;
constexpr std::size_t kSessionVisualProfileIdOffset = kFormalWireSessionDescriptorVisualProfileIdOffset;
constexpr std::size_t kSessionOriginalFileSizeOffset = kFormalWireSessionDescriptorOriginalFileSizeOffset;
constexpr std::size_t kSessionSourceSegmentTargetBytesOffset = kFormalWireSessionDescriptorSourceSegmentTargetBytesOffset;
constexpr std::size_t kSessionSegmentCountOffset = kFormalWireSessionDescriptorSegmentCountOffset;
constexpr std::size_t kSessionCompressionPolicyOffset = kFormalWireSessionDescriptorCompressionPolicyOffset;
constexpr std::size_t kSessionDigestAlgorithmOffset = kFormalWireSessionDescriptorDigestAlgorithmOffset;
constexpr std::size_t kSessionFeatureFlagsOffset = kFormalWireSessionDescriptorFeatureFlagsOffset;
constexpr std::size_t kSessionFileNameUtf8BytesOffset = kFormalWireSessionDescriptorFileNameUtf8BytesOffset;
constexpr std::size_t kSessionFileNameUtf8Offset = kFormalWireSessionDescriptorFileNameUtf8Offset;
constexpr std::size_t kSegmentSessionTagOffset = kFormalWireSegmentDescriptorSessionTagOffset;
constexpr std::size_t kSegmentOrdinalOffset = kFormalWireSegmentDescriptorOrdinalOffset;
constexpr std::size_t kSegmentRawOffsetOffset = kFormalWireSegmentDescriptorRawOffsetOffset;
constexpr std::size_t kSegmentRawSizeOffset = kFormalWireSegmentDescriptorRawSizeOffset;
constexpr std::size_t kSegmentEncodedSizeOffset = kFormalWireSegmentDescriptorEncodedSizeOffset;
constexpr std::size_t kSegmentCompressionCodecOffset = kFormalWireSegmentDescriptorCompressionCodecOffset;
constexpr std::size_t kSegmentOuterFecModeOffset = kFormalWireSegmentDescriptorOuterFecModeOffset;
constexpr std::size_t kSegmentOuterBlockBytesOffset = kFormalWireSegmentDescriptorOuterBlockBytesOffset;
constexpr std::size_t kSegmentRawDigestOffset = kFormalWireSegmentDescriptorRawDigestOffset;
constexpr std::size_t kSegmentEncodedDigestOffset = kFormalWireSegmentDescriptorEncodedDigestOffset;
constexpr std::size_t kSegmentFlagsOffset = kFormalWireSegmentDescriptorFlagsOffset;
constexpr std::size_t kSegmentWirehairProfileOffset = kFormalWireSegmentDescriptorWirehairProfileOffset;
constexpr std::size_t kFinalSessionIdOffset = kFormalWireFinalManifestSessionIdOffset;
constexpr std::size_t kFinalOriginalFileSizeOffset = kFormalWireFinalManifestOriginalFileSizeOffset;
constexpr std::size_t kFinalSegmentCountOffset = kFormalWireFinalManifestSegmentCountOffset;
constexpr std::size_t kFinalWholeFileDigestOffset = kFormalWireFinalManifestWholeFileDigestOffset;
constexpr std::size_t kFinalDigestAlgorithmOffset = kFormalWireFinalManifestDigestAlgorithmOffset;

constexpr std::array<std::byte, 4> kWirehairMagic{
    static_cast<std::byte>('W'),
    static_cast<std::byte>('H'),
    static_cast<std::byte>('V'),
    static_cast<std::byte>('2')};
constexpr std::uint16_t kWirehairEncodingVersion = 1;
constexpr std::uint16_t kWirehairEncodedBytes = 32;

[[nodiscard]] ProtocolStatus ValidateDigestAlgorithm(
    const DigestAlgorithm digestAlgorithm,
    const std::size_t fieldOffset) noexcept
{
    if (digestAlgorithm != DigestAlgorithm::Blake3_256)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidEnumValue,
            fieldOffset);
    }

    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus ValidateCompressionCodec(
    const CompressionCodec compressionCodec,
    const std::size_t fieldOffset) noexcept
{
    switch (compressionCodec)
    {
    case CompressionCodec::Raw:
    case CompressionCodec::Zstandard:
        return ProtocolStatus::Success();
    default:
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidEnumValue,
            fieldOffset);
    }
}

[[nodiscard]] ProtocolStatus ValidateOuterFecMode(
    const OuterFecMode outerFecMode,
    const std::size_t fieldOffset) noexcept
{
    switch (outerFecMode)
    {
    case OuterFecMode::WirehairV2:
    case OuterFecMode::DirectRepeat:
        return ProtocolStatus::Success();
    default:
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidEnumValue,
            fieldOffset);
    }
}

[[nodiscard]] ProtocolStatus ValidateSessionShape(
    const std::uint64_t originalFileSize,
    const std::uint64_t segmentCount,
    const std::size_t segmentCountOffset) noexcept
{
    if (originalFileSize == 0)
    {
        if (segmentCount != 0)
        {
            return ProtocolStatus::Failure(
                ProtocolErrorCode::InvalidDescriptor,
                segmentCountOffset);
        }
        return ProtocolStatus::Success();
    }

    if (segmentCount == 0 || segmentCount > originalFileSize)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidDescriptor,
            segmentCountOffset);
    }

    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus WirehairProfileFailureAt(
    const std::size_t fieldOffset,
    const std::size_t relativeOffset) noexcept
{
    const auto offsetResult = CheckedAddSize(
        fieldOffset,
        relativeOffset,
        fieldOffset);
    if (!offsetResult)
    {
        return ProtocolStatus::Failure(
            offsetResult.Error().code,
            offsetResult.Error().offset);
    }

    return ProtocolStatus::Failure(
        ProtocolErrorCode::InvalidWirehairProfile,
        offsetResult.Value());
}

[[nodiscard]] ProtocolStatus ValidateResourceLimits(
    const std::uint64_t originalFileSize,
    const std::uint64_t segmentCount,
    const ReceiverResourcePolicy& resourcePolicy,
    const std::size_t originalFileSizeOffset,
    const std::size_t segmentCountOffset) noexcept
{
    if (originalFileSize > resourcePolicy.maxAcceptedFileBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            originalFileSizeOffset);
    }

    if (segmentCount > resourcePolicy.maxSegmentCount)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            segmentCountOffset);
    }

    if (originalFileSize != 0)
    {
        const std::uint64_t minimumSegmentCount = 1ULL +
            ((originalFileSize - 1ULL) / resourcePolicy.maxRawSegmentBytes);
        if (segmentCount < minimumSegmentCount)
        {
            return ProtocolStatus::Failure(
                ProtocolErrorCode::ResourceLimitExceeded,
                segmentCountOffset);
        }
    }

    const auto countSizeResult = CheckedUint64ToSize(
        segmentCount,
        segmentCountOffset);
    if (!countSizeResult)
    {
        return ProtocolStatus::Failure(
            countSizeResult.Error().code,
            countSizeResult.Error().offset);
    }

    return ProtocolStatus::Success();
}

template <typename ValueType>
[[nodiscard]] ProtocolResult<ValueType> FailureFrom(
    const ProtocolError& error) noexcept
{
    return ProtocolResult<ValueType>::Failure(error.code, error.offset);
}

[[nodiscard]] ProtocolStatus CopyStatusFailure(
    const ProtocolError& error) noexcept
{
    return ProtocolStatus::Failure(error.code, error.offset);
}

[[nodiscard]] ProtocolStatus OffsetStatus(
    const ProtocolError& error,
    const std::size_t baseOffset) noexcept
{
    const auto offsetResult = CheckedAddSize(baseOffset, error.offset, baseOffset);
    if (!offsetResult)
    {
        return ProtocolStatus::Failure(offsetResult.Error().code, offsetResult.Error().offset);
    }
    return ProtocolStatus::Failure(error.code, offsetResult.Value());
}

[[nodiscard]] ProtocolStatus ValidateOptionalExtensions(
    const std::span<const std::byte> extensions,
    const std::size_t baseOffset) noexcept
{
    ByteReader reader(extensions);
    std::uint16_t previousType = 0;
    bool hasPreviousType = false;
    while (reader.Remaining() != 0)
    {
        const std::size_t extensionOffset = reader.Position();
        if (reader.Remaining() < kDescriptorTlvHeaderBytes)
        {
            return ProtocolStatus::Failure(ProtocolErrorCode::TruncatedInput, baseOffset + extensionOffset);
        }

        const auto typeResult = reader.ReadUint16();
        const auto flagsResult = reader.ReadUint16();
        const auto lengthResult = reader.ReadUint32();
        if (!typeResult || !flagsResult || !lengthResult)
        {
            const ProtocolError& error = !typeResult ? typeResult.Error() : (!flagsResult ? flagsResult.Error() : lengthResult.Error());
            return OffsetStatus(error, baseOffset);
        }
        if (typeResult.Value() == 0 || (hasPreviousType && typeResult.Value() <= previousType))
        {
            return ProtocolStatus::Failure(ProtocolErrorCode::InvalidDescriptor, baseOffset + extensionOffset);
        }
        if ((flagsResult.Value() & ~kDescriptorTlvKnownFlags) != 0)
        {
            return ProtocolStatus::Failure(ProtocolErrorCode::NonZeroReservedBits, baseOffset + extensionOffset + 2);
        }
        if ((flagsResult.Value() & kDescriptorTlvOptionalFlag) == 0)
        {
            return ProtocolStatus::Failure(ProtocolErrorCode::UnknownMandatoryFeature, baseOffset + extensionOffset);
        }
        const auto valueResult = reader.ReadBytes(lengthResult.Value());
        if (!valueResult)
        {
            return OffsetStatus(valueResult.Error(), baseOffset);
        }
        previousType = typeResult.Value();
        hasPreviousType = true;
    }
    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus ValidateDescriptorEnvelope(
    const std::span<const std::byte> input,
    const std::uint16_t expectedHeaderBytes,
    const std::size_t minimumBytes,
    const std::size_t maximumBytes) noexcept
{
    ByteReader reader(input);
    const auto schemaResult = reader.ReadUint16();
    if (!schemaResult)
    {
        return CopyStatusFailure(schemaResult.Error());
    }
    const auto headerBytesResult = reader.ReadUint16();
    if (!headerBytesResult)
    {
        return CopyStatusFailure(headerBytesResult.Error());
    }
    const auto totalBytesResult = reader.ReadUint32();
    if (!totalBytesResult)
    {
        return CopyStatusFailure(totalBytesResult.Error());
    }
    if (schemaResult.Value() != kDescriptorSchemaVersion)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::UnsupportedDescriptorSchema, kDescriptorSchemaVersionOffset);
    }
    if (headerBytesResult.Value() != expectedHeaderBytes)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::UnsupportedDescriptorSchema, kDescriptorHeaderBytesOffset);
    }
    if (input.size() < minimumBytes || input.size() > maximumBytes || totalBytesResult.Value() != input.size())
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidRecordSize, kDescriptorTotalBytesOffset);
    }

    const std::size_t crcOffset = input.size() - kDescriptorCrcBytes;
    ByteReader crcReader(input.subspan(crcOffset));
    const auto storedCrcResult = crcReader.ReadUint32();
    if (!storedCrcResult)
    {
        return OffsetStatus(storedCrcResult.Error(), crcOffset);
    }
    if (storedCrcResult.Value() != ComputeCrc32c(input.first(crcOffset)))
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::CrcMismatch, crcOffset);
    }
    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus WriteDescriptorPrefix(
    ByteWriter& writer,
    const std::uint16_t headerBytes,
    const std::uint32_t totalBytes) noexcept
{
    ProtocolStatus status = writer.WriteUint16(kDescriptorSchemaVersion);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint16(headerBytes);
    if (!status)
    {
        return status;
    }
    return writer.WriteUint32(totalBytes);
}

template <typename WriteFunction>
[[nodiscard]] ProtocolStatus SerializeAtomically(
    const std::size_t expectedBytes,
    const std::span<std::byte> output,
    WriteFunction&& writeFunction) noexcept
{
    if (expectedBytes > kMaximumDescriptorPayloadBytes || output.size() != expectedBytes)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidRecordSize, 0);
    }

    std::array<std::byte, kMaximumDescriptorPayloadBytes> scratch{};
    ByteWriter writer(std::span<std::byte>(scratch).first(expectedBytes));
    const ProtocolStatus writeStatus = writeFunction(writer);
    if (!writeStatus)
    {
        return writeStatus;
    }
    if (writer.Position() != expectedBytes - kDescriptorCrcBytes)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InternalDescriptorStateError, writer.Position());
    }
    const std::uint32_t crc = ComputeCrc32c(std::span<const std::byte>(scratch).first(writer.Position()));
    const ProtocolStatus crcStatus = writer.WriteUint32(crc);
    if (!crcStatus)
    {
        return crcStatus;
    }
    if (writer.Position() != expectedBytes)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InternalDescriptorStateError, writer.Position());
    }

    std::copy_n(scratch.begin(), expectedBytes, output.begin());
    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus WriteSessionDescriptor(
    const SessionDescriptor& descriptor,
    const std::uint32_t totalBytes,
    ByteWriter& writer) noexcept
{
    ProtocolStatus status = WriteDescriptorPrefix(writer, static_cast<std::uint16_t>(kSessionDescriptorHeaderBytes), totalBytes);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint16(descriptor.protocolVersion.major);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint16(descriptor.protocolVersion.minor);
    if (!status)
    {
        return status;
    }
    status = writer.WriteFixedBytes(descriptor.sessionId.bytes);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(descriptor.sessionVisualProfileId);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(descriptor.originalFileSize);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint32(descriptor.sourceSegmentTargetBytes);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(descriptor.segmentCount);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(static_cast<std::underlying_type_t<CompressionPolicy>>(descriptor.compressionPolicy));
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(static_cast<std::underlying_type_t<DigestAlgorithm>>(descriptor.digestAlgorithm));
    if (!status)
    {
        return status;
    }
    status = writer.WriteCanonicalZeroPadding(2);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(descriptor.featureFlags);
    if (!status)
    {
        return status;
    }
    const std::span<const std::byte> fileNameBytes(reinterpret_cast<const std::byte*>(descriptor.fileNameUtf8.data()), descriptor.fileNameUtf8.size());
    status = writer.WriteUint16(static_cast<std::uint16_t>(fileNameBytes.size()));
    if (!status)
    {
        return status;
    }
    status = writer.WriteBytes(fileNameBytes);
    if (!status)
    {
        return status;
    }
    return writer.WriteBytes(descriptor.optionalExtensions);
}

[[nodiscard]] ProtocolStatus WriteSegmentDescriptor(
    const SegmentDescriptor& descriptor,
    const std::uint32_t totalBytes,
    ByteWriter& writer) noexcept
{
    ProtocolStatus status = WriteDescriptorPrefix(writer, kSegmentDescriptorHeaderBytes, totalBytes);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(descriptor.sessionTag.value);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(descriptor.segmentOrdinal);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(descriptor.rawOffset);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(descriptor.rawSize);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(descriptor.encodedSize);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(static_cast<std::underlying_type_t<CompressionCodec>>(descriptor.compressionCodec));
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(static_cast<std::underlying_type_t<OuterFecMode>>(descriptor.outerFecMode));
    if (!status)
    {
        return status;
    }
    status = writer.WriteCanonicalZeroPadding(2);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint32(descriptor.outerBlockBytes);
    if (!status)
    {
        return status;
    }
    status = writer.WriteFixedBytes(descriptor.rawDigest.bytes);
    if (!status)
    {
        return status;
    }
    status = writer.WriteFixedBytes(descriptor.encodedDigest.bytes);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(descriptor.flags);
    if (!status)
    {
        return status;
    }
    if (descriptor.outerFecMode == OuterFecMode::WirehairV2)
    {
        if (!descriptor.wirehairV2SerializedProfile.has_value())
        {
            return ProtocolStatus::Failure(ProtocolErrorCode::InternalDescriptorStateError, kSegmentWirehairProfileOffset);
        }
        return writer.WriteFixedBytes(descriptor.wirehairV2SerializedProfile->bytes);
    }
    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus WriteFinalManifest(
    const FinalManifest& finalManifest,
    ByteWriter& writer) noexcept
{
    ProtocolStatus status = WriteDescriptorPrefix(writer, kFinalManifestHeaderBytes, static_cast<std::uint32_t>(kFinalManifestPayloadBytes));
    if (!status)
    {
        return status;
    }
    status = writer.WriteFixedBytes(finalManifest.sessionId.bytes);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(finalManifest.originalFileSize);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(finalManifest.segmentCount);
    if (!status)
    {
        return status;
    }
    status = writer.WriteFixedBytes(finalManifest.wholeFileDigest.bytes);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(static_cast<std::underlying_type_t<DigestAlgorithm>>(finalManifest.digestAlgorithm));
    if (!status)
    {
        return status;
    }
    return writer.WriteCanonicalZeroPadding(7);
}

} // namespace

ProtocolStatus ValidateFileNameUtf8(
    const std::string_view fileNameUtf8,
    const std::size_t fieldOffset) noexcept
{
    if (fileNameUtf8.empty() || fileNameUtf8.size() > kMaximumFileNameUtf8Bytes)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidFileName, fieldOffset);
    }
    const ProtocolStatus utf8Status = ValidateUtf8(fileNameUtf8, fieldOffset);
    if (!utf8Status)
    {
        return utf8Status;
    }
    if (fileNameUtf8 == "." || fileNameUtf8 == ".." || fileNameUtf8.back() == '.' || fileNameUtf8.back() == ' ')
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidFileName, fieldOffset);
    }

    constexpr std::string_view invalidCharacters = "<>:\"/\\|?*";
    for (std::size_t byteIndex = 0; byteIndex < fileNameUtf8.size(); byteIndex++)
    {
        const unsigned char byteValue = static_cast<unsigned char>(fileNameUtf8[byteIndex]);
        if (byteValue < 0x20U || invalidCharacters.find(fileNameUtf8[byteIndex]) != std::string_view::npos)
        {
            return ProtocolStatus::Failure(ProtocolErrorCode::InvalidFileName, fieldOffset + byteIndex);
        }
    }

    const std::size_t dotOffset = fileNameUtf8.find('.');
    const std::string_view deviceStem = fileNameUtf8.substr(0, dotOffset);
    const auto equalsAsciiIgnoreCase = [deviceStem](const std::string_view candidate) noexcept
    {
        if (deviceStem.size() != candidate.size())
        {
            return false;
        }
        for (std::size_t characterIndex = 0; characterIndex < deviceStem.size(); characterIndex++)
        {
            const unsigned char left = static_cast<unsigned char>(deviceStem[characterIndex]);
            const unsigned char right = static_cast<unsigned char>(candidate[characterIndex]);
            if (std::toupper(left) != std::toupper(right))
            {
                return false;
            }
        }
        return true;
    };
    if (equalsAsciiIgnoreCase("CON") || equalsAsciiIgnoreCase("PRN") || equalsAsciiIgnoreCase("AUX") ||
        equalsAsciiIgnoreCase("NUL") || equalsAsciiIgnoreCase("CONIN$") || equalsAsciiIgnoreCase("CONOUT$"))
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidFileName, fieldOffset);
    }
    if (deviceStem.size() == 4)
    {
        const char digit = deviceStem[3];
        const bool hasReservedDigit = digit >= '1' && digit <= '9';
        const std::string_view prefix = deviceStem.substr(0, 3);
        const auto prefixEquals = [prefix](const std::string_view candidate) noexcept
        {
            if (prefix.size() != candidate.size())
            {
                return false;
            }
            for (std::size_t characterIndex = 0; characterIndex < prefix.size(); characterIndex++)
            {
                const unsigned char left = static_cast<unsigned char>(prefix[characterIndex]);
                const unsigned char right = static_cast<unsigned char>(candidate[characterIndex]);
                if (std::toupper(left) != std::toupper(right))
                {
                    return false;
                }
            }
            return true;
        };
        if (hasReservedDigit && (prefixEquals("COM") || prefixEquals("LPT")))
        {
            return ProtocolStatus::Failure(ProtocolErrorCode::InvalidFileName, fieldOffset);
        }
    }
    return ProtocolStatus::Success();
}

ProtocolStatus ValidateReceiverResourcePolicy(
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    constexpr std::uint64_t maximumUint64 =
        std::numeric_limits<std::uint64_t>::max();
    if (resourcePolicy.maxAcceptedFileBytes == 0 ||
        resourcePolicy.maxSegmentCount == 0 ||
        resourcePolicy.maxRawSegmentBytes == 0 ||
        resourcePolicy.maxEncodedSegmentBytes == 0 ||
        resourcePolicy.maxOuterBlockBytes == 0 ||
        resourcePolicy.maxDescriptorStateBytes == 0 ||
        resourcePolicy.maxConcurrentSessions == 0 ||
        resourcePolicy.maxTotalDescriptorStateBytes == 0 ||
        resourcePolicy.maxDirectRepeatBlockCount == 0 ||
        resourcePolicy.maxActiveOuterFecDecoders == 0 ||
        resourcePolicy.maxOuterFecDecoderBytes == 0 ||
        resourcePolicy.maxTotalOuterFecDecoderBytes == 0 ||
        resourcePolicy.maxControlRecordBytes == 0 ||
        resourcePolicy.maxConcurrentControlReassemblies == 0 ||
        resourcePolicy.maxControlReassemblyBytes == 0 ||
        resourcePolicy.maxControlFragmentsPerRecord == 0 ||
        resourcePolicy.maxControlReassemblyInactivityObservations == 0 ||
        resourcePolicy.maxOrphanTransportBytes == 0 ||
        resourcePolicy.maxOrphanTransportBlocks == 0 ||
        resourcePolicy.maxZstdWindowBytes == 0 ||
        resourcePolicy.maxResumeBytes == 0 ||
        resourcePolicy.maxOutputPreallocationBytesWithoutPrompt == 0 ||
        resourcePolicy.maxAcceptedFileBytes == maximumUint64 ||
        resourcePolicy.maxSegmentCount == maximumUint64 ||
        resourcePolicy.maxRawSegmentBytes == maximumUint64 ||
        resourcePolicy.maxEncodedSegmentBytes == maximumUint64 ||
        resourcePolicy.maxOuterBlockBytes >
            kMaximumTransportPayloadBytes ||
        resourcePolicy.maxDescriptorStateBytes == maximumUint64 ||
        resourcePolicy.maxConcurrentSessions == maximumUint64 ||
        resourcePolicy.maxTotalDescriptorStateBytes == maximumUint64 ||
        resourcePolicy.maxDirectRepeatBlockCount >
            kMaximumRepresentableDirectRepeatBlockCount ||
        resourcePolicy.maxActiveOuterFecDecoders == maximumUint64 ||
        resourcePolicy.maxOuterFecDecoderBytes == maximumUint64 ||
        resourcePolicy.maxTotalOuterFecDecoderBytes == maximumUint64 ||
        resourcePolicy.maxControlRecordBytes < kMinimumControlRecordBytes ||
        resourcePolicy.maxControlRecordBytes > kMaximumControlRecordBytes ||
        resourcePolicy.maxConcurrentControlReassemblies == maximumUint64 ||
        resourcePolicy.maxControlReassemblyBytes == maximumUint64 ||
        resourcePolicy.maxControlFragmentsPerRecord > UINT16_MAX ||
        resourcePolicy.maxControlReassemblyInactivityObservations ==
            maximumUint64 ||
        resourcePolicy.maxOrphanTransportBytes == maximumUint64 ||
        resourcePolicy.maxOrphanTransportBlocks == maximumUint64 ||
        resourcePolicy.maxZstdWindowBytes == maximumUint64 ||
        resourcePolicy.maxResumeBytes == maximumUint64 ||
        resourcePolicy.maxOutputPreallocationBytesWithoutPrompt ==
            maximumUint64 ||
        resourcePolicy.maxDescriptorStateBytes >
            resourcePolicy.maxTotalDescriptorStateBytes ||
        resourcePolicy.maxOuterFecDecoderBytes >
            resourcePolicy.maxTotalOuterFecDecoderBytes ||
        resourcePolicy.maxControlRecordBytes >
            resourcePolicy.maxControlReassemblyBytes ||
        resourcePolicy.maxControlFragmentsPerRecord >
            resourcePolicy.maxControlRecordBytes ||
        resourcePolicy.maxOutputPreallocationBytesWithoutPrompt >
            resourcePolicy.maxAcceptedFileBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidResourcePolicy,
            0);
    }

    const auto segmentCountSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxSegmentCount);
    const auto rawSegmentSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxRawSegmentBytes);
    const auto encodedSegmentSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxEncodedSegmentBytes);
    const auto descriptorBudgetSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxDescriptorStateBytes);
    const auto concurrentSessionCountResult = CheckedUint64ToSize(
        resourcePolicy.maxConcurrentSessions);
    const auto activeOuterFecDecoderCountResult = CheckedUint64ToSize(
        resourcePolicy.maxActiveOuterFecDecoders);
    const auto directRepeatBlockCountResult = CheckedUint64ToSize(
        resourcePolicy.maxDirectRepeatBlockCount);
    const auto outerFecDecoderBudgetSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxOuterFecDecoderBytes);
    const auto controlRecordSizeResult = CheckedNarrowUnsigned<std::size_t>(
        resourcePolicy.maxControlRecordBytes);
    const auto concurrentControlCountResult = CheckedUint64ToSize(
        resourcePolicy.maxConcurrentControlReassemblies);
    const auto controlReassemblyBudgetSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxControlReassemblyBytes);
    const auto controlFragmentCountResult = CheckedUint64ToSize(
        resourcePolicy.maxControlFragmentsPerRecord);
    const auto controlInactivityObservationResult = CheckedUint64ToSize(
        resourcePolicy.maxControlReassemblyInactivityObservations);
    const auto orphanTransportBudgetSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxOrphanTransportBytes);
    const auto orphanTransportBlockCountResult = CheckedUint64ToSize(
        resourcePolicy.maxOrphanTransportBlocks);
    const auto zstdWindowBudgetSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxZstdWindowBytes);
    const auto resumeStateBudgetSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxResumeBytes);
    const auto outputPreallocationPromptSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxOutputPreallocationBytesWithoutPrompt);
    if (!segmentCountSizeResult ||
        !rawSegmentSizeResult ||
        !encodedSegmentSizeResult ||
        !descriptorBudgetSizeResult ||
        !concurrentSessionCountResult ||
        !directRepeatBlockCountResult ||
        !activeOuterFecDecoderCountResult ||
        !outerFecDecoderBudgetSizeResult ||
        !controlRecordSizeResult ||
        !concurrentControlCountResult ||
        !controlReassemblyBudgetSizeResult ||
        !controlFragmentCountResult ||
        !controlInactivityObservationResult ||
        !orphanTransportBudgetSizeResult ||
        !orphanTransportBlockCountResult ||
        !zstdWindowBudgetSizeResult ||
        !resumeStateBudgetSizeResult ||
        !outputPreallocationPromptSizeResult)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidResourcePolicy,
            0);
    }

    return ProtocolStatus::Success();
}

ProtocolStatus ValidateSessionDescriptor(
    const SessionDescriptor& descriptor) noexcept
{
    const ProtocolVersion currentVersion = GetProtocolVersion();
    if (descriptor.protocolVersion.major != currentVersion.major)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::UnsupportedProtocolMajor,
            kSessionProtocolMajorOffset);
    }
    if (descriptor.protocolVersion.minor != currentVersion.minor)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::UnsupportedProtocolMinor,
            kSessionProtocolMinorOffset);
    }

    const ProtocolStatus digestStatus = ValidateDigestAlgorithm(
        descriptor.digestAlgorithm,
        kSessionDigestAlgorithmOffset);
    if (!digestStatus)
    {
        return digestStatus;
    }

    // PBProtocol validates the self-describing wire shape. Product/profile
    // availability belongs to the application binding so historical internal
    // comparison profiles can still emit formal descriptors without teaching
    // this Qt-free protocol layer about PBModulation constants.
    if (descriptor.sessionVisualProfileId == 0)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidDescriptor, kSessionVisualProfileIdOffset);
    }
    if (descriptor.sourceSegmentTargetBytes == 0)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidDescriptor, kSessionSourceSegmentTargetBytesOffset);
    }
    if (descriptor.compressionPolicy != CompressionPolicy::AutomaticZstandardLevel3RawFallback)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidEnumValue, kSessionCompressionPolicyOffset);
    }
    if ((descriptor.featureFlags & kSessionMandatoryFeatureMask) != 0)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::UnknownMandatoryFeature, kSessionFeatureFlagsOffset);
    }
    const ProtocolStatus fileNameStatus = ValidateFileNameUtf8(descriptor.fileNameUtf8, kSessionFileNameUtf8Offset);
    if (!fileNameStatus)
    {
        return fileNameStatus;
    }
    const ProtocolStatus extensionStatus = ValidateOptionalExtensions(
        descriptor.optionalExtensions,
        kSessionFileNameUtf8Offset + descriptor.fileNameUtf8.size());
    if (!extensionStatus)
    {
        return extensionStatus;
    }
    const auto serializedSizeResult = GetSerializedSize(descriptor);
    if (!serializedSizeResult)
    {
        return CopyStatusFailure(serializedSizeResult.Error());
    }

    return ValidateSessionShape(
        descriptor.originalFileSize,
        descriptor.segmentCount,
        kSessionSegmentCountOffset);
}

ProtocolStatus ValidateSessionDescriptor(
    const SessionDescriptor& descriptor,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    const ProtocolStatus policyStatus = ValidateReceiverResourcePolicy(
        resourcePolicy);
    if (!policyStatus)
    {
        return policyStatus;
    }

    const ProtocolStatus descriptorStatus = ValidateSessionDescriptor(descriptor);
    if (!descriptorStatus)
    {
        return descriptorStatus;
    }

    return ValidateResourceLimits(
        descriptor.originalFileSize,
        descriptor.segmentCount,
        resourcePolicy,
        kSessionOriginalFileSizeOffset,
        kSessionSegmentCountOffset);
}

ProtocolStatus ValidateWirehairV2SerializedProfile(
    const WirehairV2SerializedProfile& profile,
    const std::uint64_t expectedMessageBytes,
    const std::uint32_t expectedBlockBytes,
    const std::size_t fieldOffset) noexcept
{
    ByteReader reader(profile.bytes);

    const auto magicResult = reader.ReadFixedBytes<4>();
    const auto versionResult = reader.ReadUint16();
    const auto encodedBytesResult = reader.ReadUint16();
    const auto profileIdResult = reader.ReadUint64();
    const auto messageBytesResult = reader.ReadUint64();
    const auto blockBytesResult = reader.ReadUint32();
    const auto seedAttemptResult = reader.ReadUint8();
    const ProtocolStatus reservedStatus = reader.ReadReservedZeroBytes(3);

    if (!magicResult || !versionResult || !encodedBytesResult ||
        !profileIdResult || !messageBytesResult || !blockBytesResult ||
        !seedAttemptResult)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalDescriptorStateError,
            fieldOffset);
    }
    if (!reservedStatus)
    {
        return WirehairProfileFailureAt(
            fieldOffset,
            reservedStatus.Error().offset);
    }
    const ProtocolStatus consumedStatus = reader.RequireFullyConsumed();
    if (!consumedStatus)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalDescriptorStateError,
            fieldOffset);
    }

    if (magicResult.Value() != kWirehairMagic)
    {
        return WirehairProfileFailureAt(fieldOffset, 0);
    }
    if (versionResult.Value() != kWirehairEncodingVersion)
    {
        return WirehairProfileFailureAt(fieldOffset, 4);
    }
    if (encodedBytesResult.Value() != kWirehairEncodedBytes)
    {
        return WirehairProfileFailureAt(fieldOffset, 6);
    }
    if (profileIdResult.Value() != kWirehairV2CertifiedProfileId)
    {
        return WirehairProfileFailureAt(fieldOffset, 8);
    }
    if (messageBytesResult.Value() != expectedMessageBytes ||
        messageBytesResult.Value() == 0)
    {
        return WirehairProfileFailureAt(fieldOffset, 16);
    }
    if (blockBytesResult.Value() != expectedBlockBytes ||
        blockBytesResult.Value() == 0 ||
        blockBytesResult.Value() > kMaximumTransportPayloadBytes)
    {
        return WirehairProfileFailureAt(fieldOffset, 24);
    }

    const std::uint64_t blockCount =
        1ULL + ((messageBytesResult.Value() - 1ULL) / blockBytesResult.Value());
    if (blockCount < kWirehairV2MinimumBlockCount ||
        blockCount > kWirehairV2MaximumBlockCount)
    {
        return WirehairProfileFailureAt(fieldOffset, 16);
    }

    return ProtocolStatus::Success();
}

ProtocolResult<std::uint64_t> GetDirectRepeatBlockCount(
    const std::uint64_t encodedSize,
    const std::uint32_t outerBlockBytes,
    const std::size_t fieldOffset) noexcept
{
    if (outerBlockBytes == 0 ||
        outerBlockBytes > kMaximumTransportPayloadBytes)
    {
        return ProtocolResult<std::uint64_t>::Failure(
            ProtocolErrorCode::InvalidDescriptor,
            fieldOffset);
    }
    if (encodedSize == 0)
    {
        return ProtocolResult<std::uint64_t>::Success(0);
    }

    const std::uint64_t blockCount =
        1ULL + ((encodedSize - 1ULL) / outerBlockBytes);
    if (blockCount > kMaximumRepresentableDirectRepeatBlockCount)
    {
        return ProtocolResult<std::uint64_t>::Failure(
            ProtocolErrorCode::LengthOverflow,
            fieldOffset);
    }

    return ProtocolResult<std::uint64_t>::Success(blockCount);
}

namespace {

[[nodiscard]] ProtocolStatus ValidateSegmentDescriptorImplementation(
    const SegmentDescriptor& descriptor,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy* const resourcePolicy) noexcept
{
    const ProtocolStatus sessionStatus = resourcePolicy == nullptr
        ? ValidateSessionDescriptor(sessionDescriptor)
        : ValidateSessionDescriptor(sessionDescriptor, *resourcePolicy);
    if (!sessionStatus)
    {
        return sessionStatus;
    }

    if (descriptor.sessionTag != DeriveSessionTag(sessionDescriptor.sessionId))
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::SessionTagMismatch,
            kSegmentSessionTagOffset);
    }
    if (descriptor.segmentOrdinal >= sessionDescriptor.segmentCount)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::SegmentOrdinalOutOfRange,
            kSegmentOrdinalOffset);
    }
    if (descriptor.rawSize == 0)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidDescriptor,
            kSegmentRawSizeOffset);
    }
    if (descriptor.encodedSize == 0)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidDescriptor,
            kSegmentEncodedSizeOffset);
    }
    if (resourcePolicy != nullptr &&
        descriptor.rawSize > resourcePolicy->maxRawSegmentBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kSegmentRawSizeOffset);
    }
    if (resourcePolicy != nullptr &&
        descriptor.encodedSize > resourcePolicy->maxEncodedSegmentBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kSegmentEncodedSizeOffset);
    }
    if (descriptor.outerBlockBytes == 0 ||
        descriptor.outerBlockBytes > kMaximumTransportPayloadBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidDescriptor,
            kSegmentOuterBlockBytesOffset);
    }
    if (resourcePolicy != nullptr &&
        descriptor.outerBlockBytes > resourcePolicy->maxOuterBlockBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kSegmentOuterBlockBytesOffset);
    }

    const auto rawEndResult = CheckedAddUint64(
        descriptor.rawOffset,
        descriptor.rawSize,
        kSegmentRawOffsetOffset);
    if (!rawEndResult)
    {
        return CopyStatusFailure(rawEndResult.Error());
    }
    if (rawEndResult.Value() > sessionDescriptor.originalFileSize)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::SegmentRangeOutOfBounds,
            kSegmentRawOffsetOffset);
    }

    const ProtocolStatus compressionStatus = ValidateCompressionCodec(
        descriptor.compressionCodec,
        kSegmentCompressionCodecOffset);
    if (!compressionStatus)
    {
        return compressionStatus;
    }
    if (descriptor.compressionCodec == CompressionCodec::Raw &&
        descriptor.encodedSize != descriptor.rawSize)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidDescriptor,
            kSegmentEncodedSizeOffset);
    }
    if (descriptor.compressionCodec == CompressionCodec::Raw &&
        descriptor.encodedDigest.bytes != descriptor.rawDigest.bytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidDescriptor,
            kSegmentEncodedDigestOffset);
    }

    const ProtocolStatus outerFecStatus = ValidateOuterFecMode(
        descriptor.outerFecMode,
        kSegmentOuterFecModeOffset);
    if (!outerFecStatus)
    {
        return outerFecStatus;
    }
    if (descriptor.flags != 0)
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::UnknownMandatoryFeature, kSegmentFlagsOffset);
    }

    if (descriptor.outerFecMode == OuterFecMode::WirehairV2)
    {
        if (!descriptor.wirehairV2SerializedProfile.has_value())
        {
            return ProtocolStatus::Failure(
                ProtocolErrorCode::InvalidDescriptor,
                kSegmentWirehairProfileOffset);
        }
        return ValidateWirehairV2SerializedProfile(
            *descriptor.wirehairV2SerializedProfile,
            descriptor.encodedSize,
            descriptor.outerBlockBytes,
            kSegmentWirehairProfileOffset);
    }

    if (descriptor.wirehairV2SerializedProfile.has_value())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidDescriptor,
            kSegmentWirehairProfileOffset);
    }

    const auto directBlockCountResult = GetDirectRepeatBlockCount(
        descriptor.encodedSize,
        descriptor.outerBlockBytes,
        kSegmentOuterBlockBytesOffset);
    if (!directBlockCountResult)
    {
        return CopyStatusFailure(directBlockCountResult.Error());
    }
    if (resourcePolicy != nullptr &&
        directBlockCountResult.Value() >
            resourcePolicy->maxDirectRepeatBlockCount)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kSegmentOuterBlockBytesOffset);
    }

    return ProtocolStatus::Success();
}

} // namespace

ProtocolStatus ValidateSegmentDescriptor(
    const SegmentDescriptor& descriptor,
    const SessionDescriptor& sessionDescriptor) noexcept
{
    return ValidateSegmentDescriptorImplementation(
        descriptor,
        sessionDescriptor,
        nullptr);
}

ProtocolStatus ValidateSegmentDescriptor(
    const SegmentDescriptor& descriptor,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    return ValidateSegmentDescriptorImplementation(
        descriptor,
        sessionDescriptor,
        &resourcePolicy);
}

namespace {

[[nodiscard]] ProtocolStatus ValidateFinalManifestImplementation(
    const FinalManifest& finalManifest,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy* const resourcePolicy) noexcept
{
    const ProtocolStatus sessionStatus = resourcePolicy == nullptr
        ? ValidateSessionDescriptor(sessionDescriptor)
        : ValidateSessionDescriptor(sessionDescriptor, *resourcePolicy);
    if (!sessionStatus)
    {
        return sessionStatus;
    }

    const ProtocolStatus digestStatus = ValidateDigestAlgorithm(
        finalManifest.digestAlgorithm,
        kFinalDigestAlgorithmOffset);
    if (!digestStatus)
    {
        return digestStatus;
    }

    const ProtocolStatus shapeStatus = ValidateSessionShape(
        finalManifest.originalFileSize,
        finalManifest.segmentCount,
        kFinalSegmentCountOffset);
    if (!shapeStatus)
    {
        return shapeStatus;
    }
    if (resourcePolicy != nullptr)
    {
        const ProtocolStatus resourceStatus = ValidateResourceLimits(
            finalManifest.originalFileSize,
            finalManifest.segmentCount,
            *resourcePolicy,
            kFinalOriginalFileSizeOffset,
            kFinalSegmentCountOffset);
        if (!resourceStatus)
        {
            return resourceStatus;
        }
    }

    if (finalManifest.sessionId != sessionDescriptor.sessionId)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::SessionMismatch,
            kFinalSessionIdOffset);
    }
    if (finalManifest.originalFileSize != sessionDescriptor.originalFileSize)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::SessionMismatch,
            kFinalOriginalFileSizeOffset);
    }
    if (finalManifest.segmentCount != sessionDescriptor.segmentCount)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::SessionMismatch,
            kFinalSegmentCountOffset);
    }
    if (finalManifest.digestAlgorithm != sessionDescriptor.digestAlgorithm)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::SessionMismatch,
            kFinalDigestAlgorithmOffset);
    }

    if (finalManifest.originalFileSize == 0 &&
        finalManifest.wholeFileDigest != GetEmptyBlake3WholeFileDigest())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::DigestMismatch,
            kFinalWholeFileDigestOffset);
    }

    return ProtocolStatus::Success();
}

} // namespace

ProtocolStatus ValidateFinalManifest(
    const FinalManifest& finalManifest,
    const SessionDescriptor& sessionDescriptor) noexcept
{
    return ValidateFinalManifestImplementation(
        finalManifest,
        sessionDescriptor,
        nullptr);
}

ProtocolStatus ValidateFinalManifest(
    const FinalManifest& finalManifest,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    return ValidateFinalManifestImplementation(
        finalManifest,
        sessionDescriptor,
        &resourcePolicy);
}

ProtocolResult<std::size_t> GetSerializedSize(
    const SessionDescriptor& descriptor) noexcept
{
    if (descriptor.fileNameUtf8.size() > kMaximumFileNameUtf8Bytes)
    {
        return ProtocolResult<std::size_t>::Failure(ProtocolErrorCode::LengthLimitExceeded, kSessionFileNameUtf8BytesOffset);
    }
    const auto nameEndResult = CheckedAddSize(kSessionDescriptorHeaderBytes, descriptor.fileNameUtf8.size(), kSessionFileNameUtf8BytesOffset);
    if (!nameEndResult)
    {
        return FailureFrom<std::size_t>(nameEndResult.Error());
    }
    const auto extensionEndResult = CheckedAddSize(nameEndResult.Value(), descriptor.optionalExtensions.size(), nameEndResult.Value());
    if (!extensionEndResult)
    {
        return FailureFrom<std::size_t>(extensionEndResult.Error());
    }
    const auto totalBytesResult = CheckedAddSize(extensionEndResult.Value(), kDescriptorCrcBytes, extensionEndResult.Value());
    if (!totalBytesResult)
    {
        return FailureFrom<std::size_t>(totalBytesResult.Error());
    }
    if (totalBytesResult.Value() > kMaximumDescriptorPayloadBytes)
    {
        return ProtocolResult<std::size_t>::Failure(ProtocolErrorCode::LengthLimitExceeded, extensionEndResult.Value());
    }
    return totalBytesResult;
}

ProtocolResult<std::size_t> GetSerializedSize(
    const SegmentDescriptor& descriptor) noexcept
{
    const ProtocolStatus modeStatus = ValidateOuterFecMode(descriptor.outerFecMode, kSegmentOuterFecModeOffset);
    if (!modeStatus)
    {
        return FailureFrom<std::size_t>(modeStatus.Error());
    }
    if (descriptor.outerFecMode == OuterFecMode::WirehairV2)
    {
        if (!descriptor.wirehairV2SerializedProfile.has_value())
        {
            return ProtocolResult<std::size_t>::Failure(ProtocolErrorCode::InvalidDescriptor, kSegmentWirehairProfileOffset);
        }
        return ProtocolResult<std::size_t>::Success(kWirehairV2SegmentDescriptorPayloadBytes);
    }
    if (descriptor.wirehairV2SerializedProfile.has_value())
    {
        return ProtocolResult<std::size_t>::Failure(ProtocolErrorCode::InvalidDescriptor, kSegmentWirehairProfileOffset);
    }
    return ProtocolResult<std::size_t>::Success(kDirectRepeatSegmentDescriptorPayloadBytes);
}

ProtocolStatus SerializeSessionDescriptor(
    const SessionDescriptor& descriptor,
    const std::span<std::byte> output) noexcept
{
    const ProtocolStatus validationStatus = ValidateSessionDescriptor(descriptor);
    if (!validationStatus)
    {
        return validationStatus;
    }
    const auto serializedSizeResult = GetSerializedSize(descriptor);
    if (!serializedSizeResult)
    {
        return CopyStatusFailure(serializedSizeResult.Error());
    }
    const std::size_t serializedSize = serializedSizeResult.Value();
    return SerializeAtomically(serializedSize, output, [&descriptor, serializedSize](ByteWriter& writer) noexcept
    {
        return WriteSessionDescriptor(descriptor, static_cast<std::uint32_t>(serializedSize), writer);
    });
}

ProtocolStatus SerializeSessionDescriptor(
    const SessionDescriptor& descriptor,
    const ReceiverResourcePolicy& resourcePolicy,
    const std::span<std::byte> output) noexcept
{
    const ProtocolStatus validationStatus = ValidateSessionDescriptor(descriptor, resourcePolicy);
    if (!validationStatus)
    {
        return validationStatus;
    }
    return SerializeSessionDescriptor(descriptor, output);
}

ProtocolStatus SerializeSegmentDescriptor(
    const SegmentDescriptor& descriptor,
    const SessionDescriptor& sessionDescriptor,
    const std::span<std::byte> output) noexcept
{
    const ProtocolStatus validationStatus = ValidateSegmentDescriptor(descriptor, sessionDescriptor);
    if (!validationStatus)
    {
        return validationStatus;
    }
    const auto serializedSizeResult = GetSerializedSize(descriptor);
    if (!serializedSizeResult)
    {
        return CopyStatusFailure(serializedSizeResult.Error());
    }
    const std::size_t serializedSize = serializedSizeResult.Value();
    return SerializeAtomically(serializedSize, output, [&descriptor, serializedSize](ByteWriter& writer) noexcept
    {
        return WriteSegmentDescriptor(descriptor, static_cast<std::uint32_t>(serializedSize), writer);
    });
}

ProtocolStatus SerializeSegmentDescriptor(
    const SegmentDescriptor& descriptor,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy,
    const std::span<std::byte> output) noexcept
{
    const ProtocolStatus validationStatus = ValidateSegmentDescriptor(descriptor, sessionDescriptor, resourcePolicy);
    if (!validationStatus)
    {
        return validationStatus;
    }
    return SerializeSegmentDescriptor(descriptor, sessionDescriptor, output);
}

ProtocolStatus SerializeFinalManifest(
    const FinalManifest& finalManifest,
    const SessionDescriptor& sessionDescriptor,
    const std::span<std::byte> output) noexcept
{
    const ProtocolStatus validationStatus = ValidateFinalManifest(finalManifest, sessionDescriptor);
    if (!validationStatus)
    {
        return validationStatus;
    }
    return SerializeAtomically(kFinalManifestPayloadBytes, output, [&finalManifest](ByteWriter& writer) noexcept
    {
        return WriteFinalManifest(finalManifest, writer);
    });
}

ProtocolStatus SerializeFinalManifest(
    const FinalManifest& finalManifest,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy,
    const std::span<std::byte> output) noexcept
{
    const ProtocolStatus validationStatus = ValidateFinalManifest(finalManifest, sessionDescriptor, resourcePolicy);
    if (!validationStatus)
    {
        return validationStatus;
    }
    return SerializeFinalManifest(finalManifest, sessionDescriptor, output);
}

ProtocolResult<SessionDescriptor> ParseSessionDescriptor(
    const std::span<const std::byte> input,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    const ProtocolStatus envelopeStatus = ValidateDescriptorEnvelope(
        input,
        static_cast<std::uint16_t>(kSessionDescriptorHeaderBytes),
        kMinimumSessionDescriptorPayloadBytes,
        kMaximumDescriptorPayloadBytes);
    if (!envelopeStatus)
    {
        return FailureFrom<SessionDescriptor>(envelopeStatus.Error());
    }

    ByteReader reader(input.first(input.size() - kDescriptorCrcBytes));
    const auto schemaResult = reader.ReadUint16();
    const auto headerBytesResult = reader.ReadUint16();
    const auto totalBytesResult = reader.ReadUint32();
    const auto majorResult = reader.ReadUint16();
    const auto minorResult = reader.ReadUint16();
    const auto sessionIdResult = reader.ReadFixedBytes<kSessionIdBytes>();
    const auto visualProfileResult = reader.ReadUint64();
    const auto originalFileSizeResult = reader.ReadUint64();
    const auto sourceSegmentTargetResult = reader.ReadUint32();
    const auto segmentCountResult = reader.ReadUint64();
    const auto compressionPolicyResult = reader.ReadUint8();
    const auto digestAlgorithmResult = reader.ReadUint8();
    if (!schemaResult || !headerBytesResult || !totalBytesResult || !majorResult || !minorResult || !sessionIdResult ||
        !visualProfileResult || !originalFileSizeResult || !sourceSegmentTargetResult || !segmentCountResult ||
        !compressionPolicyResult || !digestAlgorithmResult)
    {
        return ProtocolResult<SessionDescriptor>::Failure(ProtocolErrorCode::InternalDescriptorStateError, reader.Position());
    }
    const ProtocolStatus reservedStatus = reader.ReadReservedZeroBytes(2);
    if (!reservedStatus)
    {
        return FailureFrom<SessionDescriptor>(reservedStatus.Error());
    }
    const auto featureFlagsResult = reader.ReadUint64();
    const auto fileNameBytesResult = reader.ReadUint16();
    if (!featureFlagsResult || !fileNameBytesResult)
    {
        const ProtocolError& error = !featureFlagsResult ? featureFlagsResult.Error() : fileNameBytesResult.Error();
        return FailureFrom<SessionDescriptor>(error);
    }
    if (fileNameBytesResult.Value() > kMaximumFileNameUtf8Bytes)
    {
        return ProtocolResult<SessionDescriptor>::Failure(ProtocolErrorCode::LengthLimitExceeded, kSessionFileNameUtf8BytesOffset);
    }
    const auto fileNameResult = reader.ReadBytes(fileNameBytesResult.Value());
    if (!fileNameResult)
    {
        return FailureFrom<SessionDescriptor>(fileNameResult.Error());
    }
    const auto extensionResult = reader.ReadBytes(reader.Remaining());
    if (!extensionResult)
    {
        return FailureFrom<SessionDescriptor>(extensionResult.Error());
    }

    try
    {
        const std::string fileName(
            reinterpret_cast<const char*>(fileNameResult.Value().data()),
            fileNameResult.Value().size());
        const std::vector<std::byte> extensions(extensionResult.Value().begin(), extensionResult.Value().end());
        const SessionDescriptor descriptor{
            ProtocolVersion{majorResult.Value(), minorResult.Value()},
            SessionId{sessionIdResult.Value()},
            originalFileSizeResult.Value(),
            segmentCountResult.Value(),
            static_cast<DigestAlgorithm>(digestAlgorithmResult.Value()),
            visualProfileResult.Value(),
            sourceSegmentTargetResult.Value(),
            static_cast<CompressionPolicy>(compressionPolicyResult.Value()),
            featureFlagsResult.Value(),
            fileName,
            extensions};
        const ProtocolStatus validationStatus = ValidateSessionDescriptor(descriptor, resourcePolicy);
        if (!validationStatus)
        {
            return FailureFrom<SessionDescriptor>(validationStatus.Error());
        }
        return ProtocolResult<SessionDescriptor>::Success(descriptor);
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolResult<SessionDescriptor>::Failure(ProtocolErrorCode::ResourceExhausted, kSessionFileNameUtf8Offset);
    }
}

ProtocolResult<SegmentDescriptor> ParseSegmentDescriptor(
    const std::span<const std::byte> input,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    const ProtocolStatus envelopeStatus = ValidateDescriptorEnvelope(
        input,
        kSegmentDescriptorHeaderBytes,
        kDirectRepeatSegmentDescriptorPayloadBytes,
        kWirehairV2SegmentDescriptorPayloadBytes);
    if (!envelopeStatus)
    {
        return FailureFrom<SegmentDescriptor>(envelopeStatus.Error());
    }

    ByteReader reader(input.first(input.size() - kDescriptorCrcBytes));
    const auto schemaResult = reader.ReadUint16();
    const auto headerBytesResult = reader.ReadUint16();
    const auto totalBytesResult = reader.ReadUint32();
    const auto sessionTagResult = reader.ReadUint64();
    const auto segmentOrdinalResult = reader.ReadUint64();
    const auto rawOffsetResult = reader.ReadUint64();
    const auto rawSizeResult = reader.ReadUint64();
    const auto encodedSizeResult = reader.ReadUint64();
    const auto compressionCodecResult = reader.ReadUint8();
    const auto outerFecModeResult = reader.ReadUint8();
    if (!schemaResult || !headerBytesResult || !totalBytesResult || !sessionTagResult || !segmentOrdinalResult ||
        !rawOffsetResult || !rawSizeResult || !encodedSizeResult || !compressionCodecResult || !outerFecModeResult)
    {
        return ProtocolResult<SegmentDescriptor>::Failure(ProtocolErrorCode::InternalDescriptorStateError, reader.Position());
    }
    const ProtocolStatus reservedStatus = reader.ReadReservedZeroBytes(2);
    if (!reservedStatus)
    {
        return FailureFrom<SegmentDescriptor>(reservedStatus.Error());
    }
    const auto outerBlockBytesResult = reader.ReadUint32();
    const auto rawDigestResult = reader.ReadFixedBytes<kDigestBytes>();
    const auto encodedDigestResult = reader.ReadFixedBytes<kDigestBytes>();
    const auto flagsResult = reader.ReadUint64();
    if (!outerBlockBytesResult || !rawDigestResult || !encodedDigestResult || !flagsResult)
    {
        return ProtocolResult<SegmentDescriptor>::Failure(ProtocolErrorCode::InternalDescriptorStateError, reader.Position());
    }

    const CompressionCodec compressionCodec = static_cast<CompressionCodec>(compressionCodecResult.Value());
    const ProtocolStatus compressionStatus = ValidateCompressionCodec(compressionCodec, kSegmentCompressionCodecOffset);
    if (!compressionStatus)
    {
        return FailureFrom<SegmentDescriptor>(compressionStatus.Error());
    }
    const OuterFecMode outerFecMode = static_cast<OuterFecMode>(outerFecModeResult.Value());
    const ProtocolStatus modeStatus = ValidateOuterFecMode(outerFecMode, kSegmentOuterFecModeOffset);
    if (!modeStatus)
    {
        return FailureFrom<SegmentDescriptor>(modeStatus.Error());
    }

    std::optional<WirehairV2SerializedProfile> wirehairProfile;
    if (outerFecMode == OuterFecMode::WirehairV2)
    {
        const auto wirehairProfileResult = reader.ReadFixedBytes<kWirehairV2SerializedProfileBytes>();
        if (!wirehairProfileResult)
        {
            return FailureFrom<SegmentDescriptor>(wirehairProfileResult.Error());
        }
        wirehairProfile = WirehairV2SerializedProfile{wirehairProfileResult.Value()};
    }
    const ProtocolStatus consumedStatus = reader.RequireFullyConsumed();
    if (!consumedStatus)
    {
        return FailureFrom<SegmentDescriptor>(consumedStatus.Error());
    }

    const SegmentDescriptor descriptor{
        SessionTag{sessionTagResult.Value()},
        segmentOrdinalResult.Value(),
        rawOffsetResult.Value(),
        rawSizeResult.Value(),
        encodedSizeResult.Value(),
        compressionCodec,
        outerFecMode,
        outerBlockBytesResult.Value(),
        RawDigest{rawDigestResult.Value()},
        EncodedDigest{encodedDigestResult.Value()},
        wirehairProfile,
        flagsResult.Value()};
    const auto expectedSizeResult = GetSerializedSize(descriptor);
    if (!expectedSizeResult || expectedSizeResult.Value() != input.size())
    {
        return ProtocolResult<SegmentDescriptor>::Failure(ProtocolErrorCode::InvalidRecordSize, kDescriptorTotalBytesOffset);
    }
    const ProtocolStatus validationStatus = ValidateSegmentDescriptor(descriptor, sessionDescriptor, resourcePolicy);
    if (!validationStatus)
    {
        return FailureFrom<SegmentDescriptor>(validationStatus.Error());
    }
    return ProtocolResult<SegmentDescriptor>::Success(descriptor);
}

ProtocolResult<FinalManifest> ParseFinalManifest(
    const std::span<const std::byte> input,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    const ProtocolStatus envelopeStatus = ValidateDescriptorEnvelope(
        input,
        kFinalManifestHeaderBytes,
        kFinalManifestPayloadBytes,
        kFinalManifestPayloadBytes);
    if (!envelopeStatus)
    {
        return FailureFrom<FinalManifest>(envelopeStatus.Error());
    }

    ByteReader reader(input.first(input.size() - kDescriptorCrcBytes));
    const auto schemaResult = reader.ReadUint16();
    const auto headerBytesResult = reader.ReadUint16();
    const auto totalBytesResult = reader.ReadUint32();
    const auto sessionIdResult = reader.ReadFixedBytes<kSessionIdBytes>();
    const auto originalFileSizeResult = reader.ReadUint64();
    const auto segmentCountResult = reader.ReadUint64();
    const auto wholeFileDigestResult = reader.ReadFixedBytes<kDigestBytes>();
    const auto digestAlgorithmResult = reader.ReadUint8();
    if (!schemaResult || !headerBytesResult || !totalBytesResult || !sessionIdResult || !originalFileSizeResult ||
        !segmentCountResult || !wholeFileDigestResult || !digestAlgorithmResult)
    {
        return ProtocolResult<FinalManifest>::Failure(ProtocolErrorCode::InternalDescriptorStateError, reader.Position());
    }
    const ProtocolStatus reservedStatus = reader.ReadReservedZeroBytes(7);
    if (!reservedStatus)
    {
        return FailureFrom<FinalManifest>(reservedStatus.Error());
    }
    const ProtocolStatus consumedStatus = reader.RequireFullyConsumed();
    if (!consumedStatus)
    {
        return FailureFrom<FinalManifest>(consumedStatus.Error());
    }

    const FinalManifest finalManifest{
        SessionId{sessionIdResult.Value()},
        originalFileSizeResult.Value(),
        segmentCountResult.Value(),
        WholeFileDigest{wholeFileDigestResult.Value()},
        static_cast<DigestAlgorithm>(digestAlgorithmResult.Value())};
    const ProtocolStatus validationStatus = ValidateFinalManifest(finalManifest, sessionDescriptor, resourcePolicy);
    if (!validationStatus)
    {
        return FailureFrom<FinalManifest>(validationStatus.Error());
    }
    return ProtocolResult<FinalManifest>::Success(finalManifest);
}

} // namespace pbprotocol
