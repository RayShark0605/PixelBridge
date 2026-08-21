#include "pbprotocol/byte_io.h"

#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace pbprotocol {

namespace {

[[nodiscard]] std::uint8_t ToUint8(const std::byte value) noexcept
{
    return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] std::uint8_t ToUint8(const char value) noexcept
{
    return static_cast<std::uint8_t>(static_cast<unsigned char>(value));
}

[[nodiscard]] bool IsContinuationByte(const std::uint8_t value) noexcept
{
    return value >= 0x80U && value <= 0xBFU;
}

[[nodiscard]] ProtocolStatus InvalidUtf8At(
    const std::size_t baseOffset,
    const std::size_t relativeOffset) noexcept
{
    const auto offsetResult = CheckedAddSize(baseOffset, relativeOffset, baseOffset);
    if (!offsetResult)
    {
        return ProtocolStatus::Failure(
            offsetResult.Error().code,
            offsetResult.Error().offset);
    }

    return ProtocolStatus::Failure(
        ProtocolErrorCode::InvalidUtf8,
        offsetResult.Value());
}

[[nodiscard]] ProtocolResult<std::size_t> GetLengthPrefixByteCount(
    const LengthPrefixWidth prefixWidth,
    const std::size_t errorOffset) noexcept
{
    switch (prefixWidth)
    {
    case LengthPrefixWidth::Uint16:
        return ProtocolResult<std::size_t>::Success(sizeof(std::uint16_t));
    case LengthPrefixWidth::Uint32:
        return ProtocolResult<std::size_t>::Success(sizeof(std::uint32_t));
    case LengthPrefixWidth::Uint64:
        return ProtocolResult<std::size_t>::Success(sizeof(std::uint64_t));
    default:
        return ProtocolResult<std::size_t>::Failure(
            ProtocolErrorCode::InvalidLengthPrefixWidth,
            errorOffset);
    }
}

[[nodiscard]] ProtocolStatus ValidateLengthFitsPrefix(
    const std::uint64_t length,
    const LengthPrefixWidth prefixWidth,
    const std::size_t errorOffset) noexcept
{
    switch (prefixWidth)
    {
    case LengthPrefixWidth::Uint16:
        if (length > std::numeric_limits<std::uint16_t>::max())
        {
            return ProtocolStatus::Failure(
                ProtocolErrorCode::LengthNarrowing,
                errorOffset);
        }
        break;
    case LengthPrefixWidth::Uint32:
        if (length > std::numeric_limits<std::uint32_t>::max())
        {
            return ProtocolStatus::Failure(
                ProtocolErrorCode::LengthNarrowing,
                errorOffset);
        }
        break;
    case LengthPrefixWidth::Uint64:
        break;
    default:
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidLengthPrefixWidth,
            errorOffset);
    }

    return ProtocolStatus::Success();
}

} // namespace

ProtocolStatus ValidateUtf8(
    const std::string_view text,
    const std::size_t baseOffset) noexcept
{
    std::size_t index = 0;
    while (index < text.size())
    {
        const std::uint8_t firstByte = ToUint8(text[index]);
        if (firstByte <= 0x7FU)
        {
            index++;
            continue;
        }

        std::size_t sequenceLength = 0;
        if (firstByte >= 0xC2U && firstByte <= 0xDFU)
        {
            sequenceLength = 2;
        }
        else if (firstByte >= 0xE0U && firstByte <= 0xEFU)
        {
            sequenceLength = 3;
        }
        else if (firstByte >= 0xF0U && firstByte <= 0xF4U)
        {
            sequenceLength = 4;
        }
        else
        {
            return InvalidUtf8At(baseOffset, index);
        }

        if (sequenceLength > text.size() - index)
        {
            return InvalidUtf8At(baseOffset, index);
        }

        const std::uint8_t secondByte = ToUint8(text[index + 1]);
        if (!IsContinuationByte(secondByte))
        {
            return InvalidUtf8At(baseOffset, index + 1);
        }

        if ((firstByte == 0xE0U && secondByte < 0xA0U) ||
            (firstByte == 0xEDU && secondByte > 0x9FU) ||
            (firstByte == 0xF0U && secondByte < 0x90U) ||
            (firstByte == 0xF4U && secondByte > 0x8FU))
        {
            return InvalidUtf8At(baseOffset, index + 1);
        }

        for (std::size_t continuationIndex = 2;
             continuationIndex < sequenceLength;
             continuationIndex++)
        {
            if (!IsContinuationByte(ToUint8(text[index + continuationIndex])))
            {
                return InvalidUtf8At(baseOffset, index + continuationIndex);
            }
        }

        index += sequenceLength;
    }

    return ProtocolStatus::Success();
}

