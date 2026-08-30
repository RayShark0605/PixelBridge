#include "pbremotevisualsimulator/channel_transform.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace pbremotevisualsimulator
{
namespace
{

inline constexpr std::uint64_t kBgraPixelBytes = 4;
inline constexpr char kImageDigestDomain[] = "PixelBridge.RemoteVisualChannelImage.1";

struct ValidatedView
{
    std::uint64_t rowBytes = 0;
    std::uint64_t activeBytes = 0;
};

struct ValidationResult
{
    ChannelTransformErrorCode code = ChannelTransformErrorCode::None;
    ValidatedView view;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return code == ChannelTransformErrorCode::None;
    }
};

[[nodiscard]] bool IsValidPolicy(const ChannelTransformPolicy& policy) noexcept
{
    return policy.maximumDimension > 0 && policy.maximumPixels > 0 && policy.maximumResidentBytes > 0 &&
        policy.maximumWorkUnits > 0 && policy.maximumTransforms > 0 && std::isfinite(policy.minimumScale) &&
        std::isfinite(policy.maximumScale) && policy.minimumScale >= kMinimumChannelScale &&
        policy.minimumScale <= policy.maximumScale && policy.maximumScale <= kMaximumChannelScale &&
        policy.maximumDimension <= kMaximumChannelDimension && policy.maximumPixels <= kMaximumChannelPixels &&
        policy.maximumResidentBytes <= kMaximumChannelResidentBytes && policy.maximumWorkUnits <= kMaximumChannelWorkUnits &&
        policy.maximumTransforms <= kMaximumChannelTransforms;
}

[[nodiscard]] ValidationResult ValidateView(const BgraImageView& view,
    const ChannelTransformPolicy& policy) noexcept
{
    if (view.width == 0 || view.height == 0 || view.pixels.data() == nullptr)
    {
        return {ChannelTransformErrorCode::InvalidView, {}};
    }
    if (view.width > policy.maximumDimension || view.height > policy.maximumDimension)
    {
        return {ChannelTransformErrorCode::DimensionLimitExceeded, {}};
    }
    const auto pixelsResult = pbprotocol::CheckedMultiplyUint64(view.width, view.height);
    if (!pixelsResult)
    {
        return {ChannelTransformErrorCode::PixelLimitExceeded, {}};
    }
    if (pixelsResult.Value() > policy.maximumPixels)
    {
        return {ChannelTransformErrorCode::PixelLimitExceeded, {}};
    }
    const auto rowBytesResult = pbprotocol::CheckedMultiplyUint64(view.width, kBgraPixelBytes);
    const auto activeBytesResult = rowBytesResult ?
        pbprotocol::CheckedMultiplyUint64(rowBytesResult.Value(), view.height) : rowBytesResult;
    if (!rowBytesResult || !activeBytesResult || activeBytesResult.Value() > policy.maximumResidentBytes)
    {
        return {ChannelTransformErrorCode::ByteLimitExceeded, {}};
    }
    if (view.rowPitch < rowBytesResult.Value())
    {
        return {ChannelTransformErrorCode::InvalidView, {}};
    }
    const auto lastRowResult = pbprotocol::CheckedMultiplyUint64(view.rowPitch, view.height - 1ULL);
    const auto extentResult = lastRowResult ?
        pbprotocol::CheckedAddUint64(lastRowResult.Value(), rowBytesResult.Value()) : lastRowResult;
    if (!lastRowResult || !extentResult || extentResult.Value() > view.pixels.size())
    {
        return {ChannelTransformErrorCode::InvalidView, {}};
    }
    if (extentResult.Value() > policy.maximumResidentBytes)
    {
        return {ChannelTransformErrorCode::ByteLimitExceeded, {}};
    }
    return {ChannelTransformErrorCode::None, {rowBytesResult.Value(), activeBytesResult.Value()}};
}

[[nodiscard]] std::array<std::byte, 8> EncodeUint32Pair(const std::uint32_t first,
    const std::uint32_t second) noexcept
{
    std::array<std::byte, 8> encoded{};
    for (std::size_t i = 0; i < 4; i++)
    {
        encoded[i] = static_cast<std::byte>((first >> (i * 8)) & 0xFFU);
        encoded[i + 4] = static_cast<std::byte>((second >> (i * 8)) & 0xFFU);
    }
    return encoded;
}

[[nodiscard]] std::array<std::byte, kChannelDigestBytes> ComputeValidatedImageBlake3(
    const BgraImageView& image, const ValidatedView& validated) noexcept
{
    pbprotocol::Blake3Hasher hasher;
    hasher.Update(std::as_bytes(std::span{kImageDigestDomain, sizeof(kImageDigestDomain)}));
    const auto dimensions = EncodeUint32Pair(image.width, image.height);
    hasher.Update(dimensions);
    for (std::uint32_t row = 0; row < image.height; row++)
    {
        const std::size_t offset = static_cast<std::size_t>(row) * image.rowPitch;
        hasher.Update(image.pixels.subspan(offset, static_cast<std::size_t>(validated.rowBytes)));
    }
    return hasher.Finalize();
}

[[nodiscard]] ChannelTransformResult<BgraImage> CloneView(const BgraImageView& source,
    const ValidatedView& validated)
{
    try
    {
        BgraImage output;
        output.width = source.width;
        output.height = source.height;
        output.rowPitch = static_cast<std::size_t>(validated.rowBytes);
        output.pixels.resize(static_cast<std::size_t>(validated.activeBytes));
        for (std::uint32_t row = 0; row < source.height; row++)
        {
            const std::size_t sourceOffset = static_cast<std::size_t>(row) * source.rowPitch;
            const std::size_t outputOffset = static_cast<std::size_t>(row) * output.rowPitch;
            std::memcpy(output.pixels.data() + outputOffset, source.pixels.data() + sourceOffset, output.rowPitch);
        }
        return ChannelTransformResult<BgraImage>::Success(std::move(output));
    }
    catch (const std::bad_alloc&)
    {
        return ChannelTransformResult<BgraImage>::Failure(ChannelTransformErrorCode::AllocationFailure);
    }
    catch (const std::length_error&)
    {
        return ChannelTransformResult<BgraImage>::Failure(ChannelTransformErrorCode::AllocationFailure);
    }
}

[[nodiscard]] bool TryAddWithin(const std::uint64_t left, const std::uint64_t right,
    const std::uint64_t limit, std::uint64_t& output) noexcept
{
    const auto result = pbprotocol::CheckedAddUint64WithinLimit(left, right, limit);
    if (!result)
    {
        return false;
    }
    output = result.Value();
    return true;
}

[[nodiscard]] ChannelTransformErrorCode ValidateResample(const ResampleTransform& transform,
    const ChannelTransformPolicy& policy, std::uint64_t& outputBytes, std::uint64_t& workUnits) noexcept
{
    if (transform.outputWidth == 0 || transform.outputHeight == 0 || !std::isfinite(transform.scaleX) ||
        !std::isfinite(transform.scaleY) || !std::isfinite(transform.originX) || !std::isfinite(transform.originY) ||
        (transform.filter != ResampleFilter::Area && transform.filter != ResampleFilter::Bilinear) ||
        transform.scaleX < policy.minimumScale || transform.scaleX > policy.maximumScale ||
        transform.scaleY < policy.minimumScale || transform.scaleY > policy.maximumScale)
    {
        return ChannelTransformErrorCode::InvalidParameter;
    }
    const double maximumOriginMagnitude = static_cast<double>(policy.maximumDimension) * 2.0;
    if (std::abs(transform.originX) > maximumOriginMagnitude || std::abs(transform.originY) > maximumOriginMagnitude)
    {
        return ChannelTransformErrorCode::InvalidParameter;
    }
    if (transform.outputWidth > policy.maximumDimension || transform.outputHeight > policy.maximumDimension)
    {
        return ChannelTransformErrorCode::DimensionLimitExceeded;
    }
    const auto pixelsResult = pbprotocol::CheckedMultiplyUint64(transform.outputWidth, transform.outputHeight);
    if (!pixelsResult || pixelsResult.Value() > policy.maximumPixels)
    {
        return ChannelTransformErrorCode::PixelLimitExceeded;
    }
    const auto bytesResult = pixelsResult ? pbprotocol::CheckedMultiplyUint64(pixelsResult.Value(), kBgraPixelBytes) : pixelsResult;
    if (!bytesResult || bytesResult.Value() > policy.maximumResidentBytes)
    {
        return ChannelTransformErrorCode::ByteLimitExceeded;
    }
    const std::uint64_t sampleColumns = transform.filter == ResampleFilter::Bilinear ? 2ULL :
        static_cast<std::uint64_t>(std::ceil(1.0 / transform.scaleX)) + 1ULL;
    const std::uint64_t sampleRows = transform.filter == ResampleFilter::Bilinear ? 2ULL :
        static_cast<std::uint64_t>(std::ceil(1.0 / transform.scaleY)) + 1ULL;
    const auto samplesResult = pbprotocol::CheckedMultiplyUint64(sampleColumns, sampleRows);
    const auto workResult = samplesResult ?
        pbprotocol::CheckedMultiplyUint64(pixelsResult.Value(), samplesResult.Value()) : samplesResult;
    if (!workResult || workResult.Value() > policy.maximumWorkUnits)
    {
        return ChannelTransformErrorCode::WorkLimitExceeded;
    }
    outputBytes = bytesResult.Value();
    workUnits = workResult.Value();
    return ChannelTransformErrorCode::None;
}

[[nodiscard]] std::byte RoundedByte(const double value) noexcept
{
    const long rounded = std::lround(std::clamp(value, 0.0, 255.0));
    return static_cast<std::byte>(static_cast<unsigned char>(rounded));
}

void WriteBorder(std::byte* destination, const std::array<std::byte, 4>& border) noexcept
{
    std::memcpy(destination, border.data(), border.size());
}

void SampleBilinear(const BgraImageView& source, const ResampleTransform& transform,
    const std::uint32_t outputX, const std::uint32_t outputY, std::byte* destination) noexcept
{
    const double logicalEdgeX = (static_cast<double>(outputX) + 0.5 - transform.originX) / transform.scaleX;
    const double logicalEdgeY = (static_cast<double>(outputY) + 0.5 - transform.originY) / transform.scaleY;
    if (logicalEdgeX < 0 || logicalEdgeX >= source.width || logicalEdgeY < 0 || logicalEdgeY >= source.height)
    {
        WriteBorder(destination, transform.borderBgra);
        return;
    }
    const double sourceCenterX = logicalEdgeX - 0.5;
    const double sourceCenterY = logicalEdgeY - 0.5;
    const std::int64_t left = static_cast<std::int64_t>(std::floor(sourceCenterX));
    const std::int64_t top = static_cast<std::int64_t>(std::floor(sourceCenterY));
    const double fractionX = sourceCenterX - static_cast<double>(left);
    const double fractionY = sourceCenterY - static_cast<double>(top);
    const std::uint32_t x0 = static_cast<std::uint32_t>(std::clamp<std::int64_t>(left, 0, source.width - 1ULL));
    const std::uint32_t y0 = static_cast<std::uint32_t>(std::clamp<std::int64_t>(top, 0, source.height - 1ULL));
    const std::uint32_t x1 = static_cast<std::uint32_t>(std::clamp<std::int64_t>(left + 1, 0, source.width - 1ULL));
    const std::uint32_t y1 = static_cast<std::uint32_t>(std::clamp<std::int64_t>(top + 1, 0, source.height - 1ULL));
    const std::byte* row0 = source.pixels.data() + static_cast<std::size_t>(y0) * source.rowPitch;
    const std::byte* row1 = source.pixels.data() + static_cast<std::size_t>(y1) * source.rowPitch;
    for (std::size_t channel = 0; channel < 4; channel++)
    {
        const double topValue = std::to_integer<unsigned char>(row0[static_cast<std::size_t>(x0) * 4 + channel]) * (1.0 - fractionX) +
            std::to_integer<unsigned char>(row0[static_cast<std::size_t>(x1) * 4 + channel]) * fractionX;
        const double bottomValue = std::to_integer<unsigned char>(row1[static_cast<std::size_t>(x0) * 4 + channel]) * (1.0 - fractionX) +
            std::to_integer<unsigned char>(row1[static_cast<std::size_t>(x1) * 4 + channel]) * fractionX;
        destination[channel] = RoundedByte(topValue * (1.0 - fractionY) + bottomValue * fractionY);
    }
}

void SampleArea(const BgraImageView& source, const ResampleTransform& transform,
    const std::uint32_t outputX, const std::uint32_t outputY, std::byte* destination) noexcept
{
    const double sourceLeft = (static_cast<double>(outputX) - transform.originX) / transform.scaleX;
    const double sourceRight = (static_cast<double>(outputX) + 1.0 - transform.originX) / transform.scaleX;
    const double sourceTop = (static_cast<double>(outputY) - transform.originY) / transform.scaleY;
    const double sourceBottom = (static_cast<double>(outputY) + 1.0 - transform.originY) / transform.scaleY;
    const double totalArea = (sourceRight - sourceLeft) * (sourceBottom - sourceTop);
    const double clippedLeft = std::clamp(sourceLeft, 0.0, static_cast<double>(source.width));
    const double clippedRight = std::clamp(sourceRight, 0.0, static_cast<double>(source.width));
    const double clippedTop = std::clamp(sourceTop, 0.0, static_cast<double>(source.height));
    const double clippedBottom = std::clamp(sourceBottom, 0.0, static_cast<double>(source.height));
    if (clippedLeft >= clippedRight || clippedTop >= clippedBottom)
    {
        WriteBorder(destination, transform.borderBgra);
        return;
    }
    const double insideArea = (clippedRight - clippedLeft) * (clippedBottom - clippedTop);
    std::array<double, 4> sums{};
    for (std::size_t channel = 0; channel < 4; channel++)
    {
        sums[channel] = std::to_integer<unsigned char>(transform.borderBgra[channel]) * (totalArea - insideArea);
    }
    const std::int64_t firstX = static_cast<std::int64_t>(std::floor(clippedLeft));
    const std::int64_t endX = static_cast<std::int64_t>(std::ceil(clippedRight));
    const std::int64_t firstY = static_cast<std::int64_t>(std::floor(clippedTop));
    const std::int64_t endY = static_cast<std::int64_t>(std::ceil(clippedBottom));
    for (std::int64_t sourceY = firstY; sourceY < endY; sourceY++)
    {
        if (sourceY < 0 || sourceY >= source.height)
        {
            continue;
        }
        const double weightY = std::max(0.0, std::min(clippedBottom, static_cast<double>(sourceY + 1)) -
            std::max(clippedTop, static_cast<double>(sourceY)));
        const std::byte* row = source.pixels.data() + static_cast<std::size_t>(sourceY) * source.rowPitch;
        for (std::int64_t sourceX = firstX; sourceX < endX; sourceX++)
        {
            if (sourceX < 0 || sourceX >= source.width)
            {
                continue;
            }
            const double weightX = std::max(0.0, std::min(clippedRight, static_cast<double>(sourceX + 1)) -
                std::max(clippedLeft, static_cast<double>(sourceX)));
            const double weight = weightX * weightY;
            const std::byte* pixel = row + static_cast<std::size_t>(sourceX) * 4;
            for (std::size_t channel = 0; channel < 4; channel++)
            {
                sums[channel] += std::to_integer<unsigned char>(pixel[channel]) * weight;
            }
        }
    }
    for (std::size_t channel = 0; channel < 4; channel++)
    {
        destination[channel] = RoundedByte(sums[channel] / totalArea);
    }
}

[[nodiscard]] ChannelTransformResult<BgraImage> ApplyResample(const BgraImageView& source,
    const ResampleTransform& transform, const std::uint64_t outputBytes)
{
    try
    {
        BgraImage output;
        output.width = transform.outputWidth;
        output.height = transform.outputHeight;
        output.rowPitch = static_cast<std::size_t>(transform.outputWidth) * 4;
        output.pixels.resize(static_cast<std::size_t>(outputBytes));
        if (transform.outputWidth == source.width && transform.outputHeight == source.height &&
            transform.scaleX == 1.0 && transform.scaleY == 1.0 && transform.originX == 0.0 && transform.originY == 0.0)
        {
            for (std::uint32_t row = 0; row < source.height; row++)
            {
                std::memcpy(output.pixels.data() + static_cast<std::size_t>(row) * output.rowPitch,
                    source.pixels.data() + static_cast<std::size_t>(row) * source.rowPitch, output.rowPitch);
            }
            return ChannelTransformResult<BgraImage>::Success(std::move(output));
        }
        for (std::uint32_t outputY = 0; outputY < output.height; outputY++)
        {
            for (std::uint32_t outputX = 0; outputX < output.width; outputX++)
            {
                std::byte* destination = output.pixels.data() + static_cast<std::size_t>(outputY) * output.rowPitch +
                    static_cast<std::size_t>(outputX) * 4;
                if (transform.filter == ResampleFilter::Bilinear)
                {
                    SampleBilinear(source, transform, outputX, outputY, destination);
                }
                else
                {
                    SampleArea(source, transform, outputX, outputY, destination);
                }
            }
        }
        return ChannelTransformResult<BgraImage>::Success(std::move(output));
    }
    catch (const std::bad_alloc&)
    {
        return ChannelTransformResult<BgraImage>::Failure(ChannelTransformErrorCode::AllocationFailure);
    }
    catch (const std::length_error&)
    {
        return ChannelTransformResult<BgraImage>::Failure(ChannelTransformErrorCode::AllocationFailure);
    }
}

[[nodiscard]] ChannelTransformErrorCode ValidateBlockReplacement(const BgraImage& current,
    const std::optional<BgraImage>& reference, const BlockReplacementTransform& transform) noexcept
{
    if (!reference)
    {
        return ChannelTransformErrorCode::MissingReference;
    }
    if (current.width != reference->width || current.height != reference->height)
    {
        return ChannelTransformErrorCode::ReferenceGeometryMismatch;
    }
    if (transform.width == 0 || transform.height == 0 || transform.x >= current.width || transform.y >= current.height ||
        transform.width > current.width - transform.x || transform.height > current.height - transform.y)
    {
        return ChannelTransformErrorCode::RectangleOutOfBounds;
    }
    return ChannelTransformErrorCode::None;
}

void ApplyBlockReplacement(BgraImage& current, const BgraImage& reference,
    const BlockReplacementTransform& transform) noexcept
{
    const std::size_t copyBytes = static_cast<std::size_t>(transform.width) * 4;
    for (std::uint32_t row = 0; row < transform.height; row++)
    {
        const std::size_t currentOffset = static_cast<std::size_t>(transform.y + row) * current.rowPitch +
            static_cast<std::size_t>(transform.x) * 4;
        const std::size_t referenceOffset = static_cast<std::size_t>(transform.y + row) * reference.rowPitch +
            static_cast<std::size_t>(transform.x) * 4;
        std::memcpy(current.pixels.data() + currentOffset, reference.pixels.data() + referenceOffset, copyBytes);
    }
}

void AppendUnsigned(std::string& output, const std::uint64_t value)
{
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{})
    {
        throw std::length_error("integer serialization failed");
    }
    output.append(buffer.data(), converted.ptr);
}

