#include "pbprotocol/control_fragment_codec.h"

#include "pbprotocol/byte_io.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/crc32c.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <stdexcept>
#include <vector>

namespace pbprotocol {

namespace {

constexpr std::size_t kControlRecordIdOffset = 0;
constexpr std::size_t kFragmentIndexOffset = 8;
constexpr std::size_t kFragmentCountOffset = 10;
constexpr std::size_t kTotalRecordBytesOffset = 12;
constexpr std::size_t kFragmentBytesOffset = 16;
constexpr std::size_t kFragmentFlagsOffset = 18;

template <typename ValueType>
[[nodiscard]] ProtocolResult<ValueType> FailureFrom(
    const ProtocolError& error) noexcept
{
    return ProtocolResult<ValueType>::Failure(error.code, error.offset);
}

[[nodiscard]] ProtocolStatus ValidateControlFragment(
    const ControlFragmentView& fragment) noexcept
{
    if (fragment.totalRecordBytes < kMinimumControlRecordBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidRecordSize,
            kTotalRecordBytesOffset);
    }
    if (fragment.totalRecordBytes > kMaximumControlRecordBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::LengthLimitExceeded,
            kTotalRecordBytesOffset);
    }
    if (fragment.fragmentCount == 0 ||
        fragment.fragmentIndex >= fragment.fragmentCount ||
        fragment.fragmentCount > fragment.totalRecordBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidControlFragment,
            fragment.fragmentCount == 0
                ? kFragmentCountOffset
                : fragment.fragmentIndex >= fragment.fragmentCount
                    ? kFragmentIndexOffset
                    : kFragmentCountOffset);
    }
    if (fragment.payload.empty())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidControlFragment,
            kFragmentBytesOffset);
    }
    if (fragment.payload.size() > kMaximumControlFragmentPayloadBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::LengthLimitExceeded,
            kFragmentBytesOffset);
    }
    if (fragment.flags != 0)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::NonZeroReservedBits,
            kFragmentFlagsOffset);
    }

    const std::size_t totalRecordBytes = fragment.totalRecordBytes;
    const std::size_t minimumOtherFragmentBytes =
        static_cast<std::size_t>(fragment.fragmentCount) - 1U;
    if (fragment.payload.size() >
        totalRecordBytes - minimumOtherFragmentBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidControlFragment,
            kFragmentBytesOffset);
    }
    if ((fragment.fragmentCount == 1 &&
         fragment.payload.size() != totalRecordBytes) ||
        (fragment.fragmentCount > 1 &&
         fragment.payload.size() >= totalRecordBytes))
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidControlFragment,
            kFragmentBytesOffset);
    }

    return ProtocolStatus::Success();
}

[[nodiscard]] ProtocolStatus WriteControlFragmentPrefix(
    const ControlFragmentView& fragment,
    const std::uint16_t fragmentBytes,
    ByteWriter& writer) noexcept
{
    ProtocolStatus status = writer.WriteUint64(fragment.controlRecordId);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint16(fragment.fragmentIndex);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint16(fragment.fragmentCount);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint32(fragment.totalRecordBytes);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint16(fragmentBytes);
    if (!status)
    {
        return status;
    }
    return writer.WriteUint16(fragment.flags);
}

} // namespace

ProtocolResult<std::size_t> GetSerializedSize(
    const ControlFragmentView& fragment) noexcept
{
    const ProtocolStatus validationStatus = ValidateControlFragment(fragment);
    if (!validationStatus)
    {
        return FailureFrom<std::size_t>(validationStatus.Error());
    }

    return CheckedAddWithinLimit(
        kControlFragmentPrefixBytes + kControlFragmentCrcBytes,
        fragment.payload.size(),
        kMaximumControlFragmentBytes,
        kFragmentBytesOffset);
}

