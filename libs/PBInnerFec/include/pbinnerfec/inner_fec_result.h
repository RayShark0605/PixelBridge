#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace pbinnerfec {

enum class InnerFecErrorCode : std::uint8_t
{
    None,
    // profileId / matrixId not present in the three frozen DVB-S2 Short profiles.
    UnknownProfileId,
    UnknownMatrixId,
    // A profile value breaks its frozen internal consistency relations.
    InvalidProfile,
    // The embedded matrix table no longer digests to the profile-pinned digest.
    MatrixDigestMismatch,
    // A dimension (N/K/parity/M/Q/lines) is outside the frozen short-frame set.
    InvalidDimensions,
    // An input/output span size does not match the frozen requirement. detail
    // carries the expected element or byte count.
    InvalidInput,
    // A decode option is outside its documented range. detail carries the
    // offending value.
    InvalidOption,
    // A default-constructed or moved-from decoder instance.
    InvalidState,
    // The decoder exhausted maxIterations without a zero syndrome. detail
    // carries iterationsUsed (== maxIterations). On this failure the output
    // span is left untouched.
    SyndromeFailure,
    OutOfMemory
};

struct InnerFecError
{
    InnerFecErrorCode code = InnerFecErrorCode::None;
    std::uint64_t detail = 0;

    bool operator==(const InnerFecError&) const = default;
};

class InnerFecStatus
{
public:
    [[nodiscard]] static InnerFecStatus Success() noexcept
    {
        return InnerFecStatus(InnerFecError{});
    }

    [[nodiscard]] static InnerFecStatus Failure(
        const InnerFecErrorCode code,
        const std::uint64_t detail = 0) noexcept
    {
        const InnerFecErrorCode failureCode = code == InnerFecErrorCode::None
            ? InnerFecErrorCode::InvalidState
            : code;
        return InnerFecStatus(InnerFecError{failureCode, detail});
    }

    [[nodiscard]] bool HasValue() const noexcept
    {
        return error_.code == InnerFecErrorCode::None;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return HasValue();
    }

    [[nodiscard]] const InnerFecError& Error() const noexcept
    {
        return error_;
    }

private:
    explicit InnerFecStatus(const InnerFecError error) noexcept
        : error_(error)
    {
    }

    InnerFecError error_;
};

template <typename ValueType>
class InnerFecResult
{
public:
    [[nodiscard]] static InnerFecResult Success(ValueType value)
    {
        return InnerFecResult(std::move(value));
    }

    [[nodiscard]] static InnerFecResult Failure(
        const InnerFecErrorCode code,
        const std::uint64_t detail = 0)
    {
        const InnerFecErrorCode failureCode = code == InnerFecErrorCode::None
            ? InnerFecErrorCode::InvalidState
            : code;
        return InnerFecResult(InnerFecError{failureCode, detail});
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

    [[nodiscard]] const InnerFecError& Error() const noexcept
    {
        return error_;
    }

private:
    explicit InnerFecResult(ValueType value)
        : value_(std::move(value))
    {
    }

    explicit InnerFecResult(const InnerFecError error)
        : value_(std::nullopt), error_(error)
    {
    }

    std::optional<ValueType> value_;
    InnerFecError error_;
};

} // namespace pbinnerfec