ByteReader::ByteReader(const std::span<const std::byte> input) noexcept
    : input_(input)
{
}

ByteReader::ByteReader(
    const std::span<const std::byte> input,
    const std::size_t baseOffset) noexcept
    : input_(input),
      baseOffset_(baseOffset)
{
}

std::size_t ByteReader::Position() const noexcept
{
    return position_;
}

std::size_t ByteReader::AbsolutePosition() const noexcept
{
    return baseOffset_ + position_;
}

std::size_t ByteReader::Remaining() const noexcept
{
    return input_.size() - position_;
}

ProtocolResult<std::span<const std::byte>> ByteReader::ReadBytes(
    const std::size_t byteCount) noexcept
{
    if (byteCount > Remaining())
    {
        return ProtocolResult<std::span<const std::byte>>::Failure(
            ProtocolErrorCode::TruncatedInput,
            AbsolutePosition());
    }

    const std::span<const std::byte> bytes = input_.subspan(position_, byteCount);
    position_ += byteCount;
    return ProtocolResult<std::span<const std::byte>>::Success(bytes);
}

ProtocolResult<std::uint16_t> ByteReader::ReadUint16() noexcept
{
    const auto bytesResult = ReadBytes(sizeof(std::uint16_t));
    if (!bytesResult)
    {
        return ProtocolResult<std::uint16_t>::Failure(
            bytesResult.Error().code,
            bytesResult.Error().offset);
    }

    const std::span<const std::byte> bytes = bytesResult.Value();
    const std::uint16_t value =
        static_cast<std::uint16_t>(ToUint8(bytes[0])) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(ToUint8(bytes[1])) << 8U);
    return ProtocolResult<std::uint16_t>::Success(value);
}

ProtocolResult<std::uint32_t> ByteReader::ReadUint32() noexcept
{
    const auto bytesResult = ReadBytes(sizeof(std::uint32_t));
    if (!bytesResult)
    {
        return ProtocolResult<std::uint32_t>::Failure(
            bytesResult.Error().code,
            bytesResult.Error().offset);
    }

    const std::span<const std::byte> bytes = bytesResult.Value();
    std::uint32_t value = 0;
    for (std::size_t byteIndex = 0; byteIndex < bytes.size(); byteIndex++)
    {
        value |= static_cast<std::uint32_t>(ToUint8(bytes[byteIndex]))
            << static_cast<unsigned int>(byteIndex * 8U);
    }
    return ProtocolResult<std::uint32_t>::Success(value);
}

ProtocolResult<std::uint64_t> ByteReader::ReadUint64() noexcept
{
    const auto bytesResult = ReadBytes(sizeof(std::uint64_t));
    if (!bytesResult)
    {
        return ProtocolResult<std::uint64_t>::Failure(
            bytesResult.Error().code,
            bytesResult.Error().offset);
    }

    const std::span<const std::byte> bytes = bytesResult.Value();
    std::uint64_t value = 0;
    for (std::size_t byteIndex = 0; byteIndex < bytes.size(); byteIndex++)
    {
        value |= static_cast<std::uint64_t>(ToUint8(bytes[byteIndex]))
            << static_cast<unsigned int>(byteIndex * 8U);
    }
    return ProtocolResult<std::uint64_t>::Success(value);
}

ProtocolResult<std::uint64_t> ByteReader::ReadLengthPrefix(
    const LengthPrefixWidth prefixWidth) noexcept
{
    switch (prefixWidth)
    {
    case LengthPrefixWidth::Uint16:
    {
        const auto valueResult = ReadUint16();
        if (!valueResult)
        {
            return ProtocolResult<std::uint64_t>::Failure(
                valueResult.Error().code,
                valueResult.Error().offset);
        }
        return ProtocolResult<std::uint64_t>::Success(valueResult.Value());
    }
    case LengthPrefixWidth::Uint32:
    {
        const auto valueResult = ReadUint32();
        if (!valueResult)
        {
            return ProtocolResult<std::uint64_t>::Failure(
                valueResult.Error().code,
                valueResult.Error().offset);
        }
        return ProtocolResult<std::uint64_t>::Success(valueResult.Value());
    }
    case LengthPrefixWidth::Uint64:
        return ReadUint64();
    default:
        return ProtocolResult<std::uint64_t>::Failure(
            ProtocolErrorCode::InvalidLengthPrefixWidth,
            AbsolutePosition());
    }
}

