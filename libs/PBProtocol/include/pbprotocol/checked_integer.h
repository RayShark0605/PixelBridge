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
    if (right > std::numeric_limits<std::size_t>::max() - left)
    {
        return ProtocolResult<std::size_t>::Failure(
            ProtocolErrorCode::LengthOverflow,
            errorOffset);
    }

    return ProtocolResult<std::size_t>::Success(left + right);
}

} // namespace pbprotocol
