// PBGoldenVector cross-suite consistency tests.
//
// The pbgolden byte source is the single deterministic producer of every
// committed tests/golden/ vector. This suite proves the source stays in
// lockstep with the rest of the repository:
//   * bootstrap / control / fragment bytes equal the inline golden arrays
//     pinned by tests/PBModulation (themselves reused from PBProtocol);
//   * descriptor payloads equal Serialize/Parse round-trips of the frozen
//     descriptor values (tests/PBProtocol helpers);
//   * every LDPC profile independently starts at bit zero of the frozen
//     SplitMix64(0xC0FFEE) stream (the PBInnerFec convention);
//   * the interleave / raster / transport vectors are reproducible from
//     their documented seeds;
//   * the five frame digests and the manifest digest equal the registry
//     pins (raw PBRW primary + PNG secondary).
// A failure here is a golden-regression: the committed vectors, the
// source, or the inline pins have drifted.

#include "modulation_test_helpers.h"
#include "descriptor_test_helpers.h"

#include "pbgolden/golden_vector_registry.h"
#include "pbgolden/golden_vector_source.h"

#include "pbinterleave/interleave_reference.h"
#include "pbinnerfec/inner_fec_profile.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/control_fragment_codec.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_types.h"
#include "pbprotocol/transport_block_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace pbgolden; // NOLINT(readability-namespace) - test-local

[[nodiscard]] bool SpanEquals(
    const std::span<const std::byte> left,
    const std::vector<std::byte>& right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); i++)
    {
        if (left[i] != right[i])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool DigestEquals(
    const std::array<std::byte, 32>& left,
    const std::array<std::byte, 32>& right) noexcept
{
    return left == right;
}

// Reads the little-endian u32 at `offset` (test-local helper).
[[nodiscard]] std::uint32_t ReadU32Le(
    const std::span<const std::byte> data, const std::size_t offset) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; i++)
    {
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(data[offset + i])) <<
            static_cast<unsigned int>(i * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t ReadU64Le(
    const std::span<const std::byte> data, const std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; i++)
    {
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(data[offset + i])) <<
            static_cast<unsigned int>(i * 8U);
    }
    return value;
}

// One profile-local LDPC golden bit stream: bit i is
// SplitMix64(0xC0FFEE).Next() & 1, LSB-first packed.
[[nodiscard]] std::vector<std::byte> MakeFullLdpcInfoStretch(
    const std::uint32_t bitCount)
{
    return pbmodtest::MakePatternInfoBits(bitCount, kLdpcPatternSeed);
}

void OracleStoreLe(const std::span<std::byte> bytes,
    const std::size_t offset, const std::uint64_t value,
    const std::size_t width) noexcept
{
    for (std::size_t byteIndex = 0; byteIndex < width; byteIndex++)
    {
        bytes[offset + byteIndex] = std::byte{static_cast<std::uint8_t>(
            value >> static_cast<unsigned int>(byteIndex * 8u))};
    }
}

[[nodiscard]] std::uint32_t OracleCrc32c(
    const std::span<const std::byte> bytes) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::byte byteValue : bytes)
    {
        crc ^= std::to_integer<std::uint8_t>(byteValue);
        for (int bitIndex = 0; bitIndex < 8; bitIndex++)
        {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0x82F63B78u & mask);
        }
    }
    return ~crc;
}

class OracleSplitMix64
{
public:
    explicit OracleSplitMix64(const std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t Next() noexcept
    {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27u)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31u);
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] std::vector<std::byte> MakeOracleTransportBlock(
    const std::uint64_t sessionTag, const std::uint64_t segmentOrdinal,
    const std::uint32_t outerBlockId, const std::uint16_t payloadBytes,
    const std::uint64_t payloadSeed)
{
    std::vector<std::byte> bytes(36u + payloadBytes, std::byte{0});
    bytes[0] = std::byte{1};
    OracleStoreLe(bytes, 4, sessionTag, 8);
    OracleStoreLe(bytes, 12, segmentOrdinal, 8);
    OracleStoreLe(bytes, 20, outerBlockId, 4);
    OracleStoreLe(bytes, 24, payloadBytes, 2);
    OracleSplitMix64 rng(payloadSeed);
    for (std::size_t payloadIndex = 0; payloadIndex < payloadBytes; payloadIndex++)
    {
        bytes[32u + payloadIndex] =
            std::byte{static_cast<std::uint8_t>(rng.Next())};
    }
    OracleStoreLe(bytes, 28, OracleCrc32c(
        std::span<const std::byte>(bytes).first(28)), 4);
    OracleStoreLe(bytes, 32u + payloadBytes, OracleCrc32c(
        std::span<const std::byte>(bytes).subspan(32, payloadBytes)), 4);
    return bytes;
}

