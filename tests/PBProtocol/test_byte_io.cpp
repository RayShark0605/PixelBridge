#include "pbprotocol/byte_io.h"

#include "pbprotocol/checked_integer.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

[[nodiscard]] std::byte Byte(const unsigned int value)
{
    return static_cast<std::byte>(value);
}

using BorrowedByteArray = std::array<std::byte, 2>;
using OwnedByteVector = std::vector<std::byte>;

static_assert(std::is_constructible_v<pbprotocol::ByteReader, BorrowedByteArray&>);
static_assert(std::is_constructible_v<pbprotocol::ByteReader, OwnedByteVector&>);
static_assert(
    std::is_constructible_v<pbprotocol::ByteReader, std::span<const std::byte>>);
static_assert(!std::is_constructible_v<pbprotocol::ByteReader, BorrowedByteArray>);
static_assert(!std::is_constructible_v<pbprotocol::ByteReader, OwnedByteVector>);

} // namespace

TEST_CASE("ByteReader accepts live lvalue contiguous storage",
          "[pbprotocol][lifetime]")
{
    std::vector<std::byte> input{Byte(0x34), Byte(0x12)};
    pbprotocol::ByteReader reader(input);

    const auto valueResult = reader.ReadUint16();
    REQUIRE(valueResult);
    REQUIRE(valueResult.Value() == 0x1234U);
    REQUIRE(reader.RequireFullyConsumed());
}

TEST_CASE("Little-endian integers have exact wire bytes", "[pbprotocol][wire]")
{
    std::array<std::byte, 14> buffer{};
    pbprotocol::ByteWriter writer(buffer);

    REQUIRE(writer.WriteUint16(0x1234U));
    REQUIRE(writer.WriteUint32(0x89ABCDEFU));
    REQUIRE(writer.WriteUint64(0x0123456789ABCDEFULL));
    REQUIRE(writer.Position() == buffer.size());

    const std::array<std::byte, 14> expected{
        Byte(0x34), Byte(0x12),
        Byte(0xEF), Byte(0xCD), Byte(0xAB), Byte(0x89),
        Byte(0xEF), Byte(0xCD), Byte(0xAB), Byte(0x89),
        Byte(0x67), Byte(0x45), Byte(0x23), Byte(0x01)};
    REQUIRE(buffer == expected);

    pbprotocol::ByteReader reader(buffer);
    const auto uint16Result = reader.ReadUint16();
    const auto uint32Result = reader.ReadUint32();
    const auto uint64Result = reader.ReadUint64();
    REQUIRE(uint16Result);
    REQUIRE(uint32Result);
    REQUIRE(uint64Result);
    REQUIRE(uint16Result.Value() == 0x1234U);
    REQUIRE(uint32Result.Value() == 0x89ABCDEFU);
    REQUIRE(uint64Result.Value() == 0x0123456789ABCDEFULL);
    REQUIRE(reader.RequireFullyConsumed());
}

TEST_CASE("Fixed byte arrays include the zero-length case", "[pbprotocol][wire]")
{
    const std::array<std::byte, 0> emptyBytes{};
    const std::array<std::byte, 3> sourceBytes{
        Byte(0x10), Byte(0x20), Byte(0x30)};
    std::array<std::byte, 3> buffer{};
    pbprotocol::ByteWriter writer(buffer);

    REQUIRE(writer.WriteFixedBytes(emptyBytes));
    REQUIRE(writer.Position() == 0);
    REQUIRE(writer.WriteFixedBytes(sourceBytes));

    pbprotocol::ByteReader reader(buffer);
    const auto emptyResult = reader.ReadFixedBytes<0>();
    const auto bytesResult = reader.ReadFixedBytes<3>();
    REQUIRE(emptyResult);
    REQUIRE(bytesResult);
    REQUIRE(bytesResult.Value() == sourceBytes);
    REQUIRE(reader.RequireFullyConsumed());
}

