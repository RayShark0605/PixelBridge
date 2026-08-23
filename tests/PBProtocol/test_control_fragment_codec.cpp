#include "descriptor_test_helpers.h"

#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/control_fragment_codec.h"
#include "pbprotocol/crc32c.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace {

using pbprotocol::test::Byte;

constexpr std::size_t kFragmentCountOffset = 10;
constexpr std::size_t kTotalRecordBytesOffset = 12;
constexpr std::size_t kFragmentBytesOffset = 16;
constexpr std::size_t kFlagsOffset = 18;

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

constexpr std::array<std::byte, 48> kFragmentZeroGolden{
    Byte(0x08), Byte(0x07), Byte(0x06), Byte(0x05),
    Byte(0x04), Byte(0x03), Byte(0x02), Byte(0x01),
    Byte(0x00), Byte(0x00), Byte(0x03), Byte(0x00),
    Byte(0x43), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x18), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x50), Byte(0x42), Byte(0x43), Byte(0x52),
    Byte(0x01), Byte(0x01), Byte(0x08), Byte(0x07),
    Byte(0x06), Byte(0x05), Byte(0x04), Byte(0x03),
    Byte(0x02), Byte(0x01), Byte(0xD0), Byte(0xBA),
    Byte(0x97), Byte(0xD9), Byte(0x4B), Byte(0x20),
    Byte(0xDF), Byte(0x81), Byte(0x43), Byte(0x00),
    Byte(0x02), Byte(0x54), Byte(0xCD), Byte(0x9D)};

constexpr std::array<std::byte, 48> kFragmentOneGolden{
    Byte(0x08), Byte(0x07), Byte(0x06), Byte(0x05),
    Byte(0x04), Byte(0x03), Byte(0x02), Byte(0x01),
    Byte(0x01), Byte(0x00), Byte(0x03), Byte(0x00),
    Byte(0x43), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x18), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x01), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x01),
    Byte(0x02), Byte(0x03), Byte(0x04), Byte(0x05),
    Byte(0x06), Byte(0x07), Byte(0x08), Byte(0x09),
    Byte(0x0A), Byte(0x0B), Byte(0x0C), Byte(0x0D),
    Byte(0x0E), Byte(0x0F), Byte(0x75), Byte(0x00),
    Byte(0xD8), Byte(0x94), Byte(0xFF), Byte(0xF3)};

constexpr std::array<std::byte, 43> kFragmentTwoGolden{
    Byte(0x08), Byte(0x07), Byte(0x06), Byte(0x05),
    Byte(0x04), Byte(0x03), Byte(0x02), Byte(0x01),
    Byte(0x02), Byte(0x00), Byte(0x03), Byte(0x00),
    Byte(0x43), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x13), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x01), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x01), Byte(0xC8),
    Byte(0x83), Byte(0x38), Byte(0xA1),
    Byte(0x66), Byte(0x6F), Byte(0x10), Byte(0x40)};

template <typename Range>
concept CanParseFragmentRange = requires(Range&& input)
{
    pbprotocol::ParseControlFragment(std::forward<Range>(input));
};

template <typename Range>
concept CanGetFragmentRange = requires(Range&& input)
{
    pbprotocol::GetControlFragment(
        1,
        std::forward<Range>(input),
        0,
        24);
};

static_assert(!CanParseFragmentRange<std::vector<std::byte>>);
static_assert(CanParseFragmentRange<std::span<const std::byte>>);
static_assert(!CanGetFragmentRange<std::vector<std::byte>>);
static_assert(CanGetFragmentRange<std::span<const std::byte>>);

