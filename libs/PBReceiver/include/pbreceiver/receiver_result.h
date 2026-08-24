#pragma once

#include "pbcompression/compression_error.h"
#include "pbouterfec/outer_fec_result.h"
#include "pbprotocol/protocol_result.h"

#include <optional>
#include <utility>
#include <variant>

namespace pbreceiver
{

using ReceiverError = std::variant<
    pbprotocol::ProtocolError,
    pbouterfec::OuterFecError,
    pbcompression::CompressionError>;

template <typename ValueType>
class ReceiverResult
{
public:
    [[nodiscard]] static ReceiverResult Success(ValueType value)
    {
        return ReceiverResult(std::move(value));
    }

    [[nodiscard]] static ReceiverResult Failure(ReceiverError error)
    {
        return ReceiverResult(std::move(error));
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

    [[nodiscard]] const ReceiverError& Error() const
    {
        return error_.value();
    }

private:
    explicit ReceiverResult(ValueType value)
        : value_(std::move(value))
    {
    }

    explicit ReceiverResult(ReceiverError error)
        : error_(std::move(error))
    {
    }

    std::optional<ValueType> value_;
    std::optional<ReceiverError> error_;
};

} // namespace pbreceiver
