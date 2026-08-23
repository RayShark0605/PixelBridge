#include "pbprotocol/bootstrap_control_codec.h"

#include "pbprotocol/byte_io.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/crc32c.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace pbprotocol {

namespace {

constexpr std::size_t kBootstrapMagicOffset = 0;
constexpr std::size_t kBootstrapVersionOffset = 4;
constexpr std::size_t kBootstrapProtocolMajorOffset = 5;
constexpr std::size_t kBootstrapProtocolMinorOffset = 6;
constexpr std::size_t kBootstrapFlagsOffset = 36;
constexpr std::size_t kBootstrapCrcOffset = 40;

constexpr std::size_t kControlMagicOffset = 0;
constexpr std::size_t kControlVersionOffset = 4;
constexpr std::size_t kControlRecordTypeOffset = 5;
constexpr std::size_t kControlRecordBytesOffset = 22;

template <typename ValueType>
[[nodiscard]] ProtocolResult<ValueType> FailureFrom(
    const ProtocolError& error) noexcept
{
    return ProtocolResult<ValueType>::Failure(error.code, error.offset);
}

[[nodiscard]] ProtocolStatus ValidateMagic(
    const std::span<const std::byte> receivedMagic,
    const std::span<const std::byte> expectedMagic,
    const std::size_t fieldOffset) noexcept
{
    for (std::size_t byteIndex = 0;
         byteIndex < expectedMagic.size();
         byteIndex++)
    {
        if (receivedMagic[byteIndex] != expectedMagic[byteIndex])
        {
            return ProtocolStatus::Failure(
                ProtocolErrorCode::InvalidMagic,
                fieldOffset + byteIndex);
        }
    }

    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus ValidateBootstrapRecord(
    const BootstrapRecord& record) noexcept
{
    if (record.bootstrapVersion != kBootstrapVersion)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::UnsupportedBootstrapVersion,
            kBootstrapVersionOffset);
    }

    const auto versionClassification = ClassifyProtocolVersion(
        record.protocolVersion,
        kBootstrapProtocolMajorOffset);
    if (!versionClassification)
    {
        return ProtocolStatus::Failure(
            versionClassification.Error().code,
            versionClassification.Error().offset);
    }
    if (versionClassification.Value() ==
        ProtocolVersionClassification::NewerMinorRequiresOptionalValidation)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::UnsupportedProtocolMinor,
            kBootstrapProtocolMinorOffset);
    }

    if (record.flags != 0)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::NonZeroReservedBits,
            kBootstrapFlagsOffset);
    }

    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus ValidateControlRecordType(
    const ControlRecordType recordType) noexcept
{
    switch (recordType)
    {
    case ControlRecordType::SessionDescriptor:
    case ControlRecordType::SegmentDescriptor:
    case ControlRecordType::FinalManifest:
        return ProtocolStatus::Success();
    default:
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidEnumValue,
            kControlRecordTypeOffset);
    }
}

[[nodiscard]] ProtocolStatus ValidateControlRecordHeader(
    const ControlRecordView& record) noexcept
{
    if (record.controlVersion != kControlVersion)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::UnsupportedControlVersion,
            kControlVersionOffset);
    }

    return ValidateControlRecordType(record.recordType);
}

[[nodiscard]] ProtocolStatus WriteBootstrapRecord(
    const BootstrapRecord& record,
    ByteWriter& writer) noexcept
{
    ProtocolStatus status = writer.WriteFixedBytes(kBootstrapRecordMagic);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(record.bootstrapVersion);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(
        static_cast<std::uint8_t>(record.protocolVersion.major));
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(
        static_cast<std::uint8_t>(record.protocolVersion.minor));
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(record.visualLayoutVersion);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(record.visualProfileId);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(record.sessionTag.value);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(record.frameSequence);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint32(record.controlEpoch);
    if (!status)
    {
        return status;
    }
    return writer.WriteUint32(record.flags);
}

[[nodiscard]] ProtocolStatus WriteControlRecordPrefix(
    const ControlRecordView& record,
    const std::uint32_t recordBytes,
    ByteWriter& writer) noexcept
{
    ProtocolStatus status = writer.WriteFixedBytes(kControlRecordMagic);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(record.controlVersion);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(
        static_cast<std::underlying_type_t<ControlRecordType>>(
            record.recordType));
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(record.controlSequence);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(record.sessionTag.value);
    if (!status)
    {
        return status;
    }
    return writer.WriteUint32(recordBytes);
}

} // namespace

