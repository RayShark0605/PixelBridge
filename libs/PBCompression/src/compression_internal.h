#pragma once

#include "pbcompression/compression_error.h"

#include <cstddef>

namespace pbcompression {
namespace detail {

// Fixed classification table for ZSTD_isError results. Error codes added by
// future zstd revisions are not silently reclassified: they fall through to
// ZstdError with the raw value preserved in detail (fail closed).
[[nodiscard]] CompressionError MapZstdFunctionResult(
    const std::size_t functionResult) noexcept;

} // namespace detail
} // namespace pbcompression