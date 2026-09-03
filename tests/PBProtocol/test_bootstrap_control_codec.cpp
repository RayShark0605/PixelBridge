#include "descriptor_test_helpers.h"

#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace {

using pbprotocol::test::Byte;

constexpr std::size_t kBootstrapCrcOffset = 40;
constexpr std::size_t kControlRecordBytesOffset = 22;

constexpr std::array<std::byte, pbprotocol::kBootstrapRecordBytes>
    kBootstrapGolden{
        Byte(0x50), Byte(0x42), Byte(0x52), Byte(0x47),
        Byte(0x01), Byte(0x01), Byte(0x00), Byte(0x01),
        Byte(0x08), Byte(0x07), Byte(0x06), Byte(0x05),
        Byte(0x04), Byte(0x03), Byte(0x02), Byte(0x01),
        Byte(0xD0), Byte(0xBA), Byte(0x97), Byte(0xD9),
        Byte(0x4B), Byte(0x20), Byte(0xDF), Byte(0x81),
        Byte(0x18), Byte(0x17), Byte(0x16), Byte(0x15),
        Byte(0x14), Byte(0x13), Byte(0x12), Byte(0x11),
        Byte(0x24), Byte(0x23), Byte(0x22), Byte(0x21),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0xEA), Byte(0xE1), Byte(0x88), Byte(0xD4)};

constexpr std::array<std::byte, 37> kSessionDescriptorGolden{
    Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x01), Byte(0x02), Byte(0x03),
    Byte(0x04), Byte(0x05), Byte(0x06), Byte(0x07),
    Byte(0x08), Byte(0x09), Byte(0x0A), Byte(0x0B),
    Byte(0x0C), Byte(0x0D), Byte(0x0E), Byte(0x0F),
    Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x01)};

constexpr std::array<std::byte, 67> kControlGolden{
    Byte(0x50), Byte(0x42), Byte(0x43), Byte(0x52),
    Byte(0x01), Byte(0x01),
    Byte(0x08), Byte(0x07), Byte(0x06), Byte(0x05),
    Byte(0x04), Byte(0x03), Byte(0x02), Byte(0x01),
    Byte(0xD0), Byte(0xBA), Byte(0x97), Byte(0xD9),
    Byte(0x4B), Byte(0x20), Byte(0xDF), Byte(0x81),
    Byte(0x43), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x01), Byte(0x02), Byte(0x03),
    Byte(0x04), Byte(0x05), Byte(0x06), Byte(0x07),
    Byte(0x08), Byte(0x09), Byte(0x0A), Byte(0x0B),
    Byte(0x0C), Byte(0x0D), Byte(0x0E), Byte(0x0F),
    Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x01),
    Byte(0xC8), Byte(0x83), Byte(0x38), Byte(0xA1)};

template <typename Range>
concept CanParseControlRange = requires(Range&& input)
{
    pbprotocol::ParseControlRecord(std::forward<Range>(input));
};

static_assert(!CanParseControlRange<std::vector<std::byte>>);
static_assert(CanParseControlRange<std::span<const std::byte>>);

[[nodiscard]] pbprotocol::BootstrapRecord MakeBootstrapRecord() noexcept
{
    return pbprotocol::BootstrapRecord{
        pbprotocol::kBootstrapVersion,
        pbprotocol::GetProtocolVersion(),
        1,
        0x0102030405060708ULL,
        pbprotocol::SessionTag{0x81DF204BD997BAD0ULL},
        0x1112131415161718ULL,
        0x21222324U,
        0};
}

[[nodiscard]] pbprotocol::ControlRecordView MakeControlRecord(
    const std::span<const std::byte> payload = kSessionDescriptorGolden) noexcept
{
    return pbprotocol::ControlRecordView{
        pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor,
        0x0102030405060708ULL,
        pbprotocol::SessionTag{0x81DF204BD997BAD0ULL},
        payload};
}