ProtocolResult<std::size_t> GetSerializedSize(
    const ControlRecordView& record) noexcept
{
    const ProtocolStatus headerStatus = ValidateControlRecordHeader(record);
    if (!headerStatus)
    {
        return ProtocolResult<std::size_t>::Failure(
            headerStatus.Error().code,
            headerStatus.Error().offset);
    }

    return CheckedAddWithinLimit(
        kMinimumControlRecordBytes,
        record.payload.size(),
        kMaximumControlRecordBytes,
        kControlRecordBytesOffset);
}

ProtocolStatus SerializeBootstrapRecord(
    const BootstrapRecord& record,
    const std::span<std::byte> output) noexcept
{
    const ProtocolStatus validationStatus = ValidateBootstrapRecord(record);
    if (!validationStatus)
    {
        return validationStatus;
    }
    if (output.size() != kBootstrapRecordBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidRecordSize,
            0);
    }

    std::array<std::byte, kBootstrapRecordBytes> scratch{};
    ByteWriter writer(scratch);
    ProtocolStatus status = WriteBootstrapRecord(record, writer);
    if (!status)
    {
        return status;
    }
    if (writer.Position() != kBootstrapCrcOffset)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            writer.Position());
    }

    const std::uint32_t crc32c = ComputeCrc32c(
        std::span<const std::byte>(scratch).first(kBootstrapCrcOffset));
    status = writer.WriteUint32(crc32c);
    if (!status)
    {
        return status;
    }
    if (writer.Position() != scratch.size())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            writer.Position());
    }

    std::copy(scratch.begin(), scratch.end(), output.begin());
    return ProtocolStatus::Success();
}

ProtocolResult<BootstrapRecord> ParseBootstrapRecord(
    const std::span<const std::byte> input) noexcept
{
    if (input.size() < kBootstrapRecordBytes)
    {
        return ProtocolResult<BootstrapRecord>::Failure(
            ProtocolErrorCode::TruncatedInput,
            input.size());
    }
    if (input.size() > kBootstrapRecordBytes)
    {
        return ProtocolResult<BootstrapRecord>::Failure(
            ProtocolErrorCode::TrailingBytes,
            kBootstrapRecordBytes);
    }

    const ProtocolStatus magicStatus = ValidateMagic(
        input.first(kBootstrapRecordMagic.size()),
        kBootstrapRecordMagic,
        kBootstrapMagicOffset);
    if (!magicStatus)
    {
        return FailureFrom<BootstrapRecord>(magicStatus.Error());
    }

    ByteReader reader(input);
    const auto magicResult =
        reader.ReadFixedBytes<kBootstrapRecordMagic.size()>();
    if (!magicResult)
    {
        return FailureFrom<BootstrapRecord>(magicResult.Error());
    }
    const auto bootstrapVersionResult = reader.ReadUint8();
    if (!bootstrapVersionResult)
    {
        return FailureFrom<BootstrapRecord>(bootstrapVersionResult.Error());
    }
    const auto protocolMajorResult = reader.ReadUint8();
    if (!protocolMajorResult)
    {
        return FailureFrom<BootstrapRecord>(protocolMajorResult.Error());
    }
    const auto protocolMinorResult = reader.ReadUint8();
    if (!protocolMinorResult)
    {
        return FailureFrom<BootstrapRecord>(protocolMinorResult.Error());
    }
    const auto visualLayoutVersionResult = reader.ReadUint8();
    if (!visualLayoutVersionResult)
    {
        return FailureFrom<BootstrapRecord>(visualLayoutVersionResult.Error());
    }
    const auto visualProfileIdResult = reader.ReadUint64();
    if (!visualProfileIdResult)
    {
        return FailureFrom<BootstrapRecord>(visualProfileIdResult.Error());
    }
    const auto sessionTagResult = reader.ReadUint64();
    if (!sessionTagResult)
    {
        return FailureFrom<BootstrapRecord>(sessionTagResult.Error());
    }
    const auto frameSequenceResult = reader.ReadUint64();
    if (!frameSequenceResult)
    {
        return FailureFrom<BootstrapRecord>(frameSequenceResult.Error());
    }
    const auto controlEpochResult = reader.ReadUint32();
    if (!controlEpochResult)
    {
        return FailureFrom<BootstrapRecord>(controlEpochResult.Error());
    }
    const auto flagsResult = reader.ReadUint32();
    if (!flagsResult)
    {
        return FailureFrom<BootstrapRecord>(flagsResult.Error());
    }
    const auto crc32cResult = reader.ReadUint32();
    if (!crc32cResult)
    {
        return FailureFrom<BootstrapRecord>(crc32cResult.Error());
    }
    const ProtocolStatus consumedStatus = reader.RequireFullyConsumed();
    if (!consumedStatus)
    {
        return FailureFrom<BootstrapRecord>(consumedStatus.Error());
    }

    const std::uint32_t expectedCrc32c = ComputeCrc32c(
        input.first(kBootstrapCrcOffset));
    if (crc32cResult.Value() != expectedCrc32c)
    {
        return ProtocolResult<BootstrapRecord>::Failure(
            ProtocolErrorCode::CrcMismatch,
            kBootstrapCrcOffset);
    }

    const BootstrapRecord record{
        bootstrapVersionResult.Value(),
        ProtocolVersion{
            protocolMajorResult.Value(),
            protocolMinorResult.Value()},
        visualLayoutVersionResult.Value(),
        visualProfileIdResult.Value(),
        SessionTag{sessionTagResult.Value()},
        frameSequenceResult.Value(),
        controlEpochResult.Value(),
        flagsResult.Value()};
    const ProtocolStatus validationStatus = ValidateBootstrapRecord(record);
    if (!validationStatus)
    {
        return FailureFrom<BootstrapRecord>(validationStatus.Error());
    }

    return ProtocolResult<BootstrapRecord>::Success(record);
}