ProtocolResult<std::uint16_t> GetControlFragmentCount(
    const std::span<const std::byte> recordBytes,
    const std::uint16_t maxFragmentPayloadBytes) noexcept
{
    if (maxFragmentPayloadBytes == 0)
    {
        return ProtocolResult<std::uint16_t>::Failure(
            ProtocolErrorCode::InvalidControlFragment,
            kFragmentBytesOffset);
    }

    const auto recordResult = ParseControlRecord(recordBytes);
    if (!recordResult)
    {
        return FailureFrom<std::uint16_t>(recordResult.Error());
    }

    const std::size_t payloadLimit = maxFragmentPayloadBytes;
    const auto roundedNumeratorResult = CheckedAddSize(
        recordBytes.size(),
        payloadLimit - 1U,
        kFragmentCountOffset);
    if (!roundedNumeratorResult)
    {
        return FailureFrom<std::uint16_t>(roundedNumeratorResult.Error());
    }
    const std::size_t fragmentCount =
        roundedNumeratorResult.Value() / payloadLimit;
    const auto fragmentCountResult = CheckedNarrowUnsigned<std::uint16_t>(
        fragmentCount,
        kFragmentCountOffset);
    if (!fragmentCountResult || fragmentCountResult.Value() == 0)
    {
        return ProtocolResult<std::uint16_t>::Failure(
            ProtocolErrorCode::LengthLimitExceeded,
            kFragmentCountOffset);
    }

    return fragmentCountResult;
}

ProtocolResult<ControlFragmentView> GetControlFragment(
    const std::uint64_t controlRecordId,
    const std::span<const std::byte> recordBytes,
    const std::uint16_t fragmentIndex,
    const std::uint16_t maxFragmentPayloadBytes) noexcept
{
    const auto fragmentCountResult = GetControlFragmentCount(
        recordBytes,
        maxFragmentPayloadBytes);
    if (!fragmentCountResult)
    {
        return FailureFrom<ControlFragmentView>(fragmentCountResult.Error());
    }
    if (fragmentIndex >= fragmentCountResult.Value())
    {
        return ProtocolResult<ControlFragmentView>::Failure(
            ProtocolErrorCode::InvalidControlFragment,
            kFragmentIndexOffset);
    }

    const std::size_t payloadLimit = maxFragmentPayloadBytes;
    const auto payloadOffsetResult = CheckedMultiplyUnsigned<std::size_t>(
        fragmentIndex,
        payloadLimit,
        kFragmentIndexOffset);
    if (!payloadOffsetResult ||
        payloadOffsetResult.Value() >= recordBytes.size())
    {
        return ProtocolResult<ControlFragmentView>::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            kFragmentIndexOffset);
    }
    const std::size_t payloadOffset = payloadOffsetResult.Value();
    const std::size_t payloadBytes = std::min(
        payloadLimit,
        recordBytes.size() - payloadOffset);
    const auto totalRecordBytesResult = CheckedNarrowUnsigned<std::uint32_t>(
        recordBytes.size(),
        kTotalRecordBytesOffset);
    if (!totalRecordBytesResult)
    {
        return FailureFrom<ControlFragmentView>(
            totalRecordBytesResult.Error());
    }

    return ProtocolResult<ControlFragmentView>::Success(ControlFragmentView{
        controlRecordId,
        fragmentIndex,
        fragmentCountResult.Value(),
        totalRecordBytesResult.Value(),
        0,
        recordBytes.subspan(payloadOffset, payloadBytes)});
}

ProtocolStatus SerializeControlFragment(
    const ControlFragmentView& fragment,
    const std::span<std::byte> output) noexcept
{
    const auto serializedSizeResult = GetSerializedSize(fragment);
    if (!serializedSizeResult)
    {
        return ProtocolStatus::Failure(
            serializedSizeResult.Error().code,
            serializedSizeResult.Error().offset);
    }
    if (output.size() != serializedSizeResult.Value())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidRecordSize,
            0);
    }

    const auto fragmentBytesResult = CheckedNarrowUnsigned<std::uint16_t>(
        fragment.payload.size(),
        kFragmentBytesOffset);
    if (!fragmentBytesResult)
    {
        return ProtocolStatus::Failure(
            fragmentBytesResult.Error().code,
            fragmentBytesResult.Error().offset);
    }

    std::vector<std::byte> scratch;
    try
    {
        scratch.resize(serializedSizeResult.Value());
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kFragmentBytesOffset);
    }
    catch (const std::length_error&)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kFragmentBytesOffset);
    }

    ByteWriter writer(scratch);
    ProtocolStatus status = WriteControlFragmentPrefix(
        fragment,
        fragmentBytesResult.Value(),
        writer);
    if (!status)
    {
        return status;
    }
    status = writer.WriteBytes(fragment.payload);
    if (!status)
    {
        return status;
    }

    const std::size_t crcOffset = scratch.size() - kControlFragmentCrcBytes;
    if (writer.Position() != crcOffset)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            writer.Position());
    }
    status = writer.WriteUint32(ComputeCrc32c(
        std::span<const std::byte>(scratch).first(crcOffset)));
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