void StoreUint16(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint16_t value)
{
    REQUIRE(offset <= bytes.size());
    REQUIRE(sizeof(value) <= bytes.size() - offset);
    for (std::size_t byteIndex = 0; byteIndex < sizeof(value); byteIndex++)
    {
        bytes[offset + byteIndex] = Byte(static_cast<std::uint8_t>(
            (value >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU));
    }
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

void RefreshFragmentCrc(const std::span<std::byte> bytes)
{
    REQUIRE(bytes.size() >= pbprotocol::kMinimumControlFragmentBytes);
    const std::size_t crcOffset =
        bytes.size() - pbprotocol::kControlFragmentCrcBytes;
    StoreUint32(
        bytes,
        crcOffset,
        pbprotocol::ComputeCrc32c(
            std::span<const std::byte>(bytes).first(crcOffset)));
}

[[nodiscard]] std::vector<std::byte> SerializeFragment(
    const pbprotocol::ControlFragmentView& fragment)
{
    const auto sizeResult = pbprotocol::GetSerializedSize(fragment);
    REQUIRE(sizeResult);
    std::vector<std::byte> bytes(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeControlFragment(fragment, bytes));
    return bytes;
}

} // namespace

TEST_CASE("PB-Control-Fragment-1 matches independent Golden Vectors",
          "[pbprotocol][control][fragment][wire][golden]")
{
    STATIC_REQUIRE(pbprotocol::kControlFragmentPrefixBytes == 20);
    STATIC_REQUIRE(pbprotocol::kControlFragmentCrcBytes == 4);
    STATIC_REQUIRE(pbprotocol::kMaximumControlFragmentPayloadBytes == 65535);

    const auto countResult = pbprotocol::GetControlFragmentCount(
        kControlGolden,
        24);
    REQUIRE(countResult);
    REQUIRE(countResult.Value() == 3);

    const std::array<std::span<const std::byte>, 3> expectedFragments{
        kFragmentZeroGolden,
        kFragmentOneGolden,
        kFragmentTwoGolden};
    const std::array<std::uint32_t, 3> expectedCrcs{
        0x9DCD5402U,
        0xF3FF94D8U,
        0x40106F66U};

    for (std::uint16_t fragmentIndex = 0;
         fragmentIndex < expectedFragments.size();
         fragmentIndex++)
    {
        const auto fragmentResult = pbprotocol::GetControlFragment(
            0x0102030405060708ULL,
            kControlGolden,
            fragmentIndex,
            24);
        REQUIRE(fragmentResult);
        const std::vector<std::byte> bytes = SerializeFragment(
            fragmentResult.Value());
        REQUIRE(std::ranges::equal(bytes, expectedFragments[fragmentIndex]));

        const std::size_t crcOffset =
            expectedFragments[fragmentIndex].size() -
            pbprotocol::kControlFragmentCrcBytes;
        REQUIRE(
            pbprotocol::ComputeCrc32c(
                expectedFragments[fragmentIndex].first(crcOffset)) ==
            expectedCrcs[fragmentIndex]);

        const auto parsedResult = pbprotocol::ParseControlFragment(
            expectedFragments[fragmentIndex]);
        REQUIRE(parsedResult);
        REQUIRE(parsedResult.Value().controlRecordId ==
            0x0102030405060708ULL);
        REQUIRE(parsedResult.Value().fragmentIndex == fragmentIndex);
        REQUIRE(parsedResult.Value().fragmentCount == 3);
        REQUIRE(parsedResult.Value().totalRecordBytes == kControlGolden.size());
        REQUIRE(parsedResult.Value().flags == 0);
        REQUIRE(std::ranges::equal(
            parsedResult.Value().payload,
            fragmentResult.Value().payload));
        REQUIRE(SerializeFragment(parsedResult.Value()) == bytes);
    }
}

TEST_CASE("PB-Control-Fragment-1 rejects malformed shape and every truncation",
          "[pbprotocol][control][fragment][wire][validation]")
{
    for (std::size_t truncatedSize = 0;
         truncatedSize < kFragmentZeroGolden.size();
         truncatedSize++)
    {
        INFO("truncatedSize=" << truncatedSize);
        REQUIRE_FALSE(pbprotocol::ParseControlFragment(
            std::span<const std::byte>(kFragmentZeroGolden).first(
                truncatedSize)));
    }

    SECTION("CRC mismatch")
    {
        auto bytes = kFragmentZeroGolden;
        bytes[20] ^= Byte(0x01);
        const auto result = pbprotocol::ParseControlFragment(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::CrcMismatch);
        REQUIRE(result.Error().offset == 44);
    }

    SECTION("trailing data is not accepted")
    {
        std::vector<std::byte> bytes(
            kFragmentZeroGolden.begin(),
            kFragmentZeroGolden.end());
        bytes.push_back(Byte(0));
        const auto result = pbprotocol::ParseControlFragment(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::InvalidRecordSize,
                kFragmentBytesOffset});
    }

    SECTION("reserved flags fail closed after a valid CRC")
    {
        auto bytes = kFragmentZeroGolden;
        StoreUint16(bytes, kFlagsOffset, 1);
        RefreshFragmentCrc(bytes);
        const auto result = pbprotocol::ParseControlFragment(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::NonZeroReservedBits,
                kFlagsOffset});
    }

    SECTION("zero count, out-of-range index, and impossible count fail closed")
    {
        auto zeroCount = kFragmentZeroGolden;
        StoreUint16(zeroCount, kFragmentCountOffset, 0);
        RefreshFragmentCrc(zeroCount);
        REQUIRE_FALSE(pbprotocol::ParseControlFragment(zeroCount));

        auto badIndex = kFragmentZeroGolden;
        StoreUint16(badIndex, 8, 3);
        RefreshFragmentCrc(badIndex);
        REQUIRE_FALSE(pbprotocol::ParseControlFragment(badIndex));

        auto impossibleCount = kFragmentZeroGolden;
        StoreUint16(impossibleCount, kFragmentCountOffset, 68);
        RefreshFragmentCrc(impossibleCount);
        const auto impossibleResult = pbprotocol::ParseControlFragment(
            impossibleCount);
        REQUIRE_FALSE(impossibleResult);
        REQUIRE(impossibleResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidControlFragment);
    }

    SECTION("declared fragment length must equal the exact input")
    {
        auto bytes = kFragmentZeroGolden;
        StoreUint16(bytes, kFragmentBytesOffset, 23);
        RefreshFragmentCrc(bytes);
        const auto result = pbprotocol::ParseControlFragment(bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidRecordSize);
    }

    SECTION("total record size obeys PB-Control-1 bounds")
    {
        auto tooSmall = kFragmentZeroGolden;
        StoreUint32(tooSmall, kTotalRecordBytesOffset, 29);
        RefreshFragmentCrc(tooSmall);
        const auto smallResult = pbprotocol::ParseControlFragment(tooSmall);
        REQUIRE_FALSE(smallResult);
        REQUIRE(smallResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidRecordSize);

        auto tooLarge = kFragmentZeroGolden;
        StoreUint32(tooLarge, kTotalRecordBytesOffset, 65537);
        RefreshFragmentCrc(tooLarge);
        const auto largeResult = pbprotocol::ParseControlFragment(tooLarge);
        REQUIRE_FALSE(largeResult);
        REQUIRE(largeResult.Error().code ==
            pbprotocol::ProtocolErrorCode::LengthLimitExceeded);
    }
}

