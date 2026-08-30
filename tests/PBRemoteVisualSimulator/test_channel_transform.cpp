#include "pbremotevisualsimulator/channel_transform.h"

#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

struct PaddedImage
{
    std::vector<std::byte> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t rowPitch = 0;

    [[nodiscard]] pbremotevisualsimulator::BgraImageView View() const noexcept
    {
        return {pixels, width, height, rowPitch};
    }
};

PaddedImage MakePatternImage(const std::uint32_t width, const std::uint32_t height,
    const std::size_t padding, const std::uint64_t seed)
{
    PaddedImage image;
    image.width = width;
    image.height = height;
    image.rowPitch = static_cast<std::size_t>(width) * 4 + padding;
    image.pixels.assign(image.rowPitch * height, std::byte{0xA5});
    std::uint64_t state = seed;
    for (std::uint32_t y = 0; y < height; y++)
    {
        for (std::uint32_t x = 0; x < width; x++)
        {
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            const std::size_t offset = static_cast<std::size_t>(y) * image.rowPitch + static_cast<std::size_t>(x) * 4;
            image.pixels[offset] = static_cast<std::byte>(state >> 56);
            image.pixels[offset + 1] = static_cast<std::byte>((state >> 40) ^ x);
            image.pixels[offset + 2] = static_cast<std::byte>((state >> 24) ^ y);
            image.pixels[offset + 3] = std::byte{255};
        }
    }
    return image;
}

PaddedImage MakeConstantImage(const std::uint32_t width, const std::uint32_t height,
    const std::array<std::byte, 4>& bgra)
{
    PaddedImage image;
    image.width = width;
    image.height = height;
    image.rowPitch = static_cast<std::size_t>(width) * 4;
    image.pixels.resize(image.rowPitch * height);
    for (std::size_t offset = 0; offset < image.pixels.size(); offset += 4)
    {
        std::copy(bgra.begin(), bgra.end(), image.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    return image;
}

std::vector<std::byte> TightPixels(const PaddedImage& image)
{
    std::vector<std::byte> output(static_cast<std::size_t>(image.width) * image.height * 4);
    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4;
    for (std::uint32_t row = 0; row < image.height; row++)
    {
        std::copy_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * image.rowPitch),
            rowBytes, output.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * rowBytes));
    }
    return output;
}

std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeLowFpsRecord(const std::uint64_t sessionTag,
    const std::uint64_t frameSequence)
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = pbmodulation::kRemoteVisualLowFpsLayoutVersion;
    record.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    record.sessionTag.value = sessionTag;
    record.frameSequence = frameSequence;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bytes{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
    return bytes;
}

std::vector<std::byte> MakeDiagnosticData(const std::span<const std::byte> record)
{
    std::vector<std::byte> data(pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, data));
    return data;
}

pbremotevisualsimulator::BgraImageView MakeBgraView(const std::vector<std::byte>& pixels)
{
    return {pixels, pbmodulation::kLocalDesktopCanvasWidth, pbmodulation::kLocalDesktopCanvasHeight,
        static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) * 4};
}

pbremotevisualsimulator::BlockReplacementTransform FindStaleRegionTransform(const std::uint64_t sessionTag,
    const std::uint64_t previousSequence, const std::uint64_t currentSequence)
{
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        if (mapping.role != pbmodulation::RemoteVisualTileRole::FreshnessTag)
        {
            continue;
        }
        bool previousBit = false;
        bool currentBit = false;
        REQUIRE(pbmodulation::GetRemoteVisualFreshnessBit(sessionTag, previousSequence, physical, previousBit));
        REQUIRE(pbmodulation::GetRemoteVisualFreshnessBit(sessionTag, currentSequence, physical, currentBit));
        if (previousBit != currentBit)
        {
            const std::uint32_t regionColumn = mapping.regionId % pbmodulation::kRemoteVisualFreshnessRegionColumns;
            const std::uint32_t regionRow = mapping.regionId / pbmodulation::kRemoteVisualFreshnessRegionColumns;
            return {96 + regionColumn * 128, 96 + regionRow * 128, 128, 128};
        }
    }
    throw std::runtime_error("No freshness tag changed between the selected frame sequences");
}

} // namespace

