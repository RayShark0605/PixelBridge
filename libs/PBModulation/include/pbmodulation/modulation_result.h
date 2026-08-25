#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace pbmodulation {

// Error codes for the Phase-0 CPU reference raster/demod and frame I/O.
// The convention mirrors pbprotocol::ProtocolStatus / pbinnerfec::
// InnerFecStatus: a failure never mutates any caller output buffer.
enum class ModulationErrorCode : std::uint8_t
{
    None,
    // Input span/size does not match the frozen reference requirement.
    InvalidInput,
    // Caller output buffer is smaller than the fixed frame size.
    OutputBufferTooSmall,
    // A frozen zero-only reserved field is not zero (container reserved
    // bytes, or a reserved lane symbol that is not the frozen symbol 0).
    NonZeroReservedByte,
    // A container magic is not the frozen value.
    InvalidMagic,
    // A container version is not the frozen reference version.
    UnsupportedVersion,
    // A container/manifest is truncated.
    TruncatedInput,
    // A container has trailing bytes after the declared content.
    TrailingBytes,
    // A CRC over frozen content does not match.
    CrcMismatch,
    // Frame dimensions do not match the fixed 1920x1080 reference canvas.
    FrameGeometryMismatch,
    // A decoded tile average is not within the frozen margin of any level.
    OffConstellationLevel,
    // A decoded tile average is exactly halfway between two levels.
    AmbiguousLevel,
    // A data-carrying pixel has B != G or G != R (luma-only contract).
    ChromaChannelMismatch,
    // A pixel alpha channel is not the frozen value 255.
    AlphaChannelViolation,
    // The two spatially separated bootstrap copies disagree (design 16.4):
    // the whole frame is torn and is rejected as an erasure.
    TornFrame,
    // A frozen sync/guard/pilot pixel differs from the canonical raster.
    FrozenRegionMismatch,
    // Region manifest fails the frozen geometry/capacity validation.
    ManifestValidationFailed,
    // PNG content cannot be normalized to 8-bit RGBA.
    UnsupportedPngFormat,
    // PNG stream is malformed (signature, chunk CRC, zlib stream, ...).
    PngDecodeError,
    // PNG encoder failure while writing the frozen output format.
    PngEncodeError,
    // Memory allocation failed; the operation is rejected as a whole.
    MemoryAllocationFailure,
    // An internal check that must never fire; indicates a library bug.
    InternalInvariantViolation
};

struct ModulationError
{
    ModulationErrorCode code = ModulationErrorCode::None;
    // Canvas linear pixel index (y * 1920 + x) for frame errors, the input
    // byte offset for container/manifest errors, or 0 when not applicable.
    std::size_t offset = 0;

    bool operator==(const ModulationError&) const = default;
};

class ModulationStatus
{
public:
    [[nodiscard]] static ModulationStatus Success() noexcept
    {
        return ModulationStatus(ModulationError{});
    }

    [[nodiscard]] static ModulationStatus Failure(
        const ModulationErrorCode code,
        const std::size_t offset) noexcept
    {
        const ModulationErrorCode failureCode =
            code == ModulationErrorCode::None
                ? ModulationErrorCode::InternalInvariantViolation
                : code;
        return ModulationStatus(ModulationError{failureCode, offset});
    }

    [[nodiscard]] bool HasValue() const noexcept
    {
        return error_.code == ModulationErrorCode::None;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return HasValue();
    }

    [[nodiscard]] const ModulationError& Error() const noexcept
    {
        return error_;
    }

private:
    explicit ModulationStatus(const ModulationError error) noexcept
        : error_(error)
    {
    }

    ModulationError error_;
};

template <typename ValueType>
class ModulationResult
{
public:
    [[nodiscard]] static ModulationResult Success(ValueType value)
    {
        return ModulationResult(std::move(value));
    }

    [[nodiscard]] static ModulationResult Failure(
        const ModulationErrorCode code,
        const std::size_t offset)
    {
        const ModulationErrorCode failureCode =
            code == ModulationErrorCode::None
                ? ModulationErrorCode::InternalInvariantViolation
                : code;
        return ModulationResult(ModulationError{failureCode, offset});
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
    // throws std::bad_optional_access. Callers must test HasValue() first.
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

    [[nodiscard]] const ModulationError& Error() const noexcept
    {
        return error_;
    }

private:
    explicit ModulationResult(ValueType value)
        : value_(std::move(value))
    {
    }

    explicit ModulationResult(const ModulationError error)
        : error_(error)
    {
    }

    std::optional<ValueType> value_;
    ModulationError error_;
};

} // namespace pbmodulation