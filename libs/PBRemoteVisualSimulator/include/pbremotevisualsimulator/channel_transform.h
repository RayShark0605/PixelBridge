#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pbremotevisualsimulator
{

inline constexpr char kChannelManifestSchema[] = "PixelBridge.RemoteVisualChannelManifest.1";
inline constexpr std::uint32_t kChannelManifestVersion = 1;
inline constexpr std::size_t kChannelDigestBytes = 32;
inline constexpr std::size_t kNoTransformIndex = static_cast<std::size_t>(-1);
inline constexpr std::uint32_t kMaximumChannelDimension = 8192;
inline constexpr std::uint64_t kMaximumChannelPixels = 8ULL * 1024 * 1024;
inline constexpr std::uint64_t kMaximumChannelResidentBytes = 128ULL * 1024 * 1024;
inline constexpr std::uint64_t kMaximumChannelWorkUnits = 256ULL * 1024 * 1024;
inline constexpr std::size_t kMaximumChannelTransforms = 64;
inline constexpr double kMinimumChannelScale = 0.125;
inline constexpr double kMaximumChannelScale = 4.0;

enum class ChannelTransformErrorCode : std::uint8_t
{
    None,
    InvalidView,
    InvalidPolicy,
    TooManyTransforms,
    InvalidParameter,
    DimensionLimitExceeded,
    PixelLimitExceeded,
    ByteLimitExceeded,
    WorkLimitExceeded,
    MissingReference,
    ReferenceGeometryMismatch,
    RectangleOutOfBounds,
    AllocationFailure,
    InternalInvariantViolation
};

struct ChannelTransformError
{
    ChannelTransformErrorCode code = ChannelTransformErrorCode::None;
    std::size_t transformIndex = kNoTransformIndex;

    bool operator==(const ChannelTransformError&) const = default;
};

template <typename ValueType>
class ChannelTransformResult
{
public:
    [[nodiscard]] static ChannelTransformResult Success(ValueType value)
    {
        return ChannelTransformResult(std::move(value));
    }

    [[nodiscard]] static ChannelTransformResult Failure(const ChannelTransformErrorCode code,
        const std::size_t transformIndex = kNoTransformIndex)
    {
        const ChannelTransformErrorCode failureCode = code == ChannelTransformErrorCode::None ?
            ChannelTransformErrorCode::InternalInvariantViolation : code;
        return ChannelTransformResult(ChannelTransformError{failureCode, transformIndex});
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

    [[nodiscard]] const ChannelTransformError& Error() const noexcept
    {
        return error_;
    }

private:
    explicit ChannelTransformResult(ValueType value)
        : value_(std::move(value))
    {
    }

    explicit ChannelTransformResult(const ChannelTransformError error) noexcept
        : error_(error)
    {
    }

    std::optional<ValueType> value_;
    ChannelTransformError error_;
};

struct BgraImageView
{
    // Borrowed BGRA8 pixels. Rows may contain padding; only width*4 active
    // bytes are hashed and transformed. ExecuteChannelTransformPlan validates
    // the complete last active row before reading any pixel.
    std::span<const std::byte> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t rowPitch = 0;
};

struct BgraImage
{
    // Successful executions publish a tightly packed owned image. A failed
    // execution never exposes a partially transformed image.
    std::vector<std::byte> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t rowPitch = 0;

    [[nodiscard]] BgraImageView View() const noexcept
    {
        return {pixels, width, height, rowPitch};
    }
};

enum class ResampleFilter : std::uint8_t
{
    Bilinear,
    Area
};

struct ResampleTransform
{
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    double scaleX = 0;
    double scaleY = 0;
    double originX = 0;
    double originY = 0;
    ResampleFilter filter = ResampleFilter::Area;
    std::array<std::byte, 4> borderBgra{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}};

    bool operator==(const ResampleTransform&) const = default;
};

struct BlockReplacementTransform
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const BlockReplacementTransform&) const = default;
};

using ChannelTransform = std::variant<ResampleTransform, BlockReplacementTransform>;

struct ChannelTransformPlan
{
    // The seed is part of the canonical manifest even when all selected
    // transforms are deterministic. Future stochastic transforms must derive
    // every random choice from this value rather than ambient process state.
    std::uint64_t seed = 0;
    std::span<const ChannelTransform> transforms;
};

struct ChannelTransformPolicy
{
    // These are simulator resource limits, not wire/profile parameters.
    std::uint32_t maximumDimension = kMaximumChannelDimension;
    std::uint64_t maximumPixels = kMaximumChannelPixels;
    std::uint64_t maximumResidentBytes = kMaximumChannelResidentBytes;
    std::uint64_t maximumWorkUnits = kMaximumChannelWorkUnits;
    std::size_t maximumTransforms = kMaximumChannelTransforms;
    double minimumScale = kMinimumChannelScale;
    double maximumScale = kMaximumChannelScale;
};

struct ChannelTransformRecord
{
    std::size_t index = 0;
    ChannelTransform transform;
    std::uint32_t inputWidth = 0;
    std::uint32_t inputHeight = 0;
    std::array<std::byte, kChannelDigestBytes> inputBlake3{};
    std::optional<std::array<std::byte, kChannelDigestBytes>> referenceInputBlake3;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::array<std::byte, kChannelDigestBytes> outputBlake3{};
};

struct ChannelTransformExecution
{
    std::uint64_t seed = 0;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::array<std::byte, kChannelDigestBytes> sourceBlake3{};
    std::optional<std::array<std::byte, kChannelDigestBytes>> referenceBlake3;
    std::vector<ChannelTransformRecord> records;
    BgraImage output;
    std::array<std::byte, kChannelDigestBytes> outputBlake3{};
    std::string canonicalManifestJson;
    std::array<std::byte, kChannelDigestBytes> manifestBlake3{};
};

[[nodiscard]] ChannelTransformResult<ChannelTransformExecution> ExecuteChannelTransformPlan(
    const BgraImageView& source, const std::optional<BgraImageView>& reference,
    const ChannelTransformPlan& plan, const ChannelTransformPolicy& policy = {});

[[nodiscard]] std::string ChannelDigestToHex(std::span<const std::byte, kChannelDigestBytes> digest);
[[nodiscard]] const char* GetChannelTransformErrorName(ChannelTransformErrorCode code) noexcept;

} // namespace pbremotevisualsimulator