TEST_CASE("All explicit length prefix widths round trip", "[pbprotocol][wire][length]")
{
    const std::array<std::byte, 1> firstPayload{Byte(0xA1)};
    const std::array<std::byte, 2> secondPayload{Byte(0xB1), Byte(0xB2)};
    const std::array<std::byte, 0> emptyPayload{};
    std::array<std::byte, 17> buffer{};
    pbprotocol::ByteWriter writer(buffer);

    REQUIRE(writer.WriteLengthDelimitedBytes(
        pbprotocol::LengthPrefixWidth::Uint16,
        firstPayload,
        firstPayload.size()));
    REQUIRE(writer.WriteLengthDelimitedBytes(
        pbprotocol::LengthPrefixWidth::Uint32,
        secondPayload,
        secondPayload.size()));
    REQUIRE(writer.WriteLengthDelimitedBytes(
        pbprotocol::LengthPrefixWidth::Uint64,
        emptyPayload,
        emptyPayload.size()));

    const std::array<std::byte, 17> expected{
        Byte(0x01), Byte(0x00), Byte(0xA1),
        Byte(0x02), Byte(0x00), Byte(0x00), Byte(0x00), Byte(0xB1), Byte(0xB2),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00)};
    REQUIRE(buffer == expected);

    pbprotocol::ByteReader reader(buffer);
    const auto firstResult = reader.ReadLengthDelimitedBytes(
        pbprotocol::LengthPrefixWidth::Uint16,
        firstPayload.size());
    const auto secondResult = reader.ReadLengthDelimitedBytes(
        pbprotocol::LengthPrefixWidth::Uint32,
        secondPayload.size());
    const auto emptyResult = reader.ReadLengthDelimitedBytes(
        pbprotocol::LengthPrefixWidth::Uint64,
        emptyPayload.size());
    REQUIRE(firstResult);
    REQUIRE(secondResult);
    REQUIRE(emptyResult);
    REQUIRE(std::equal(
        firstResult.Value().begin(), firstResult.Value().end(), firstPayload.begin()));
    REQUIRE(std::equal(
        secondResult.Value().begin(), secondResult.Value().end(), secondPayload.begin()));
    REQUIRE(emptyResult.Value().empty());
    REQUIRE(reader.RequireFullyConsumed());
}

TEST_CASE("UTF-8 length-delimited fields validate and borrow their payload",
          "[pbprotocol][wire][utf8]")
{
    const std::string_view text = "PixelBridge 像素桥";
    std::array<std::byte, 64> buffer{};
    pbprotocol::ByteWriter writer(buffer);

    REQUIRE(writer.WriteLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint16,
        text,
        text.size()));
    REQUIRE(writer.WriteLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint64,
        std::string_view{},
        0));

    pbprotocol::ByteReader reader(writer.WrittenBytes());
    const auto textResult = reader.ReadLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint16,
        text.size());
    REQUIRE(textResult);
    REQUIRE(textResult.Value() == text);
    const auto emptyResult = reader.ReadLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint64,
        0);
    REQUIRE(emptyResult);
    REQUIRE(emptyResult.Value().empty());
    REQUIRE(reader.RequireFullyConsumed());
}

TEST_CASE("UTF-8 length prefixes and limits use encoded byte counts",
          "[pbprotocol][wire][utf8][length]")
{
    const std::string text{
        "\xE5\x83\x8F"
        "\xF0\x9F\x98\x80",
        7};
    REQUIRE(text.size() == 7);

    const std::array<std::byte, 9> expectedWire{
        Byte(0x07), Byte(0x00),
        Byte(0xE5), Byte(0x83), Byte(0x8F),
        Byte(0xF0), Byte(0x9F), Byte(0x98), Byte(0x80)};

    std::array<std::byte, 9> output{};
    output.fill(Byte(0xA5));
    const std::array<std::byte, 9> originalOutput = output;
    pbprotocol::ByteWriter writer(output);

    const auto limitedWriteStatus = writer.WriteLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint16,
        text,
        6);
    REQUIRE_FALSE(limitedWriteStatus);
    REQUIRE(
        limitedWriteStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::LengthLimitExceeded);
    REQUIRE(writer.Position() == 0);
    REQUIRE(output == originalOutput);

    REQUIRE(writer.WriteLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint16,
        text,
        7));
    REQUIRE(output == expectedWire);

    pbprotocol::ByteReader limitedReader(expectedWire);
    const auto limitedReadResult = limitedReader.ReadLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint16,
        6);
    REQUIRE_FALSE(limitedReadResult);
    REQUIRE(
        limitedReadResult.Error().code ==
        pbprotocol::ProtocolErrorCode::LengthLimitExceeded);
    REQUIRE(limitedReader.Position() == 0);

    pbprotocol::ByteReader exactReader(expectedWire);
    const auto exactReadResult = exactReader.ReadLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint16,
        7);
    REQUIRE(exactReadResult);
    REQUIRE(exactReadResult.Value() == text);
    REQUIRE(exactReader.RequireFullyConsumed());
}