TEST_CASE("RemoteVisual channel identity canonicalizes padding and emits an auditable manifest",
    "[remote-visual][simulator][identity][manifest]")
{
    const PaddedImage padded = MakePatternImage(17, 11, 13, 0xA92B3C4D5E6F7081ULL);
    const PaddedImage differentlyPadded = [&padded]()
    {
        PaddedImage output;
        output.width = padded.width;
        output.height = padded.height;
        output.rowPitch = static_cast<std::size_t>(padded.width) * 4 + 29;
        output.pixels.assign(output.rowPitch * output.height, std::byte{0x3C});
        const std::size_t rowBytes = static_cast<std::size_t>(padded.width) * 4;
        for (std::uint32_t row = 0; row < padded.height; row++)
        {
            std::copy_n(padded.pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * padded.rowPitch),
                rowBytes, output.pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * output.rowPitch));
        }
        return output;
    }();
    const pbremotevisualsimulator::ChannelTransformPlan plan{0x1020304050607080ULL, {}};
    const auto firstResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(padded.View(), std::nullopt, plan);
    const auto secondResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(differentlyPadded.View(), std::nullopt, plan);
    REQUIRE(firstResult);
    REQUIRE(secondResult);
    const auto& first = firstResult.Value();
    const auto& second = secondResult.Value();
    REQUIRE(first.records.empty());
    REQUIRE(first.manifestVersion == pbremotevisualsimulator::kChannelManifestVersion);
    REQUIRE(first.output.width == padded.width);
    REQUIRE(first.output.height == padded.height);
    REQUIRE(first.output.rowPitch == static_cast<std::size_t>(padded.width) * 4);
    REQUIRE(first.output.pixels == TightPixels(padded));
    REQUIRE(first.sourceBlake3 == first.outputBlake3);
    REQUIRE(first.sourceBlake3 == second.sourceBlake3);
    REQUIRE(first.output.pixels == second.output.pixels);
    REQUIRE(first.canonicalManifestJson == second.canonicalManifestJson);
    REQUIRE(first.manifestBlake3 == second.manifestBlake3);
    REQUIRE(first.canonicalManifestJson.starts_with("{\"schema\":\"PixelBridge.RemoteVisualChannelManifest.1\""));
    REQUIRE(first.canonicalManifestJson.find("\"seedHex\":\"1020304050607080\"") != std::string::npos);
    REQUIRE(first.canonicalManifestJson.find("\"transforms\":[]") != std::string::npos);
    REQUIRE(first.canonicalManifestJson.find(pbremotevisualsimulator::ChannelDigestToHex(first.outputBlake3)) !=
        std::string::npos);
}

