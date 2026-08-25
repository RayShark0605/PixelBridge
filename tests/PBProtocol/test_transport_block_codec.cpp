#include "pbprotocol/transport_block_codec.h"

#include "pbprotocol/crc32c.h"
#include "pbprotocol/protocol_result.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

[[nodiscard]] bool SpansEqual(const std::span<const std::byte> left,
    const std::span<const std::byte> right) noexcept
{
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

// Deterministic PRNG (splitmix64), identical formula to the other test
// suites so cross-suite patterns stay byte-compatible.
class SplitMix64
{
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t Next() noexcept
    {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t mixed = state_;
        mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
        return mixed ^ (mixed >> 31);
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] std::byte Byte(const std::uint8_t value) noexcept
{
    return static_cast<std::byte>(value);
}

constexpr std::uint64_t kGoldenSessionTag = 0x81DF204BD997BAD0ULL;

[[nodiscard]] pbprotocol::TransportBlockHeader MakeHeader(
    const std::uint16_t payloadBytes,
    const std::uint64_t sessionTag = kGoldenSessionTag,
    const std::uint64_t segmentOrdinal = 0,
    const std::uint32_t outerBlockId = 0) noexcept
{
    return pbprotocol::TransportBlockHeader{
        pbprotocol::kTransportBlockTypeData,
        pbprotocol::kTransportProtocolMinor,
        0,
        pbprotocol::SessionTag{sessionTag},
        segmentOrdinal,
        outerBlockId,
        payloadBytes};
}

[[nodiscard]] std::vector<std::byte> MakePatternPayload(
    const std::size_t byteCount)
{
    std::vector<std::byte> payload(byteCount);
    SplitMix64 patternRng(0xC0FFEE);
    for (std::size_t byteIndex = 0; byteIndex < payload.size(); byteIndex++)
    {
        payload[byteIndex] =
            Byte(static_cast<std::uint8_t>(patternRng.Next() & 0xFFU));
    }
    return payload;
}

[[nodiscard]] std::vector<std::byte> SerializeOrDie(
    const pbprotocol::TransportBlockHeader& header,
    const std::span<const std::byte> payload)
{
    std::vector<std::byte> buffer(pbprotocol::GetTransportSerializedSize(
        header));
    REQUIRE(pbprotocol::SerializeTransportBlock(
        header, payload, std::span<std::byte>(buffer)));
    return buffer;
}

// Serializes, then rewrites a whole field in place (leaving the header CRC
// stale unless recomputeHeaderCrc is set) and returns the mutated bytes.
[[nodiscard]] std::vector<std::byte> SerializeWithFieldOverride(
    const pbprotocol::TransportBlockHeader& header,
    const std::span<const std::byte> payload,
    const std::size_t fieldOffset,
    const std::span<const std::byte> fieldBytes,
    const bool recomputeHeaderCrc)
{
    std::vector<std::byte> buffer = SerializeOrDie(header, payload);
    REQUIRE(fieldOffset + fieldBytes.size() <=
        pbprotocol::kTransportHeaderBytes);
    for (std::size_t byteIndex = 0; byteIndex < fieldBytes.size();
         byteIndex++)
    {
        buffer[fieldOffset + byteIndex] = fieldBytes[byteIndex];
    }
    if (recomputeHeaderCrc)
    {
        const std::uint32_t headerCrc = pbprotocol::ComputeCrc32c(
            std::span<const std::byte>(buffer).first(
                pbprotocol::kTransportHeaderCrcCoverageBytes));
        for (std::size_t byteIndex = 0; byteIndex < 4; byteIndex++)
        {
            buffer[pbprotocol::kTransportHeaderCrcOffset + byteIndex] =
                Byte(static_cast<std::uint8_t>(
                    (headerCrc >> (byteIndex * 8U)) & 0xFFU));
        }
    }
    return buffer;
}

[[nodiscard]] std::array<std::byte, 8> EncodeUint64(
    const std::uint64_t value) noexcept
{
    std::array<std::byte, 8> bytes{};
    for (std::size_t byteIndex = 0; byteIndex < 8; byteIndex++)
    {
        bytes[byteIndex] = Byte(static_cast<std::uint8_t>(
            (value >> (byteIndex * 8U)) & 0xFFU));
    }
    return bytes;
}

[[nodiscard]] std::array<std::byte, 4> EncodeUint32(
    const std::uint32_t value) noexcept
{
    std::array<std::byte, 4> bytes{};
    for (std::size_t byteIndex = 0; byteIndex < 4; byteIndex++)
    {
        bytes[byteIndex] = Byte(static_cast<std::uint8_t>(
            (value >> (byteIndex * 8U)) & 0xFFU));
    }
    return bytes;
}

[[nodiscard]] std::array<std::byte, 2> EncodeUint16(
    const std::uint16_t value) noexcept
{
    return {Byte(static_cast<std::uint8_t>(value & 0xFFU)),
        Byte(static_cast<std::uint8_t>((value >> 8) & 0xFFU))};
}

} // namespace

TEST_CASE("Transport block structured round-trip sweep",
          "[pbprotocol][transport][roundtrip]")
{
    const std::uint64_t tagValues[] = {
        0ULL, 1ULL, kGoldenSessionTag,
        std::numeric_limits<std::uint64_t>::max()};
    const std::uint64_t ordinalValues[] = {
        0ULL, 1ULL, 0xFFFFFFFFULL, 0x100000000ULL,
        std::numeric_limits<std::uint64_t>::max()};
    const std::uint32_t outerBlockIdValues[] = {
        0U, 1U, std::numeric_limits<std::uint32_t>::max()};
    const std::uint16_t payloadSizes[] = {0U, 1U, 2U, 65534U, 65535U};

    for (const std::uint64_t sessionTag : tagValues)
    {
        for (const std::uint64_t segmentOrdinal : ordinalValues)
        {
            for (const std::uint32_t outerBlockId : outerBlockIdValues)
            {
                for (const std::uint16_t payloadSize : payloadSizes)
                {
                    const auto payload =
                        MakePatternPayload(payloadSize);
                    const pbprotocol::TransportBlockHeader header =
                        MakeHeader(
                            payloadSize,
                            sessionTag,
                            segmentOrdinal,
                            outerBlockId);
                    const std::vector<std::byte> serialized =
                        SerializeOrDie(header, payload);
                    REQUIRE(serialized.size() ==
                        pbprotocol::kTransportMinimumBlockBytes +
                            payloadSize);

                    const auto parsed =
                        pbprotocol::ParseTransportBlock(
                            std::span<const std::byte>(serialized));
                    REQUIRE(parsed);
                    REQUIRE(parsed.Value().header == header);
                    REQUIRE(SpansEqual(parsed.Value().payload,
                        std::span<const std::byte>(payload)));
                }
            }
        }
    }
}

TEST_CASE("Transport block header per-byte differential scan",
          "[pbprotocol][transport][differential]")
{
    // A canonical 9-byte-payload block; every one of the 256 values at
    // every header byte position is parsed and its outcome pinned.
    const std::vector<std::byte> payload = MakePatternPayload(9);
    const pbprotocol::TransportBlockHeader header = MakeHeader(9);
    const std::vector<std::byte> canonical = SerializeOrDie(header, payload);
    for (std::size_t scanOffset = 0;
         scanOffset < pbprotocol::kTransportHeaderBytes;
         scanOffset++)
    {
        for (std::uint16_t scanValue = 0; scanValue <= 0xFFU; scanValue++)
        {
            std::vector<std::byte> mutated = canonical;
            mutated[scanOffset] = Byte(static_cast<std::uint8_t>(scanValue));
            const auto parsed = pbprotocol::ParseTransportBlock(
                std::span<const std::byte>(mutated));
            if (static_cast<std::uint8_t>(scanValue) ==
                static_cast<std::uint8_t>(canonical[scanOffset]))
            {
                REQUIRE(parsed);
                continue;
            }
            REQUIRE_FALSE(parsed);
            const pbprotocol::ProtocolError error = parsed.Error();
            if (scanOffset == pbprotocol::kTransportBlockTypeOffset)
            {
                REQUIRE(error.code ==
                    pbprotocol::ProtocolErrorCode::InvalidEnumValue);
                REQUIRE(error.offset ==
                    pbprotocol::kTransportBlockTypeOffset);
            }
            else if (scanOffset ==
                pbprotocol::kTransportProtocolMinorOffset)
            {
                REQUIRE(error.code ==
                    pbprotocol::ProtocolErrorCode::
                        UnsupportedProtocolMinor);
                REQUIRE(error.offset ==
                    pbprotocol::kTransportProtocolMinorOffset);
            }
            else if (scanOffset >= pbprotocol::kTransportFlagsOffset &&
                scanOffset <= pbprotocol::kTransportFlagsOffset + 1)
            {
                REQUIRE(error.code ==
                    pbprotocol::ProtocolErrorCode::NonZeroReservedBits);
                REQUIRE(error.offset ==
                    pbprotocol::kTransportFlagsOffset);
            }
            else if (scanOffset >=
                pbprotocol::kTransportReservedOffset &&
                scanOffset <=
                    pbprotocol::kTransportReservedOffset + 1)
            {
                REQUIRE(error.code ==
                    pbprotocol::ProtocolErrorCode::NonZeroReservedBits);
                REQUIRE(error.offset ==
                    pbprotocol::kTransportReservedOffset);
            }
            else
            {
                // Every remaining header byte is covered by the header CRC:
                // a single-byte change must surface as CrcMismatch at the
                // CRC field offset.
                REQUIRE(error.code ==
                    pbprotocol::ProtocolErrorCode::CrcMismatch);
                REQUIRE(error.offset ==
                    pbprotocol::kTransportHeaderCrcOffset);
            }
        }
    }
}

TEST_CASE("Transport block field matrix with CRC repair hits later gates",
          "[pbprotocol][transport][fieldmatrix][crcrepair]")
{
    const std::vector<std::byte> payload = MakePatternPayload(8);
    const pbprotocol::TransportBlockHeader header = MakeHeader(8);

    // Corrupt the length field with a repaired header CRC: the parser must
    // now fail on the exact-length gate (truncation or trailing), proving
    // the header CRC alone does not authenticate the length semantics.
    const auto truncatedLength = SerializeWithFieldOverride(
        header,
        payload,
        pbprotocol::kTransportPayloadBytesOffset,
        EncodeUint16(static_cast<std::uint16_t>(payload.size() + 1)),
        true);
    const auto truncatedParse = pbprotocol::ParseTransportBlock(
        std::span<const std::byte>(truncatedLength));
    REQUIRE_FALSE(truncatedParse);
    REQUIRE(truncatedParse.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::TruncatedInput,
            truncatedLength.size()});

    const auto trailingLength = SerializeWithFieldOverride(
        header,
        payload,
        pbprotocol::kTransportPayloadBytesOffset,
        EncodeUint16(7),
        true);
    const auto trailingParse = pbprotocol::ParseTransportBlock(
        std::span<const std::byte>(trailingLength));
    REQUIRE_FALSE(trailingParse);
    REQUIRE(trailingParse.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::TrailingBytes,
            pbprotocol::kTransportMinimumBlockBytes + 7});

    // Max length field with a repaired header CRC: the wire length exceeds
    // the input, so the parser reports truncation at the input size.
    const auto maxLength = SerializeWithFieldOverride(
        header,
        payload,
        pbprotocol::kTransportPayloadBytesOffset,
        EncodeUint16(std::numeric_limits<std::uint16_t>::max()),
        true);
    const auto maxLengthParse = pbprotocol::ParseTransportBlock(
        std::span<const std::byte>(maxLength));
    REQUIRE_FALSE(maxLengthParse);
    REQUIRE(maxLengthParse.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::TruncatedInput,
            maxLength.size()});

    // Flags and reserved sweeps: each non-zero value fails at its own field
    // offset before the header CRC is even checked.
    const std::uint16_t flagValues[] = {1U, 0x7FFFU, 0xFFFFU};
    for (const std::uint16_t flags : flagValues)
    {
        const auto mutated = SerializeWithFieldOverride(
            header,
            payload,
            pbprotocol::kTransportFlagsOffset,
            EncodeUint16(flags),
            false);
        const auto parsed = pbprotocol::ParseTransportBlock(
            std::span<const std::byte>(mutated));
        REQUIRE_FALSE(parsed);
        REQUIRE(parsed.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::NonZeroReservedBits,
                pbprotocol::kTransportFlagsOffset});
    }
    const std::uint16_t reservedValues[] = {1U, 0xFFFFU};
    for (const std::uint16_t reserved : reservedValues)
    {
        const auto mutated = SerializeWithFieldOverride(
            header,
            payload,
            pbprotocol::kTransportReservedOffset,
            EncodeUint16(reserved),
            false);
        const auto parsed = pbprotocol::ParseTransportBlock(
            std::span<const std::byte>(mutated));
        REQUIRE_FALSE(parsed);
        REQUIRE(parsed.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::NonZeroReservedBits,
                pbprotocol::kTransportReservedOffset});
    }

    // Header CRC field corruptions (1-bit flip, all zero, all ones) all hit
    // the CRC gate at the header CRC offset.
    const std::vector<std::byte> canonical = SerializeOrDie(header, payload);
    const std::uint8_t originalCrcFirstByte =
        static_cast<std::uint8_t>(
            canonical[pbprotocol::kTransportHeaderCrcOffset]);
    const std::array<std::byte, 4> zeroCrc{
        Byte(0), Byte(0), Byte(0), Byte(0)};
    const std::array<std::byte, 4> onesCrc{
        Byte(0xFF), Byte(0xFF), Byte(0xFF), Byte(0xFF)};
    const std::array<std::byte, 4> bitFlipCrc{
        Byte(static_cast<std::uint8_t>(originalCrcFirstByte ^ 0x01U)),
        Byte(static_cast<std::uint8_t>(
            canonical[pbprotocol::kTransportHeaderCrcOffset + 1])),
        Byte(static_cast<std::uint8_t>(
            canonical[pbprotocol::kTransportHeaderCrcOffset + 2])),
        Byte(static_cast<std::uint8_t>(
            canonical[pbprotocol::kTransportHeaderCrcOffset + 3]))};
    for (const std::array<std::byte, 4>& crcOverride :
         {zeroCrc, onesCrc, bitFlipCrc})
    {
        const auto mutated = SerializeWithFieldOverride(
            header,
            payload,
            pbprotocol::kTransportHeaderCrcOffset,
            crcOverride,
            false);
        const auto parsed = pbprotocol::ParseTransportBlock(
            std::span<const std::byte>(mutated));
        REQUIRE_FALSE(parsed);
        REQUIRE(parsed.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::CrcMismatch,
                pbprotocol::kTransportHeaderCrcOffset});
    }
}