TEST_CASE("Truncated primitive and fixed reads leave the cursor unchanged",
          "[pbprotocol][wire][truncation]")
{
    SECTION("integer")
    {
        const std::array<std::byte, 1> input{Byte(0x12)};
        pbprotocol::ByteReader reader(input);
        const auto result = reader.ReadUint16();

        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::TruncatedInput);
        REQUIRE(result.Error().offset == 0);
        REQUIRE(reader.Position() == 0);
    }

    SECTION("fixed array")
    {
        const std::array<std::byte, 2> input{Byte(0x12), Byte(0x34)};
        pbprotocol::ByteReader reader(input);
        const auto result = reader.ReadFixedBytes<3>();

        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::TruncatedInput);
        REQUIRE(reader.Position() == 0);
    }
}

TEST_CASE("Length-delimited truncation is atomic", "[pbprotocol][wire][truncation]")
{
    SECTION("truncated prefix")
    {
        const std::array<std::byte, 1> input{Byte(0x03)};
        pbprotocol::ByteReader reader(input);
        const auto result = reader.ReadLengthDelimitedBytes(
            pbprotocol::LengthPrefixWidth::Uint16,
            16);

        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::TruncatedInput);
        REQUIRE(result.Error().offset == 0);
        REQUIRE(reader.Position() == 0);
    }

    SECTION("truncated payload")
    {
        const std::array<std::byte, 4> input{
            Byte(0x03), Byte(0x00), Byte('a'), Byte('b')};
        pbprotocol::ByteReader reader(input);
        const auto result = reader.ReadLengthDelimitedBytes(
            pbprotocol::LengthPrefixWidth::Uint16,
            16);

        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::TruncatedInput);
        REQUIRE(result.Error().offset == 2);
        REQUIRE(reader.Position() == 0);
    }
}

TEST_CASE("Length limits fail before any read or write mutation",
          "[pbprotocol][wire][length]")
{
    const std::array<std::byte, 6> input{
        Byte(0x04), Byte(0x00), Byte('a'), Byte('b'), Byte('c'), Byte('d')};
    pbprotocol::ByteReader reader(input);
    const auto readResult = reader.ReadLengthDelimitedBytes(
        pbprotocol::LengthPrefixWidth::Uint16,
        3);
    REQUIRE_FALSE(readResult);
    REQUIRE(readResult.Error().code == pbprotocol::ProtocolErrorCode::LengthLimitExceeded);
    REQUIRE(reader.Position() == 0);

    std::array<std::byte, 6> output{};
    output.fill(Byte(0xA5));
    const std::array<std::byte, 6> originalOutput = output;
    pbprotocol::ByteWriter writer(output);
    const auto writeStatus = writer.WriteLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint16,
        "abcd",
        3);
    REQUIRE_FALSE(writeStatus);
    REQUIRE(writeStatus.Error().code == pbprotocol::ProtocolErrorCode::LengthLimitExceeded);
    REQUIRE(writer.Position() == 0);
    REQUIRE(output == originalOutput);
}

TEST_CASE("Payload length must fit the selected prefix width",
          "[pbprotocol][wire][length]")
{
    const std::array<std::byte, 65536> overlongPayload{};
    std::array<std::byte, 2> output{Byte(0xA5), Byte(0xA5)};
    const std::array<std::byte, 2> originalOutput = output;
    pbprotocol::ByteWriter writer(output);

    const auto status = writer.WriteLengthDelimitedBytes(
        pbprotocol::LengthPrefixWidth::Uint16,
        overlongPayload,
        overlongPayload.size());
    REQUIRE_FALSE(status);
    REQUIRE(status.Error().code == pbprotocol::ProtocolErrorCode::LengthNarrowing);
    REQUIRE(status.Error().offset == 0);
    REQUIRE(writer.Position() == 0);
    REQUIRE(output == originalOutput);
}