void StoreUint32(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint32_t value)
{
    REQUIRE(offset <= bytes.size());
    REQUIRE(sizeof(value) <= bytes.size() - offset);
    for (std::size_t byteIndex = 0; byteIndex < sizeof(value); byteIndex++)
    {
        bytes[offset + byteIndex] = Byte(static_cast<std::uint8_t>(
            (value >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU));
    }
}

void RefreshBootstrapCrc(const std::span<std::byte> bytes)
{
    REQUIRE(bytes.size() == pbprotocol::kBootstrapRecordBytes);
    const std::uint32_t crc32c = pbprotocol::ComputeCrc32c(
        std::span<const std::byte>(bytes).first(kBootstrapCrcOffset));
    StoreUint32(bytes, kBootstrapCrcOffset, crc32c);
}

void RefreshControlCrc(const std::span<std::byte> bytes)
{
    REQUIRE(bytes.size() >= pbprotocol::kMinimumControlRecordBytes);
    const std::size_t crcOffset =
        bytes.size() - pbprotocol::kControlRecordCrcBytes;
    const std::uint32_t crc32c = pbprotocol::ComputeCrc32c(
        std::span<const std::byte>(bytes).first(crcOffset));
    StoreUint32(bytes, crcOffset, crc32c);
}

void RefreshDescriptorCrc(const std::span<std::byte> bytes)
{
    REQUIRE(bytes.size() >= pbprotocol::kDescriptorCrcBytes);
    const std::size_t crcOffset = bytes.size() - pbprotocol::kDescriptorCrcBytes;
    const std::uint32_t crc32c = pbprotocol::ComputeCrc32c(
        std::span<const std::byte>(bytes).first(crcOffset));
    StoreUint32(bytes, crcOffset, crc32c);
}

[[nodiscard]] std::vector<std::byte> WrapControlPayload(
    const pbprotocol::ControlRecordType recordType,
    const std::uint64_t controlSequence,
    const pbprotocol::SessionTag sessionTag,
    const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView record{
        pbprotocol::kControlVersion,
        recordType,
        controlSequence,
        sessionTag,
        payload};
    const auto serializedSizeResult = pbprotocol::GetSerializedSize(record);
    REQUIRE(serializedSizeResult);

    std::vector<std::byte> bytes(serializedSizeResult.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    return bytes;
}

} // namespace

TEST_CASE("PB-Bootstrap-1 matches the independent 44-byte Golden Vector",
          "[pbprotocol][bootstrap][wire][golden]")
{
    STATIC_REQUIRE(pbprotocol::kBootstrapRecordBytes == 44);
    const pbprotocol::BootstrapRecord record = MakeBootstrapRecord();

    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bytes{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
    REQUIRE(bytes == kBootstrapGolden);
    REQUIRE(
        pbprotocol::ComputeCrc32c(
            std::span<const std::byte>(kBootstrapGolden).first(
                kBootstrapCrcOffset)) ==
        0xD488E1EAU);

    const auto parsedResult = pbprotocol::ParseBootstrapRecord(kBootstrapGolden);
    REQUIRE(parsedResult);
    REQUIRE(parsedResult.Value() == record);

    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> reserialized{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(
        parsedResult.Value(),
        reserialized));
    REQUIRE(reserialized == kBootstrapGolden);
}

TEST_CASE("PB-Bootstrap-1 parser rejects every truncated prefix and trailing data",
          "[pbprotocol][bootstrap][wire][length]")
{
    for (std::size_t byteCount = 0;
         byteCount < kBootstrapGolden.size();
         byteCount++)
    {
        CAPTURE(byteCount);
        const auto result = pbprotocol::ParseBootstrapRecord(
            std::span<const std::byte>(kBootstrapGolden).first(byteCount));
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::TruncatedInput,
                byteCount});
    }

    std::array<std::byte, pbprotocol::kBootstrapRecordBytes + 1> overlong{};
    std::copy(kBootstrapGolden.begin(), kBootstrapGolden.end(), overlong.begin());
    const auto overlongResult = pbprotocol::ParseBootstrapRecord(overlong);
    REQUIRE_FALSE(overlongResult);
    REQUIRE(
        overlongResult.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::TrailingBytes,
            pbprotocol::kBootstrapRecordBytes});
}

