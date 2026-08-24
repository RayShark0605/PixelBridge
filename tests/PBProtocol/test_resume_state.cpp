#include "descriptor_test_helpers.h"

#include "pbprotocol/resume_state.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using pbprotocol::kResumeRecordEnvelopeBytes;
using pbprotocol::ParseResumeRecord;
using pbprotocol::ProtocolErrorCode;
using pbprotocol::ReceiverResourcePolicy;
using pbprotocol::SerializeResumeRecord;
using pbprotocol::ValidateResumeStateBudget;

[[nodiscard]] std::vector<std::byte> MakePayload(
    const std::size_t byteCount,
    const std::uint8_t base)
{
    std::vector<std::byte> payload(byteCount);
    for (std::size_t byteIndex = 0; byteIndex < byteCount; byteIndex++)
    {
        payload[byteIndex] =
            static_cast<std::byte>(base + byteIndex % 26U);
    }
    return payload;
}

[[nodiscard]] std::vector<std::byte> SerializeRecord(
    const std::span<const std::byte> payload)
{
    std::vector<std::byte> record(kResumeRecordEnvelopeBytes + payload.size());
    const pbprotocol::ProtocolStatus status = SerializeResumeRecord(
        payload,
        std::span<std::byte>(record));
    REQUIRE(status.HasValue());
    return record;
}

[[nodiscard]] ReceiverResourcePolicy MakeResumePolicy(
    const std::uint64_t resumeBytes)
{
    ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    resourcePolicy.maxResumeBytes = resumeBytes;
    return resourcePolicy;
}

// Writes a little-endian u64 at the given offset for mutation tests.
void WriteUint64LittleEndian(
    std::span<std::byte> record,
    const std::size_t offset,
    const std::uint64_t value)
{
    for (std::size_t byteIndex = 0; byteIndex < sizeof(value); byteIndex++)
    {
        record[offset + byteIndex] = static_cast<std::byte>(
            (value >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU);
    }
}

} // namespace

TEST_CASE("Resume records round-trip including the empty payload",
          "[pbprotocol][resume]")
{
    const ReceiverResourcePolicy resourcePolicy = MakeResumePolicy(1024);

    const std::span<const std::byte> emptyPayload{};
    const std::vector<std::byte> emptyRecord = SerializeRecord(emptyPayload);
    REQUIRE(emptyRecord.size() == kResumeRecordEnvelopeBytes);
    const auto emptyParseResult = ParseResumeRecord(
        std::span<const std::byte>(emptyRecord), resourcePolicy);
    REQUIRE(emptyParseResult.HasValue());
    REQUIRE(emptyParseResult.Value().empty());

    const std::vector<std::byte> payload = MakePayload(32, 0x61);
    const std::vector<std::byte> record = SerializeRecord(payload);
    REQUIRE(record.size() == kResumeRecordEnvelopeBytes + payload.size());
    const auto parseResult = ParseResumeRecord(
        std::span<const std::byte>(record), resourcePolicy);
    REQUIRE(parseResult.HasValue());
    REQUIRE(parseResult.Value() == payload);

    // Extra output capacity is left untouched.
    const std::vector<std::byte> payload2 = MakePayload(4, 0x62);
    std::vector<std::byte> oversizedOutput(kResumeRecordEnvelopeBytes + 16);
    for (auto& recordByte : oversizedOutput)
    {
        recordByte = std::byte{0xEE};
    }
    REQUIRE(SerializeResumeRecord(
        std::span<const std::byte>(payload2),
        std::span<std::byte>(oversizedOutput)));
    const std::size_t writtenBytes =
        kResumeRecordEnvelopeBytes + payload2.size();
    for (std::size_t byteIndex = writtenBytes;
         byteIndex < oversizedOutput.size();
         byteIndex++)
    {
        REQUIRE(oversizedOutput[byteIndex] == std::byte{0xEE});
    }

    // A too-small output buffer fails without writing.
    std::vector<std::byte> smallOutput(writtenBytes - 1);
    const pbprotocol::ProtocolStatus smallStatus = SerializeResumeRecord(
        std::span<const std::byte>(payload2),
        std::span<std::byte>(smallOutput));
    REQUIRE_FALSE(smallStatus);
    REQUIRE(smallStatus.Error().code
        == ProtocolErrorCode::OutputBufferTooSmall);
}