TEST_CASE("Transport block length boundaries",
          "[pbprotocol][transport][length]")
{
    const std::uint16_t declaredSizes[] = {0U, 1U, 65535U};
    for (const std::uint16_t declaredSize : declaredSizes)
    {
        const auto payload = MakePatternPayload(declaredSize);
        const pbprotocol::TransportBlockHeader header =
            MakeHeader(declaredSize);
        std::vector<std::byte> serialized = SerializeOrDie(
            header, payload);
        const std::size_t exactSize = serialized.size();

        auto exactParse = pbprotocol::ParseTransportBlock(
            std::span<const std::byte>(serialized));
        REQUIRE(exactParse);

        serialized.pop_back();
        const auto shortParse = pbprotocol::ParseTransportBlock(
            std::span<const std::byte>(serialized));
        REQUIRE_FALSE(shortParse);
        REQUIRE(shortParse.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::TruncatedInput,
                exactSize - 1});
        serialized.push_back(Byte(0xFF));

        serialized.push_back(Byte(0x5A));
        const auto longParse = pbprotocol::ParseTransportBlock(
            std::span<const std::byte>(serialized));
        REQUIRE_FALSE(longParse);
        REQUIRE(longParse.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::TrailingBytes,
                exactSize});
    }

    // Absolute short-input sweep against a valid empty-payload header.
    const pbprotocol::TransportBlockHeader header = MakeHeader(0);
    const std::vector<std::byte> empty = SerializeOrDie(header, {});
    const std::size_t shortLengths[] = {0U, 1U, 31U, 32U, 33U, 35U};
    for (const std::size_t shortLength : shortLengths)
    {
        const auto parsed = pbprotocol::ParseTransportBlock(
            std::span<const std::byte>(empty).first(shortLength));
        REQUIRE_FALSE(parsed);
        REQUIRE(parsed.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::TruncatedInput,
                shortLength});
    }
}