void AppendUint64HexString(std::string& output, const std::uint64_t value)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    output.push_back('"');
    for (std::size_t nibble = 0; nibble < 16; nibble++)
    {
        const std::size_t shift = (15 - nibble) * 4;
        output.push_back(kHexDigits[(value >> shift) & 0x0FU]);
    }
    output.push_back('"');
}

void AppendBinary64(std::string& output, const double value)
{
    const double normalized = value == 0.0 ? 0.0 : value;
    AppendUint64HexString(output, std::bit_cast<std::uint64_t>(normalized));
}

void AppendDigest(std::string& output, const std::array<std::byte, kChannelDigestBytes>& digest)
{
    output.push_back('"');
    output.append(ChannelDigestToHex(digest));
    output.push_back('"');
}

void AppendTransformParameters(std::string& output, const ChannelTransform& transform)
{
    std::visit([&output](const auto& typedTransform)
    {
        using TransformType = std::decay_t<decltype(typedTransform)>;
        if constexpr (std::is_same_v<TransformType, ResampleTransform>)
        {
            output.append("{\"filter\":\"");
            output.append(typedTransform.filter == ResampleFilter::Area ? "area" : "bilinear");
            output.append("\",\"outputWidth\":");
            AppendUnsigned(output, typedTransform.outputWidth);
            output.append(",\"outputHeight\":");
            AppendUnsigned(output, typedTransform.outputHeight);
            output.append(",\"scaleXBinary64\":");
            AppendBinary64(output, typedTransform.scaleX);
            output.append(",\"scaleYBinary64\":");
            AppendBinary64(output, typedTransform.scaleY);
            output.append(",\"originXBinary64\":");
            AppendBinary64(output, typedTransform.originX);
            output.append(",\"originYBinary64\":");
            AppendBinary64(output, typedTransform.originY);
            output.append(",\"borderBgra\":[");
            for (std::size_t channel = 0; channel < typedTransform.borderBgra.size(); channel++)
            {
                if (channel != 0)
                {
                    output.push_back(',');
                }
                AppendUnsigned(output, std::to_integer<unsigned char>(typedTransform.borderBgra[channel]));
            }
            output.append("]}");
        }
        else
        {
            output.append("{\"x\":");
            AppendUnsigned(output, typedTransform.x);
            output.append(",\"y\":");
            AppendUnsigned(output, typedTransform.y);
            output.append(",\"width\":");
            AppendUnsigned(output, typedTransform.width);
            output.append(",\"height\":");
            AppendUnsigned(output, typedTransform.height);
            output.push_back('}');
        }
    }, transform);
}