TEST_CASE("Length-delimited writes preserve aliased payload bytes",
          "[pbprotocol][wire][length]")
{
    std::array<std::byte, 6> buffer{
        Byte(0x11), Byte(0x22), Byte(0x33), Byte(0x44), Byte(0x55), Byte(0x66)};
    const std::span<const std::byte> aliasedPayload(buffer.data(), 3);
    pbprotocol::ByteWriter writer(buffer);

    REQUIRE(writer.WriteLengthDelimitedBytes(
        pbprotocol::LengthPrefixWidth::Uint16,
        aliasedPayload,
        aliasedPayload.size()));

    const std::array<std::byte, 6> expected{
        Byte(0x03), Byte(0x00), Byte(0x11), Byte(0x22), Byte(0x33), Byte(0x66)};
    REQUIRE(buffer == expected);
}

TEST_CASE("Invalid length prefix selections fail without mutation",
          "[pbprotocol][wire][length]")
{
    const auto invalidWidth = static_cast<pbprotocol::LengthPrefixWidth>(0xFFU);
    const std::array<std::byte, 2> input{Byte(0x00), Byte(0x00)};
    pbprotocol::ByteReader reader(input);
    const auto readResult = reader.ReadLengthDelimitedBytes(invalidWidth, 0);
    REQUIRE_FALSE(readResult);
    REQUIRE(
        readResult.Error().code ==
        pbprotocol::ProtocolErrorCode::InvalidLengthPrefixWidth);
    REQUIRE(reader.Position() == 0);

    std::array<std::byte, 2> output{Byte(0xA5), Byte(0xA5)};
    const std::array<std::byte, 2> originalOutput = output;
    pbprotocol::ByteWriter writer(output);
    const auto writeStatus = writer.WriteLengthDelimitedBytes(
        invalidWidth,
        std::span<const std::byte>{},
        0);
    REQUIRE_FALSE(writeStatus);
    REQUIRE(
        writeStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::InvalidLengthPrefixWidth);
    REQUIRE(writer.Position() == 0);
    REQUIRE(output == originalOutput);
}

TEST_CASE("Strict UTF-8 rejects malformed and overlong encodings",
          "[pbprotocol][wire][utf8]")
{
    SECTION("maximum valid code point")
    {
        const std::string maximumCodePoint{
            static_cast<char>(0xF4),
            static_cast<char>(0x8F),
            static_cast<char>(0xBF),
            static_cast<char>(0xBF)};
        REQUIRE(pbprotocol::ValidateUtf8(maximumCodePoint, 3));
    }

    SECTION("overlong NUL")
    {
        const std::string overlong{
            static_cast<char>(0xC0),
            static_cast<char>(0x80)};
        const auto status = pbprotocol::ValidateUtf8(overlong, 40);
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().code == pbprotocol::ProtocolErrorCode::InvalidUtf8);
        REQUIRE(status.Error().offset == 40);
    }

    SECTION("truncated sequence")
    {
        const std::string truncated{
            static_cast<char>(0xE2),
            static_cast<char>(0x82)};
        const auto status = pbprotocol::ValidateUtf8(truncated, 5);
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().offset == 5);
    }

    SECTION("surrogate")
    {
        const std::string surrogate{
            static_cast<char>(0xED),
            static_cast<char>(0xA0),
            static_cast<char>(0x80)};
        const auto status = pbprotocol::ValidateUtf8(surrogate, 9);
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().offset == 10);
    }

    SECTION("above Unicode range")
    {
        const std::string aboveRange{
            static_cast<char>(0xF4),
            static_cast<char>(0x90),
            static_cast<char>(0x80),
            static_cast<char>(0x80)};
        const auto status = pbprotocol::ValidateUtf8(aboveRange, 12);
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().offset == 13);
    }

    SECTION("invalid continuation")
    {
        const std::string invalidContinuation{
            static_cast<char>(0xE2),
            static_cast<char>(0x28),
            static_cast<char>(0xA1)};
        const auto status = pbprotocol::ValidateUtf8(invalidContinuation, 20);
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().offset == 21);
    }
}

