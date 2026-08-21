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
    InternalDescriptorStateError
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
        return ProtocolStatus(ProtocolError{code, offset});
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
        return ProtocolResult(ProtocolError{code, offset});
    }

    [[nodiscard]] bool HasValue() const noexcept
    {
        return value_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return HasValue();
    }

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