ProtocolResult<std::span<const std::byte>> ByteReader::ReadLengthDelimitedBytes(
    const LengthPrefixWidth prefixWidth,
    const std::size_t maximumByteLength) noexcept
{
    ByteReader candidate = *this;
    const std::size_t lengthOffset = candidate.AbsolutePosition();
    const auto lengthResult = candidate.ReadLengthPrefix(prefixWidth);
    if (!lengthResult)
    {
        return ProtocolResult<std::span<const std::byte>>::Failure(
            lengthResult.Error().code,
            lengthResult.Error().offset);
    }

    const auto payloadSizeResult = CheckedUint64ToSize(lengthResult.Value(), lengthOffset);
    if (!payloadSizeResult)
    {
        return ProtocolResult<std::span<const std::byte>>::Failure(
            payloadSizeResult.Error().code,
            payloadSizeResult.Error().offset);
    }

    const std::size_t payloadByteCount = payloadSizeResult.Value();
    if (payloadByteCount > maximumByteLength)
    {
        return ProtocolResult<std::span<const std::byte>>::Failure(
            ProtocolErrorCode::LengthLimitExceeded,
            lengthOffset);
    }

    const auto endPositionResult = CheckedAddSize(
        candidate.position_,
        payloadByteCount,
        lengthOffset);
    if (!endPositionResult)
    {
        return ProtocolResult<std::span<const std::byte>>::Failure(
            endPositionResult.Error().code,
            endPositionResult.Error().offset);
    }

    const std::size_t endPosition = endPositionResult.Value();
    if (endPosition > candidate.input_.size())
    {
        return ProtocolResult<std::span<const std::byte>>::Failure(
            ProtocolErrorCode::TruncatedInput,
            candidate.AbsolutePosition());
    }

    const std::span<const std::byte> payload = candidate.input_.subspan(
        candidate.position_,
        payloadByteCount);
    candidate.position_ = endPosition;
    *this = candidate;
    return ProtocolResult<std::span<const std::byte>>::Success(payload);
}

ProtocolResult<std::string_view> ByteReader::ReadLengthDelimitedUtf8(
    const LengthPrefixWidth prefixWidth,
    const std::size_t maximumByteLength) noexcept
{
    ByteReader candidate = *this;
    const auto bytesResult = candidate.ReadLengthDelimitedBytes(
        prefixWidth,
        maximumByteLength);
    if (!bytesResult)
    {
        return ProtocolResult<std::string_view>::Failure(
            bytesResult.Error().code,
            bytesResult.Error().offset);
    }

    const std::span<const std::byte> bytes = bytesResult.Value();
    const std::size_t payloadPosition = candidate.position_ - bytes.size();
    const std::size_t payloadOffset = candidate.baseOffset_ + payloadPosition;
    const std::string_view text = bytes.empty()
        ? std::string_view{}
        : std::string_view(
            static_cast<const char*>(static_cast<const void*>(bytes.data())),
            bytes.size());
    const ProtocolStatus utf8Status = ValidateUtf8(text, payloadOffset);
    if (!utf8Status)
    {
        return ProtocolResult<std::string_view>::Failure(
            utf8Status.Error().code,
            utf8Status.Error().offset);
    }

    *this = candidate;
    return ProtocolResult<std::string_view>::Success(text);
}

ProtocolResult<ByteReader> ByteReader::ReadLengthDelimitedReader(
    const LengthPrefixWidth prefixWidth,
    const std::size_t maximumByteLength) noexcept
{
    ByteReader candidate = *this;
    const auto bytesResult = candidate.ReadLengthDelimitedBytes(
        prefixWidth,
        maximumByteLength);
    if (!bytesResult)
    {
        return ProtocolResult<ByteReader>::Failure(
            bytesResult.Error().code,
            bytesResult.Error().offset);
    }

    const std::span<const std::byte> bytes = bytesResult.Value();
    const std::size_t payloadPosition = candidate.position_ - bytes.size();
    const auto payloadOffsetResult = CheckedAddSize(
        candidate.baseOffset_,
        payloadPosition,
        candidate.baseOffset_);
    if (!payloadOffsetResult)
    {
        return ProtocolResult<ByteReader>::Failure(
            payloadOffsetResult.Error().code,
            payloadOffsetResult.Error().offset);
    }

    ByteReader nestedReader(bytes, payloadOffsetResult.Value());
    *this = candidate;
    return ProtocolResult<ByteReader>::Success(nestedReader);
}