TEST_CASE("Invalid UTF-8 length-delimited operations are atomic",
          "[pbprotocol][wire][utf8]")
{
    const std::string invalidText{
        static_cast<char>(0xC0),
        static_cast<char>(0x80)};

    std::array<std::byte, 4> output{};
    output.fill(Byte(0xA5));
    const std::array<std::byte, 4> originalOutput = output;
    pbprotocol::ByteWriter writer(output);
    const auto writeStatus = writer.WriteLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint16,
        invalidText,
        invalidText.size());
    REQUIRE_FALSE(writeStatus);
    REQUIRE(writeStatus.Error().code == pbprotocol::ProtocolErrorCode::InvalidUtf8);
    REQUIRE(writeStatus.Error().offset == 2);
    REQUIRE(writer.Position() == 0);
    REQUIRE(output == originalOutput);

    const std::array<std::byte, 4> input{
        Byte(0x02), Byte(0x00), Byte(0xC0), Byte(0x80)};
    pbprotocol::ByteReader reader(input);
    const auto readResult = reader.ReadLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint16,
        invalidText.size());
    REQUIRE_FALSE(readResult);
    REQUIRE(readResult.Error().code == pbprotocol::ProtocolErrorCode::InvalidUtf8);
    REQUIRE(readResult.Error().offset == 2);
    REQUIRE(reader.Position() == 0);
}

TEST_CASE("Reserved and canonical padding validation report absolute offsets",
          "[pbprotocol][wire][padding]")
{
    const std::array<std::byte, 6> input{
        Byte(0x04), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x05), Byte(0x00)};
    pbprotocol::ByteReader parentReader(input);
    const auto nestedResult = parentReader.ReadLengthDelimitedReader(
        pbprotocol::LengthPrefixWidth::Uint16,
        4);
    REQUIRE(nestedResult);
    REQUIRE(parentReader.Position() == input.size());

    pbprotocol::ByteReader nestedReader = nestedResult.Value();
    const auto reservedStatus = nestedReader.ReadReservedZeroBytes(4);
    REQUIRE_FALSE(reservedStatus);
    REQUIRE(reservedStatus.Error().code == pbprotocol::ProtocolErrorCode::NonZeroReservedByte);
    REQUIRE(reservedStatus.Error().offset == 4);
    REQUIRE(nestedReader.Position() == 0);

    const std::array<std::byte, 3> badPadding{
        Byte(0x00), Byte(0x01), Byte(0x00)};
    pbprotocol::ByteReader paddingReader(badPadding);
    const auto paddingStatus = paddingReader.ReadCanonicalZeroPadding(3);
    REQUIRE_FALSE(paddingStatus);
    REQUIRE(paddingStatus.Error().code == pbprotocol::ProtocolErrorCode::NonCanonicalPadding);
    REQUIRE(paddingStatus.Error().offset == 1);
    REQUIRE(paddingReader.Position() == 0);
}

TEST_CASE("Canonical padding rejects every nonzero position atomically",
          "[pbprotocol][wire][padding]")
{
    const std::array<std::size_t, 3> invalidIndices{0, 1, 2};
    for (const std::size_t invalidIndex : invalidIndices)
    {
        CAPTURE(invalidIndex);
        std::array<std::byte, 3> padding{};
        padding[invalidIndex] = Byte(0x01);
        pbprotocol::ByteReader reader(padding);

        const auto status = reader.ReadCanonicalZeroPadding(padding.size());
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().code == pbprotocol::ProtocolErrorCode::NonCanonicalPadding);
        REQUIRE(status.Error().offset == invalidIndex);
        REQUIRE(reader.Position() == 0);
    }

    const std::array<std::byte, 3> canonicalPadding{};
    pbprotocol::ByteReader reader(canonicalPadding);
    REQUIRE(reader.ReadCanonicalZeroPadding(0));
    REQUIRE(reader.Position() == 0);
    REQUIRE(reader.ReadCanonicalZeroPadding(canonicalPadding.size()));
    REQUIRE(reader.RequireFullyConsumed());
}