TEST_CASE("Transport block payload CRC branch isolation",
          "[pbprotocol][transport][payloadcrc]")
{
    const std::vector<std::byte> payload = MakePatternPayload(64);
    const pbprotocol::TransportBlockHeader header = MakeHeader(64);
    std::vector<std::byte> serialized = SerializeOrDie(header, payload);

    serialized[32] = Byte(static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(serialized[32]) ^ 0xFFU));
    const auto parsed = pbprotocol::ParseTransportBlock(
        std::span<const std::byte>(serialized));
    REQUIRE_FALSE(parsed);
    REQUIRE(parsed.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::CrcMismatch,
            pbprotocol::kTransportPayloadOffset + 64});

    // An empty payload block pins the CRC-32C of an empty input in the
    // trailing CRC field.
    const std::vector<std::byte> emptyBlock =
        SerializeOrDie(MakeHeader(0), {});
    const std::uint32_t emptyCrc =
        pbprotocol::ComputeCrc32c({});
    std::uint32_t storedEmptyCrc = 0;
    for (std::size_t byteIndex = 0; byteIndex < 4; byteIndex++)
    {
        storedEmptyCrc |= static_cast<std::uint32_t>(
            emptyBlock[32 + byteIndex])
            << static_cast<unsigned int>(byteIndex * 8U);
    }
    REQUIRE(storedEmptyCrc == emptyCrc);
    REQUIRE(emptyCrc == 0x00000000U);
}

