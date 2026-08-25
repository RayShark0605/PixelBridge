#include "pbprotocol/transport_block_codec.h"

#include "pbprotocol/byte_io.h"
#include "pbprotocol/crc32c.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>

namespace pbprotocol {

namespace {

[[nodiscard]] ProtocolStatus ValidateTransportHeaderSemantics(
    const TransportBlockHeader& header,
    const std::size_t baseOffset) noexcept
{
    if (header.blockType != kTransportBlockTypeData)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidEnumValue,
            baseOffset + kTransportBlockTypeOffset);
    }
    if (header.protocolMinor != kTransportProtocolMinor)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::UnsupportedProtocolMinor,
            baseOffset + kTransportProtocolMinorOffset);
    }
    if (header.flags != 0)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::NonZeroReservedBits,
            baseOffset + kTransportFlagsOffset);
    }
    return ProtocolStatus::Success();
}

[[nodiscard]] std::uint16_t ReadUint16LittleEndian(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(input[offset]) |
        (std::to_integer<std::uint16_t>(input[offset + 1]) << 8));
}

[[nodiscard]] std::uint32_t ReadUint32LittleEndian(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t byteIndex = 0; byteIndex < 4; byteIndex++)
    {
        value |= std::to_integer<std::uint32_t>(input[offset + byteIndex]) <<
            static_cast<unsigned int>(byteIndex * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t ReadUint64LittleEndian(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t byteIndex = 0; byteIndex < 8; byteIndex++)
    {
        value |= std::to_integer<std::uint64_t>(input[offset + byteIndex]) <<
            static_cast<unsigned int>(byteIndex * 8U);
    }
    return value;
}

[[nodiscard]] bool SpansOverlap(
    const std::span<const std::byte> first,
    const std::span<const std::byte> second) noexcept
{
    if (first.empty() || second.empty())
    {
        return false;
    }

    const std::byte* const firstBegin = first.data();
    const std::byte* const firstEnd = firstBegin + first.size();
    const std::byte* const secondBegin = second.data();
    const std::byte* const secondEnd = secondBegin + second.size();
    const std::less<const std::byte*> addressLess;
    return addressLess(firstBegin, secondEnd) &&
        addressLess(secondBegin, firstEnd);
}

[[nodiscard]] ProtocolResult<TransportBlockHeader> ParseTransportHeader(
    const std::span<const std::byte> input) noexcept
{
    if (input.size() < kTransportHeaderBytes)
    {
        return ProtocolResult<TransportBlockHeader>::Failure(
            ProtocolErrorCode::TruncatedInput,
            input.size());
    }

    const TransportBlockHeader header{
        std::to_integer<std::uint8_t>(input[kTransportBlockTypeOffset]),
        std::to_integer<std::uint8_t>(input[kTransportProtocolMinorOffset]),
        ReadUint16LittleEndian(input, kTransportFlagsOffset),
        SessionTag{ReadUint64LittleEndian(input, kTransportSessionTagOffset)},
        ReadUint64LittleEndian(input, kTransportSegmentOrdinalOffset),
        ReadUint32LittleEndian(input, kTransportOuterBlockIdOffset),
        ReadUint16LittleEndian(input, kTransportPayloadBytesOffset)};
    const ProtocolStatus semanticsStatus =
        ValidateTransportHeaderSemantics(header, 0);
    if (!semanticsStatus)
    {
        return ProtocolResult<TransportBlockHeader>::Failure(
            semanticsStatus.Error().code,
            semanticsStatus.Error().offset);
    }
    if (ReadUint16LittleEndian(input, kTransportReservedOffset) != 0)
    {
        return ProtocolResult<TransportBlockHeader>::Failure(
            ProtocolErrorCode::NonZeroReservedBits,
            kTransportReservedOffset);
    }

    const std::uint32_t recomputedHeaderCrc = ComputeCrc32c(
        input.first(kTransportHeaderCrcCoverageBytes));
    const std::uint32_t storedHeaderCrc =
        ReadUint32LittleEndian(input, kTransportHeaderCrcOffset);
    if (recomputedHeaderCrc != storedHeaderCrc)
    {
        return ProtocolResult<TransportBlockHeader>::Failure(
            ProtocolErrorCode::CrcMismatch,
            kTransportHeaderCrcOffset);
    }
    return ProtocolResult<TransportBlockHeader>::Success(header);
}

} // namespace

