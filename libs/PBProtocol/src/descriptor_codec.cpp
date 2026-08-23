#include "pbprotocol/descriptor_codec.h"

#include "pbprotocol/byte_io.h"
#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pbprotocol {

namespace {

constexpr std::size_t kSessionProtocolMajorOffset = 0;
constexpr std::size_t kSessionProtocolMinorOffset = 2;
constexpr std::size_t kSessionIdOffset = 4;
constexpr std::size_t kSessionOriginalFileSizeOffset = 20;
constexpr std::size_t kSessionSegmentCountOffset = 28;
constexpr std::size_t kSessionDigestAlgorithmOffset = 36;

constexpr std::size_t kSegmentSessionTagOffset = 0;
constexpr std::size_t kSegmentOrdinalOffset = 8;
constexpr std::size_t kSegmentRawOffsetOffset = 16;
constexpr std::size_t kSegmentRawSizeOffset = 24;
constexpr std::size_t kSegmentEncodedSizeOffset = 32;
constexpr std::size_t kSegmentCompressionCodecOffset = 40;
constexpr std::size_t kSegmentOuterFecModeOffset = 41;
constexpr std::size_t kSegmentOuterBlockBytesOffset = 42;
constexpr std::size_t kSegmentRawDigestOffset = 46;
constexpr std::size_t kSegmentEncodedDigestOffset = 78;
constexpr std::size_t kSegmentWirehairProfileOffset = 110;

constexpr std::size_t kFinalSessionIdOffset = 0;
constexpr std::size_t kFinalOriginalFileSizeOffset = 16;
constexpr std::size_t kFinalSegmentCountOffset = 24;
constexpr std::size_t kFinalWholeFileDigestOffset = 32;
constexpr std::size_t kFinalDigestAlgorithmOffset = 64;

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

template <std::size_t OutputBytes, typename WriteFunction>
[[nodiscard]] ProtocolStatus SerializeAtomically(
    const std::span<std::byte> output,
    WriteFunction&& writeFunction) noexcept
{
    if (output.size() != OutputBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidRecordSize,
            0);
    }

    std::array<std::byte, OutputBytes> scratch{};
    ByteWriter writer(scratch);
    const ProtocolStatus writeStatus = writeFunction(writer);
    if (!writeStatus)
    {
        return writeStatus;
    }

    if (writer.Position() != scratch.size())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalDescriptorStateError,
            writer.Position());
    }

    std::copy(scratch.begin(), scratch.end(), output.begin());
    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus WriteSessionDescriptor(
    const SessionDescriptor& descriptor,
    ByteWriter& writer) noexcept
{
    ProtocolStatus status = writer.WriteUint16(descriptor.protocolVersion.major);
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
    status = writer.WriteUint64(descriptor.originalFileSize);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(descriptor.segmentCount);
    if (!status)
    {
        return status;
    }
    return writer.WriteUint8(
        static_cast<std::underlying_type_t<DigestAlgorithm>>(
            descriptor.digestAlgorithm));
}