TEST_CASE("Transport information block framing boundaries",
          "[pbprotocol][transport][framing]")
{
    const std::vector<std::byte> payload = MakePatternPayload(1314);
    const pbprotocol::TransportBlockHeader header = MakeHeader(1314);
    const std::vector<std::byte> block = SerializeOrDie(header, payload);
    REQUIRE(block.size() == 1350);

    const std::size_t infoSizes[] = {35U, 36U, 37U, 1350U, 65570U,
        65571U, 65572U};
    for (const std::size_t infoSize : infoSizes)
    {
        std::vector<std::byte> infoBlock(infoSize);
        const auto framed = pbprotocol::FrameTransportBlockIntoInfoBlock(
            std::span<const std::byte>(block),
            infoSize,
            std::span<std::byte>(infoBlock));
        if (infoSize < pbprotocol::kTransportMinimumBlockBytes ||
            infoSize > pbprotocol::kTransportMaximumBlockBytes ||
            infoSize < block.size())
        {
            REQUIRE_FALSE(framed);
            continue;
        }
        REQUIRE(framed);
        const auto extracted =
            pbprotocol::ExtractTransportBlockFromInfoBlock(
                std::span<const std::byte>(infoBlock));
        REQUIRE(extracted);
        REQUIRE(SpansEqual(extracted.Value(), std::span<const std::byte>(block)));
    }

    // A too-small information buffer must be rejected before any write.
    std::vector<std::byte> tooSmall(1349);
    const auto rejected = pbprotocol::FrameTransportBlockIntoInfoBlock(
        std::span<const std::byte>(block),
        1349,
        std::span<std::byte>(tooSmall));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.Error().code ==
        pbprotocol::ProtocolErrorCode::LengthLimitExceeded);

    // Output buffer size mismatch fails closed.
    std::vector<std::byte> mismatch(1349);
    const auto mismatchStatus = pbprotocol::FrameTransportBlockIntoInfoBlock(
        std::span<const std::byte>(block),
        1350,
        std::span<std::byte>(mismatch));
    REQUIRE_FALSE(mismatchStatus);
    REQUIRE(mismatchStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::OutputBufferTooSmall);

    // Maximum wire length framing: 65,571 bytes is wider than UINT16_MAX,
    // so this is the u16-wraparound regression guard.
    const auto maxPayload = MakePatternPayload(
        std::numeric_limits<std::uint16_t>::max());
    const pbprotocol::TransportBlockHeader maxHeader =
        MakeHeader(std::numeric_limits<std::uint16_t>::max());
    const std::vector<std::byte> maxBlock =
        SerializeOrDie(maxHeader, maxPayload);
    REQUIRE(maxBlock.size() == pbprotocol::kTransportMaximumBlockBytes);
    std::vector<std::byte> maxInfo(pbprotocol::kTransportMaximumBlockBytes);
    REQUIRE(pbprotocol::FrameTransportBlockIntoInfoBlock(
        std::span<const std::byte>(maxBlock),
        pbprotocol::kTransportMaximumBlockBytes,
        std::span<std::byte>(maxInfo)));
    const auto maxExtracted =
        pbprotocol::ExtractTransportBlockFromInfoBlock(
            std::span<const std::byte>(maxInfo));
    REQUIRE(maxExtracted);
    REQUIRE(SpansEqual(maxExtracted.Value(), std::span<const std::byte>(maxBlock)));
}