TEST_CASE("PB-Bootstrap-1 detects Magic CRC version and reserved flag failures",
          "[pbprotocol][bootstrap][wire][validation]")
{
    SECTION("Magic is checked before CRC")
    {
        auto bytes = kBootstrapGolden;
        bytes[0] ^= Byte(0x01);
        const auto result = pbprotocol::ParseBootstrapRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::InvalidMagic,
                0});
    }

    SECTION("BootstrapVersion is exact after a valid CRC")
    {
        auto bytes = kBootstrapGolden;
        bytes[4] = Byte(0x02);
        RefreshBootstrapCrc(bytes);
        const auto result = pbprotocol::ParseBootstrapRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::UnsupportedBootstrapVersion,
                4});
    }

    SECTION("Protocol Major is exact after a valid CRC")
    {
        auto bytes = kBootstrapGolden;
        bytes[5] = Byte(0x02);
        RefreshBootstrapCrc(bytes);
        const auto result = pbprotocol::ParseBootstrapRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::UnsupportedProtocolMajor,
                5});
    }

    SECTION("Protocol Minor cannot introduce unframed extensions")
    {
        auto bytes = kBootstrapGolden;
        bytes[6] = Byte(0x01);
        RefreshBootstrapCrc(bytes);
        const auto result = pbprotocol::ParseBootstrapRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::UnsupportedProtocolMinor,
                6});
    }

    SECTION("Flags are reserved zero in v1")
    {
        auto bytes = kBootstrapGolden;
        bytes[36] = Byte(0x01);
        RefreshBootstrapCrc(bytes);
        const auto result = pbprotocol::ParseBootstrapRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::NonZeroReservedBits,
                36});
    }

    SECTION("A protected field mutation fails CRC before field semantics")
    {
        auto bytes = kBootstrapGolden;
        bytes[8] ^= Byte(0x80);
        const auto result = pbprotocol::ParseBootstrapRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::CrcMismatch,
                kBootstrapCrcOffset});
    }

    SECTION("A CRC trailer mutation is rejected")
    {
        auto bytes = kBootstrapGolden;
        bytes.back() ^= Byte(0x01);
        const auto result = pbprotocol::ParseBootstrapRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::CrcMismatch,
                kBootstrapCrcOffset});
    }
}

TEST_CASE("PB-Bootstrap-1 serializer is atomic and supports field maxima",
          "[pbprotocol][bootstrap][wire][atomic]")
{
    const pbprotocol::BootstrapRecord validRecord = MakeBootstrapRecord();

    SECTION("Wrong output sizes do not modify output")
    {
        std::array<std::byte, pbprotocol::kBootstrapRecordBytes - 1> shortOutput{};
        shortOutput.fill(Byte(0xA5));
        const auto originalShortOutput = shortOutput;
        const auto shortStatus = pbprotocol::SerializeBootstrapRecord(
            validRecord,
            shortOutput);
        REQUIRE_FALSE(shortStatus);
        REQUIRE(
            shortStatus.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidRecordSize);
        REQUIRE(shortOutput == originalShortOutput);

        std::array<std::byte, pbprotocol::kBootstrapRecordBytes + 1> longOutput{};
        longOutput.fill(Byte(0x5A));
        const auto originalLongOutput = longOutput;
        const auto longStatus = pbprotocol::SerializeBootstrapRecord(
            validRecord,
            longOutput);
        REQUIRE_FALSE(longStatus);
        REQUIRE(
            longStatus.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidRecordSize);
        REQUIRE(longOutput == originalLongOutput);
    }

    SECTION("Invalid logical records do not modify output")
    {
        std::array<std::byte, pbprotocol::kBootstrapRecordBytes> output{};
        output.fill(Byte(0xA5));
        const auto originalOutput = output;

        auto invalidVersion = validRecord;
        invalidVersion.bootstrapVersion = 2;
        REQUIRE_FALSE(pbprotocol::SerializeBootstrapRecord(
            invalidVersion,
            output));
        REQUIRE(output == originalOutput);

        auto invalidProtocol = validRecord;
        invalidProtocol.protocolVersion.minor = 1;
        REQUIRE_FALSE(pbprotocol::SerializeBootstrapRecord(
            invalidProtocol,
            output));
        REQUIRE(output == originalOutput);

        auto invalidFlags = validRecord;
        invalidFlags.flags = 1;
        REQUIRE_FALSE(pbprotocol::SerializeBootstrapRecord(
            invalidFlags,
            output));
        REQUIRE(output == originalOutput);
    }

    SECTION("Representable field maxima round-trip")
    {
        const pbprotocol::BootstrapRecord maximumRecord{
            pbprotocol::kBootstrapVersion,
            pbprotocol::GetProtocolVersion(),
            std::numeric_limits<std::uint8_t>::max(),
            std::numeric_limits<std::uint64_t>::max(),
            pbprotocol::SessionTag{std::numeric_limits<std::uint64_t>::max()},
            std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint32_t>::max(),
            0};
        std::array<std::byte, pbprotocol::kBootstrapRecordBytes> output{};
        REQUIRE(pbprotocol::SerializeBootstrapRecord(maximumRecord, output));
        const auto parsedResult = pbprotocol::ParseBootstrapRecord(output);
        REQUIRE(parsedResult);
        REQUIRE(parsedResult.Value() == maximumRecord);
    }
}