ProtocolResult<ControlFragmentView> ParseControlFragment(
    const std::span<const std::byte> input) noexcept
{
    if (input.size() < kMinimumControlFragmentBytes)
    {
        return ProtocolResult<ControlFragmentView>::Failure(
            ProtocolErrorCode::TruncatedInput,
            input.size());
    }
    if (input.size() > kMaximumControlFragmentBytes)
    {
        return ProtocolResult<ControlFragmentView>::Failure(
            ProtocolErrorCode::LengthLimitExceeded,
            kFragmentBytesOffset);
    }

    ByteReader reader(input);
    const auto controlRecordIdResult = reader.ReadUint64();
    if (!controlRecordIdResult)
    {
        return FailureFrom<ControlFragmentView>(controlRecordIdResult.Error());
    }
    const auto fragmentIndexResult = reader.ReadUint16();
    if (!fragmentIndexResult)
    {
        return FailureFrom<ControlFragmentView>(fragmentIndexResult.Error());
    }
    const auto fragmentCountResult = reader.ReadUint16();
    if (!fragmentCountResult)
    {
        return FailureFrom<ControlFragmentView>(fragmentCountResult.Error());
    }
    const auto totalRecordBytesResult = reader.ReadUint32();
    if (!totalRecordBytesResult)
    {
        return FailureFrom<ControlFragmentView>(totalRecordBytesResult.Error());
    }
    const auto fragmentBytesResult = reader.ReadUint16();
    if (!fragmentBytesResult)
    {
        return FailureFrom<ControlFragmentView>(fragmentBytesResult.Error());
    }
    const auto flagsResult = reader.ReadUint16();
    if (!flagsResult)
    {
        return FailureFrom<ControlFragmentView>(flagsResult.Error());
    }

    const std::size_t declaredFragmentBytes = fragmentBytesResult.Value();
    const auto expectedInputBytesResult = CheckedAddWithinLimit(
        kControlFragmentPrefixBytes + kControlFragmentCrcBytes,
        declaredFragmentBytes,
        kMaximumControlFragmentBytes,
        kFragmentBytesOffset);
    if (!expectedInputBytesResult)
    {
        return FailureFrom<ControlFragmentView>(expectedInputBytesResult.Error());
    }
    if (expectedInputBytesResult.Value() != input.size())
    {
        return ProtocolResult<ControlFragmentView>::Failure(
            ProtocolErrorCode::InvalidRecordSize,
            kFragmentBytesOffset);
    }

    const auto payloadResult = reader.ReadBytes(declaredFragmentBytes);
    if (!payloadResult)
    {
        return FailureFrom<ControlFragmentView>(payloadResult.Error());
    }
    const auto crc32cResult = reader.ReadUint32();
    if (!crc32cResult)
    {
        return FailureFrom<ControlFragmentView>(crc32cResult.Error());
    }
    const ProtocolStatus consumedStatus = reader.RequireFullyConsumed();
    if (!consumedStatus)
    {
        return FailureFrom<ControlFragmentView>(consumedStatus.Error());
    }

    const std::size_t crcOffset = input.size() - kControlFragmentCrcBytes;
    if (crc32cResult.Value() != ComputeCrc32c(input.first(crcOffset)))
    {
        return ProtocolResult<ControlFragmentView>::Failure(
            ProtocolErrorCode::CrcMismatch,
            crcOffset);
    }

    const ControlFragmentView fragment{
        controlRecordIdResult.Value(),
        fragmentIndexResult.Value(),
        fragmentCountResult.Value(),
        totalRecordBytesResult.Value(),
        flagsResult.Value(),
        payloadResult.Value()};
    const ProtocolStatus validationStatus = ValidateControlFragment(fragment);
    if (!validationStatus)
    {
        return FailureFrom<ControlFragmentView>(validationStatus.Error());
    }

    return ProtocolResult<ControlFragmentView>::Success(fragment);
}

} // namespace pbprotocol