ProtocolStatus ByteReader::ReadReservedZeroBytes(
    const std::size_t byteCount) noexcept
{
    if (byteCount > Remaining())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::TruncatedInput,
            AbsolutePosition());
    }

    for (std::size_t byteIndex = 0; byteIndex < byteCount; byteIndex++)
    {
        if (input_[position_ + byteIndex] != std::byte{0})
        {
            return ProtocolStatus::Failure(
                ProtocolErrorCode::NonZeroReservedByte,
                AbsolutePosition() + byteIndex);
        }
    }

    position_ += byteCount;
    return ProtocolStatus::Success();
}

ProtocolStatus ByteReader::ReadCanonicalZeroPadding(
    const std::size_t byteCount) noexcept
{
    if (byteCount > Remaining())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::TruncatedInput,
            AbsolutePosition());
    }

    for (std::size_t byteIndex = 0; byteIndex < byteCount; byteIndex++)
    {
        if (input_[position_ + byteIndex] != std::byte{0})
        {
            return ProtocolStatus::Failure(
                ProtocolErrorCode::NonCanonicalPadding,
                AbsolutePosition() + byteIndex);
        }
    }

    position_ += byteCount;
    return ProtocolStatus::Success();
}

ProtocolStatus ByteReader::RequireFullyConsumed() const noexcept
{
    if (Remaining() != 0)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::TrailingBytes,
            AbsolutePosition());
    }

    return ProtocolStatus::Success();
}

ByteWriter::ByteWriter(const std::span<std::byte> output) noexcept
    : output_(output)
{
}

std::size_t ByteWriter::Position() const noexcept
{
    return position_;
}

std::size_t ByteWriter::Remaining() const noexcept
{
    return output_.size() - position_;
}

std::span<const std::byte> ByteWriter::WrittenBytes() const noexcept
{
    return output_.first(position_);
}

ProtocolStatus ByteWriter::WriteUint16(const std::uint16_t value) noexcept
{
    if (sizeof(value) > Remaining())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::OutputBufferTooSmall,
            position_);
    }

    output_[position_] = static_cast<std::byte>(value & 0xFFU);
    output_[position_ + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    position_ += sizeof(value);
    return ProtocolStatus::Success();
}

ProtocolStatus ByteWriter::WriteUint32(const std::uint32_t value) noexcept
{
    if (sizeof(value) > Remaining())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::OutputBufferTooSmall,
            position_);
    }

    for (std::size_t byteIndex = 0; byteIndex < sizeof(value); byteIndex++)
    {
        output_[position_ + byteIndex] = static_cast<std::byte>(
            (value >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU);
    }
    position_ += sizeof(value);
    return ProtocolStatus::Success();
}

ProtocolStatus ByteWriter::WriteUint64(const std::uint64_t value) noexcept
{
    if (sizeof(value) > Remaining())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::OutputBufferTooSmall,
            position_);
    }

    for (std::size_t byteIndex = 0; byteIndex < sizeof(value); byteIndex++)
    {
        output_[position_ + byteIndex] = static_cast<std::byte>(
            (value >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU);
    }
    position_ += sizeof(value);
    return ProtocolStatus::Success();
}

ProtocolStatus ByteWriter::WriteBytes(
    const std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() > Remaining())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::OutputBufferTooSmall,
            position_);
    }

    if (!bytes.empty())
    {
        std::memmove(
            output_.data() + position_,
            bytes.data(),
            bytes.size());
    }
    position_ += bytes.size();
    return ProtocolStatus::Success();
}

ProtocolResult<ByteWriter::LengthWritePlan> ByteWriter::PrepareLengthDelimitedWrite(
    const LengthPrefixWidth prefixWidth,
    const std::size_t payloadByteCount,
    const std::size_t maximumByteLength) const noexcept
{
    const std::size_t lengthOffset = position_;
    const auto prefixByteCountResult = GetLengthPrefixByteCount(
        prefixWidth,
        lengthOffset);
    if (!prefixByteCountResult)
    {
        return ProtocolResult<LengthWritePlan>::Failure(
            prefixByteCountResult.Error().code,
            prefixByteCountResult.Error().offset);
    }

    if (payloadByteCount > maximumByteLength)
    {
        return ProtocolResult<LengthWritePlan>::Failure(
            ProtocolErrorCode::LengthLimitExceeded,
            lengthOffset);
    }

    const auto encodedLengthResult = CheckedNarrowUnsigned<std::uint64_t>(
        payloadByteCount,
        lengthOffset);
    if (!encodedLengthResult)
    {
        return ProtocolResult<LengthWritePlan>::Failure(
            encodedLengthResult.Error().code,
            encodedLengthResult.Error().offset);
    }

    const ProtocolStatus lengthStatus = ValidateLengthFitsPrefix(
        encodedLengthResult.Value(),
        prefixWidth,
        lengthOffset);
    if (!lengthStatus)
    {
        return ProtocolResult<LengthWritePlan>::Failure(
            lengthStatus.Error().code,
            lengthStatus.Error().offset);
    }

    const auto totalByteCountResult = CheckedAddSize(
        prefixByteCountResult.Value(),
        payloadByteCount,
        lengthOffset);
    if (!totalByteCountResult)
    {
        return ProtocolResult<LengthWritePlan>::Failure(
            totalByteCountResult.Error().code,
            totalByteCountResult.Error().offset);
    }

    if (totalByteCountResult.Value() > Remaining())
    {
        return ProtocolResult<LengthWritePlan>::Failure(
            ProtocolErrorCode::OutputBufferTooSmall,
            lengthOffset);
    }

    const LengthWritePlan plan{
        prefixByteCountResult.Value(),
        totalByteCountResult.Value(),
        encodedLengthResult.Value()};
    return ProtocolResult<LengthWritePlan>::Success(plan);
}

