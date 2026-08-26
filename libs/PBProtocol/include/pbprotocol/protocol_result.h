#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace pbprotocol {

enum class ProtocolErrorCode : std::uint8_t
{
    None,
    TruncatedInput,
    OutputBufferTooSmall,
    LengthNarrowing,
    LengthOverflow,
    LengthLimitExceeded,
    InvalidUtf8,
    NonZeroReservedByte,
    NonCanonicalPadding,
    UnsupportedProtocolMajor,
    UnknownMandatoryFeature,
    ConflictingFeatureFlags,
    TrailingBytes,
    InvalidLengthPrefixWidth,
    UnsupportedProtocolMinor,
    InvalidEnumValue,
    InvalidRecordSize,
    InvalidDescriptor,
    InvalidResourcePolicy,
    ResourceLimitExceeded,
    ResourceExhausted,
    SessionTagMismatch,
    SessionMismatch,
    UnknownSession,
    SegmentOrdinalOutOfRange,
    SegmentRangeOutOfBounds,
    SegmentOverlap,
    SegmentGap,
    SegmentMapIncomplete,
    DescriptorConflict,
    InvalidWirehairProfile,
    MissingFinalManifest,
    DigestMismatch,
    InternalDescriptorStateError,
    InternalInvariantViolation,
    SessionTagCollision,
    CsprngFailure,
    InvalidMagic,
    UnsupportedBootstrapVersion,
    UnsupportedControlVersion,
    CrcMismatch,
    NonZeroReservedBits,
    InvalidControlFragment,
    ControlFragmentConflict,
    ControlReassemblyQuotaExceeded,
    InvalidObservationOrdinal,
    // Local diagnostics for receiver resource-policy gates. These values are
    // never serialized into wire bytes; keep the enum append-only so recorded
    // diagnostic values remain stable.
    OutputReservationDenied,
    OrphanPayloadConflict,
    UnknownSegment,
    // Input and output spans of a serialization/transfer API overlap; the
    // operation is rejected before any write (mirrors the Inner-FEC encoder
    // overlap rejection). Appended after UnknownSegment so previously
    // recorded diagnostic values stay stable.
    OverlappingSpans,
    // resume.state document key conflict: the same record key (SessionId +
    // SegmentOrdinal for completed records, SegmentOrdinal per active cache
    // type) appears with different valid content, or one ordinal is claimed by
    // both a completed and an active-cache record. Load rejects fail-closed;
    // never latest-wins. Appended so recorded diagnostic values stay stable.
    ResumeRecordConflict,
    // resume.state file IO failure: stat/open/regular-file check failed for
    // ReadResumeStateFile or open/write failed for WriteResumeStateFile. This
    // is a receiver-local persistence diagnostic and never enters wire bytes.
    ResumeStateIoFailure
};

struct ProtocolError
{
    ProtocolErrorCode code = ProtocolErrorCode::None;
    std::size_t offset = 0;

    bool operator==(const ProtocolError&) const = default;
};

class ProtocolStatus
{
public:
    [[nodiscard]] static ProtocolStatus Success() noexcept
    {
        return ProtocolStatus(ProtocolError{});
    }

    [[nodiscard]] static ProtocolStatus Failure(
        const ProtocolErrorCode code,
        const std::size_t offset) noexcept
    {
        const ProtocolErrorCode failureCode = code == ProtocolErrorCode::None
            ? ProtocolErrorCode::InternalInvariantViolation
            : code;
        return ProtocolStatus(ProtocolError{failureCode, offset});
    }

    [[nodiscard]] bool HasValue() const noexcept
    {
        return error_.code == ProtocolErrorCode::None;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return HasValue();
    }

    [[nodiscard]] const ProtocolError& Error() const noexcept
    {
        return error_;
    }

private:
    explicit ProtocolStatus(const ProtocolError error) noexcept
        : error_(error)
    {
    }

    ProtocolError error_;
};

template <typename ValueType>
class ProtocolResult
{
public:
    [[nodiscard]] static ProtocolResult Success(ValueType value)
    {
        return ProtocolResult(std::move(value));
    }

    [[nodiscard]] static ProtocolResult Failure(
        const ProtocolErrorCode code,
        const std::size_t offset)
    {
        const ProtocolErrorCode failureCode = code == ProtocolErrorCode::None
            ? ProtocolErrorCode::InternalInvariantViolation
            : code;
        return ProtocolResult(ProtocolError{failureCode, offset});
    }

    [[nodiscard]] bool HasValue() const noexcept
    {
        return value_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return HasValue();
    }

    // Value() follows std::optional::value(): calling it on a failed result
    // throws std::bad_optional_access. Callers must test HasValue() first, and
    // noexcept code must not call Value() on an unchecked result.
    [[nodiscard]] ValueType& Value() &
    {
        return value_.value();
    }

    [[nodiscard]] const ValueType& Value() const&
    {
        return value_.value();
    }

    [[nodiscard]] ValueType&& Value() &&
    {
        return std::move(value_).value();
    }

    [[nodiscard]] const ProtocolError& Error() const noexcept
    {
        return error_;
    }

private:
    explicit ProtocolResult(ValueType value)
        : value_(std::move(value))
    {
    }

    explicit ProtocolResult(const ProtocolError error)
        : error_(error)
    {
    }

    std::optional<ValueType> value_;
    ProtocolError error_;
};

} // namespace pbprotocol