TEST_CASE("RemoteVisual channel transform order, seed and per-step hashes are deterministic",
    "[remote-visual][simulator][determinism][pipeline]")
{
    const PaddedImage source = MakePatternImage(32, 24, 5, 0x1111222233334444ULL);
    const PaddedImage reference = MakePatternImage(32, 24, 7, 0x9999AAAABBBBCCCCULL);
    const pbremotevisualsimulator::ResampleTransform resample{48, 40, 1.25, 1.25, 2.5, 3.0,
        pbremotevisualsimulator::ResampleFilter::Area, {std::byte{3}, std::byte{5}, std::byte{7}, std::byte{255}}};
    const pbremotevisualsimulator::BlockReplacementTransform block{10, 9, 12, 8};
    const std::array<pbremotevisualsimulator::ChannelTransform, 2> transforms{resample, block};
    const pbremotevisualsimulator::ChannelTransformPlan plan{77, transforms};
    const auto firstResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(source.View(), reference.View(), plan);
    const auto secondResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(source.View(), reference.View(), plan);
    REQUIRE(firstResult);
    REQUIRE(secondResult);
    const auto& first = firstResult.Value();
    const auto& second = secondResult.Value();
    REQUIRE(first.output.pixels == second.output.pixels);
    REQUIRE(first.outputBlake3 == second.outputBlake3);
    REQUIRE(first.canonicalManifestJson == second.canonicalManifestJson);
    REQUIRE(first.manifestBlake3 == second.manifestBlake3);
    REQUIRE(first.records.size() == 2);
    REQUIRE(first.records[0].outputBlake3 == first.records[1].inputBlake3);
    REQUIRE(first.records.back().outputBlake3 == first.outputBlake3);
    REQUIRE(first.records[0].referenceInputBlake3.has_value());
    REQUIRE(first.records[1].referenceInputBlake3.has_value());

    const std::array<pbremotevisualsimulator::ChannelTransform, 1> referenceTransforms{resample};
    const auto referenceResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(reference.View(), std::nullopt,
        {77, referenceTransforms});
    REQUIRE(referenceResult);
    const auto& transformedReference = referenceResult.Value().output;
    const std::size_t blockRowBytes = static_cast<std::size_t>(block.width) * 4;
    for (std::uint32_t row = 0; row < block.height; row++)
    {
        const std::size_t offset = static_cast<std::size_t>(block.y + row) * first.output.rowPitch +
            static_cast<std::size_t>(block.x) * 4;
        REQUIRE(std::equal(first.output.pixels.begin() + static_cast<std::ptrdiff_t>(offset),
            first.output.pixels.begin() + static_cast<std::ptrdiff_t>(offset + blockRowBytes),
            transformedReference.pixels.begin() + static_cast<std::ptrdiff_t>(offset)));
    }

    const std::array<pbremotevisualsimulator::ChannelTransform, 2> reversedTransforms{
        pbremotevisualsimulator::BlockReplacementTransform{5, 4, 12, 8}, resample};
    const auto reversedResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(source.View(), reference.View(),
        {77, reversedTransforms});
    REQUIRE(reversedResult);
    REQUIRE(reversedResult.Value().outputBlake3 != first.outputBlake3);
    REQUIRE(reversedResult.Value().canonicalManifestJson != first.canonicalManifestJson);

    const auto differentSeedResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(source.View(), reference.View(),
        {78, transforms});
    REQUIRE(differentSeedResult);
    REQUIRE(differentSeedResult.Value().outputBlake3 == first.outputBlake3);
    REQUIRE(differentSeedResult.Value().manifestBlake3 != first.manifestBlake3);
}

TEST_CASE("RemoteVisual fixed spatial color and temporal impairments share one deterministic manifest pipeline",
    "[remote-visual][simulator][impairment][determinism]")
{
    const PaddedImage source = MakePatternImage(17, 13, 9, 0x123456789ABCDEF0ULL);
    const PaddedImage reference = MakePatternImage(17, 13, 11, 0x0FEDCBA987654321ULL);
    const std::array<pbremotevisualsimulator::ChannelTransform, 6> transforms{
        pbremotevisualsimulator::Kernel3x3Transform{pbremotevisualsimulator::FixedKernel3x3::GaussianBlur, 2},
        pbremotevisualsimulator::ColorTransferTransform{0.875, 12.5, 1.125},
        pbremotevisualsimulator::ChromaSubsample420Transform{},
        pbremotevisualsimulator::SolidOverlayTransform{2, 3, 5, 4,
            {std::byte{12}, std::byte{34}, std::byte{56}, std::byte{255}}, 97},
        pbremotevisualsimulator::ReferenceBlendTransform{64},
        pbremotevisualsimulator::CropTransform{1, 2, 14, 10}};
    const auto firstResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(source.View(), reference.View(),
        {0x8877665544332211ULL, transforms});
    const auto secondResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(source.View(), reference.View(),
        {0x8877665544332211ULL, transforms});
    REQUIRE(firstResult);
    REQUIRE(secondResult);
    const auto& first = firstResult.Value();
    const auto& second = secondResult.Value();
    REQUIRE(first.records.size() == transforms.size());
    REQUIRE(first.manifestVersion == pbremotevisualsimulator::kChannelManifestVersionV2);
    REQUIRE(first.output.width == 14);
    REQUIRE(first.output.height == 10);
    REQUIRE(first.output.pixels == second.output.pixels);
    REQUIRE(first.outputBlake3 == second.outputBlake3);
    REQUIRE(first.canonicalManifestJson == second.canonicalManifestJson);
    REQUIRE(first.manifestBlake3 == second.manifestBlake3);
    REQUIRE(first.canonicalManifestJson.starts_with("{\"schema\":\"PixelBridge.RemoteVisualChannelManifest.2\""));
    for (std::size_t index = 1; index < first.records.size(); index++)
    {
        REQUIRE(first.records[index - 1].outputBlake3 == first.records[index].inputBlake3);
    }
    for (const std::string kind : {"kernel-3x3", "color-transfer", "chroma-420", "solid-overlay",
        "reference-blend", "crop"})
    {
        REQUIRE(first.canonicalManifestJson.find("\"kind\":\"" + kind + "\"") != std::string::npos);
    }
    REQUIRE(first.canonicalManifestJson.find("\"gainBinary64\":\"3fec000000000000\"") != std::string::npos);
}