TEST_CASE("Resume record truncation fails at every boundary",
          "[pbprotocol][resume]")
{
    const ReceiverResourcePolicy resourcePolicy = MakeResumePolicy(1024);
    const std::vector<std::byte> payload = MakePayload(5, 0x63);
    const std::vector<std::byte> record = SerializeRecord(payload);

    for (std::size_t prefixLength = 0; prefixLength < record.size();
         prefixLength++)
    {
        const auto truncatedResult = ParseResumeRecord(
            std::span<const std::byte>(record.data(), prefixLength),
            resourcePolicy);
        REQUIRE_FALSE(truncatedResult.HasValue());
        REQUIRE(truncatedResult.Error().code
            == ProtocolErrorCode::TruncatedInput);
    }
}

TEST_CASE("Resume record header violations fail with exact codes",
          "[pbprotocol][resume]")
{
    const ReceiverResourcePolicy resourcePolicy = MakeResumePolicy(1024);
    const std::vector<std::byte> payload = MakePayload(5, 0x64);

    std::vector<std::byte> badMagicRecord = SerializeRecord(payload);
    badMagicRecord[0] ^= std::byte{0xFF};
    auto magicResult = ParseResumeRecord(
        std::span<const std::byte>(badMagicRecord), resourcePolicy);
    REQUIRE_FALSE(magicResult.HasValue());
    REQUIRE(magicResult.Error().code == ProtocolErrorCode::InvalidMagic);

    std::vector<std::byte> badVersionRecord = SerializeRecord(payload);
    badVersionRecord[4] = std::byte{2};
    auto versionResult = ParseResumeRecord(
        std::span<const std::byte>(badVersionRecord), resourcePolicy);
    REQUIRE_FALSE(versionResult.HasValue());
    REQUIRE(versionResult.Error().code == ProtocolErrorCode::InvalidEnumValue);

    std::vector<std::byte> badReservedRecord = SerializeRecord(payload);
    badReservedRecord[5] = std::byte{1};
    auto reservedResult = ParseResumeRecord(
        std::span<const std::byte>(badReservedRecord), resourcePolicy);
    REQUIRE_FALSE(reservedResult.HasValue());
    REQUIRE(reservedResult.Error().code
        == ProtocolErrorCode::NonZeroReservedByte);
}

TEST_CASE("Resume record length fields fail closed on overflow and mismatch",
          "[pbprotocol][resume]")
{
    const ReceiverResourcePolicy resourcePolicy = MakeResumePolicy(1024);
    const std::vector<std::byte> payload = MakePayload(5, 0x65);

    // payloadLength of UINT64_MAX must fail as overflow before any budget or
    // truncation comparison.
    std::vector<std::byte> overflowRecord = SerializeRecord(payload);
    WriteUint64LittleEndian(
        std::span<std::byte>(overflowRecord),
        6,
        std::numeric_limits<std::uint64_t>::max());
    auto overflowResult = ParseResumeRecord(
        std::span<const std::byte>(overflowRecord), resourcePolicy);
    REQUIRE_FALSE(overflowResult.HasValue());
    REQUIRE(overflowResult.Error().code == ProtocolErrorCode::LengthOverflow);

    // A length field claiming more bytes than remain is truncated input.
    std::vector<std::byte> longClaimRecord = SerializeRecord(payload);
    WriteUint64LittleEndian(
        std::span<std::byte>(longClaimRecord), 6, 100);
    auto longClaimResult = ParseResumeRecord(
        std::span<const std::byte>(longClaimRecord), resourcePolicy);
    REQUIRE_FALSE(longClaimResult.HasValue());
    REQUIRE(longClaimResult.Error().code
        == ProtocolErrorCode::TruncatedInput);

    // Trailing bytes after a complete record are rejected.
    std::vector<std::byte> trailingRecord = SerializeRecord(payload);
    trailingRecord.push_back(std::byte{0x7F});
    auto trailingResult = ParseResumeRecord(
        std::span<const std::byte>(trailingRecord), resourcePolicy);
    REQUIRE_FALSE(trailingResult.HasValue());
    REQUIRE(trailingResult.Error().code == ProtocolErrorCode::TrailingBytes);
}

