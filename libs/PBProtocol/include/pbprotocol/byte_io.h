#pragma once

#include "pbprotocol/protocol_result.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace pbprotocol {

enum class LengthPrefixWidth : std::uint8_t
{
    Uint16,
    Uint32,
    Uint64
};

// Validates UTF-8 well-formedness only. Higher-level field rules such as file
// name sanitization and Unicode normalization remain the caller's concern.
[[nodiscard]] ProtocolStatus ValidateUtf8(
    std::string_view text,
    std::size_t baseOffset = 0) noexcept;

class ByteReader
{
public:
    explicit ByteReader(std::span<const std::byte> input) noexcept;

    [[nodiscard]] std::size_t Position() const noexcept;
    [[nodiscard]] std::size_t AbsolutePosition() const noexcept;
    [[nodiscard]] std::size_t Remaining() const noexcept;

    [[nodiscard]] ProtocolResult<std::uint8_t> ReadUint8() noexcept;
    [[nodiscard]] ProtocolResult<std::uint16_t> ReadUint16() noexcept;
    [[nodiscard]] ProtocolResult<std::uint32_t> ReadUint32() noexcept;
    [[nodiscard]] ProtocolResult<std::uint64_t> ReadUint64() noexcept;
    [[nodiscard]] ProtocolResult<std::span<const std::byte>> ReadBytes(
        std::size_t byteCount) noexcept;

    template <std::size_t ByteCount>
    [[nodiscard]] ProtocolResult<std::array<std::byte, ByteCount>> ReadFixedBytes() noexcept
    {
        const auto bytesResult = ReadBytes(ByteCount);
        if (!bytesResult)
        {
            return ProtocolResult<std::array<std::byte, ByteCount>>::Failure(
                bytesResult.Error().code,
                bytesResult.Error().offset);
        }

        std::array<std::byte, ByteCount> bytes{};
        std::copy(bytesResult.Value().begin(), bytesResult.Value().end(), bytes.begin());
        return ProtocolResult<std::array<std::byte, ByteCount>>::Success(bytes);
    }

    // Returned spans/string views and nested readers borrow input_. The caller
    // must keep the original input storage alive for their entire use.
    [[nodiscard]] ProtocolResult<std::span<const std::byte>> ReadLengthDelimitedBytes(
        LengthPrefixWidth prefixWidth,
        std::size_t maximumByteLength) noexcept;
    [[nodiscard]] ProtocolResult<std::string_view> ReadLengthDelimitedUtf8(
        LengthPrefixWidth prefixWidth,
        std::size_t maximumByteLength) noexcept;
    [[nodiscard]] ProtocolResult<ByteReader> ReadLengthDelimitedReader(
        LengthPrefixWidth prefixWidth,
        std::size_t maximumByteLength) noexcept;

    [[nodiscard]] ProtocolStatus ReadReservedZeroBytes(std::size_t byteCount) noexcept;
    [[nodiscard]] ProtocolStatus ReadCanonicalZeroPadding(std::size_t byteCount) noexcept;
    [[nodiscard]] ProtocolStatus RequireFullyConsumed() const noexcept;

private:
    ByteReader(
        std::span<const std::byte> input,
        std::size_t baseOffset) noexcept;

    [[nodiscard]] ProtocolResult<std::uint64_t> ReadLengthPrefix(
        LengthPrefixWidth prefixWidth) noexcept;

    std::span<const std::byte> input_;
    std::size_t position_ = 0;
    std::size_t baseOffset_ = 0;
};

class ByteWriter
{
public:
    explicit ByteWriter(std::span<std::byte> output) noexcept;

    [[nodiscard]] std::size_t Position() const noexcept;
    [[nodiscard]] std::size_t Remaining() const noexcept;
    [[nodiscard]] std::span<const std::byte> WrittenBytes() const noexcept;

    [[nodiscard]] ProtocolStatus WriteUint8(std::uint8_t value) noexcept;
    [[nodiscard]] ProtocolStatus WriteUint16(std::uint16_t value) noexcept;
    [[nodiscard]] ProtocolStatus WriteUint32(std::uint32_t value) noexcept;
    [[nodiscard]] ProtocolStatus WriteUint64(std::uint64_t value) noexcept;
    [[nodiscard]] ProtocolStatus WriteBytes(std::span<const std::byte> bytes) noexcept;

    template <std::size_t ByteCount>
    [[nodiscard]] ProtocolStatus WriteFixedBytes(
        const std::array<std::byte, ByteCount>& bytes) noexcept
    {
        return WriteBytes(std::span<const std::byte, ByteCount>(bytes));
    }

    template <std::size_t ByteCount>
    [[nodiscard]] ProtocolStatus WriteFixedBytes(
        const std::span<const std::byte, ByteCount> bytes) noexcept
    {
        return WriteBytes(bytes);
    }

    [[nodiscard]] ProtocolStatus WriteLengthDelimitedBytes(
        LengthPrefixWidth prefixWidth,
        std::span<const std::byte> bytes,
        std::size_t maximumByteLength) noexcept;
    [[nodiscard]] ProtocolStatus WriteLengthDelimitedUtf8(
        LengthPrefixWidth prefixWidth,
        std::string_view text,
        std::size_t maximumByteLength) noexcept;
    [[nodiscard]] ProtocolStatus WriteCanonicalZeroPadding(std::size_t byteCount) noexcept;

private:
    struct LengthWritePlan
    {
        std::size_t prefixByteCount = 0;
        std::size_t totalByteCount = 0;
        std::uint64_t encodedLength = 0;
    };

    [[nodiscard]] ProtocolResult<LengthWritePlan> PrepareLengthDelimitedWrite(
        LengthPrefixWidth prefixWidth,
        std::size_t payloadByteCount,
        std::size_t maximumByteLength) const noexcept;
    void WriteLengthPrefixUnchecked(
        LengthPrefixWidth prefixWidth,
        std::uint64_t value) noexcept;

    std::span<std::byte> output_;
    std::size_t position_ = 0;
};

} // namespace pbprotocol