TEST_CASE("PB-Control-1 matches the independent SessionDescriptor Golden Vector",
          "[pbprotocol][control][wire][golden]")
{
    STATIC_REQUIRE(pbprotocol::kControlRecordPrefixBytes == 26);
    STATIC_REQUIRE(pbprotocol::kMinimumControlRecordBytes == 30);
    STATIC_REQUIRE(pbprotocol::kMaximumControlRecordBytes == 65536);
    STATIC_REQUIRE(pbprotocol::kMaximumControlPayloadBytes == 65506);

    const pbprotocol::ControlRecordView record = MakeControlRecord();
    const auto sizeResult = pbprotocol::GetSerializedSize(record);
    REQUIRE(sizeResult);
    REQUIRE(sizeResult.Value() == kControlGolden.size());

    std::array<std::byte, kControlGolden.size()> bytes{};
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    REQUIRE(bytes == kControlGolden);
    REQUIRE(
        pbprotocol::ComputeCrc32c(
            std::span<const std::byte>(kControlGolden).first(63)) ==
        0xA13883C8U);

    const auto parsedResult = pbprotocol::ParseControlRecord(kControlGolden);
    REQUIRE(parsedResult);
    REQUIRE(parsedResult.Value().controlVersion == pbprotocol::kControlVersion);
    REQUIRE(
        parsedResult.Value().recordType ==
        pbprotocol::ControlRecordType::SessionDescriptor);
    REQUIRE(parsedResult.Value().controlSequence == 0x0102030405060708ULL);
    REQUIRE(
        parsedResult.Value().sessionTag ==
        pbprotocol::SessionTag{0x81DF204BD997BAD0ULL});
    REQUIRE(std::ranges::equal(
        parsedResult.Value().payload,
        kSessionDescriptorGolden));

    std::array<std::byte, kControlGolden.size()> reserialized{};
    REQUIRE(pbprotocol::SerializeControlRecord(
        parsedResult.Value(),
        reserialized));
    REQUIRE(reserialized == kControlGolden);
}

TEST_CASE("PB-Control-1 parser enforces bounded exact RecordBytes",
          "[pbprotocol][control][wire][length]")
{
    for (std::size_t byteCount = 0;
         byteCount < kControlGolden.size();
         byteCount++)
    {
        CAPTURE(byteCount);
        const auto result = pbprotocol::ParseControlRecord(
            std::span<const std::byte>(kControlGolden).first(byteCount));
        REQUIRE_FALSE(result);
        if (byteCount < pbprotocol::kMinimumControlRecordBytes)
        {
            REQUIRE(
                result.Error() ==
                pbprotocol::ProtocolError{
                    pbprotocol::ProtocolErrorCode::TruncatedInput,
                    byteCount});
        }
        else
        {
            REQUIRE(
                result.Error() ==
                pbprotocol::ProtocolError{
                    pbprotocol::ProtocolErrorCode::InvalidRecordSize,
                    kControlRecordBytesOffset});
        }
    }

    SECTION("Declared size below the structural minimum is invalid")
    {
        auto bytes = kControlGolden;
        StoreUint32(bytes, kControlRecordBytesOffset, 29);
        const auto result = pbprotocol::ParseControlRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidRecordSize);
    }

    SECTION("Declared size above the protocol maximum is rejected")
    {
        auto bytes = kControlGolden;
        StoreUint32(bytes, kControlRecordBytesOffset, 65537);
        const auto result = pbprotocol::ParseControlRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error().code ==
            pbprotocol::ProtocolErrorCode::LengthLimitExceeded);
    }

    SECTION("Declared size must equal the supplied record")
    {
        auto bytes = kControlGolden;
        StoreUint32(bytes, kControlRecordBytesOffset, 66);
        const auto result = pbprotocol::ParseControlRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidRecordSize);

        std::vector<std::byte> trailing(
            kControlGolden.begin(),
            kControlGolden.end());
        trailing.push_back(Byte(0x00));
        const auto trailingResult = pbprotocol::ParseControlRecord(trailing);
        REQUIRE_FALSE(trailingResult);
        REQUIRE(
            trailingResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidRecordSize);
    }

    SECTION("Actual input above the protocol maximum fails before parsing")
    {
        const std::vector<std::byte> oversized(
            pbprotocol::kMaximumControlRecordBytes + 1,
            Byte(0x00));
        const auto result = pbprotocol::ParseControlRecord(oversized);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::LengthLimitExceeded,
                kControlRecordBytesOffset});
    }
}