[[nodiscard]] ProtocolStatus WriteSegmentDescriptor(
    const SegmentDescriptor& descriptor,
    ByteWriter& writer) noexcept
{
    ProtocolStatus status = writer.WriteUint64(descriptor.sessionTag.value);
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
    status = writer.WriteUint8(
        static_cast<std::underlying_type_t<CompressionCodec>>(
            descriptor.compressionCodec));
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(
        static_cast<std::underlying_type_t<OuterFecMode>>(
            descriptor.outerFecMode));
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

    if (descriptor.outerFecMode == OuterFecMode::WirehairV2)
    {
        if (!descriptor.wirehairV2SerializedProfile.has_value())
        {
            return ProtocolStatus::Failure(
                ProtocolErrorCode::InternalDescriptorStateError,
                kSegmentWirehairProfileOffset);
        }
        return writer.WriteFixedBytes(
            descriptor.wirehairV2SerializedProfile->bytes);
    }

    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus WriteFinalManifest(
    const FinalManifest& finalManifest,
    ByteWriter& writer) noexcept
{
    ProtocolStatus status = writer.WriteFixedBytes(finalManifest.sessionId.bytes);
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
    return writer.WriteUint8(
        static_cast<std::underlying_type_t<DigestAlgorithm>>(
            finalManifest.digestAlgorithm));
}

} // namespace

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
        resourcePolicy.maxDescriptorStateBytes >
            resourcePolicy.maxTotalDescriptorStateBytes ||
        resourcePolicy.maxOuterFecDecoderBytes >
            resourcePolicy.maxTotalOuterFecDecoderBytes)
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
    if (!segmentCountSizeResult ||
        !rawSegmentSizeResult ||
        !encodedSegmentSizeResult ||
        !descriptorBudgetSizeResult ||
        !concurrentSessionCountResult ||
        !directRepeatBlockCountResult ||
        !activeOuterFecDecoderCountResult ||
        !outerFecDecoderBudgetSizeResult)
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
    const SegmentDescriptor& descriptor) noexcept
{
    const ProtocolStatus modeStatus = ValidateOuterFecMode(
        descriptor.outerFecMode,
        kSegmentOuterFecModeOffset);
    if (!modeStatus)
    {
        return FailureFrom<std::size_t>(modeStatus.Error());
    }

    if (descriptor.outerFecMode == OuterFecMode::WirehairV2)
    {
        if (!descriptor.wirehairV2SerializedProfile.has_value())
        {
            return ProtocolResult<std::size_t>::Failure(
                ProtocolErrorCode::InvalidDescriptor,
                kSegmentWirehairProfileOffset);
        }
        const auto serializedSizeResult = CheckedAddSize(
            kDirectRepeatSegmentDescriptorPayloadBytes,
            kWirehairV2SerializedProfileBytes,
            kSegmentWirehairProfileOffset);
        if (!serializedSizeResult)
        {
            return FailureFrom<std::size_t>(serializedSizeResult.Error());
        }
        if (serializedSizeResult.Value() !=
            kWirehairV2SegmentDescriptorPayloadBytes)
        {
            return ProtocolResult<std::size_t>::Failure(
                ProtocolErrorCode::InternalDescriptorStateError,
                kSegmentWirehairProfileOffset);
        }
        return serializedSizeResult;
    }

    if (descriptor.wirehairV2SerializedProfile.has_value())
    {
        return ProtocolResult<std::size_t>::Failure(
            ProtocolErrorCode::InvalidDescriptor,
            kSegmentWirehairProfileOffset);
    }
    return ProtocolResult<std::size_t>::Success(
        kDirectRepeatSegmentDescriptorPayloadBytes);
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

    return SerializeAtomically<kSessionDescriptorPayloadBytes>(
        output,
        [&descriptor](ByteWriter& writer) noexcept
        {
            return WriteSessionDescriptor(descriptor, writer);
        });
}

ProtocolStatus SerializeSessionDescriptor(
    const SessionDescriptor& descriptor,
    const ReceiverResourcePolicy& resourcePolicy,
    const std::span<std::byte> output) noexcept
{
    const ProtocolStatus validationStatus = ValidateSessionDescriptor(
        descriptor,
        resourcePolicy);
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
    const ProtocolStatus validationStatus = ValidateSegmentDescriptor(
        descriptor,
        sessionDescriptor);
    if (!validationStatus)
    {
        return validationStatus;
    }

    const auto serializedSizeResult = GetSerializedSize(descriptor);
    if (!serializedSizeResult)
    {
        return CopyStatusFailure(serializedSizeResult.Error());
    }

    if (serializedSizeResult.Value() == kWirehairV2SegmentDescriptorPayloadBytes)
    {
        return SerializeAtomically<kWirehairV2SegmentDescriptorPayloadBytes>(
            output,
            [&descriptor](ByteWriter& writer) noexcept
            {
                return WriteSegmentDescriptor(descriptor, writer);
            });
    }

    return SerializeAtomically<kDirectRepeatSegmentDescriptorPayloadBytes>(
        output,
        [&descriptor](ByteWriter& writer) noexcept
        {
            return WriteSegmentDescriptor(descriptor, writer);
        });
}