TEST_CASE("Canonical zero padding writer is bounded and deterministic",
          "[pbprotocol][wire][padding]")
{
    std::array<std::byte, 5> buffer{};
    buffer.fill(Byte(0xA5));
    pbprotocol::ByteWriter writer(buffer);

    REQUIRE(writer.WriteUint16(0x1234U));
    const std::array<std::byte, 5> beforeZeroLengthWrite = buffer;
    REQUIRE(writer.WriteCanonicalZeroPadding(0));
    REQUIRE(writer.Position() == sizeof(std::uint16_t));
    REQUIRE(buffer == beforeZeroLengthWrite);
    REQUIRE(writer.WriteCanonicalZeroPadding(3));
    const std::array<std::byte, 5> expected{
        Byte(0x34), Byte(0x12), Byte(0x00), Byte(0x00), Byte(0x00)};
    REQUIRE(buffer == expected);

    const std::array<std::byte, 5> beforeFailure = buffer;
    const auto failureStatus = writer.WriteCanonicalZeroPadding(1);
    REQUIRE_FALSE(failureStatus);
    REQUIRE(failureStatus.Error().code == pbprotocol::ProtocolErrorCode::OutputBufferTooSmall);
    REQUIRE(writer.Position() == buffer.size());
    REQUIRE(buffer == beforeFailure);
}

TEST_CASE("Nested uint64 length arithmetic cannot wrap",
          "[pbprotocol][wire][length][overflow]")
{
    const std::array<std::byte, 10> input{
        Byte(0x08), Byte(0x00),
        Byte(0xFF), Byte(0xFF), Byte(0xFF), Byte(0xFF),
        Byte(0xFF), Byte(0xFF), Byte(0xFF), Byte(0xFF)};
    pbprotocol::ByteReader parentReader(input);
    const auto nestedResult = parentReader.ReadLengthDelimitedReader(
        pbprotocol::LengthPrefixWidth::Uint16,
        8);
    REQUIRE(nestedResult);

    pbprotocol::ByteReader nestedReader = nestedResult.Value();
    const auto maliciousResult = nestedReader.ReadLengthDelimitedBytes(
        pbprotocol::LengthPrefixWidth::Uint64,
        std::numeric_limits<std::size_t>::max());
    REQUIRE_FALSE(maliciousResult);
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t))
    {
        REQUIRE(maliciousResult.Error().code == pbprotocol::ProtocolErrorCode::LengthNarrowing);
    }
    else
    {
        REQUIRE(maliciousResult.Error().code == pbprotocol::ProtocolErrorCode::LengthOverflow);
    }
    REQUIRE(maliciousResult.Error().offset == 2);
    REQUIRE(nestedReader.Position() == 0);
}

TEST_CASE("Checked uint64 to size conversion covers native and narrower bounds",
          "[pbprotocol][wire][length][overflow]")
{
    const std::uint64_t nativeMaximum = static_cast<std::uint64_t>(
        std::numeric_limits<std::size_t>::max());
    const auto nativeResult = pbprotocol::CheckedUint64ToSize(nativeMaximum, 3);
    REQUIRE(nativeResult);
    REQUIRE(nativeResult.Value() == std::numeric_limits<std::size_t>::max());

    const auto narrowerResult = pbprotocol::CheckedNarrowUnsigned<std::uint32_t>(
        std::numeric_limits<std::uint64_t>::max(),
        7);
    REQUIRE_FALSE(narrowerResult);
    REQUIRE(narrowerResult.Error().code == pbprotocol::ProtocolErrorCode::LengthNarrowing);
    REQUIRE(narrowerResult.Error().offset == 7);

    if constexpr (sizeof(std::size_t) == sizeof(std::uint64_t))
    {
        const auto fullWidthResult = pbprotocol::CheckedUint64ToSize(
            std::numeric_limits<std::uint64_t>::max(),
            11);
        REQUIRE(fullWidthResult);
    }
}

TEST_CASE("Checked unsigned multiplication rejects overflow without wrapping",
          "[pbprotocol][integer][overflow]")
{
    const auto exactResult = pbprotocol::CheckedMultiplyUint64(
        std::numeric_limits<std::uint64_t>::max(),
        1,
        29);
    REQUIRE(exactResult);
    REQUIRE(
        exactResult.Value() ==
        std::numeric_limits<std::uint64_t>::max());

    const auto overflowResult = pbprotocol::CheckedMultiplyUint64(
        std::numeric_limits<std::uint64_t>::max(),
        2,
        31);
    REQUIRE_FALSE(overflowResult);
    REQUIRE(
        overflowResult.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::LengthOverflow,
            31});
}