[[nodiscard]] std::string SerializeManifest(const ChannelTransformExecution& execution)
{
    std::string output;
    output.reserve(768 + execution.records.size() * 640);
    output.append("{\"schema\":\"");
    output.append(kChannelManifestSchema);
    output.append("\",\"version\":");
    AppendUnsigned(output, kChannelManifestVersion);
    output.append(",\"seedHex\":");
    AppendUint64HexString(output, execution.seed);
    output.append(",\"source\":{\"format\":\"bgra8\",\"width\":");
    AppendUnsigned(output, execution.sourceWidth);
    output.append(",\"height\":");
    AppendUnsigned(output, execution.sourceHeight);
    output.append(",\"blake3\":");
    AppendDigest(output, execution.sourceBlake3);
    output.append("},\"referenceBlake3\":");
    if (execution.referenceBlake3)
    {
        AppendDigest(output, *execution.referenceBlake3);
    }
    else
    {
        output.append("null");
    }
    output.append(",\"transforms\":[");
    for (std::size_t i = 0; i < execution.records.size(); i++)
    {
        const ChannelTransformRecord& record = execution.records[i];
        if (i != 0)
        {
            output.push_back(',');
        }
        output.append("{\"index\":");
        AppendUnsigned(output, record.index);
        output.append(",\"kind\":\"");
        output.append(std::holds_alternative<ResampleTransform>(record.transform) ? "resample" : "block-replacement");
        output.append("\",\"input\":{\"width\":");
        AppendUnsigned(output, record.inputWidth);
        output.append(",\"height\":");
        AppendUnsigned(output, record.inputHeight);
        output.append(",\"blake3\":");
        AppendDigest(output, record.inputBlake3);
        output.append("},\"referenceInputBlake3\":");
        if (record.referenceInputBlake3)
        {
            AppendDigest(output, *record.referenceInputBlake3);
        }
        else
        {
            output.append("null");
        }
        output.append(",\"parameters\":");
        AppendTransformParameters(output, record.transform);
        output.append(",\"output\":{\"width\":");
        AppendUnsigned(output, record.outputWidth);
        output.append(",\"height\":");
        AppendUnsigned(output, record.outputHeight);
        output.append(",\"blake3\":");
        AppendDigest(output, record.outputBlake3);
        output.append("}}");
    }
    output.append("],\"output\":{\"format\":\"bgra8\",\"width\":");
    AppendUnsigned(output, execution.output.width);
    output.append(",\"height\":");
    AppendUnsigned(output, execution.output.height);
    output.append(",\"blake3\":");
    AppendDigest(output, execution.outputBlake3);
    output.append("}}");
    return output;
}

} // namespace