ProtocolStatus SerializeSegmentDescriptor(
    const SegmentDescriptor& descriptor,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy,
    const std::span<std::byte> output) noexcept
{
    const ProtocolStatus validationStatus = ValidateSegmentDescriptor(
        descriptor,
        sessionDescriptor,
        resourcePolicy);
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
    const ProtocolStatus validationStatus = ValidateFinalManifest(
        finalManifest,
        sessionDescriptor);
    if (!validationStatus)
    {
        return validationStatus;
    }

    return SerializeAtomically<kFinalManifestPayloadBytes>(
        output,
        [&finalManifest](ByteWriter& writer) noexcept
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
    const ProtocolStatus validationStatus = ValidateFinalManifest(
        finalManifest,
        sessionDescriptor,
        resourcePolicy);
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
    ByteReader reader(input);
    const auto majorResult = reader.ReadUint16();
    if (!majorResult)
    {
        return FailureFrom<SessionDescriptor>(majorResult.Error());
    }
    const auto minorResult = reader.ReadUint16();
    if (!minorResult)
    {
        return FailureFrom<SessionDescriptor>(minorResult.Error());
    }
    const auto sessionIdResult = reader.ReadFixedBytes<kSessionIdBytes>();
    if (!sessionIdResult)
    {
        return FailureFrom<SessionDescriptor>(sessionIdResult.Error());
    }
    const auto originalFileSizeResult = reader.ReadUint64();
    if (!originalFileSizeResult)
    {
        return FailureFrom<SessionDescriptor>(originalFileSizeResult.Error());
    }
    const auto segmentCountResult = reader.ReadUint64();
    if (!segmentCountResult)
    {
        return FailureFrom<SessionDescriptor>(segmentCountResult.Error());
    }
    const auto digestAlgorithmResult = reader.ReadUint8();
    if (!digestAlgorithmResult)
    {
        return FailureFrom<SessionDescriptor>(digestAlgorithmResult.Error());
    }
    const ProtocolStatus consumedStatus = reader.RequireFullyConsumed();
    if (!consumedStatus)
    {
        return FailureFrom<SessionDescriptor>(consumedStatus.Error());
    }

    const SessionDescriptor descriptor{
        ProtocolVersion{majorResult.Value(), minorResult.Value()},
        SessionId{sessionIdResult.Value()},
        originalFileSizeResult.Value(),
        segmentCountResult.Value(),
        static_cast<DigestAlgorithm>(digestAlgorithmResult.Value())};
    const ProtocolStatus validationStatus = ValidateSessionDescriptor(
        descriptor,
        resourcePolicy);
    if (!validationStatus)
    {
        return FailureFrom<SessionDescriptor>(validationStatus.Error());
    }

    return ProtocolResult<SessionDescriptor>::Success(descriptor);
}

ProtocolResult<SegmentDescriptor> ParseSegmentDescriptor(
    const std::span<const std::byte> input,
    const SessionDescriptor& sessionDescriptor,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    ByteReader reader(input);
    const auto sessionTagResult = reader.ReadUint64();
    if (!sessionTagResult)
    {
        return FailureFrom<SegmentDescriptor>(sessionTagResult.Error());
    }
    const auto segmentOrdinalResult = reader.ReadUint64();
    if (!segmentOrdinalResult)
    {
        return FailureFrom<SegmentDescriptor>(segmentOrdinalResult.Error());
    }
    const auto rawOffsetResult = reader.ReadUint64();
    if (!rawOffsetResult)
    {
        return FailureFrom<SegmentDescriptor>(rawOffsetResult.Error());
    }
    const auto rawSizeResult = reader.ReadUint64();
    if (!rawSizeResult)
    {
        return FailureFrom<SegmentDescriptor>(rawSizeResult.Error());
    }
    const auto encodedSizeResult = reader.ReadUint64();
    if (!encodedSizeResult)
    {
        return FailureFrom<SegmentDescriptor>(encodedSizeResult.Error());
    }
    const auto compressionCodecResult = reader.ReadUint8();
    if (!compressionCodecResult)
    {
        return FailureFrom<SegmentDescriptor>(compressionCodecResult.Error());
    }
    const CompressionCodec compressionCodec =
        static_cast<CompressionCodec>(compressionCodecResult.Value());
    const ProtocolStatus compressionStatus = ValidateCompressionCodec(
        compressionCodec,
        kSegmentCompressionCodecOffset);
    if (!compressionStatus)
    {
        return FailureFrom<SegmentDescriptor>(compressionStatus.Error());
    }

    const auto outerFecModeResult = reader.ReadUint8();
    if (!outerFecModeResult)
    {
        return FailureFrom<SegmentDescriptor>(outerFecModeResult.Error());
    }
    const OuterFecMode outerFecMode =
        static_cast<OuterFecMode>(outerFecModeResult.Value());
    const ProtocolStatus modeStatus = ValidateOuterFecMode(
        outerFecMode,
        kSegmentOuterFecModeOffset);
    if (!modeStatus)
    {
        return FailureFrom<SegmentDescriptor>(modeStatus.Error());
    }

    const auto outerBlockBytesResult = reader.ReadUint32();
    if (!outerBlockBytesResult)
    {
        return FailureFrom<SegmentDescriptor>(outerBlockBytesResult.Error());
    }
    const auto rawDigestResult = reader.ReadFixedBytes<kDigestBytes>();
    if (!rawDigestResult)
    {
        return FailureFrom<SegmentDescriptor>(rawDigestResult.Error());
    }
    const auto encodedDigestResult = reader.ReadFixedBytes<kDigestBytes>();
    if (!encodedDigestResult)
    {
        return FailureFrom<SegmentDescriptor>(encodedDigestResult.Error());
    }

    std::optional<WirehairV2SerializedProfile> wirehairProfile;
    if (outerFecMode == OuterFecMode::WirehairV2)
    {
        const auto wirehairProfileResult =
            reader.ReadFixedBytes<kWirehairV2SerializedProfileBytes>();
        if (!wirehairProfileResult)
        {
            return FailureFrom<SegmentDescriptor>(wirehairProfileResult.Error());
        }
        wirehairProfile = WirehairV2SerializedProfile{
            wirehairProfileResult.Value()};
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
        wirehairProfile};
    const ProtocolStatus validationStatus = ValidateSegmentDescriptor(
        descriptor,
        sessionDescriptor,
        resourcePolicy);
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
    ByteReader reader(input);
    const auto sessionIdResult = reader.ReadFixedBytes<kSessionIdBytes>();
    if (!sessionIdResult)
    {
        return FailureFrom<FinalManifest>(sessionIdResult.Error());
    }
    const auto originalFileSizeResult = reader.ReadUint64();
    if (!originalFileSizeResult)
    {
        return FailureFrom<FinalManifest>(originalFileSizeResult.Error());
    }
    const auto segmentCountResult = reader.ReadUint64();
    if (!segmentCountResult)
    {
        return FailureFrom<FinalManifest>(segmentCountResult.Error());
    }
    const auto wholeFileDigestResult = reader.ReadFixedBytes<kDigestBytes>();
    if (!wholeFileDigestResult)
    {
        return FailureFrom<FinalManifest>(wholeFileDigestResult.Error());
    }
    const auto digestAlgorithmResult = reader.ReadUint8();
    if (!digestAlgorithmResult)
    {
        return FailureFrom<FinalManifest>(digestAlgorithmResult.Error());
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
    const ProtocolStatus validationStatus = ValidateFinalManifest(
        finalManifest,
        sessionDescriptor,
        resourcePolicy);
    if (!validationStatus)
    {
        return FailureFrom<FinalManifest>(validationStatus.Error());
    }

    return ProtocolResult<FinalManifest>::Success(finalManifest);
}

} // namespace pbprotocol