TEST_CASE("RemoteVisual impairment primitives preserve exact invariant cases",
    "[remote-visual][simulator][impairment][invariant]")
{
    const PaddedImage constant = MakeConstantImage(9, 7,
        {std::byte{87}, std::byte{87}, std::byte{87}, std::byte{255}});
    for (const auto kernel : {pbremotevisualsimulator::FixedKernel3x3::BoxBlur,
        pbremotevisualsimulator::FixedKernel3x3::GaussianBlur, pbremotevisualsimulator::FixedKernel3x3::Sharpen})
    {
        const std::array<pbremotevisualsimulator::ChannelTransform, 1> transforms{
            pbremotevisualsimulator::Kernel3x3Transform{kernel, 2}};
        const auto result = pbremotevisualsimulator::ExecuteChannelTransformPlan(constant.View(), std::nullopt,
            {1, transforms});
        REQUIRE(result);
        REQUIRE(result.Value().output.pixels == constant.pixels);
    }
    const std::array<pbremotevisualsimulator::ChannelTransform, 1> identityColor{
        pbremotevisualsimulator::ColorTransferTransform{1, 0, 1}};
    const auto colorResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(constant.View(), std::nullopt,
        {2, identityColor});
    REQUIRE(colorResult);
    REQUIRE(colorResult.Value().output.pixels == constant.pixels);
    const std::array<pbremotevisualsimulator::ChannelTransform, 1> chroma{
        pbremotevisualsimulator::ChromaSubsample420Transform{}};
    const auto chromaResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(constant.View(), std::nullopt,
        {3, chroma});
    REQUIRE(chromaResult);
    REQUIRE(chromaResult.Value().output.pixels == constant.pixels);

    const PaddedImage pattern = MakePatternImage(9, 7, 0, 4);
    const std::array<pbremotevisualsimulator::ChannelTransform, 1> crop{
        pbremotevisualsimulator::CropTransform{2, 1, 5, 4}};
    const auto cropResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(pattern.View(), std::nullopt,
        {4, crop});
    REQUIRE(cropResult);
    REQUIRE(cropResult.Value().output.width == 5);
    REQUIRE(cropResult.Value().output.height == 4);
    for (std::uint32_t row = 0; row < 4; row++)
    {
        const std::size_t sourceOffset = static_cast<std::size_t>(row + 1) * pattern.rowPitch + 2 * 4;
        const std::size_t outputOffset = static_cast<std::size_t>(row) * cropResult.Value().output.rowPitch;
        REQUIRE(std::equal(pattern.pixels.begin() + static_cast<std::ptrdiff_t>(sourceOffset),
            pattern.pixels.begin() + static_cast<std::ptrdiff_t>(sourceOffset + 5 * 4),
            cropResult.Value().output.pixels.begin() + static_cast<std::ptrdiff_t>(outputOffset)));
    }

    const PaddedImage reference = MakePatternImage(9, 7, 0, 5);
    const std::array<pbremotevisualsimulator::ChannelTransform, 1> selectReference{
        pbremotevisualsimulator::ReferenceBlendTransform{255}};
    const auto blendResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(pattern.View(), reference.View(),
        {5, selectReference});
    REQUIRE(blendResult);
    REQUIRE(blendResult.Value().output.pixels == reference.pixels);
}