TEST_CASE("PB-Control-1 validates Magic CRC version and record type",
          "[pbprotocol][control][wire][validation]")
{
    SECTION("Magic is checked before CRC")
    {
        auto bytes = kControlGolden;
        bytes[0] ^= Byte(0x01);
        const auto result = pbprotocol::ParseControlRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::InvalidMagic,
                0});
    }

    SECTION("ControlVersion is exact after a valid CRC")
    {
        auto bytes = kControlGolden;
        bytes[4] = Byte(0x02);
        RefreshControlCrc(bytes);
        const auto result = pbprotocol::ParseControlRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::UnsupportedControlVersion,
                4});
    }

    SECTION("All unknown record types fail closed after a valid CRC")
    {
        constexpr std::array<std::uint8_t, 3> invalidTypes{0, 4, 255};
        for (const std::uint8_t invalidType : invalidTypes)
        {
            CAPTURE(invalidType);
            auto bytes = kControlGolden;
            bytes[5] = Byte(invalidType);
            RefreshControlCrc(bytes);
            const auto result = pbprotocol::ParseControlRecord(bytes);
            REQUIRE_FALSE(result);
            REQUIRE(
                result.Error() ==
                pbprotocol::ProtocolError{
                    pbprotocol::ProtocolErrorCode::InvalidEnumValue,
                    5});
        }
    }

    SECTION("Protected header mutation fails CRC")
    {
        auto bytes = kControlGolden;
        bytes[6] ^= Byte(0x80);
        const auto result = pbprotocol::ParseControlRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::CrcMismatch,
                63});
    }

    SECTION("Payload mutation fails CRC")
    {
        auto bytes = kControlGolden;
        bytes[26] ^= Byte(0x80);
        const auto result = pbprotocol::ParseControlRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::CrcMismatch,
                63});
    }

    SECTION("CRC trailer mutation is rejected")
    {
        auto bytes = kControlGolden;
        bytes.back() ^= Byte(0x01);
        const auto result = pbprotocol::ParseControlRecord(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::CrcMismatch,
                63});
    }
}

