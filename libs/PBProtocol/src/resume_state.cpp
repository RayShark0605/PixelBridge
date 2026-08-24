#include "pbprotocol/resume_state.h"

#include "pbprotocol/byte_io.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/descriptor_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <stdexcept>
#include <vector>

namespace pbprotocol {

namespace {

constexpr std::uint8_t kResumeRecordVersion = 1;
// magic + version + reserved + payloadLength.
constexpr std::size_t kResumeRecordHeaderBytes = 14;
constexpr std::array<std::byte, 4> kResumeRecordMagic{
    std::byte{'P'},
    std::byte{'B'},
    std::byte{'R'},
    std::byte{'S'}};

} // namespace

ProtocolStatus SerializeResumeRecord(
    const std::span<const std::byte> payload,
    const std::span<std::byte> output)
{
    const auto requiredBytesResult = CheckedAddSize(
        kResumeRecordEnvelopeBytes,
        payload.size());
    if (!requiredBytesResult ||
        output.size() < requiredBytesResult.Value())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::OutputBufferTooSmall,
            0);
    }

    ByteWriter writer(output);
    ProtocolStatus status = writer.WriteFixedBytes(kResumeRecordMagic);
    if (status)
    {
        status = writer.WriteUint8(kResumeRecordVersion);
    }
    if (status)
    {
        status = writer.WriteUint8(0);
    }
    if (status)
    {
        status = writer.WriteUint64(static_cast<std::uint64_t>(payload.size()));
    }
    if (status)
    {
        status = writer.WriteBytes(payload);
    }
    if (!status)
    {
        return status;
    }

    // The CRC covers header plus payload, i.e. everything written so far.
    const std::uint32_t recordCrc = ComputeCrc32c(
        std::span<const std::byte>(output.data(), writer.Position()));
    return writer.WriteUint32(recordCrc);
}

ProtocolResult<std::vector<std::byte>> ParseResumeRecord(
    const std::span<const std::byte> input,
    const ReceiverResourcePolicy& resourcePolicy)
{
    using ResumeParseResult = ProtocolResult<std::vector<std::byte>>;

    // Resume state is untrusted persistent input: the policy gate runs before
    // any byte is interpreted.
    const ProtocolStatus policyStatus = ValidateReceiverResourcePolicy(
        resourcePolicy);
    if (!policyStatus)
    {
        return ResumeParseResult::Failure(
            policyStatus.Error().code,
            policyStatus.Error().offset);
    }
    const auto inputSizeResult = CheckedNarrowUnsigned<std::uint64_t>(
        input.size());
    if (!inputSizeResult)
    {
        return ResumeParseResult::Failure(
            inputSizeResult.Error().code,
            inputSizeResult.Error().offset);
    }
    if (inputSizeResult.Value() > resourcePolicy.maxResumeBytes)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            0);
    }

    ByteReader reader(input);
    const auto magicResult = reader.ReadFixedBytes<4>();
    if (!magicResult)
    {
        return ResumeParseResult::Failure(
            magicResult.Error().code,
            magicResult.Error().offset);
    }
    if (magicResult.Value() != kResumeRecordMagic)
    {
        return ResumeParseResult::Failure(ProtocolErrorCode::InvalidMagic, 0);
    }

    const std::size_t versionOffset = reader.Position();
    const auto versionResult = reader.ReadUint8();
    if (!versionResult)
    {
        return ResumeParseResult::Failure(
            versionResult.Error().code,
            versionResult.Error().offset);
    }
    if (versionResult.Value() != kResumeRecordVersion)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::InvalidEnumValue,
            versionOffset);
    }

    const std::size_t reservedOffset = reader.Position();
    const auto reservedResult = reader.ReadUint8();
    if (!reservedResult)
    {
        return ResumeParseResult::Failure(
            reservedResult.Error().code,
            reservedResult.Error().offset);
    }
    if (reservedResult.Value() != 0)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::NonZeroReservedByte,
            reservedOffset);
    }

    const auto payloadLengthResult = reader.ReadUint64();
    if (!payloadLengthResult)
    {
        return ResumeParseResult::Failure(
            payloadLengthResult.Error().code,
            payloadLengthResult.Error().offset);
    }

    // 18 + UINT64_MAX must fail as overflow before any budget comparison.
    const auto totalBytesResult = CheckedAddUint64(
        kResumeRecordEnvelopeBytes,
        payloadLengthResult.Value());
    if (!totalBytesResult)
    {
        return ResumeParseResult::Failure(
            totalBytesResult.Error().code,
            totalBytesResult.Error().offset);
    }

    const std::uint64_t totalRecordBytes = totalBytesResult.Value();
    if (totalRecordBytes > resourcePolicy.maxResumeBytes)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            0);
    }

    const auto totalSizeResult = CheckedUint64ToSize(totalRecordBytes);
    if (!totalSizeResult)
    {
        return ResumeParseResult::Failure(
            totalSizeResult.Error().code,
            totalSizeResult.Error().offset);
    }
    const std::size_t totalRecordSize = totalSizeResult.Value();

    if (input.size() < totalRecordSize)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::TruncatedInput,
            input.size());
    }
    if (input.size() > totalRecordSize)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::TrailingBytes,
            totalRecordSize);
    }

    const auto payloadSizeResult = CheckedUint64ToSize(
        payloadLengthResult.Value());
    if (!payloadSizeResult)
    {
        return ResumeParseResult::Failure(
            payloadSizeResult.Error().code,
            payloadSizeResult.Error().offset);
    }
    const std::size_t payloadLength = payloadSizeResult.Value();
    const auto payloadSpanResult = reader.ReadBytes(payloadLength);
    if (!payloadSpanResult)
    {
        return ResumeParseResult::Failure(
            payloadSpanResult.Error().code,
            payloadSpanResult.Error().offset);
    }

    const auto storedCrcResult = reader.ReadUint32();
    if (!storedCrcResult)
    {
        return ResumeParseResult::Failure(
            storedCrcResult.Error().code,
            storedCrcResult.Error().offset);
    }

    // The exact-size equality above already pins consumption; keep the check
    // explicit so a future edit to the size arithmetic fails closed here.
    const ProtocolStatus consumedStatus = reader.RequireFullyConsumed();
    if (!consumedStatus)
    {
        return ResumeParseResult::Failure(
            consumedStatus.Error().code,
            consumedStatus.Error().offset);
    }

    // totalRecordSize >= 18, so the subtraction cannot underflow.
    const std::uint32_t computedCrc = ComputeCrc32c(
        std::span<const std::byte>(input.data(), totalRecordSize - 4));
    if (computedCrc != storedCrcResult.Value())
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::CrcMismatch,
            totalRecordSize - 4);
    }

    std::vector<std::byte> payload;
    try
    {
        payload.assign(
            payloadSpanResult.Value().begin(),
            payloadSpanResult.Value().end());
    }
    catch (const std::bad_alloc&)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::ResourceExhausted,
            0);
    }
    catch (const std::length_error&)
    {
        return ResumeParseResult::Failure(
            ProtocolErrorCode::ResourceExhausted,
            0);
    }

    return ResumeParseResult::Success(std::move(payload));
}

ProtocolStatus ValidateResumeStateBudget(
    const std::uint64_t totalBytes,
    const ReceiverResourcePolicy& resourcePolicy) noexcept
{
    const ProtocolStatus policyStatus = ValidateReceiverResourcePolicy(
        resourcePolicy);
    if (!policyStatus)
    {
        return policyStatus;
    }

    if (totalBytes > resourcePolicy.maxResumeBytes)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            0);
    }
    return ProtocolStatus::Success();
}

} // namespace pbprotocol