TEST_CASE("RemoteVisual channel transforms reject malformed geometry and resource excess before publishing output",
    "[remote-visual][simulator][negative][resource]")
{
    const PaddedImage source = MakePatternImage(8, 8, 0, 1);
    PaddedImage truncated = source;
    truncated.pixels.pop_back();
    REQUIRE(pbremotevisualsimulator::ExecuteChannelTransformPlan(truncated.View(), std::nullopt, {1, {}}).Error().code ==
        pbremotevisualsimulator::ChannelTransformErrorCode::InvalidView);
    PaddedImage shortPitch = source;
    shortPitch.rowPitch--;
    REQUIRE(pbremotevisualsimulator::ExecuteChannelTransformPlan(shortPitch.View(), std::nullopt, {1, {}}).Error().code ==
        pbremotevisualsimulator::ChannelTransformErrorCode::InvalidView);
    pbremotevisualsimulator::BgraImageView overflowExtent = source.View();
    overflowExtent.rowPitch = std::numeric_limits<std::size_t>::max();
    REQUIRE(pbremotevisualsimulator::ExecuteChannelTransformPlan(overflowExtent, std::nullopt, {1, {}}).Error().code ==
        pbremotevisualsimulator::ChannelTransformErrorCode::InvalidView);

    pbremotevisualsimulator::ChannelTransformPolicy invalidPolicy;
    invalidPolicy.maximumTransforms = 0;
    REQUIRE(pbremotevisualsimulator::ExecuteChannelTransformPlan(source.View(), std::nullopt, {1, {}}, invalidPolicy).
        Error().code == pbremotevisualsimulator::ChannelTransformErrorCode::InvalidPolicy);
    invalidPolicy = {};
    invalidPolicy.maximumPixels = pbremotevisualsimulator::kMaximumChannelPixels + 1;
    REQUIRE(pbremotevisualsimulator::ExecuteChannelTransformPlan(source.View(), std::nullopt, {1, {}}, invalidPolicy).
        Error().code == pbremotevisualsimulator::ChannelTransformErrorCode::InvalidPolicy);

    std::vector<pbremotevisualsimulator::ChannelTransform> tooMany(65,
        pbremotevisualsimulator::BlockReplacementTransform{0, 0, 1, 1});
    REQUIRE(pbremotevisualsimulator::ExecuteChannelTransformPlan(source.View(), source.View(), {1, tooMany}).Error().code ==
        pbremotevisualsimulator::ChannelTransformErrorCode::TooManyTransforms);

    const auto RequireTransformError = [&source](const pbremotevisualsimulator::ChannelTransform& transform,
        const pbremotevisualsimulator::ChannelTransformErrorCode expected,
        const std::optional<pbremotevisualsimulator::BgraImageView>& reference = std::nullopt,
        const std::optional<pbremotevisualsimulator::ChannelTransformPolicy>& policy = std::nullopt)
    {
        const std::array<pbremotevisualsimulator::ChannelTransform, 1> transforms{transform};
        const auto result = pbremotevisualsimulator::ExecuteChannelTransformPlan(source.View(), reference,
            {9, transforms}, policy.value_or(pbremotevisualsimulator::ChannelTransformPolicy{}));
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == expected);
        REQUIRE(result.Error().transformIndex == 0);
    };

    auto invalidScale = pbremotevisualsimulator::ResampleTransform{8, 8, 1, 1, 0, 0};
    invalidScale.scaleX = std::numeric_limits<double>::quiet_NaN();
    RequireTransformError(invalidScale, pbremotevisualsimulator::ChannelTransformErrorCode::InvalidParameter);
    auto invalidFilter = pbremotevisualsimulator::ResampleTransform{8, 8, 1, 1, 0, 0};
    invalidFilter.filter = static_cast<pbremotevisualsimulator::ResampleFilter>(255);
    RequireTransformError(invalidFilter, pbremotevisualsimulator::ChannelTransformErrorCode::InvalidParameter);
    RequireTransformError(pbremotevisualsimulator::Kernel3x3Transform{
        static_cast<pbremotevisualsimulator::FixedKernel3x3>(255), 1},
        pbremotevisualsimulator::ChannelTransformErrorCode::InvalidParameter);
    RequireTransformError(pbremotevisualsimulator::Kernel3x3Transform{
        pbremotevisualsimulator::FixedKernel3x3::GaussianBlur, 0},
        pbremotevisualsimulator::ChannelTransformErrorCode::InvalidParameter);
    RequireTransformError(pbremotevisualsimulator::Kernel3x3Transform{
        pbremotevisualsimulator::FixedKernel3x3::GaussianBlur, pbremotevisualsimulator::kMaximumKernelPasses + 1},
        pbremotevisualsimulator::ChannelTransformErrorCode::InvalidParameter);
    RequireTransformError(pbremotevisualsimulator::ColorTransferTransform{
        std::numeric_limits<double>::infinity(), 0, 1},
        pbremotevisualsimulator::ChannelTransformErrorCode::InvalidParameter);
    RequireTransformError(pbremotevisualsimulator::ColorTransferTransform{1, 0, 0.1},
        pbremotevisualsimulator::ChannelTransformErrorCode::InvalidParameter);
    RequireTransformError(pbremotevisualsimulator::ResampleTransform{8193, 1, 1, 1, 0, 0},
        pbremotevisualsimulator::ChannelTransformErrorCode::DimensionLimitExceeded);
    RequireTransformError(pbremotevisualsimulator::ResampleTransform{4096, 4096, 1, 1, 0, 0},
        pbremotevisualsimulator::ChannelTransformErrorCode::PixelLimitExceeded);
    pbremotevisualsimulator::ChannelTransformPolicy workPolicy;
    workPolicy.maximumWorkUnits = 1;
    RequireTransformError(pbremotevisualsimulator::ResampleTransform{8, 8, 1, 1, 0, 0},
        pbremotevisualsimulator::ChannelTransformErrorCode::WorkLimitExceeded, std::nullopt, workPolicy);
    pbremotevisualsimulator::ChannelTransformPolicy bytePolicy;
    bytePolicy.maximumResidentBytes = 511;
    RequireTransformError(pbremotevisualsimulator::ResampleTransform{8, 8, 1, 1, 0, 0},
        pbremotevisualsimulator::ChannelTransformErrorCode::ByteLimitExceeded, std::nullopt, bytePolicy);
    RequireTransformError(pbremotevisualsimulator::BlockReplacementTransform{0, 0, 1, 1},
        pbremotevisualsimulator::ChannelTransformErrorCode::MissingReference);
    RequireTransformError(pbremotevisualsimulator::BlockReplacementTransform{7, 7, 2, 2},
        pbremotevisualsimulator::ChannelTransformErrorCode::RectangleOutOfBounds, source.View());
    RequireTransformError(pbremotevisualsimulator::CropTransform{7, 7, 2, 2},
        pbremotevisualsimulator::ChannelTransformErrorCode::RectangleOutOfBounds);
    RequireTransformError(pbremotevisualsimulator::SolidOverlayTransform{7, 7, 2, 2},
        pbremotevisualsimulator::ChannelTransformErrorCode::RectangleOutOfBounds);
    RequireTransformError(pbremotevisualsimulator::ReferenceBlendTransform{128},
        pbremotevisualsimulator::ChannelTransformErrorCode::MissingReference);

    const PaddedImage mismatchedReference = MakePatternImage(7, 8, 0, 2);
    const std::array<pbremotevisualsimulator::ChannelTransform, 1> block{
        pbremotevisualsimulator::BlockReplacementTransform{0, 0, 1, 1}};
    const auto mismatch = pbremotevisualsimulator::ExecuteChannelTransformPlan(source.View(), mismatchedReference.View(),
        {1, block});
    REQUIRE_FALSE(mismatch);
    REQUIRE(mismatch.Error().code == pbremotevisualsimulator::ChannelTransformErrorCode::ReferenceGeometryMismatch);
    REQUIRE(mismatch.Error().transformIndex == pbremotevisualsimulator::kNoTransformIndex);

    const std::array<pbremotevisualsimulator::ChannelTransform, 2> lateFailure{
        pbremotevisualsimulator::ResampleTransform{8, 8, 1, 1, 0, 0},
        pbremotevisualsimulator::BlockReplacementTransform{0, 0, 1, 1}};
    const auto lateFailureResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(source.View(), std::nullopt,
        {1, lateFailure});
    REQUIRE_FALSE(lateFailureResult);
    REQUIRE(lateFailureResult.Error().code == pbremotevisualsimulator::ChannelTransformErrorCode::MissingReference);
    REQUIRE(lateFailureResult.Error().transformIndex == 1);
}