TEST_CASE("PB-Control-Fragment-1 enforces sender and wire boundaries",
          "[pbprotocol][control][fragment][wire][boundary]")
{
    SECTION("empty payload and inconsistent single fragment are rejected")
    {
        const pbprotocol::ControlFragmentView emptyFragment{
            1, 0, 1, 30, 0, {}};
        const auto emptySizeResult = pbprotocol::GetSerializedSize(
            emptyFragment);
        REQUIRE_FALSE(emptySizeResult);
        REQUIRE(emptySizeResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidControlFragment);

        const std::array<std::byte, 29> payload{};
        const pbprotocol::ControlFragmentView shortSingle{
            1, 0, 1, 30, 0, payload};
        REQUIRE_FALSE(pbprotocol::GetSerializedSize(shortSingle));
    }

    SECTION("wire maximum fragment payload round trips")
    {
        const std::vector<std::byte> payload(
            pbprotocol::kMaximumControlFragmentPayloadBytes,
            Byte(0x5A));
        const pbprotocol::ControlFragmentView fragment{
            2,
            0,
            1,
            static_cast<std::uint32_t>(payload.size()),
            0,
            payload};
        const std::vector<std::byte> bytes = SerializeFragment(fragment);
        REQUIRE(bytes.size() == pbprotocol::kMaximumControlFragmentBytes);
        const auto parsedResult = pbprotocol::ParseControlFragment(bytes);
        REQUIRE(parsedResult);
        REQUIRE(parsedResult.Value().payload.size() == payload.size());
        REQUIRE(std::ranges::equal(parsedResult.Value().payload, payload));
    }

    SECTION("a 65536-byte Control record cannot use one-byte fragments")
    {
        const std::vector<std::byte> payload(
            pbprotocol::kMaximumControlPayloadBytes,
            Byte(0xA5));
        const pbprotocol::ControlRecordView record{
            pbprotocol::kControlVersion,
            pbprotocol::ControlRecordType::SessionDescriptor,
            1,
            pbprotocol::SessionTag{2},
            payload};
        std::vector<std::byte> recordBytes(
            pbprotocol::kMaximumControlRecordBytes);
        REQUIRE(pbprotocol::SerializeControlRecord(record, recordBytes));

        const auto impossibleCountResult =
            pbprotocol::GetControlFragmentCount(recordBytes, 1);
        REQUIRE_FALSE(impossibleCountResult);
        REQUIRE(impossibleCountResult.Error().code ==
            pbprotocol::ProtocolErrorCode::LengthLimitExceeded);

        const auto maximumPayloadCountResult =
            pbprotocol::GetControlFragmentCount(recordBytes, UINT16_MAX);
        REQUIRE(maximumPayloadCountResult);
        REQUIRE(maximumPayloadCountResult.Value() == 2);
        const auto tailResult = pbprotocol::GetControlFragment(
            3,
            recordBytes,
            1,
            UINT16_MAX);
        REQUIRE(tailResult);
        REQUIRE(tailResult.Value().payload.size() == 1);
    }

    SECTION("the largest representable fragment count is exactly 65535")
    {
        const std::vector<std::byte> payload(
            pbprotocol::kMaximumControlPayloadBytes - 1U,
            Byte(0x3C));
        const pbprotocol::ControlRecordView record{
            pbprotocol::kControlVersion,
            pbprotocol::ControlRecordType::SessionDescriptor,
            4,
            pbprotocol::SessionTag{5},
            payload};
        std::vector<std::byte> recordBytes(
            pbprotocol::kMaximumControlRecordBytes - 1U);
        REQUIRE(pbprotocol::SerializeControlRecord(record, recordBytes));

        const auto countResult = pbprotocol::GetControlFragmentCount(
            recordBytes,
            1);
        REQUIRE(countResult);
        REQUIRE(countResult.Value() == UINT16_MAX);
        const auto lastResult = pbprotocol::GetControlFragment(
            6,
            recordBytes,
            static_cast<std::uint16_t>(UINT16_MAX - 1U),
            1);
        REQUIRE(lastResult);
        REQUIRE(lastResult.Value().fragmentIndex == UINT16_MAX - 1U);
        REQUIRE(lastResult.Value().fragmentCount == UINT16_MAX);
        REQUIRE(lastResult.Value().payload.size() == 1);
    }

    SECTION("serializer is atomic and supports aliased payload")
    {
        std::vector<std::byte> output(kFragmentZeroGolden.size(), Byte(0xCC));
        std::copy_n(
            kControlGolden.begin(),
            24,
            output.begin() + pbprotocol::kControlFragmentPrefixBytes);
        const pbprotocol::ControlFragmentView aliased{
            0x0102030405060708ULL,
            0,
            3,
            67,
            0,
            std::span<const std::byte>(output).subspan(20, 24)};
        REQUIRE(pbprotocol::SerializeControlFragment(aliased, output));
        REQUIRE(std::ranges::equal(output, kFragmentZeroGolden));

        std::vector<std::byte> unchanged(47, Byte(0x7E));
        const std::vector<std::byte> before = unchanged;
        const auto fragmentResult = pbprotocol::GetControlFragment(
            1,
            kControlGolden,
            0,
            24);
        REQUIRE(fragmentResult);
        const auto status = pbprotocol::SerializeControlFragment(
            fragmentResult.Value(),
            unchanged);
        REQUIRE_FALSE(status);
        REQUIRE(unchanged == before);
    }
}