ProtocolStatus SerializeControlRecord(
    const ControlRecordView& record,
    const std::span<std::byte> output) noexcept
{
    const auto serializedSizeResult = GetSerializedSize(record);
    if (!serializedSizeResult)
    {
        return ProtocolStatus::Failure(
            serializedSizeResult.Error().code,
            serializedSizeResult.Error().offset);
    }

    const std::size_t serializedSize = serializedSizeResult.Value();
    if (output.size() != serializedSize)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidRecordSize,
            0);
    }

    const auto encodedSizeResult = CheckedNarrowUnsigned<std::uint32_t>(
        serializedSize,
        kControlRecordBytesOffset);
    if (!encodedSizeResult)
    {
        return ProtocolStatus::Failure(
            encodedSizeResult.Error().code,
            encodedSizeResult.Error().offset);
    }

    std::vector<std::byte> scratch;
    try
    {
        scratch.resize(serializedSize);
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kControlRecordBytesOffset);
    }
    catch (const std::length_error&)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kControlRecordBytesOffset);
    }

    ByteWriter writer(scratch);
    ProtocolStatus status = WriteControlRecordPrefix(
        record,
        encodedSizeResult.Value(),
        writer);
    if (!status)
    {
        return status;
    }
    status = writer.WriteBytes(record.payload);
    if (!status)
    {
        return status;
    }

    const std::size_t crcOffset = serializedSize - kControlRecordCrcBytes;
    if (writer.Position() != crcOffset)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            writer.Position());
    }
    const std::uint32_t crc32c = ComputeCrc32c(
        std::span<const std::byte>(scratch).first(crcOffset));
    status = writer.WriteUint32(crc32c);
    if (!status)
    {
        return status;
    }
    if (writer.Position() != scratch.size())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            writer.Position());
    }

    std::copy(scratch.begin(), scratch.end(), output.begin());
    return ProtocolStatus::Success();
}