void ByteWriter::WriteLengthPrefixUnchecked(
    const LengthPrefixWidth prefixWidth,
    const std::uint64_t value) noexcept
{
    std::size_t prefixByteCount = 0;
    switch (prefixWidth)
    {
    case LengthPrefixWidth::Uint16:
        prefixByteCount = sizeof(std::uint16_t);
        break;
    case LengthPrefixWidth::Uint32:
        prefixByteCount = sizeof(std::uint32_t);
        break;
    case LengthPrefixWidth::Uint64:
        prefixByteCount = sizeof(std::uint64_t);
        break;
    default:
        return;
    }

    for (std::size_t byteIndex = 0; byteIndex < prefixByteCount; byteIndex++)
    {
        output_[position_ + byteIndex] = static_cast<std::byte>(
            (value >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU);
    }
}

ProtocolStatus ByteWriter::WriteLengthDelimitedBytes(
    const LengthPrefixWidth prefixWidth,
    const std::span<const std::byte> bytes,
    const std::size_t maximumByteLength) noexcept
{
    const auto planResult = PrepareLengthDelimitedWrite(
        prefixWidth,
        bytes.size(),
        maximumByteLength);
    if (!planResult)
    {
        return ProtocolStatus::Failure(
            planResult.Error().code,
            planResult.Error().offset);
    }

    const LengthWritePlan plan = planResult.Value();
    const std::size_t originalPosition = position_;
    if (!bytes.empty())
    {
        // Move the payload first so an input span that aliases the prefix area
        // cannot be corrupted before its bytes are copied.
        std::memmove(
            output_.data() + originalPosition + plan.prefixByteCount,
            bytes.data(),
            bytes.size());
    }
    WriteLengthPrefixUnchecked(prefixWidth, plan.encodedLength);
    position_ = originalPosition + plan.totalByteCount;
    return ProtocolStatus::Success();
}

ProtocolStatus ByteWriter::WriteLengthDelimitedUtf8(
    const LengthPrefixWidth prefixWidth,
    const std::string_view text,
    const std::size_t maximumByteLength) noexcept
{
    const auto planResult = PrepareLengthDelimitedWrite(
        prefixWidth,
        text.size(),
        maximumByteLength);
    if (!planResult)
    {
        return ProtocolStatus::Failure(
            planResult.Error().code,
            planResult.Error().offset);
    }

    const LengthWritePlan plan = planResult.Value();
    const std::size_t originalPosition = position_;
    const ProtocolStatus utf8Status = ValidateUtf8(
        text,
        originalPosition + plan.prefixByteCount);
    if (!utf8Status)
    {
        return utf8Status;
    }

    if (!text.empty())
    {
        // char and std::byte both provide byte-wise views. memmove also keeps
        // an aliased source string valid until the payload has been moved.
        std::memmove(
            output_.data() + originalPosition + plan.prefixByteCount,
            text.data(),
            text.size());
    }
    WriteLengthPrefixUnchecked(prefixWidth, plan.encodedLength);
    position_ = originalPosition + plan.totalByteCount;
    return ProtocolStatus::Success();
}

ProtocolStatus ByteWriter::WriteCanonicalZeroPadding(
    const std::size_t byteCount) noexcept
{
    if (byteCount > Remaining())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::OutputBufferTooSmall,
            position_);
    }

    if (byteCount != 0)
    {
        std::fill_n(output_.begin() + position_, byteCount, std::byte{0});
    }
    position_ += byteCount;
    return ProtocolStatus::Success();
}

} // namespace pbprotocol