TEST_CASE("Transport information block extraction failure matrix",
          "[pbprotocol][transport][extraction]")
{
    const std::vector<std::byte> payload = MakePatternPayload(8);
    const pbprotocol::TransportBlockHeader header = MakeHeader(8);
    const std::size_t infoSize = 64;
    std::vector<std::byte> infoBlock(infoSize);
    REQUIRE(pbprotocol::FrameTransportBlockIntoInfoBlock(
        std::span<const std::byte>(SerializeOrDie(header, payload)),
        infoSize,
        std::span<std::byte>(infoBlock)));

    // Truncated information block.
    const auto truncated =
        pbprotocol::ExtractTransportBlockFromInfoBlock(
            std::span<const std::byte>(infoBlock).first(10));
    REQUIRE_FALSE(truncated);
    REQUIRE(truncated.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::TruncatedInput,
            10});

    // Corrupted zero padding after the block.
    std::vector<std::byte> paddedDirty = infoBlock;
    paddedDirty[44] = Byte(0x11);
    const auto dirtyPadding =
        pbprotocol::ExtractTransportBlockFromInfoBlock(
            std::span<const std::byte>(paddedDirty));
    REQUIRE_FALSE(dirtyPadding);
    REQUIRE(dirtyPadding.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::NonCanonicalPadding,
            44});

    // Corrupt header CRC inside the information block.
    std::vector<std::byte> dirtyCrc = infoBlock;
    dirtyCrc[pbprotocol::kTransportHeaderCrcOffset] =
        Byte(static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(
                dirtyCrc[pbprotocol::kTransportHeaderCrcOffset]) ^ 0x01U));
    const auto dirtyCrcExtract =
        pbprotocol::ExtractTransportBlockFromInfoBlock(
            std::span<const std::byte>(dirtyCrc));
    REQUIRE_FALSE(dirtyCrcExtract);
    REQUIRE(dirtyCrcExtract.Error().code ==
        pbprotocol::ProtocolErrorCode::CrcMismatch);

    // CRC-repaired length field pointing beyond the information block:
    // the declared block length must be bounds-checked before slicing.
    std::vector<std::byte> dirtyLength = infoBlock;
    dirtyLength[pbprotocol::kTransportPayloadBytesOffset] =
        Byte(0xFF); // 0xFFFF low byte
    const std::uint32_t repairedCrc = pbprotocol::ComputeCrc32c(
        std::span<const std::byte>(dirtyLength).first(
            pbprotocol::kTransportHeaderCrcCoverageBytes));
    for (std::size_t byteIndex = 0; byteIndex < 4; byteIndex++)
    {
        dirtyLength[pbprotocol::kTransportHeaderCrcOffset + byteIndex] =
            Byte(static_cast<std::uint8_t>(
                (repairedCrc >> (byteIndex * 8U)) & 0xFFU));
    }
    const auto dirtyLengthExtract =
        pbprotocol::ExtractTransportBlockFromInfoBlock(
            std::span<const std::byte>(dirtyLength));
    REQUIRE_FALSE(dirtyLengthExtract);
    REQUIRE(dirtyLengthExtract.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::TruncatedInput,
            infoSize});
}