bool TransportBlockView::operator==(const TransportBlockView& other) const
{
    if (header != other.header ||
        payload.size() != other.payload.size())
    {
        return false;
    }
    return std::equal(payload.begin(), payload.end(),
        other.payload.begin());
}

ProtocolStatus SerializeTransportBlock(
    const TransportBlockHeader& header,
    const std::span<const std::byte> payload,
    const std::span<std::byte> output) noexcept
{
    const ProtocolStatus semanticsStatus =
        ValidateTransportHeaderSemantics(header, 0);
    if (!semanticsStatus)
    {
        return semanticsStatus;
    }
    // The header is written into output before the payload is copied, so an
    // overlapping payload/output pair would read bytes the serializer itself
    // just destroyed; reject any intersection fail-closed (mirrors the
    // Inner-FEC encoder overlap rejection).
    if (SpansOverlap(payload, output))
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::OverlappingSpans, 0);
    }
    const std::size_t serializedSize = GetTransportSerializedSize(header);
    if (payload.size() != header.payloadBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidRecordSize,
            kTransportPayloadBytesOffset);
    }
    if (output.size() != serializedSize)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidRecordSize,
            0);
    }

    ByteWriter writer(output);
    ProtocolStatus status = writer.WriteUint8(header.blockType);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint8(header.protocolMinor);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint16(header.flags);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(header.sessionTag.value);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint64(header.segmentOrdinal);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint32(header.outerBlockId);
    if (!status)
    {
        return status;
    }
    status = writer.WriteUint16(header.payloadBytes);
    if (!status)
    {
        return status;
    }
    // Reserved word is frozen zero for the provisional layout.
    status = writer.WriteUint16(0);
    if (!status)
    {
        return status;
    }

    // The header CRC covers exactly the 28 header bytes written so
    // far (every field before the CRC field; a CRC cannot cover its
    // own stored value). The writer position is exactly
    // kTransportHeaderCrcCoverageBytes here.
    const std::uint32_t headerCrc = ComputeCrc32c(
        std::span<const std::byte>(output)
            .first(kTransportHeaderCrcCoverageBytes));
    status = writer.WriteUint32(headerCrc);
    if (!status)
    {
        return status;
    }
    status = writer.WriteBytes(payload);
    if (!status)
    {
        return status;
    }
    const std::uint32_t payloadCrc = ComputeCrc32c(payload);
    status = writer.WriteUint32(payloadCrc);
    if (!status)
    {
        return status;
    }
    if (writer.Position() != serializedSize)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            writer.Position());
    }
    return ProtocolStatus::Success();
}

ProtocolResult<TransportBlockView> ParseTransportBlock(
    const std::span<const std::byte> input) noexcept
{
    const auto headerResult = ParseTransportHeader(input);
    if (!headerResult)
    {
        return ProtocolResult<TransportBlockView>::Failure(
            headerResult.Error().code,
            headerResult.Error().offset);
    }
    const TransportBlockHeader& header = headerResult.Value();

    // The wire length is exactly 36 + PayloadBytes: accept neither trailing
    // bytes nor implicit truncation.
    const std::size_t expectedSize =
        GetTransportSerializedSize(header);
    if (input.size() < expectedSize)
    {
        return ProtocolResult<TransportBlockView>::Failure(
            ProtocolErrorCode::TruncatedInput,
            input.size());
    }
    if (input.size() > expectedSize)
    {
        return ProtocolResult<TransportBlockView>::Failure(
            ProtocolErrorCode::TrailingBytes,
            expectedSize);
    }

    const std::span<const std::byte> payload =
        input.subspan(kTransportPayloadOffset, header.payloadBytes);
    const std::uint32_t payloadCrc = ComputeCrc32c(payload);
    const std::size_t payloadCrcOffset =
        kTransportPayloadOffset + header.payloadBytes;
    const std::uint32_t storedPayloadCrc =
        ReadUint32LittleEndian(input, payloadCrcOffset);
    if (payloadCrc != storedPayloadCrc)
    {
        return ProtocolResult<TransportBlockView>::Failure(
            ProtocolErrorCode::CrcMismatch,
            payloadCrcOffset);
    }

    const TransportBlockView view{header, payload};
    return ProtocolResult<TransportBlockView>::Success(view);
}