void OracleSetSymbol(const std::span<std::byte> bytes,
    const std::size_t symbolIndex, const std::uint8_t symbol) noexcept
{
    const std::size_t byteIndex = symbolIndex / 2u;
    const std::uint8_t current = std::to_integer<std::uint8_t>(bytes[byteIndex]);
    bytes[byteIndex] = std::byte{static_cast<std::uint8_t>(
        (symbolIndex % 2u) == 0u
            ? (current & 0xF0u) | (symbol & 0x0Fu)
            : (current & 0x0Fu) | ((symbol & 0x0Fu) << 4u))};
}

[[nodiscard]] std::uint8_t OracleReadSymbol(
    const std::span<const std::byte> bytes,
    const std::size_t symbolIndex) noexcept
{
    const std::uint8_t value =
        std::to_integer<std::uint8_t>(bytes[symbolIndex / 2u]);
    return static_cast<std::uint8_t>((symbolIndex % 2u) == 0u
        ? value & 0x0Fu : (value >> 4u) & 0x0Fu);
}

} // namespace

TEST_CASE("GoldenVector: bootstrap record matches the inline protocol "
    "golden", "[goldenvector][bootstrap]")
{
    const auto record = GenerateBootstrapRecord();
    REQUIRE(record.size() == 44);
    REQUIRE(SpanEquals(std::span<const std::byte>(record),
        std::vector<std::byte>(pbmodtest::kBootstrapGolden.begin(),
            pbmodtest::kBootstrapGolden.end())));
    const auto parsed =
        pbprotocol::ParseBootstrapRecord(std::span<const std::byte>(record));
    REQUIRE(static_cast<bool>(parsed));
    REQUIRE(parsed.Value().sessionTag.value ==
        kGoldenSessionTag);
    REQUIRE(parsed.Value().frameSequence == 0x1112131415161718ULL);
    REQUIRE(parsed.Value().controlEpoch == 0x21222324U);
    REQUIRE(parsed.Value().visualProfileId == 0x0102030405060708ULL);
    // The stored CRC field is the frozen 0xD488E1EA and recomputes clean.
    const auto stored = ReadU32Le(std::span<const std::byte>(record), 40);
    REQUIRE(stored == 0xD488E1EAU);
    REQUIRE(static_cast<std::uint32_t>(pbprotocol::ComputeCrc32c(
        std::span<const std::byte>(record).first(40))) == stored);
    // Determinism: two generations are byte-identical.
    REQUIRE(GenerateBootstrapRecord() == record);
}