TEST_CASE("RemoteVisual LF4 production reference recovers a scaled temporally replaced simulator block",
    "[remote-visual][simulator][low-fps][fec][transport][integration]")
{
    constexpr std::uint64_t sessionTag = 0xE17A9C2046B38D5FULL;
    constexpr std::uint64_t previousSequence = 410;
    constexpr std::uint64_t currentSequence = 411;
    const auto previousRecord = MakeLowFpsRecord(sessionTag, previousSequence);
    const auto currentRecord = MakeLowFpsRecord(sessionTag, currentSequence);
    const auto previousData = MakeDiagnosticData(previousRecord);
    const auto currentData = MakeDiagnosticData(currentRecord);
    std::vector<std::byte> previous(static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight * 4);
    std::vector<std::byte> current(previous.size());
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(previousRecord, previousData, previous));
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(currentRecord, currentData, current));
    const auto staleBlock = FindStaleRegionTransform(sessionTag, previousSequence, currentSequence);
    const pbremotevisualsimulator::ResampleTransform resample{2442, 1384, 1.259375, 1.2592592592592593,
        11.25, 13.5, pbremotevisualsimulator::ResampleFilter::Area,
        {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}}};
    const std::array<pbremotevisualsimulator::ChannelTransform, 2> transforms{staleBlock, resample};
    const auto executionResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(MakeBgraView(current),
        MakeBgraView(previous), {0xC0DEC0DE, transforms});
    REQUIRE(executionResult);
    const auto& execution = executionResult.Value();
    REQUIRE(execution.records.size() == 2);
    REQUIRE(execution.records.back().outputBlake3 == execution.outputBlake3);
    auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(channelResult);
    auto channel = std::move(channelResult.Value());
    const pbmodulation::LumaView view{execution.output.pixels, execution.output.width, execution.output.height,
        execution.output.rowPitch, pbmodulation::LumaPixelFormat::Bgra8};
    const auto observation = channel.DecodeRemoteVisualLowFps(view);
    INFO(pbmodulation::GetRemoteVisualLowFpsErasureName(observation.modulation.erasure));
    INFO(observation.modulation.staleRegions);
    INFO(observation.modulation.erasedDataMetrics);
    INFO(observation.evaluation.fecFailures);
    REQUIRE(observation.modulation.IsAccepted());
    REQUIRE(observation.modulation.staleRegions == 1);
    REQUIRE(observation.modulation.erasedDataMetrics > 0);
    REQUIRE(observation.evaluation.IsVerified());
    REQUIRE(observation.evaluation.acceptedTransportBlocks == pbmodulation::kRemoteVisualLowFpsCodewords);
}