ProtocolStatus FrameTransportBlockIntoInfoBlock(
    const std::span<const std::byte> serializedBlock,
    const std::size_t infoSize,
    const std::span<std::byte> outInfoBlock) noexcept
{
    if (infoSize < kTransportMinimumBlockBytes ||
        infoSize > kTransportMaximumBlockBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidRecordSize,
            0);
    }
    if (outInfoBlock.size() != infoSize)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::OutputBufferTooSmall,
            0);
    }
    if (serializedBlock.size() > infoSize)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::LengthLimitExceeded,
            serializedBlock.size());
    }
    // Zero-fill and copy are memcpy/memset semantics, so an overlapping
    // block/info pair is undefined behavior; reject fail-closed before any
    // write (mirrors the Inner-FEC encoder overlap rejection).
    if (SpansOverlap(serializedBlock, outInfoBlock))
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::OverlappingSpans, 0);
    }
    const auto parsedBlock = ParseTransportBlock(serializedBlock);
    if (!parsedBlock)
    {
        return ProtocolStatus::Failure(
            parsedBlock.Error().code,
            parsedBlock.Error().offset);
    }
    std::memset(outInfoBlock.data(), 0, infoSize);
    std::memcpy(
        outInfoBlock.data(),
        serializedBlock.data(),
        serializedBlock.size());
    return ProtocolStatus::Success();
}

ProtocolResult<std::span<const std::byte>>
ExtractTransportBlockFromInfoBlock(
    const std::span<const std::byte> infoBlock) noexcept
{
    const auto headerResult = ParseTransportHeader(infoBlock);
    if (!headerResult)
    {
        return ProtocolResult<std::span<const std::byte>>::Failure(
            headerResult.Error().code,
            headerResult.Error().offset);
    }

    // The block length comes from the header, which must validate (including
    // the header CRC) before it is trusted for slicing. The declared length
    // is bounds-checked against the input before any slice is taken, so a
    // corrupt PayloadBytes field cannot drive an out-of-range subspan.
    const std::size_t blockLength =
        GetTransportSerializedSize(headerResult.Value());
    if (blockLength > infoBlock.size())
    {
        return ProtocolResult<std::span<const std::byte>>::Failure(
            ProtocolErrorCode::TruncatedInput,
            infoBlock.size());
    }
    const std::span<const std::byte> blockView = infoBlock.first(blockLength);
    const auto blockResult = ParseTransportBlock(blockView);
    if (!blockResult)
    {
        return ProtocolResult<std::span<const std::byte>>::Failure(
            blockResult.Error().code,
            blockResult.Error().offset);
    }
    const std::size_t blockSize = GetTransportSerializedSize(
        blockResult.Value().header);

    // The unused information tail is canonical zero padding.
    const std::span<const std::byte> padding =
        infoBlock.subspan(blockSize);
    for (std::size_t byteIndex = 0; byteIndex < padding.size(); byteIndex++)
    {
        if (padding[byteIndex] != std::byte{0})
        {
            return ProtocolResult<std::span<const std::byte>>::Failure(
                ProtocolErrorCode::NonCanonicalPadding,
                blockSize + byteIndex);
        }
    }
    return ProtocolResult<std::span<const std::byte>>::Success(
        infoBlock.first(blockSize));
}

} // namespace pbprotocol