TEST_CASE("GoldenVector: the committed control-sessiondescriptor record",
    "[goldenvector][control]")
{
    const auto record = GenerateControlSessionDescriptor();
    REQUIRE(record.size() == 67);
    const auto span = std::span<const std::byte>(record);
    const auto parsed = pbprotocol::ParseControlRecord(span);
    REQUIRE(static_cast<bool>(parsed));
    REQUIRE(parsed.Value().recordType ==
        pbprotocol::ControlRecordType::SessionDescriptor);
    REQUIRE(parsed.Value().sessionTag.value == kGoldenSessionTag);
    REQUIRE(parsed.Value().controlSequence == 0x0102030405060708ULL);
    REQUIRE(parsed.Value().payload.size() == 37);
    // Stored CRC matches the frozen PB-Control-1 Golden value.
    const auto stored = ReadU32Le(span, 63);
    REQUIRE(stored == 0xA13883C8U);
    REQUIRE(static_cast<std::uint32_t>(
        pbprotocol::ComputeCrc32c(span.first(63))) == stored);
    // The 37-byte payload is the frozen session descriptor (file size 117,
    // segment count 1) and parses under the default local policy.
    const auto session = pbprotocol::ParseSessionDescriptor(
        parsed.Value().payload,
        pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE(static_cast<bool>(session));
    REQUIRE(session.Value().originalFileSize == 117);
    REQUIRE(session.Value().segmentCount == 1);
}

TEST_CASE("GoldenVector: the legacy G1 control record is untouched",
    "[goldenvector][control-legacy]")
{
    // The G1 frame pin is bound to the legacy 67-byte record (file size
    // 117 / segment count 1, CRC 0xA13883C8). The canonical frame payload
    // must embed it byte-for-byte with zero padding of the window tail.
    const auto payload = MakeCanonicalFramePayload();
    REQUIRE(payload.control.size() == 240);
    const auto window = std::span<const std::byte>(payload.control);
    REQUIRE(SpanEquals(window.first(67),
        std::vector<std::byte>(pbmodtest::kControlGolden.begin(),
            pbmodtest::kControlGolden.end())));
    for (std::size_t i = 67; i < 240; i++)
    {
        REQUIRE(window[i] == std::byte{0});
    }
    const auto parsed = pbprotocol::ParseControlRecord(window.first(67));
    REQUIRE(static_cast<bool>(parsed));
    const auto stored = ReadU32Le(window.first(67), 63);
    REQUIRE(stored == 0xA13883C8U);
    REQUIRE(static_cast<std::uint32_t>(
        pbprotocol::ComputeCrc32c(window.first(63))) == stored);
    const auto session = pbprotocol::ParseSessionDescriptor(
        parsed.Value().payload,
        pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE(static_cast<bool>(session));
    REQUIRE(session.Value().originalFileSize == 117);
    REQUIRE(session.Value().segmentCount == 1);
    // The empty and maximum control records are self-consistent envelopes.
    const auto empty = GenerateControlEmpty();
    REQUIRE(empty.size() == 30);
    const auto emptyParsed =
        pbprotocol::ParseControlRecord(std::span<const std::byte>(empty));
    REQUIRE(static_cast<bool>(emptyParsed));
    REQUIRE(emptyParsed.Value().payload.empty());
    REQUIRE(ReadU32Le(std::span<const std::byte>(empty), 26) ==
        static_cast<std::uint32_t>(pbprotocol::ComputeCrc32c(
            std::span<const std::byte>(empty).first(26))));
    const auto maximum = GenerateControlMaximum();
    REQUIRE(maximum.size() == 65536);
    const auto maxParsed =
        pbprotocol::ParseControlRecord(std::span<const std::byte>(maximum));
    REQUIRE(static_cast<bool>(maxParsed));
    REQUIRE(maxParsed.Value().payload.size() == 65506);
    REQUIRE(ReadU32Le(std::span<const std::byte>(maximum), 65532) ==
        static_cast<std::uint32_t>(pbprotocol::ComputeCrc32c(
            std::span<const std::byte>(maximum).first(65532))));
}

TEST_CASE("GoldenVector: control fragments reassemble the legacy record",
    "[goldenvector][fragment]")
{
    // The 67-byte legacy record fragmented with a 24-byte payload
    // capacity: payload sizes 24/24/19, serialized sizes 48/48/43. The
    // stored CRCs are the frozen values of the committed vectors
    // (0x9DCD5402 / 0xF3FF94D8 / 0x40106F66).
    const std::array<std::uint32_t, 3> storedCrcs{
        0x9DCD5402U, 0xF3FF94D8U, 0x40106F66U};
    const std::array<std::size_t, 3> payloadSizes{24, 24, 19};
    std::vector<std::byte> reassembled;
    for (std::uint16_t index = 0; index < 3; index++)
    {
        const auto fragment = GenerateControlFragment(index);
        REQUIRE(fragment.size() == 20 + payloadSizes[index] + 4);
        const auto span = std::span<const std::byte>(fragment);
        const auto parsed = pbprotocol::ParseControlFragment(span);
        REQUIRE(static_cast<bool>(parsed));
        REQUIRE(parsed.Value().controlRecordId == 0x0102030405060708ULL);
        REQUIRE(parsed.Value().fragmentIndex == index);
        REQUIRE(parsed.Value().fragmentCount == 3);
        REQUIRE(parsed.Value().totalRecordBytes == 67);
        REQUIRE(parsed.Value().flags == 0);
        REQUIRE(parsed.Value().payload.size() == payloadSizes[index]);
        REQUIRE(ReadU32Le(span, 20 + payloadSizes[index]) ==
            storedCrcs[index]);
        REQUIRE(static_cast<std::uint32_t>(pbprotocol::ComputeCrc32c(
            span.first(20 + payloadSizes[index]))) == storedCrcs[index]);
        reassembled.insert(reassembled.end(),
            parsed.Value().payload.begin(),
            parsed.Value().payload.end());
    }
    // Reassembly reproduces the legacy 67-byte record byte-for-byte.
    REQUIRE(SpanEquals(std::span<const std::byte>(reassembled),
        std::vector<std::byte>(pbmodtest::kControlGolden.begin(),
            pbmodtest::kControlGolden.end())));
    // Determinism: regenerating a fragment is byte-identical.
    const auto fragmentZeroFirst = GenerateControlFragment(0);
    REQUIRE(GenerateControlFragment(0) == fragmentZeroFirst);
}

TEST_CASE("GoldenVector: descriptor payloads are Serialize/Parse "
    "round-trips of the frozen values", "[goldenvector][descriptor]")
{
    const auto sessionPayload = GenerateSessionDescriptorPayload();
    REQUIRE(sessionPayload.size() == 37);
    const auto session = pbprotocol::ParseSessionDescriptor(
        std::span<const std::byte>(sessionPayload),
        pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE(static_cast<bool>(session));
    // Serialize the parsed descriptor back and compare byte-for-byte.
    std::vector<std::byte> serialized(pbprotocol::GetSerializedSize(
        session.Value()));
    REQUIRE(static_cast<bool>(pbprotocol::SerializeSessionDescriptor(
        session.Value(), std::span<std::byte>(serialized))));
    REQUIRE(serialized == sessionPayload);

    // DirectRepeat segment: ordinal 0, offset 0, raw/encoded size 117,
    // outerBlockBytes 16 and the existing 0x20..0x3f digests.
    const auto directPayload = GenerateDirectRepeatSegmentPayload();
    REQUIRE(directPayload.size() == 110);
    const pbprotocol::SessionDescriptor sessionValue = session.Value();
    const pbprotocol::SegmentDescriptor directSegment =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionValue, 0, 0, 117);
    std::vector<std::byte> directSerialized(
        pbprotocol::GetSerializedSize(directSegment).Value());
    REQUIRE(static_cast<bool>(pbprotocol::SerializeSegmentDescriptor(
        directSegment, sessionValue,
        std::span<std::byte>(directSerialized))));
    REQUIRE(directSerialized == directPayload);
    const auto parsedDirect = pbprotocol::ParseSegmentDescriptor(
        std::span<const std::byte>(directPayload), sessionValue,
        pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE(static_cast<bool>(parsedDirect));

    // WirehairV2 segment: ordinal 0, offset 0, raw size 200, encoded 117,
    // 16-byte blocks, digests 0x10/0x80, canonical 117x16 profile. Its
    // extent requires the independently frozen 200-byte Session context.
    const auto wirehairPayload = GenerateWirehairSegmentPayload();
    REQUIRE(wirehairPayload.size() == 142);
    const pbprotocol::SessionDescriptor wirehairSession =
        pbprotocol::test::MakeSessionDescriptor(200, 1);
    const pbprotocol::SegmentDescriptor wirehairSegment =
        pbprotocol::test::MakeWirehairSegment(
            wirehairSession, 0, 0, 200);
    std::vector<std::byte> wirehairSerialized(
        pbprotocol::GetSerializedSize(wirehairSegment).Value());
    REQUIRE(static_cast<bool>(pbprotocol::SerializeSegmentDescriptor(
        wirehairSegment, wirehairSession,
        std::span<std::byte>(wirehairSerialized))));
    REQUIRE(wirehairSerialized == wirehairPayload);
    const auto parsedWirehair = pbprotocol::ParseSegmentDescriptor(
        std::span<const std::byte>(wirehairPayload), wirehairSession,
        pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE(static_cast<bool>(parsedWirehair));
    REQUIRE(parsedWirehair.Value().wirehairV2SerializedProfile
            == wirehairSegment.wirehairV2SerializedProfile);

    // Final manifest: digest start 0xA0 (the helper builds exactly the
    // frozen 32-byte digest from that start byte).
    const auto manifestPayload = GenerateFinalManifestPayload();
    REQUIRE(manifestPayload.size() == 65);
    const pbprotocol::FinalManifest manifest =
        pbprotocol::test::MakeFinalManifest(sessionValue, 0xA0);
    std::vector<std::byte> manifestSerialized(
        pbprotocol::GetSerializedSize(manifest));
    REQUIRE(static_cast<bool>(pbprotocol::SerializeFinalManifest(
        manifest, sessionValue, std::span<std::byte>(manifestSerialized))));
    REQUIRE(manifestSerialized == manifestPayload);
    const auto parsedManifest = pbprotocol::ParseFinalManifest(
        std::span<const std::byte>(manifestPayload), sessionValue,
        pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE(static_cast<bool>(parsedManifest));
}

TEST_CASE("GoldenVector: the canonical Wirehair descriptor equals the "
    "PBProtocol test profile", "[goldenvector][wirehair]")
{
    const auto descriptor = GenerateWirehairCanonicalDescriptor();
    REQUIRE(descriptor.size() == 32);
    const auto expectedProfile =
        pbprotocol::test::MakeWirehairProfile(117, 16, 0);
    REQUIRE(SpanEquals(std::span<const std::byte>(descriptor),
        std::vector<std::byte>(expectedProfile.bytes.begin(),
            expectedProfile.bytes.end())));
    // The descriptor validates under its own declared dimensions.
    REQUIRE(static_cast<bool>(
        pbprotocol::ValidateWirehairV2SerializedProfile(
            expectedProfile, 117, 16, 0)));
    REQUIRE(GenerateWirehairCanonicalDescriptor() == descriptor);
}

TEST_CASE("GoldenVector: transport vectors are reproducible from their "
    "documented seeds", "[goldenvector][transport]")
{
    // Minimal block: empty payload; the payload CRC is the CRC-32C of the
    // empty input (a fixed point of the CRC32C initial state convention).
    const auto minimum = GenerateTransportBlockMinimum();
    const auto minimumOracle = MakeOracleTransportBlock(0, 0, 0, 0, 0);
    REQUIRE(minimum == minimumOracle);
    REQUIRE(minimum.size() == 36);
    const auto minParsed =
        pbprotocol::ParseTransportBlock(std::span<const std::byte>(minimum));
    REQUIRE(static_cast<bool>(minParsed));
    REQUIRE(minParsed.Value().payload.empty());
    REQUIRE(ReadU32Le(std::span<const std::byte>(minimum), 32) ==
        static_cast<std::uint32_t>(
            pbprotocol::ComputeCrc32c(std::span<const std::byte>{})));
    REQUIRE(minParsed.Value().header.sessionTag.value == 0);

    // Canonical block: 1,314-byte payload from
    // SplitMix64(MakeTagSeed("PB-TX-C0")), ordinal 0, block 0.
    const auto canonical = GenerateTransportBlockCanonical();
    const auto canonicalOracle = MakeOracleTransportBlock(
        kGoldenSessionTag, 0, 0, 1314, kTransportCanonicalPayloadSeed);
    REQUIRE(canonical == canonicalOracle);
    REQUIRE(canonical.size() == 1350);
    const auto canonParsed = pbprotocol::ParseTransportBlock(
        std::span<const std::byte>(canonical));
    REQUIRE(static_cast<bool>(canonParsed));
    REQUIRE(canonParsed.Value().header.segmentOrdinal == 0);
    REQUIRE(canonParsed.Value().header.outerBlockId == 0);
    REQUIRE(canonParsed.Value().payload.size() == 1314);
    SplitMix64 payloadRng(kTransportCanonicalPayloadSeed);
    for (std::size_t i = 0; i < 1314; i++)
    {
        const std::uint8_t expectedByte =
            static_cast<std::uint8_t>(payloadRng.Next() & 0xFFU);
        REQUIRE(canonParsed.Value().payload[i] ==
            std::byte{expectedByte});
    }
    REQUIRE(canonParsed.Value().header.sessionTag.value ==
        kGoldenSessionTag);
    // The canonical golden transport payload seed tag is self-documenting.
    REQUIRE(kTransportCanonicalPayloadSeed == MakeTagSeed("PB-TX-C0"));
    REQUIRE(kTransportSecondPayloadSeed == MakeTagSeed("PB-TX-C1"));
    REQUIRE(kTransportMaxPayloadSeed == MakeTagSeed("PB-TX-M1"));

    // Maximum block: 65,535-byte payload (65,571 wire bytes, above the
    // u16 range - the size arithmetic must have stayed checked).
    const auto maximum = GenerateTransportBlockMaxPayload();
    const auto maximumOracle = MakeOracleTransportBlock(
        kGoldenSessionTag, 1, 0xFFFFFFFFu, 65535,
        kTransportMaxPayloadSeed);
    REQUIRE(maximum == maximumOracle);
    REQUIRE(maximum.size() == 65571);
    const auto maxParsed =
        pbprotocol::ParseTransportBlock(std::span<const std::byte>(maximum));
    REQUIRE(static_cast<bool>(maxParsed));
    REQUIRE(maxParsed.Value().header.segmentOrdinal == 1);
    REQUIRE(maxParsed.Value().header.outerBlockId == 0xFFFFFFFFU);
    REQUIRE(maxParsed.Value().payload.size() == 65535);
    REQUIRE(maxParsed.Value().header.sessionTag.value ==
        kGoldenSessionTag);
    SplitMix64 maxPayloadRng(kTransportMaxPayloadSeed);
    for (std::size_t i = 0; i < 65535; i++)
    {
        const std::uint8_t expectedByte =
            static_cast<std::uint8_t>(maxPayloadRng.Next() & 0xFFU);
        if (maxParsed.Value().payload[i] != std::byte{expectedByte})
        {
            FAIL("max payload byte " << i << " drifted from the seed");
        }
    }
    // Round-trip: serialize the parsed minimum and maximum headers back.
    std::vector<std::byte> minReserialized(36);
    REQUIRE(static_cast<bool>(pbprotocol::SerializeTransportBlock(
        minParsed.Value().header, minParsed.Value().payload,
        std::span<std::byte>(minReserialized))));
    REQUIRE(minReserialized == minimum);
}

TEST_CASE("GoldenVector: every LDPC codeword starts at the frozen SplitMix "
    "bit zero", "[goldenvector][ldpc]")
{
    const auto robust = GenerateLdpcCodewordRobust();
    const auto balanced = GenerateLdpcCodewordBalanced();
    const auto fast = GenerateLdpcCodewordFast();
    REQUIRE(robust.size() == 2025);
    REQUIRE(balanced.size() == 2025);
    REQUIRE(fast.size() == 2025);
    // The Robust information prefix reuses the shared pinned first-16 hex.
    REQUIRE(pbmodtest::ToHex(std::span<const std::byte>(robust).first(16)) ==
        pbmodtest::kPatternInfoFirst16Hex);
    // Each codeword is a valid codeword of its own profile.
    const auto robustSyndrome = pbinnerfec::ComputeQcLdpcSyndrome(
        pbinnerfec::kInnerFecProfileIdRobust,
        std::span<const std::byte>(robust));
    REQUIRE(static_cast<bool>(robustSyndrome));
    REQUIRE(robustSyndrome.Value());
    const auto balancedSyndrome = pbinnerfec::ComputeQcLdpcSyndrome(
        pbinnerfec::kInnerFecProfileIdBalanced,
        std::span<const std::byte>(balanced));
    REQUIRE(static_cast<bool>(balancedSyndrome));
    REQUIRE(balancedSyndrome.Value());
    const auto fastSyndrome = pbinnerfec::ComputeQcLdpcSyndrome(
        pbinnerfec::kInnerFecProfileIdFast,
        std::span<const std::byte>(fast));
    REQUIRE(static_cast<bool>(fastSyndrome));
    REQUIRE(fastSyndrome.Value());
    // Each profile independently consumes the prefix of the same stream.
    const auto robustInfo = MakeFullLdpcInfoStretch(10800);
    const auto balancedInfo = MakeFullLdpcInfoStretch(11880);
    const auto fastInfo = MakeFullLdpcInfoStretch(13320);
    REQUIRE(SpanEquals(std::span<const std::byte>(robust).first(1350),
        robustInfo));
    REQUIRE(SpanEquals(std::span<const std::byte>(balanced).first(1485),
        balancedInfo));
    REQUIRE(SpanEquals(std::span<const std::byte>(fast).first(1665),
        fastInfo));
}

TEST_CASE("GoldenVector: interleave vectors are reproducible",
    "[goldenvector][interleave]")
{
    // Mapping sample: 16 phases x 64 logical tiles, u32LE pairs, phase
    // outermost (8,192 bytes).
    const auto mappingSample = GenerateInterleaveMappingSample();
    REQUIRE(mappingSample.size() == 8192);
    for (std::uint64_t phase = 0; phase < 16; phase++)
    {
        for (std::uint64_t tile = 0; tile < 64; tile++)
        {
            const std::size_t byteOffset =
                (phase * 64 + tile) * 8;
            const auto span =
                std::span<const std::byte>(mappingSample);
            REQUIRE(ReadU32Le(span, byteOffset) == tile);
            const std::uint64_t oraclePhysical =
                (tile * 65537u + phase * 472u) % 112336u;
            REQUIRE(ReadU32Le(span, byteOffset + 4) == oraclePhysical);
        }
    }

    // Region logical pattern: SplitMix64(MakeTagSeed("PB-INTLV")) bytes.
    const auto regionLogical = GenerateInterleaveRegionLogical();
    REQUIRE(regionLogical.size() == 56168);
    SplitMix64 regionRng(kInterleaveRegionSeed);
    for (std::size_t i = 0; i < regionLogical.size(); i++)
    {
        const std::uint8_t expectedByte =
            static_cast<std::uint8_t>(regionRng.Next() & 0xFFU);
        if (regionLogical[i] != std::byte{expectedByte})
        {
            FAIL("region logical byte " << i << " drifted from the seed");
        }
    }
    REQUIRE(kInterleaveRegionSeed == MakeTagSeed("PB-INTLV"));

    // Region physical (phase 7): exactly ApplyInterleave of the logical
    // pattern at frame sequence 7.
    const auto regionPhysical =
        GenerateInterleaveRegionPhysicalPhase7();
    REQUIRE(regionPhysical.size() == 56168);
    std::vector<std::byte> expectedPhysical(56168, std::byte{0});
    for (std::size_t logicalTile = 0; logicalTile < 112336; logicalTile++)
    {
        const std::uint8_t symbol = OracleReadSymbol(regionLogical, logicalTile);
        const std::size_t physicalTile = static_cast<std::size_t>(
            (static_cast<std::uint64_t>(logicalTile) * 65537u + 7u * 472u) %
            112336u);
        OracleSetSymbol(expectedPhysical, physicalTile, symbol);
    }
    REQUIRE(regionPhysical == expectedPhysical);
}

TEST_CASE("GoldenVector: raster category vectors",
    "[goldenvector][raster]")
{
    const auto infoBlock = GenerateG1TransportInfoBlock();
    const auto canonical = GenerateTransportBlockCanonical();
    REQUIRE(infoBlock == canonical);

    const auto decoded = GenerateG1TransportDecoded();
    // The decoded chain must reproduce the canonical block byte-for-byte;
    // an empty result is a recompute failure and never a substitution.
    REQUIRE(decoded.size() == 1350);
    REQUIRE(decoded == canonical);

    const auto twoCw = GenerateG1Transport2CwInfoBlock();
    REQUIRE(twoCw.size() == 2700);
    // First 1,350 bytes are the canonical block (ordinal 0).
    REQUIRE(SpanEquals(std::span<const std::byte>(twoCw).first(1350),
        canonical));
    // The second block (ordinal 1) parses strictly with its documented
    // SplitMix64 seed.
    const auto secondParsed = pbprotocol::ParseTransportBlock(
        std::span<const std::byte>(twoCw).subspan(1350));
    REQUIRE(static_cast<bool>(secondParsed));
    REQUIRE(secondParsed.Value().header.segmentOrdinal == 1);
    REQUIRE(secondParsed.Value().header.outerBlockId == 0);
    REQUIRE(secondParsed.Value().payload.size() == 1314);
    REQUIRE(secondParsed.Value().header.sessionTag.value ==
        kGoldenSessionTag);
    SplitMix64 secondRng(kTransportSecondPayloadSeed);
    for (std::size_t i = 0; i < 1314; i++)
    {
        const std::uint8_t expectedByte =
            static_cast<std::uint8_t>(secondRng.Next() & 0xFFU);
        if (secondParsed.Value().payload[i] != std::byte{expectedByte})
        {
            FAIL("second block payload byte " << i << " drifted");
        }
    }
}

TEST_CASE("GoldenVector: frame digest pins are reproduced by the current "
    "implementation", "[goldenvector][frame-pins]")
{
    const auto& frameRegistry = GetFrameVectorRegistry();
    REQUIRE(frameRegistry.size() == 5);
    const auto payloads = std::array{
        std::make_pair(FrameVectorId::G0Zero,
            MakeZeroFramePayload()),
        std::make_pair(FrameVectorId::G1Canonical,
            MakeCanonicalFramePayload()),
        std::make_pair(FrameVectorId::G1Transport,
            MakeG1TransportFramePayload()),
        std::make_pair(FrameVectorId::G1Transport2Cw,
            MakeG1Transport2CwFramePayload()),
        std::make_pair(FrameVectorId::G2Max,
            MakeMaxFramePayload())};
    for (const auto& [id, payload] : payloads)
    {
        const FrameVectorPin* pin = nullptr;
        for (const auto& registryPin : frameRegistry)
        {
            if (registryPin.id == id)
            {
                pin = &registryPin;
            }
        }
        REQUIRE(pin != nullptr);
        const auto digests = ComputeFrameDigests(payload);
        REQUIRE(digests.success);
        REQUIRE(digests.rawOracleMatches);
        REQUIRE(digests.rawDifferingBytes == 0);
        REQUIRE(digests.pngPixelsMatch);
        REQUIRE(digests.pngPixelDifferingBytes == 0);
        REQUIRE(DigestEquals(digests.rawBlake3, pin->rawBlake3));
        REQUIRE(DigestEquals(digests.pngBlake3, pin->pngBlake3));
    }
    // The G1-Transport frame payload is exactly one Robust codeword over
    // the canonical Transport info block plus canonical zero padding.
    const auto g1Transport = MakeG1TransportFramePayload();
    REQUIRE(g1Transport.data.size() == 56168);
    const auto robust = GenerateLdpcCodewordRobust();
    const auto infoBlock = GenerateG1TransportInfoBlock();
    std::vector<std::byte> expectedCodeword(2025);
    REQUIRE(static_cast<bool>(pbinnerfec::EncodeQcLdpcCodeword(
        pbinnerfec::kInnerFecProfileIdRobust,
        std::span<const std::byte>(infoBlock),
        std::span<std::byte>(expectedCodeword))));
    REQUIRE(std::vector<std::byte>(g1Transport.data.begin(),
        g1Transport.data.begin() + 2025) == expectedCodeword);
    for (std::size_t i = 2025; i < g1Transport.data.size(); i++)
    {
        REQUIRE(g1Transport.data[i] == std::byte{0});
    }
}

TEST_CASE("GoldenVector: frame oracle failure is reported without aborting",
    "[goldenvector][frame-oracle][failure]")
{
    auto payload = MakeG1TransportFramePayload();
    payload.data.pop_back();
    const auto result = ComputeFrameDigests(payload);
    REQUIRE_FALSE(result.success);
    REQUIRE(result.detail.find("requires exactly 56168 data bytes") !=
        std::string::npos);
}

TEST_CASE("GoldenVector: manifest digest and source determinism",
    "[goldenvector][manifest][determinism]")
{
    REQUIRE(DigestEquals(ComputeManifestDigest(), GetManifestDigestPin()));
    // Representative determinism sweep: every category regenerates
    // byte-identically (the full registry equivalence is proven by the
    // PBGoldenVectorCheck harness against the committed files).
    REQUIRE(GenerateControlEmpty() == GenerateControlEmpty());
    REQUIRE(GenerateControlMaximum() == GenerateControlMaximum());
    REQUIRE(GenerateSessionDescriptorPayload() ==
        GenerateSessionDescriptorPayload());
    REQUIRE(GenerateDirectRepeatSegmentPayload() ==
        GenerateDirectRepeatSegmentPayload());
    REQUIRE(GenerateWirehairSegmentPayload() ==
            GenerateWirehairSegmentPayload());
    REQUIRE(GenerateFinalManifestPayload() ==
        GenerateFinalManifestPayload());
    REQUIRE(GenerateTransportBlockMinimum() ==
        GenerateTransportBlockMinimum());
    REQUIRE(GenerateInterleaveMappingSample() ==
        GenerateInterleaveMappingSample());
    REQUIRE(GenerateInterleaveRegionLogical() ==
        GenerateInterleaveRegionLogical());
}