TEST_CASE("PB-Control-1 supports empty and maximum payload boundaries",
          "[pbprotocol][control][wire][boundary]")
{
    SECTION("The envelope permits an empty opaque payload")
    {
        const pbprotocol::ControlRecordView record = MakeControlRecord({});
        const auto sizeResult = pbprotocol::GetSerializedSize(record);
        REQUIRE(sizeResult);
        REQUIRE(sizeResult.Value() == pbprotocol::kMinimumControlRecordBytes);

        std::array<std::byte, pbprotocol::kMinimumControlRecordBytes> bytes{};
        REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
        const auto parsedResult = pbprotocol::ParseControlRecord(bytes);
        REQUIRE(parsedResult);
        REQUIRE(parsedResult.Value().payload.empty());

        const pbprotocol::ReceiverResourcePolicy resourcePolicy =
            pbprotocol::test::MakeResourcePolicy();
        const auto descriptorResult = pbprotocol::ParseSessionDescriptor(
            parsedResult.Value().payload,
            resourcePolicy);
        REQUIRE_FALSE(descriptorResult);
        REQUIRE(
            descriptorResult.Error().code ==
            pbprotocol::ProtocolErrorCode::TruncatedInput);
    }

    SECTION("The exact 64 KiB record limit round-trips")
    {
        std::vector<std::byte> payload(pbprotocol::kMaximumControlPayloadBytes);
        for (std::size_t byteIndex = 0; byteIndex < payload.size(); byteIndex++)
        {
            payload[byteIndex] = Byte(static_cast<std::uint8_t>(
                (byteIndex * 17U + 3U) & 0xFFU));
        }
        const pbprotocol::ControlRecordView record = MakeControlRecord(payload);
        const auto sizeResult = pbprotocol::GetSerializedSize(record);
        REQUIRE(sizeResult);
        REQUIRE(sizeResult.Value() == pbprotocol::kMaximumControlRecordBytes);

        std::vector<std::byte> bytes(sizeResult.Value());
        REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
        const auto parsedResult = pbprotocol::ParseControlRecord(bytes);
        REQUIRE(parsedResult);
        REQUIRE(std::ranges::equal(parsedResult.Value().payload, payload));

        std::vector<std::byte> reserialized(bytes.size());
        REQUIRE(pbprotocol::SerializeControlRecord(
            parsedResult.Value(),
            reserialized));
        REQUIRE(reserialized == bytes);
    }

    SECTION("Payload limit plus one fails without modifying output")
    {
        const std::vector<std::byte> payload(
            pbprotocol::kMaximumControlPayloadBytes + 1,
            Byte(0xA5));
        const pbprotocol::ControlRecordView record = MakeControlRecord(payload);
        const auto sizeResult = pbprotocol::GetSerializedSize(record);
        REQUIRE_FALSE(sizeResult);
        REQUIRE(
            sizeResult.Error().code ==
            pbprotocol::ProtocolErrorCode::LengthLimitExceeded);

        std::vector<std::byte> output(
            pbprotocol::kMaximumControlRecordBytes,
            Byte(0x5A));
        const auto originalOutput = output;
        const auto status = pbprotocol::SerializeControlRecord(record, output);
        REQUIRE_FALSE(status);
        REQUIRE(
            status.Error().code ==
            pbprotocol::ProtocolErrorCode::LengthLimitExceeded);
        REQUIRE(output == originalOutput);
    }
}

TEST_CASE("PB-Control-1 serializer preserves output on all validation failures",
          "[pbprotocol][control][wire][atomic]")
{
    const pbprotocol::ControlRecordView validRecord = MakeControlRecord();

    SECTION("Wrong output sizes are atomic")
    {
        std::vector<std::byte> shortOutput(
            kControlGolden.size() - 1,
            Byte(0xA5));
        const auto originalShortOutput = shortOutput;
        const auto shortStatus = pbprotocol::SerializeControlRecord(
            validRecord,
            shortOutput);
        REQUIRE_FALSE(shortStatus);
        REQUIRE(
            shortStatus.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidRecordSize);
        REQUIRE(shortOutput == originalShortOutput);

        std::vector<std::byte> longOutput(
            kControlGolden.size() + 1,
            Byte(0x5A));
        const auto originalLongOutput = longOutput;
        const auto longStatus = pbprotocol::SerializeControlRecord(
            validRecord,
            longOutput);
        REQUIRE_FALSE(longStatus);
        REQUIRE(
            longStatus.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidRecordSize);
        REQUIRE(longOutput == originalLongOutput);
    }

    SECTION("Invalid version and type are atomic")
    {
        std::array<std::byte, kControlGolden.size()> output{};
        output.fill(Byte(0xA5));
        const auto originalOutput = output;

        auto invalidVersion = validRecord;
        invalidVersion.controlVersion = 2;
        const auto versionStatus = pbprotocol::SerializeControlRecord(
            invalidVersion,
            output);
        REQUIRE_FALSE(versionStatus);
        REQUIRE(
            versionStatus.Error().code ==
            pbprotocol::ProtocolErrorCode::UnsupportedControlVersion);
        REQUIRE(output == originalOutput);

        auto invalidType = validRecord;
        invalidType.recordType = static_cast<pbprotocol::ControlRecordType>(255);
        const auto typeStatus = pbprotocol::SerializeControlRecord(
            invalidType,
            output);
        REQUIRE_FALSE(typeStatus);
        REQUIRE(
            typeStatus.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidEnumValue);
        REQUIRE(output == originalOutput);
    }

    SECTION("Payload may alias the output because serialization is staged")
    {
        const std::array<std::byte, 5> independentPayload{
            Byte(0x10),
            Byte(0x20),
            Byte(0x30),
            Byte(0x40),
            Byte(0x50)};
        const pbprotocol::ControlRecordView independentRecord =
            MakeControlRecord(independentPayload);
        const auto serializedSizeResult =
            pbprotocol::GetSerializedSize(independentRecord);
        REQUIRE(serializedSizeResult);

        std::vector<std::byte> expected(serializedSizeResult.Value());
        REQUIRE(pbprotocol::SerializeControlRecord(
            independentRecord,
            expected));

        std::vector<std::byte> aliasedOutput(
            serializedSizeResult.Value(),
            Byte(0xA5));
        constexpr std::size_t payloadStorageOffset = 7;
        std::copy(
            independentPayload.begin(),
            independentPayload.end(),
            aliasedOutput.begin() + payloadStorageOffset);

        pbprotocol::ControlRecordView aliasedRecord =
            MakeControlRecord(std::span<const std::byte>(aliasedOutput).subspan(
                payloadStorageOffset,
                independentPayload.size()));
        REQUIRE(pbprotocol::SerializeControlRecord(
            aliasedRecord,
            aliasedOutput));
        REQUIRE(aliasedOutput == expected);
    }
}