TEST_CASE("Resume record CRC mismatches fail closed",
          "[pbprotocol][resume]")
{
    const ReceiverResourcePolicy resourcePolicy = MakeResumePolicy(1024);
    const std::vector<std::byte> payload = MakePayload(5, 0x66);

    // Corrupt a payload byte: the stored CRC no longer matches.
    std::vector<std::byte> corruptedRecord = SerializeRecord(payload);
    corruptedRecord[14] ^= std::byte{0x01};
    auto corruptedResult = ParseResumeRecord(
        std::span<const std::byte>(corruptedRecord), resourcePolicy);
    REQUIRE_FALSE(corruptedResult.HasValue());
    REQUIRE(corruptedResult.Error().code == ProtocolErrorCode::CrcMismatch);

    // Corrupt the stored CRC itself.
    std::vector<std::byte> badCrcRecord = SerializeRecord(payload);
    badCrcRecord[badCrcRecord.size() - 1] ^= std::byte{0x80};
    auto badCrcResult = ParseResumeRecord(
        std::span<const std::byte>(badCrcRecord), resourcePolicy);
    REQUIRE_FALSE(badCrcResult.HasValue());
    REQUIRE(badCrcResult.Error().code == ProtocolErrorCode::CrcMismatch);
}

TEST_CASE("Resume record budget admits exactly up to maxResumeBytes",
          "[pbprotocol][resume]")
{
    // An empty-payload record is exactly 18 bytes.
    const std::vector<std::byte> emptyRecord = SerializeRecord(
        std::span<const std::byte>{});

    {
        const ReceiverResourcePolicy exactPolicy = MakeResumePolicy(18);
        const auto exactResult = ParseResumeRecord(
            std::span<const std::byte>(emptyRecord), exactPolicy);
        REQUIRE(exactResult.HasValue());
        REQUIRE(exactResult.Value().empty());
    }

    {
        const ReceiverResourcePolicy tightPolicy = MakeResumePolicy(17);
        const auto tightResult = ParseResumeRecord(
            std::span<const std::byte>(emptyRecord), tightPolicy);
        REQUIRE_FALSE(tightResult.HasValue());
        REQUIRE(tightResult.Error().code
            == ProtocolErrorCode::ResourceLimitExceeded);
    }

    // A record whose total is one byte over the budget fails even though the
    // input itself is complete and well-formed.
    const std::vector<std::byte> payload = MakePayload(1, 0x67);
    const std::vector<std::byte> record = SerializeRecord(payload);
    REQUIRE(record.size() == 19);
    const ReceiverResourcePolicy overBudgetPolicy = MakeResumePolicy(18);
    const auto overBudgetResult = ParseResumeRecord(
        std::span<const std::byte>(record), overBudgetPolicy);
    REQUIRE_FALSE(overBudgetResult.HasValue());
    REQUIRE(overBudgetResult.Error().code
        == ProtocolErrorCode::ResourceLimitExceeded);
}

TEST_CASE("Resume state budget gate validates the whole record size",
          "[pbprotocol][resume]")
{
    const std::uint64_t resumeBytes = 256;
    const ReceiverResourcePolicy resourcePolicy = MakeResumePolicy(resumeBytes);

    REQUIRE(ValidateResumeStateBudget(0, resourcePolicy).HasValue());
    REQUIRE(
        ValidateResumeStateBudget(resumeBytes, resourcePolicy).HasValue());
    const pbprotocol::ProtocolStatus overBudgetStatus =
        ValidateResumeStateBudget(resumeBytes + 1, resourcePolicy);
    REQUIRE_FALSE(overBudgetStatus);
    REQUIRE(overBudgetStatus.Error().code
        == ProtocolErrorCode::ResourceLimitExceeded);

    // A zeroed policy fails closed before any budget comparison.
    const ReceiverResourcePolicy zeroedPolicy{};
    const pbprotocol::ProtocolStatus invalidPolicyStatus =
        ValidateResumeStateBudget(0, zeroedPolicy);
    REQUIRE_FALSE(invalidPolicyStatus);
    REQUIRE(invalidPolicyStatus.Error().code
        == ProtocolErrorCode::InvalidResourcePolicy);

    // ParseResumeRecord validates the policy before touching any bytes.
    const std::vector<std::byte> emptyRecord = SerializeRecord(
        std::span<const std::byte>{});
    const auto invalidParseResult = ParseResumeRecord(
        std::span<const std::byte>(emptyRecord), zeroedPolicy);
    REQUIRE_FALSE(invalidParseResult.HasValue());
    REQUIRE(invalidParseResult.Error().code
        == ProtocolErrorCode::InvalidResourcePolicy);
}