ChannelTransformResult<ChannelTransformExecution> ExecuteChannelTransformPlan(const BgraImageView& source,
    const std::optional<BgraImageView>& reference, const ChannelTransformPlan& plan,
    const ChannelTransformPolicy& policy)
{
    if (!IsValidPolicy(policy))
    {
        return ChannelTransformResult<ChannelTransformExecution>::Failure(ChannelTransformErrorCode::InvalidPolicy);
    }
    const ValidationResult sourceValidation = ValidateView(source, policy);
    if (!sourceValidation.IsValid())
    {
        return ChannelTransformResult<ChannelTransformExecution>::Failure(sourceValidation.code);
    }
    std::optional<ValidationResult> referenceValidation;
    if (reference)
    {
        referenceValidation = ValidateView(*reference, policy);
        if (!referenceValidation->IsValid())
        {
            return ChannelTransformResult<ChannelTransformExecution>::Failure(referenceValidation->code);
        }
        if (source.width != reference->width || source.height != reference->height)
        {
            return ChannelTransformResult<ChannelTransformExecution>::Failure(
                ChannelTransformErrorCode::ReferenceGeometryMismatch);
        }
    }
    if (plan.transforms.size() > policy.maximumTransforms)
    {
        return ChannelTransformResult<ChannelTransformExecution>::Failure(ChannelTransformErrorCode::TooManyTransforms);
    }
    std::uint64_t initialResidentBytes = sourceValidation.view.activeBytes;
    if (referenceValidation && !TryAddWithin(initialResidentBytes, referenceValidation->view.activeBytes,
        policy.maximumResidentBytes, initialResidentBytes))
    {
        return ChannelTransformResult<ChannelTransformExecution>::Failure(ChannelTransformErrorCode::ByteLimitExceeded);
    }
    auto currentResult = CloneView(source, sourceValidation.view);
    if (!currentResult)
    {
        return ChannelTransformResult<ChannelTransformExecution>::Failure(currentResult.Error().code);
    }
    BgraImage current = std::move(currentResult).Value();
    std::optional<BgraImage> currentReference;
    if (reference && referenceValidation)
    {
        auto referenceResult = CloneView(*reference, referenceValidation->view);
        if (!referenceResult)
        {
            return ChannelTransformResult<ChannelTransformExecution>::Failure(referenceResult.Error().code);
        }
        currentReference = std::move(referenceResult).Value();
    }
    ChannelTransformExecution execution;
    execution.seed = plan.seed;
    execution.sourceWidth = source.width;
    execution.sourceHeight = source.height;
    execution.sourceBlake3 = ComputeValidatedImageBlake3(source, sourceValidation.view);
    if (reference && referenceValidation)
    {
        execution.referenceBlake3 = ComputeValidatedImageBlake3(*reference, referenceValidation->view);
    }
    try
    {
        execution.records.reserve(plan.transforms.size());
    }
    catch (const std::bad_alloc&)
    {
        return ChannelTransformResult<ChannelTransformExecution>::Failure(ChannelTransformErrorCode::AllocationFailure);
    }
    catch (const std::length_error&)
    {
        return ChannelTransformResult<ChannelTransformExecution>::Failure(ChannelTransformErrorCode::AllocationFailure);
    }
    std::uint64_t totalWorkUnits = 0;
    for (std::size_t transformIndex = 0; transformIndex < plan.transforms.size(); transformIndex++)
    {
        const ChannelTransform& transform = plan.transforms[transformIndex];
        ChannelTransformRecord record;
        record.index = transformIndex;
        record.transform = transform;
        record.inputWidth = current.width;
        record.inputHeight = current.height;
        const ValidatedView currentValidated{static_cast<std::uint64_t>(current.rowPitch), current.pixels.size()};
        record.inputBlake3 = ComputeValidatedImageBlake3(current.View(), currentValidated);
        if (currentReference)
        {
            const ValidatedView referenceValidated{static_cast<std::uint64_t>(currentReference->rowPitch),
                currentReference->pixels.size()};
            record.referenceInputBlake3 = ComputeValidatedImageBlake3(currentReference->View(), referenceValidated);
        }
        if (const auto* resample = std::get_if<ResampleTransform>(&transform))
        {
            std::uint64_t outputBytes = 0;
            std::uint64_t workUnits = 0;
            const ChannelTransformErrorCode validation = ValidateResample(*resample, policy, outputBytes, workUnits);
            if (validation != ChannelTransformErrorCode::None)
            {
                return ChannelTransformResult<ChannelTransformExecution>::Failure(validation, transformIndex);
            }
            std::uint64_t transformWorkUnits = workUnits;
            if (currentReference && !TryAddWithin(transformWorkUnits, workUnits, policy.maximumWorkUnits, transformWorkUnits))
            {
                return ChannelTransformResult<ChannelTransformExecution>::Failure(
                    ChannelTransformErrorCode::WorkLimitExceeded, transformIndex);
            }
            if (!TryAddWithin(totalWorkUnits, transformWorkUnits, policy.maximumWorkUnits, totalWorkUnits))
            {
                return ChannelTransformResult<ChannelTransformExecution>::Failure(
                    ChannelTransformErrorCode::WorkLimitExceeded, transformIndex);
            }
            std::uint64_t currentAndReferenceBytes = current.pixels.size();
            if (currentReference && !TryAddWithin(currentAndReferenceBytes, currentReference->pixels.size(),
                policy.maximumResidentBytes, currentAndReferenceBytes))
            {
                return ChannelTransformResult<ChannelTransformExecution>::Failure(
                    ChannelTransformErrorCode::ByteLimitExceeded, transformIndex);
            }
            std::uint64_t allocationPeak = 0;
            if (!TryAddWithin(currentAndReferenceBytes, outputBytes, policy.maximumResidentBytes, allocationPeak))
            {
                return ChannelTransformResult<ChannelTransformExecution>::Failure(
                    ChannelTransformErrorCode::ByteLimitExceeded, transformIndex);
            }
            if (currentReference)
            {
                std::uint64_t referenceAllocationPeak = 0;
                std::uint64_t newCurrentAndOldReference = 0;
                if (!TryAddWithin(outputBytes, currentReference->pixels.size(), policy.maximumResidentBytes,
                    newCurrentAndOldReference) || !TryAddWithin(newCurrentAndOldReference, outputBytes,
                    policy.maximumResidentBytes, referenceAllocationPeak))
                {
                    return ChannelTransformResult<ChannelTransformExecution>::Failure(
                        ChannelTransformErrorCode::ByteLimitExceeded, transformIndex);
                }
            }
            auto transformedResult = ApplyResample(current.View(), *resample, outputBytes);
            if (!transformedResult)
            {
                return ChannelTransformResult<ChannelTransformExecution>::Failure(transformedResult.Error().code,
                    transformIndex);
            }
            current = std::move(transformedResult).Value();
            if (currentReference)
            {
                auto transformedReferenceResult = ApplyResample(currentReference->View(), *resample, outputBytes);
                if (!transformedReferenceResult)
                {
                    return ChannelTransformResult<ChannelTransformExecution>::Failure(
                        transformedReferenceResult.Error().code, transformIndex);
                }
                currentReference = std::move(transformedReferenceResult).Value();
            }
        }
        else if (const auto* block = std::get_if<BlockReplacementTransform>(&transform))
        {
            const ChannelTransformErrorCode validation = ValidateBlockReplacement(current, currentReference, *block);
            if (validation != ChannelTransformErrorCode::None)
            {
                return ChannelTransformResult<ChannelTransformExecution>::Failure(validation, transformIndex);
            }
            const auto blockPixelsResult = pbprotocol::CheckedMultiplyUint64(block->width, block->height);
            if (!blockPixelsResult || !TryAddWithin(totalWorkUnits, blockPixelsResult.Value(),
                policy.maximumWorkUnits, totalWorkUnits))
            {
                return ChannelTransformResult<ChannelTransformExecution>::Failure(
                    ChannelTransformErrorCode::WorkLimitExceeded, transformIndex);
            }
            ApplyBlockReplacement(current, *currentReference, *block);
        }
        else
        {
            return ChannelTransformResult<ChannelTransformExecution>::Failure(
                ChannelTransformErrorCode::InternalInvariantViolation, transformIndex);
        }
        record.outputWidth = current.width;
        record.outputHeight = current.height;
        const ValidatedView outputValidated{static_cast<std::uint64_t>(current.rowPitch), current.pixels.size()};
        record.outputBlake3 = ComputeValidatedImageBlake3(current.View(), outputValidated);
        try
        {
            execution.records.push_back(std::move(record));
        }
        catch (const std::bad_alloc&)
        {
            return ChannelTransformResult<ChannelTransformExecution>::Failure(
                ChannelTransformErrorCode::AllocationFailure, transformIndex);
        }
        catch (const std::length_error&)
        {
            return ChannelTransformResult<ChannelTransformExecution>::Failure(
                ChannelTransformErrorCode::AllocationFailure, transformIndex);
        }
    }
    execution.output = std::move(current);
    const ValidatedView outputValidated{static_cast<std::uint64_t>(execution.output.rowPitch), execution.output.pixels.size()};
    execution.outputBlake3 = ComputeValidatedImageBlake3(execution.output.View(), outputValidated);
    try
    {
        execution.canonicalManifestJson = SerializeManifest(execution);
    }
    catch (const std::bad_alloc&)
    {
        return ChannelTransformResult<ChannelTransformExecution>::Failure(ChannelTransformErrorCode::AllocationFailure);
    }
    catch (const std::length_error&)
    {
        return ChannelTransformResult<ChannelTransformExecution>::Failure(ChannelTransformErrorCode::AllocationFailure);
    }
    execution.manifestBlake3 = pbprotocol::ComputeBlake3Digest(std::as_bytes(std::span{
        execution.canonicalManifestJson.data(), execution.canonicalManifestJson.size()}));
    return ChannelTransformResult<ChannelTransformExecution>::Success(std::move(execution));
}