TEST_CASE("RemoteVisual LF4 impairment matrix preserves the production truth boundary",
    "[remote-visual][simulator][low-fps][impairment][matrix]")
{
    constexpr std::uint64_t sessionTag = 0x42A17C9E5D8036BFULL;
    const auto previousRecord = MakeLowFpsRecord(sessionTag, 700);
    const auto currentRecord = MakeLowFpsRecord(sessionTag, 701);
    const auto previousData = MakeDiagnosticData(previousRecord);
    const auto currentData = MakeDiagnosticData(currentRecord);
    std::vector<std::byte> previous(static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight * 4);
    std::vector<std::byte> current(previous.size());
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(previousRecord, previousData, previous));
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(currentRecord, currentData, current));

    const auto RunCase = [&current, &previous](const std::span<const pbremotevisualsimulator::ChannelTransform> transforms,
        const bool useReference)
    {
        const std::optional<pbremotevisualsimulator::BgraImageView> reference = useReference ?
            std::optional<pbremotevisualsimulator::BgraImageView>{MakeBgraView(previous)} : std::nullopt;
        const auto executionResult = pbremotevisualsimulator::ExecuteChannelTransformPlan(MakeBgraView(current),
            reference, {0x5EED, transforms});
        REQUIRE(executionResult);
        const auto& output = executionResult.Value().output;
        auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
        REQUIRE(channelResult);
        auto channel = std::move(channelResult.Value());
        const pbmodulation::LumaView view{output.pixels, output.width, output.height, output.rowPitch,
            pbmodulation::LumaPixelFormat::Bgra8};
        return channel.DecodeRemoteVisualLowFps(view);
    };

    SECTION("mild fixed Gaussian blur remains file-truth verified")
    {
        const std::array<pbremotevisualsimulator::ChannelTransform, 1> transforms{
            pbremotevisualsimulator::Kernel3x3Transform{pbremotevisualsimulator::FixedKernel3x3::GaussianBlur, 1}};
        const auto observation = RunCase(transforms, false);
        INFO(pbmodulation::GetRemoteVisualLowFpsErasureName(observation.modulation.erasure));
        REQUIRE(observation.modulation.IsAccepted());
        REQUIRE(observation.evaluation.IsVerified());
        REQUIRE(observation.evaluation.falseAcceptedCodewords == 0);
    }
    SECTION("limited-range affine luma remains file-truth verified")
    {
        const std::array<pbremotevisualsimulator::ChannelTransform, 1> transforms{
            pbremotevisualsimulator::ColorTransferTransform{219.0 / 255.0, 16, 1}};
        const auto observation = RunCase(transforms, false);
        INFO(pbmodulation::GetRemoteVisualLowFpsErasureName(observation.modulation.erasure));
        REQUIRE(observation.modulation.IsAccepted());
        REQUIRE(observation.evaluation.IsVerified());
        REQUIRE(observation.evaluation.falseAcceptedCodewords == 0);
    }
    SECTION("fixed 4:2:0 transform preserves neutral LF4 exactly enough for truth verification")
    {
        const std::array<pbremotevisualsimulator::ChannelTransform, 1> transforms{
            pbremotevisualsimulator::ChromaSubsample420Transform{}};
        const auto observation = RunCase(transforms, false);
        INFO(pbmodulation::GetRemoteVisualLowFpsErasureName(observation.modulation.erasure));
        REQUIRE(observation.modulation.IsAccepted());
        REQUIRE(observation.evaluation.IsVerified());
        REQUIRE(observation.evaluation.falseAcceptedCodewords == 0);
    }
    SECTION("illegal crop fails closed without accepted Transport")
    {
        const std::array<pbremotevisualsimulator::ChannelTransform, 1> transforms{
            pbremotevisualsimulator::CropTransform{8, 0, pbmodulation::kLocalDesktopCanvasWidth - 8,
                pbmodulation::kLocalDesktopCanvasHeight}};
        const auto observation = RunCase(transforms, false);
        REQUIRE_FALSE(observation.modulation.IsAccepted());
        REQUIRE_FALSE(observation.evaluation.IsVerified());
        REQUIRE(observation.evaluation.acceptedTransportBlocks == 0);
        REQUIRE(observation.evaluation.falseAcceptedCodewords == 0);
    }
    SECTION("opaque codec-block overlay never creates false accepted Transport")
    {
        const std::array<pbremotevisualsimulator::ChannelTransform, 1> transforms{
            pbremotevisualsimulator::SolidOverlayTransform{96 + 4 * 128, 96 + 3 * 128, 128, 128,
                {std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}}, 255}};
        const auto observation = RunCase(transforms, false);
        REQUIRE(observation.modulation.IsAccepted());
        REQUIRE(observation.modulation.staleRegions == 1);
        REQUIRE(observation.evaluation.IsVerified());
        REQUIRE(observation.evaluation.falseAcceptedCodewords == 0);
    }
    SECTION("whole-frame temporal blend never creates false accepted Transport")
    {
        const std::array<pbremotevisualsimulator::ChannelTransform, 1> transforms{
            pbremotevisualsimulator::ReferenceBlendTransform{96}};
        const auto observation = RunCase(transforms, true);
        REQUIRE_FALSE(observation.modulation.IsAccepted());
        REQUIRE_FALSE(observation.evaluation.IsVerified());
        REQUIRE(observation.evaluation.acceptedTransportBlocks == 0);
        REQUIRE(observation.evaluation.falseAcceptedCodewords == 0);
    }
}