TEST_CASE("Partial parses and partial writes remain observable and atomic",
          "[pbprotocol][wire][partial]")
{
    const std::array<std::byte, 3> input{
        Byte(0x34), Byte(0x12), Byte(0xAA)};
    pbprotocol::ByteReader reader(input);
    const auto firstResult = reader.ReadUint16();
    REQUIRE(firstResult);
    REQUIRE(firstResult.Value() == 0x1234U);
    REQUIRE(reader.Position() == 2);

    const auto secondResult = reader.ReadUint16();
    REQUIRE_FALSE(secondResult);
    REQUIRE(reader.Position() == 2);
    const auto completeStatus = reader.RequireFullyConsumed();
    REQUIRE_FALSE(completeStatus);
    REQUIRE(completeStatus.Error().code == pbprotocol::ProtocolErrorCode::TrailingBytes);
    REQUIRE(completeStatus.Error().offset == 2);

    std::array<std::byte, 3> output{};
    output.fill(Byte(0xA5));
    pbprotocol::ByteWriter writer(output);
    REQUIRE(writer.WriteUint16(0x1234U));
    const std::array<std::byte, 3> beforeFailure = output;
    const auto writeStatus = writer.WriteUint32(0x89ABCDEFU);
    REQUIRE_FALSE(writeStatus);
    REQUIRE(writer.Position() == 2);
    REQUIRE(output == beforeFailure);
}

TEST_CASE("Synthetic primitive composition is byte exact without defining a record",
          "[pbprotocol][wire][integration]")
{
    // This fixture composes primitives only. It is not a Session, Control, or
    // Transport record and deliberately does not assign protocol field meaning.
    const std::array<std::byte, 4> fixedBytes{
        Byte(0xDE), Byte(0xAD), Byte(0xBE), Byte(0xEF)};
    const std::array<std::byte, 2> reservedBytes{Byte(0x00), Byte(0x00)};
    std::array<std::byte, 27> buffer{};
    buffer.fill(Byte(0xA5));
    pbprotocol::ByteWriter writer(buffer);

    REQUIRE(writer.WriteUint16(0x1234U));
    REQUIRE(writer.WriteUint32(0x89ABCDEFU));
    REQUIRE(writer.WriteUint64(0x0123456789ABCDEFULL));
    REQUIRE(writer.WriteFixedBytes(fixedBytes));
    REQUIRE(writer.WriteLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint16,
        "PB",
        2));
    REQUIRE(writer.WriteFixedBytes(reservedBytes));
    REQUIRE(writer.WriteCanonicalZeroPadding(3));
    REQUIRE(writer.Position() == buffer.size());

    const std::array<std::byte, 27> expected{
        Byte(0x34), Byte(0x12),
        Byte(0xEF), Byte(0xCD), Byte(0xAB), Byte(0x89),
        Byte(0xEF), Byte(0xCD), Byte(0xAB), Byte(0x89),
        Byte(0x67), Byte(0x45), Byte(0x23), Byte(0x01),
        Byte(0xDE), Byte(0xAD), Byte(0xBE), Byte(0xEF),
        Byte(0x02), Byte(0x00), Byte('P'), Byte('B'),
        Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00)};
    REQUIRE(buffer == expected);

    pbprotocol::ByteReader reader(buffer);
    REQUIRE(reader.ReadUint16().Value() == 0x1234U);
    REQUIRE(reader.ReadUint32().Value() == 0x89ABCDEFU);
    REQUIRE(reader.ReadUint64().Value() == 0x0123456789ABCDEFULL);
    REQUIRE(reader.ReadFixedBytes<4>().Value() == fixedBytes);
    REQUIRE(reader.ReadLengthDelimitedUtf8(
        pbprotocol::LengthPrefixWidth::Uint16,
        2).Value() == "PB");
    REQUIRE(reader.ReadReservedZeroBytes(2));
    REQUIRE(reader.ReadCanonicalZeroPadding(3));
    REQUIRE(reader.RequireFullyConsumed());
}
