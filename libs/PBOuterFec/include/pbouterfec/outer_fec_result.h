#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace pbouterfec {

enum class OuterFecErrorCode : std::uint8_t
{
    None,
    InvalidDescriptor,
    EncodedSizeMismatch,
    OuterBlockBytesMismatch,
    EncodedDigestMismatch,
    InvalidState,
    InvalidInput,
    BufferTooSmall,
    InvalidMagic,
    UnsupportedVersion,
    InvalidSize,
    ReservedNonzero,
    UnsupportedProfile,
    InvalidDimensions,
    BadSeed,
    ExtraInsufficient,
    CodecError,
    OutOfMemory,
    UnsupportedPlatform,
    // Keep new local API errors append-only so existing diagnostic values do
    // not change for consumers that record the underlying byte.
    InvalidResourcePolicy,
    OuterFecDecoderQuotaExceeded,
    OuterBlockConflict,
    CsprngFailure
};

enum class DecodeDisposition : std::uint8_t
{
    NeedMore,
    Ready
};

struct OuterFecError
{
    OuterFecErrorCode code = OuterFecErrorCode::None;
    // BufferTooSmall carries the required byte count. Dimension and binding
    // errors carry the offending value. Decoder quota failures carry the
    // requested admission charge or count. OuterBlockConflict carries the
    // conflicting OuterBlockId. CodecError carries the raw Wirehair result
    // when it is unknown or invalid in the current operation.
    std::uint64_t detail = 0;

    bool operator==(const OuterFecError&) const = default;
};

class OuterFecStatus
{
public:
    [[nodiscard]] static OuterFecStatus Success() noexcept
    {
        return OuterFecStatus(OuterFecError{});
    }

    [[nodiscard]] static OuterFecStatus Failure(
        const OuterFecErrorCode code,
        const std::uint64_t detail = 0) noexcept
    {
        const OuterFecErrorCode failureCode = code == OuterFecErrorCode::None
            ? OuterFecErrorCode::CodecError
            : code;
        return OuterFecStatus(OuterFecError{failureCode, detail});
    }

    [[nodiscard]] bool HasValue() const noexcept
    {
        return error_.code == OuterFecErrorCode::None;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return HasValue();
    }

    [[nodiscard]] const OuterFecError& Error() const noexcept
    {
        return error_;
    }

private:
    explicit OuterFecStatus(const OuterFecError error) noexcept
        : error_(error)
    {
    }

    OuterFecError error_;
};

template <typename ValueType>
class OuterFecResult
{
public:
    [[nodiscard]] static OuterFecResult Success(ValueType value)
    {
        return OuterFecResult(std::move(value));
    }

    [[nodiscard]] static OuterFecResult Failure(
        const OuterFecErrorCode code,
        const std::uint64_t detail = 0)
    {
        const OuterFecErrorCode failureCode = code == OuterFecErrorCode::None
            ? OuterFecErrorCode::CodecError
            : code;
        return OuterFecResult(OuterFecError{failureCode, detail});
    }

    [[nodiscard]] bool HasValue() const noexcept
    {
        return value_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return HasValue();
    }

    // Value() follows std::optional::value(). Callers must first check the
    // result, particularly in noexcept code.
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

    [[nodiscard]] const OuterFecError& Error() const noexcept
    {
        return error_;
    }

private:
    explicit OuterFecResult(ValueType value)
        : value_(std::move(value))
    {
    }

    explicit OuterFecResult(const OuterFecError error)
        : error_(error)
    {
    }

    std::optional<ValueType> value_;
    OuterFecError error_;
};

} // namespace pbouterfec
