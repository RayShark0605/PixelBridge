#pragma once

#include "pbprotocol/protocol_result.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pbprotocol {

template <std::unsigned_integral TargetType, std::unsigned_integral SourceType>
[[nodiscard]] ProtocolResult<TargetType> CheckedNarrowUnsigned(
    const SourceType value,
    const std::size_t errorOffset = 0) noexcept
{
    using CommonType = std::common_type_t<TargetType, SourceType>;
    const CommonType commonValue = static_cast<CommonType>(value);
    const CommonType maximumTargetValue =
        static_cast<CommonType>(std::numeric_limits<TargetType>::max());

    if (commonValue > maximumTargetValue)
    {
        return ProtocolResult<TargetType>::Failure(
            ProtocolErrorCode::LengthNarrowing,
            errorOffset);
    }

    return ProtocolResult<TargetType>::Success(static_cast<TargetType>(value));
}

template <std::unsigned_integral ValueType>
[[nodiscard]] ProtocolResult<ValueType> CheckedAddUnsigned(
    const ValueType left,
    const ValueType right,
    const std::size_t errorOffset = 0) noexcept
{
    if (right > std::numeric_limits<ValueType>::max() - left)
    {
        return ProtocolResult<ValueType>::Failure(
            ProtocolErrorCode::LengthOverflow,
            errorOffset);
    }

    return ProtocolResult<ValueType>::Success(
        static_cast<ValueType>(left + right));
}

template <std::unsigned_integral ValueType>
[[nodiscard]] ProtocolResult<ValueType> CheckedMultiplyUnsigned(
    const ValueType left,
    const ValueType right,
    const std::size_t errorOffset = 0) noexcept
{
    if (left != 0 && right > std::numeric_limits<ValueType>::max() / left)
    {
        return ProtocolResult<ValueType>::Failure(
            ProtocolErrorCode::LengthOverflow,
            errorOffset);
    }

    return ProtocolResult<ValueType>::Success(
        static_cast<ValueType>(left * right));
}

// Checked addition with an inclusive upper bound: the sum must both fit in
// ValueType and not exceed limit. Overflow is reported before the limit check
// so a wrapping sum can never be misclassified as LengthLimitExceeded.
template <std::unsigned_integral ValueType>
[[nodiscard]] ProtocolResult<ValueType> CheckedAddWithinLimit(
    const ValueType left,
    const ValueType right,
    const ValueType limit,
    const std::size_t errorOffset = 0) noexcept
{
    if (right > std::numeric_limits<ValueType>::max() - left)
    {
        return ProtocolResult<ValueType>::Failure(
            ProtocolErrorCode::LengthOverflow,
            errorOffset);
    }

    const ValueType sum = static_cast<ValueType>(left + right);
    if (sum > limit)
    {
        return ProtocolResult<ValueType>::Failure(
            ProtocolErrorCode::LengthLimitExceeded,
            errorOffset);
    }

    return ProtocolResult<ValueType>::Success(sum);
}

[[nodiscard]] inline ProtocolResult<std::size_t> CheckedUint64ToSize(
    const std::uint64_t value,
    const std::size_t errorOffset = 0) noexcept
{
    return CheckedNarrowUnsigned<std::size_t>(value, errorOffset);
}

[[nodiscard]] inline ProtocolResult<std::size_t> CheckedAddSize(
    const std::size_t left,
    const std::size_t right,
    const std::size_t errorOffset = 0) noexcept
{
    return CheckedAddUnsigned(left, right, errorOffset);
}

[[nodiscard]] inline ProtocolResult<std::uint64_t> CheckedAddUint64(
    const std::uint64_t left,
    const std::uint64_t right,
    const std::size_t errorOffset = 0) noexcept
{
    return CheckedAddUnsigned(left, right, errorOffset);
}

[[nodiscard]] inline ProtocolResult<std::uint64_t> CheckedMultiplyUint64(
    const std::uint64_t left,
    const std::uint64_t right,
    const std::size_t errorOffset = 0) noexcept
{
    return CheckedMultiplyUnsigned(left, right, errorOffset);
}

[[nodiscard]] inline ProtocolResult<std::uint64_t> CheckedAddUint64WithinLimit(
    const std::uint64_t left,
    const std::uint64_t right,
    const std::uint64_t limit,
    const std::size_t errorOffset = 0) noexcept
{
    return CheckedAddWithinLimit(left, right, limit, errorOffset);
}

} // namespace pbprotocol