ProtocolResult<ControlRecordView> ParseControlRecord(
    const std::span<const std::byte> input) noexcept
{
    if (input.size() < kMinimumControlRecordBytes)
    {
        return ProtocolResult<ControlRecordView>::Failure(
            ProtocolErrorCode::TruncatedInput,
            input.size());
    }
    if (input.size() > kMaximumControlRecordBytes)
    {
        return ProtocolResult<ControlRecordView>::Failure(
            ProtocolErrorCode::LengthLimitExceeded,
            kControlRecordBytesOffset);
    }

    const ProtocolStatus magicStatus = ValidateMagic(
        input.first(kControlRecordMagic.size()),
        kControlRecordMagic,
        kControlMagicOffset);
    if (!magicStatus)
    {
        return FailureFrom<ControlRecordView>(magicStatus.Error());
    }

    ByteReader reader(input);
    const auto magicResult =
        reader.ReadFixedBytes<kControlRecordMagic.size()>();
    if (!magicResult)
    {
        return FailureFrom<ControlRecordView>(magicResult.Error());
    }
    const auto controlVersionResult = reader.ReadUint8();
    if (!controlVersionResult)
    {
        return FailureFrom<ControlRecordView>(controlVersionResult.Error());
    }
    const auto recordTypeResult = reader.ReadUint8();
    if (!recordTypeResult)
    {
        return FailureFrom<ControlRecordView>(recordTypeResult.Error());
    }
    const auto controlSequenceResult = reader.ReadUint64();
    if (!controlSequenceResult)
    {
        return FailureFrom<ControlRecordView>(controlSequenceResult.Error());
    }
    const auto sessionTagResult = reader.ReadUint64();
    if (!sessionTagResult)
    {
        return FailureFrom<ControlRecordView>(sessionTagResult.Error());
    }
    const auto recordBytesResult = reader.ReadUint32();
    if (!recordBytesResult)
    {
        return FailureFrom<ControlRecordView>(recordBytesResult.Error());
    }

    const auto recordByteCountResult = CheckedNarrowUnsigned<std::size_t>(
        recordBytesResult.Value(),
        kControlRecordBytesOffset);
    if (!recordByteCountResult)
    {
        return FailureFrom<ControlRecordView>(recordByteCountResult.Error());
    }
    const std::size_t recordByteCount = recordByteCountResult.Value();
    if (recordByteCount < kMinimumControlRecordBytes)
    {
        return ProtocolResult<ControlRecordView>::Failure(
            ProtocolErrorCode::InvalidRecordSize,
            kControlRecordBytesOffset);
    }
    if (recordByteCount > kMaximumControlRecordBytes)
    {
        return ProtocolResult<ControlRecordView>::Failure(
            ProtocolErrorCode::LengthLimitExceeded,
            kControlRecordBytesOffset);
    }
    if (recordByteCount != input.size())
    {
        return ProtocolResult<ControlRecordView>::Failure(
            ProtocolErrorCode::InvalidRecordSize,
            kControlRecordBytesOffset);
    }

    const std::size_t payloadByteCount =
        recordByteCount - kMinimumControlRecordBytes;
    const auto payloadResult = reader.ReadBytes(payloadByteCount);
    if (!payloadResult)
    {
        return FailureFrom<ControlRecordView>(payloadResult.Error());
    }
    const auto crc32cResult = reader.ReadUint32();
    if (!crc32cResult)
    {
        return FailureFrom<ControlRecordView>(crc32cResult.Error());
    }
    const ProtocolStatus consumedStatus = reader.RequireFullyConsumed();
    if (!consumedStatus)
    {
        return FailureFrom<ControlRecordView>(consumedStatus.Error());
    }

    const std::size_t crcOffset = recordByteCount - kControlRecordCrcBytes;
    const std::uint32_t expectedCrc32c = ComputeCrc32c(input.first(crcOffset));
    if (crc32cResult.Value() != expectedCrc32c)
    {
        return ProtocolResult<ControlRecordView>::Failure(
            ProtocolErrorCode::CrcMismatch,
            crcOffset);
    }

    const ControlRecordView record{
        controlVersionResult.Value(),
        static_cast<ControlRecordType>(recordTypeResult.Value()),
        controlSequenceResult.Value(),
        SessionTag{sessionTagResult.Value()},
        payloadResult.Value()};
    const ProtocolStatus validationStatus = ValidateControlRecordHeader(record);
    if (!validationStatus)
    {
        return FailureFrom<ControlRecordView>(validationStatus.Error());
    }

    return ProtocolResult<ControlRecordView>::Success(record);
}

} // namespace pbprotocol