std::string ChannelDigestToHex(const std::span<const std::byte, kChannelDigestBytes> digest)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string output(kChannelDigestBytes * 2, '0');
    for (std::size_t i = 0; i < digest.size(); i++)
    {
        const unsigned int value = std::to_integer<unsigned int>(digest[i]);
        output[i * 2] = kHexDigits[value >> 4];
        output[i * 2 + 1] = kHexDigits[value & 0x0FU];
    }
    return output;
}

const char* GetChannelTransformErrorName(const ChannelTransformErrorCode code) noexcept
{
    switch (code)
    {
    case ChannelTransformErrorCode::None:
        return "None";
    case ChannelTransformErrorCode::InvalidView:
        return "InvalidView";
    case ChannelTransformErrorCode::InvalidPolicy:
        return "InvalidPolicy";
    case ChannelTransformErrorCode::TooManyTransforms:
        return "TooManyTransforms";
    case ChannelTransformErrorCode::InvalidParameter:
        return "InvalidParameter";
    case ChannelTransformErrorCode::DimensionLimitExceeded:
        return "DimensionLimitExceeded";
    case ChannelTransformErrorCode::PixelLimitExceeded:
        return "PixelLimitExceeded";
    case ChannelTransformErrorCode::ByteLimitExceeded:
        return "ByteLimitExceeded";
    case ChannelTransformErrorCode::WorkLimitExceeded:
        return "WorkLimitExceeded";
    case ChannelTransformErrorCode::MissingReference:
        return "MissingReference";
    case ChannelTransformErrorCode::ReferenceGeometryMismatch:
        return "ReferenceGeometryMismatch";
    case ChannelTransformErrorCode::RectangleOutOfBounds:
        return "RectangleOutOfBounds";
    case ChannelTransformErrorCode::AllocationFailure:
        return "AllocationFailure";
    case ChannelTransformErrorCode::InternalInvariantViolation:
        return "InternalInvariantViolation";
    }
    return "Unknown";
}

} // namespace pbremotevisualsimulator