TEST_CASE("Transport serialize buffer and payload contracts",
          "[pbprotocol][transport][serialize]")
{
    const std::vector<std::byte> payload = MakePatternPayload(4);
    const pbprotocol::TransportBlockHeader header = MakeHeader(4);
    const std::size_t exactSize =
        pbprotocol::GetTransportSerializedSize(header);

    std::vector<std::byte> tooSmall(exactSize - 1);
    const auto smallStatus = pbprotocol::SerializeTransportBlock(
        header,
        std::span<const std::byte>(payload),
        std::span<std::byte>(tooSmall));
    REQUIRE_FALSE(smallStatus);
    REQUIRE(smallStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::InvalidRecordSize);

    std::vector<std::byte> tooLarge(exactSize + 1);
    const auto largeStatus = pbprotocol::SerializeTransportBlock(
        header,
        std::span<const std::byte>(payload),
        std::span<std::byte>(tooLarge));
    REQUIRE_FALSE(largeStatus);
    REQUIRE(largeStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::InvalidRecordSize);

    // Declared and actual payload size mismatch.
    std::vector<std::byte> exact(exactSize);
    const auto mismatchStatus = pbprotocol::SerializeTransportBlock(
        header,
        std::span<const std::byte>(payload).first(3),
        std::span<std::byte>(exact));
    REQUIRE_FALSE(mismatchStatus);
    REQUIRE(mismatchStatus.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InvalidRecordSize,
            pbprotocol::kTransportPayloadBytesOffset});

    // Semantically invalid headers never serialize.
    pbprotocol::TransportBlockHeader badType = header;
    badType.blockType = 2;
    const auto badTypeStatus = pbprotocol::SerializeTransportBlock(
        badType,
        std::span<const std::byte>(payload),
        std::span<std::byte>(exact));
    REQUIRE_FALSE(badTypeStatus);
    REQUIRE(badTypeStatus.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InvalidEnumValue,
            pbprotocol::kTransportBlockTypeOffset});

    pbprotocol::TransportBlockHeader badMinor = header;
    badMinor.protocolMinor = 1;
    const auto badMinorStatus = pbprotocol::SerializeTransportBlock(
        badMinor,
        std::span<const std::byte>(payload),
        std::span<std::byte>(exact));
    REQUIRE_FALSE(badMinorStatus);
    REQUIRE(badMinorStatus.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::UnsupportedProtocolMinor,
            pbprotocol::kTransportProtocolMinorOffset});

    pbprotocol::TransportBlockHeader badFlags = header;
    badFlags.flags = 1;
    const auto badFlagsStatus = pbprotocol::SerializeTransportBlock(
        badFlags,
        std::span<const std::byte>(payload),
        std::span<std::byte>(exact));
    REQUIRE_FALSE(badFlagsStatus);
    REQUIRE(badFlagsStatus.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::NonZeroReservedBits,
            pbprotocol::kTransportFlagsOffset});
}

