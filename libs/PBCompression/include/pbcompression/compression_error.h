#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace pbcompression {

// Error taxonomy for the per-Segment compression primitives. ZstdError keeps
// the raw ZSTD_ErrorCode value in detail so callers can classify zstd
// failures without this header exposing <zstd.h>.
enum class CompressionErrorCode : std::uint8_t
{
    None,
    InvalidCompressionLevel,
    InvalidMaxWindowLog,
    InvalidMaxOutputBytes,
    InvalidExpectedRawSize,
    InvalidState,
    OutputLimitExceeded,
    RawSizeMismatch,
    IncompleteFrame,
    TrailingInput,
    CorruptedFrame,
    ZstdError,
    AllocationFailure
};

struct CompressionError
{
    CompressionErrorCode code = CompressionErrorCode::None;
    // detail carries the raw ZSTD_ErrorCode for ZstdError, the offending
    // configuration value for validation errors, and the measured byte
    // count for size related failures.
    std::uint64_t detail = 0;

    bool operator==(const CompressionError&) const = default;
};

class CompressionStatus
{
public:
    [[nodiscard]] static CompressionStatus Success() noexcept
    {
        return CompressionStatus(CompressionError{});
    }

    [[nodiscard]] static CompressionStatus Failure(
        const CompressionErrorCode code,
        const std::uint64_t detail = 0) noexcept
    {
        const CompressionErrorCode failureCode = code == CompressionErrorCode::None
            ? CompressionErrorCode::ZstdError
            : code;
        return CompressionStatus(CompressionError{failureCode, detail});
    }

    [[nodiscard]] bool HasValue() const noexcept
    {
        return error_.code == CompressionErrorCode::None;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return HasValue();
    }

    [[nodiscard]] const CompressionError& Error() const noexcept
    {
        return error_;
    }

private:
    explicit CompressionStatus(const CompressionError error) noexcept
        : error_(error)
    {
    }

    CompressionError error_;
};

template <typename ValueType>
class CompressionResult
{
public:
    [[nodiscard]] static CompressionResult Success(ValueType value)
    {
        return CompressionResult(std::move(value));
    }

    [[nodiscard]] static CompressionResult Failure(
        const CompressionErrorCode code,
        const std::uint64_t detail = 0)
    {
        const CompressionErrorCode failureCode = code == CompressionErrorCode::None
            ? CompressionErrorCode::ZstdError
            : code;
        return CompressionResult(CompressionError{failureCode, detail});
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
    // throws std::bad_optional_access. Callers must test HasValue() first,
    // and noexcept code must not call Value() on an unchecked result.
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

    [[nodiscard]] const CompressionError& Error() const noexcept
    {
        return error_;
    }

private:
    explicit CompressionResult(ValueType value)
        : value_(std::move(value))
    {
    }

    explicit CompressionResult(const CompressionError error)
        : error_(error)
    {
    }

    std::optional<ValueType> value_;
    CompressionError error_;
};

} // namespace pbcompression