TEST_CASE("PB-Control-1 composes with every existing descriptor parser",
          "[pbprotocol][control][descriptor][integration]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(234, 2);
    const pbprotocol::SessionTag sessionTag =
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId);

    SECTION("SessionDescriptor")
    {
        std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> payload{};
        REQUIRE(pbprotocol::SerializeSessionDescriptor(
            sessionDescriptor,
            resourcePolicy,
            payload));
        const std::vector<std::byte> envelope = WrapControlPayload(
            pbprotocol::ControlRecordType::SessionDescriptor,
            10,
            sessionTag,
            payload);
        const auto controlResult = pbprotocol::ParseControlRecord(envelope);
        REQUIRE(controlResult);
        REQUIRE(controlResult.Value().sessionTag == sessionTag);

        const auto descriptorResult = pbprotocol::ParseSessionDescriptor(
            controlResult.Value().payload,
            resourcePolicy);
        REQUIRE(descriptorResult);
        REQUIRE(descriptorResult.Value() == sessionDescriptor);
        REQUIRE(
            pbprotocol::DeriveSessionTag(descriptorResult.Value().sessionId) ==
            controlResult.Value().sessionTag);
    }

    SECTION("DirectRepeat SegmentDescriptor")
    {
        const pbprotocol::SegmentDescriptor segmentDescriptor =
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor,
                0,
                0,
                117);
        std::array<
            std::byte,
            pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes> payload{};
        REQUIRE(pbprotocol::SerializeSegmentDescriptor(
            segmentDescriptor,
            sessionDescriptor,
            resourcePolicy,
            payload));
        const std::vector<std::byte> envelope = WrapControlPayload(
            pbprotocol::ControlRecordType::SegmentDescriptor,
            11,
            sessionTag,
            payload);
        const auto controlResult = pbprotocol::ParseControlRecord(envelope);
        REQUIRE(controlResult);
        REQUIRE(controlResult.Value().sessionTag == sessionTag);

        const auto descriptorResult = pbprotocol::ParseSegmentDescriptor(
            controlResult.Value().payload,
            sessionDescriptor,
            resourcePolicy);
        REQUIRE(descriptorResult);
        REQUIRE(descriptorResult.Value() == segmentDescriptor);
        REQUIRE(
            descriptorResult.Value().sessionTag ==
            controlResult.Value().sessionTag);
    }

    SECTION("Wirehair V2 SegmentDescriptor")
    {
        const pbprotocol::SegmentDescriptor segmentDescriptor =
            pbprotocol::test::MakeWirehairSegment(
                sessionDescriptor,
                1,
                117,
                117);
        std::array<
            std::byte,
            pbprotocol::kWirehairV2SegmentDescriptorPayloadBytes> payload{};
        REQUIRE(pbprotocol::SerializeSegmentDescriptor(
            segmentDescriptor,
            sessionDescriptor,
            resourcePolicy,
            payload));
        const std::vector<std::byte> envelope = WrapControlPayload(
            pbprotocol::ControlRecordType::SegmentDescriptor,
            12,
            sessionTag,
            payload);
        const auto controlResult = pbprotocol::ParseControlRecord(envelope);
        REQUIRE(controlResult);
        REQUIRE(controlResult.Value().sessionTag == sessionTag);

        const auto descriptorResult = pbprotocol::ParseSegmentDescriptor(
            controlResult.Value().payload,
            sessionDescriptor,
            resourcePolicy);
        REQUIRE(descriptorResult);
        REQUIRE(descriptorResult.Value() == segmentDescriptor);
        REQUIRE(
            descriptorResult.Value().sessionTag ==
            controlResult.Value().sessionTag);
    }

    SECTION("FinalManifest")
    {
        const pbprotocol::FinalManifest finalManifest =
            pbprotocol::test::MakeFinalManifest(sessionDescriptor);
        std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> payload{};
        REQUIRE(pbprotocol::SerializeFinalManifest(
            finalManifest,
            sessionDescriptor,
            resourcePolicy,
            payload));
        const std::vector<std::byte> envelope = WrapControlPayload(
            pbprotocol::ControlRecordType::FinalManifest,
            13,
            sessionTag,
            payload);
        const auto controlResult = pbprotocol::ParseControlRecord(envelope);
        REQUIRE(controlResult);
        REQUIRE(controlResult.Value().sessionTag == sessionTag);

        const auto manifestResult = pbprotocol::ParseFinalManifest(
            controlResult.Value().payload,
            sessionDescriptor,
            resourcePolicy);
        REQUIRE(manifestResult);
        REQUIRE(manifestResult.Value() == finalManifest);
        REQUIRE(
            pbprotocol::DeriveSessionTag(manifestResult.Value().sessionId) ==
            controlResult.Value().sessionTag);
    }

    SECTION("Envelope CRC does not make malformed payload acceptable")
    {
        std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> malformedPayload{};
        REQUIRE(pbprotocol::SerializeSessionDescriptor(
            sessionDescriptor,
            resourcePolicy,
            malformedPayload));
        malformedPayload[pbprotocol::kFormalWireSessionDescriptorDigestAlgorithmOffset] = Byte(0xFF);
        RefreshDescriptorCrc(malformedPayload);
        const std::vector<std::byte> envelope = WrapControlPayload(
            pbprotocol::ControlRecordType::SessionDescriptor,
            14,
            sessionTag,
            malformedPayload);
        const auto controlResult = pbprotocol::ParseControlRecord(envelope);
        REQUIRE(controlResult);

        const auto descriptorResult = pbprotocol::ParseSessionDescriptor(
            controlResult.Value().payload,
            resourcePolicy);
        REQUIRE_FALSE(descriptorResult);
        REQUIRE(
            descriptorResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidEnumValue);
    }

    SECTION("Envelope SessionTag must be checked against the descriptor binding")
    {
        std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> payload{};
        REQUIRE(pbprotocol::SerializeSessionDescriptor(
            sessionDescriptor,
            resourcePolicy,
            payload));
        const pbprotocol::SessionTag wrongTag{sessionTag.value ^ 1ULL};
        const std::vector<std::byte> envelope = WrapControlPayload(
            pbprotocol::ControlRecordType::SessionDescriptor,
            15,
            wrongTag,
            payload);
        const auto controlResult = pbprotocol::ParseControlRecord(envelope);
        REQUIRE(controlResult);

        const auto descriptorResult = pbprotocol::ParseSessionDescriptor(
            controlResult.Value().payload,
            resourcePolicy);
        REQUIRE(descriptorResult);
        REQUIRE(
            pbprotocol::DeriveSessionTag(descriptorResult.Value().sessionId) !=
            controlResult.Value().sessionTag);
    }
}