TEST_CASE("ProtocolErrorCode wire values stay append-only",
          "[pbprotocol][transport][enumstability]")
{
    // The diagnostic values are recorded in protocol documentation and
    // telemetry; this pins the values the transport codec relies on.
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::None) == 0);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::TruncatedInput) == 1);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::OutputBufferTooSmall) == 2);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::LengthNarrowing) == 3);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::LengthOverflow) == 4);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::LengthLimitExceeded) == 5);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::NonCanonicalPadding) == 8);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::TrailingBytes) == 12);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::UnsupportedProtocolMinor) == 14);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::InvalidEnumValue) == 15);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::InvalidRecordSize) == 16);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::CrcMismatch) == 40);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::NonZeroReservedBits) == 41);
    REQUIRE(static_cast<int>(
        pbprotocol::ProtocolErrorCode::UnknownSegment) == 48);
}
TEST_CASE("Transport overlap rejection keeps both spans pristine",
          "[pbprotocol][transport][overlap]")
{
    // The serialize contract is fail-closed on overlapping input/output
    // spans (the header is written before the payload is copied). Every
    // intersection shape must fail with OverlappingSpans at offset 0 and
    // leave both spans byte-for-byte untouched.
    SECTION("payload tail overlaps the output head")
    {
        std::vector<std::byte> scratch(64);
        for (std::size_t byteIndex = 0; byteIndex < scratch.size(); byteIndex++)
        {
            scratch[byteIndex] =
                Byte(static_cast<std::uint8_t>(byteIndex & 0xFFU));
        }
        const std::span<std::byte> buffer(scratch);
        // payload starts inside the 36-byte output window.
        const std::span<const std::byte> payload = buffer.subspan(20, 16);
        const std::span<std::byte> output = buffer.first(36);
        const pbprotocol::TransportBlockHeader header = MakeHeader(16);
        const auto status = pbprotocol::SerializeTransportBlock(
            header, payload, output);
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().code ==
            pbprotocol::ProtocolErrorCode::OverlappingSpans);
        REQUIRE(status.Error().offset == 0);
        for (std::size_t byteIndex = 0; byteIndex < scratch.size(); byteIndex++)
        {
            REQUIRE(scratch[byteIndex] ==
                Byte(static_cast<std::uint8_t>(byteIndex & 0xFFU)));
        }
    }

    SECTION("output starts inside the payload")
    {
        std::vector<std::byte> scratch(120);
        const pbprotocol::TransportBlockHeader header = MakeHeader(40);
        const std::span<const std::byte> payload =
            std::span<const std::byte>(scratch).subspan(40, 40);
        const std::span<std::byte> output =
            std::span<std::byte>(scratch).subspan(36, 76);
        const auto status = pbprotocol::SerializeTransportBlock(
            header, payload, output);
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().code ==
            pbprotocol::ProtocolErrorCode::OverlappingSpans);
    }

    SECTION("exact same-base overlap is rejected")
    {
        std::vector<std::byte> scratch(36);
        const std::span<const std::byte> widePayload =
            std::span<const std::byte>(scratch);
        const std::span<std::byte> output = std::span<std::byte>(scratch);
        const pbprotocol::TransportBlockHeader header = MakeHeader(36);
        REQUIRE_FALSE(pbprotocol::SerializeTransportBlock(
            header, widePayload, output));
    }

    SECTION("adjacent (non-overlapping) spans are accepted")
    {
        std::vector<std::byte> scratch(116);
        const std::span<const std::byte> payload =
            std::span<const std::byte>(scratch).subspan(76, 40);
        const std::span<std::byte> output =
            std::span<std::byte>(scratch).first(76);
        const pbprotocol::TransportBlockHeader header = MakeHeader(40);
        REQUIRE(pbprotocol::SerializeTransportBlock(header, payload, output));
    }

    SECTION("frame overlap rejection leaves the info block pristine")
    {
        const std::vector<std::byte> block =
            SerializeOrDie(MakeHeader(8), MakePatternPayload(8));
        std::vector<std::byte> aliasing(72);
        std::copy(block.begin(), block.begin() + 44, aliasing.begin());
        const std::span<const std::byte> aliasBlock =
            std::span<const std::byte>(aliasing).first(44);
        const std::span<std::byte> aliasInfo =
            std::span<std::byte>(aliasing).subspan(8, 56);
        const auto status = pbprotocol::FrameTransportBlockIntoInfoBlock(
            aliasBlock, 56, aliasInfo);
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().code ==
            pbprotocol::ProtocolErrorCode::OverlappingSpans);
        REQUIRE(status.Error().offset == 0);
        // Nothing was written: the first 8 bytes are still the block prefix
        // and the info tail is still the pristine zero fill.
        REQUIRE(std::equal(aliasing.begin(), aliasing.begin() + 8,
            block.begin()));
        for (std::size_t byteIndex = 44; byteIndex < aliasing.size(); byteIndex++)
        {
            REQUIRE(aliasing[byteIndex] == std::byte{0});
        }
    }

    SECTION("OverlappingSpans is appended after UnknownSegment")
    {
        REQUIRE(static_cast<int>(
            pbprotocol::ProtocolErrorCode::UnknownSegment) == 48);
        REQUIRE(static_cast<int>(
            pbprotocol::ProtocolErrorCode::OverlappingSpans) == 49);
    }
}
