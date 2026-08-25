#include "pbmodulation/frame_io.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/reference_visual_profile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace
{

std::vector<std::byte> MakeZeroFrame()
{
    std::array<std::byte, pbmodulation::kReferenceBootstrapRecordBytes>
        bootstrapRecord{};
    std::vector<std::byte> control(pbmodulation::kReferenceControlWindowBytes);
    std::vector<std::byte> data(pbmodulation::kReferenceDataRegionBytes);
    const pbmodulation::ReferenceFrameInput input{
        bootstrapRecord,
        std::span<const std::byte>(control),
        std::span<const std::byte>(data)};
    std::vector<std::byte> frame(pbmodulation::kReferenceFrameBgraBytes);
    const auto status =
        pbmodulation::EncodeReferenceFrame(input, frame);
    if (!status)
    {
        return {};
    }
    return frame;
}

} // namespace

int main()
{
    if (!pbmodulation::ValidateReferenceVisualProfile())
    {
        return 1;
    }

    const std::array<std::byte, pbmodulation::kReferenceManifestBytes>
        manifest = pbmodulation::SerializeReferenceRegionManifest();
    const auto parsedManifest =
        pbmodulation::ParseReferenceRegionManifest(manifest);
    if (!parsedManifest ||
        parsedManifest.Value().regions != pbmodulation::kReferenceRegions ||
        parsedManifest.Value().canvasWidth !=
            pbmodulation::kReferenceCanvasWidth ||
        parsedManifest.Value().canvasHeight !=
            pbmodulation::kReferenceCanvasHeight)
    {
        return 2;
    }

    const std::uint32_t rawWidth = 2;
    const std::uint32_t rawHeight = 3;
    std::vector<std::byte> rawPixels(rawWidth * rawHeight * 4u);
    for (std::size_t i = 0; i < rawPixels.size(); i++)
    {
        rawPixels[i] = std::byte{static_cast<std::uint8_t>(i % 251u)};
    }
    const auto rawResult = pbmodulation::EncodeRawFrame(rawPixels, rawWidth,
        rawHeight);
    if (!rawResult)
    {
        return 3;
    }
    std::vector<std::byte> decodedPixels(rawWidth * rawHeight * 4u);
    std::uint32_t decodedWidth = 0;
    std::uint32_t decodedHeight = 0;
    const auto rawStatus = pbmodulation::DecodeRawFrame(
        std::span<const std::byte>(rawResult.Value()), decodedPixels,
        decodedWidth, decodedHeight);
    if (!rawStatus || decodedWidth != rawWidth ||
        decodedHeight != rawHeight || decodedPixels != rawPixels)
    {
        return 4;
    }

    const std::vector<std::byte> frame = MakeZeroFrame();
    if (frame.size() != pbmodulation::kReferenceFrameBgraBytes)
    {
        return 5;
    }
    const auto decodedFrame = pbmodulation::DecodeReferenceFrame(frame);
    if (!decodedFrame)
    {
        return 6;
    }
    const auto& decoded = decodedFrame.Value();
    std::size_t nonZeroBytes = 0;
    for (const std::byte value : decoded.bootstrapRecord)
    {
        if (value != std::byte{0})
        {
            nonZeroBytes++;
        }
    }
    for (const std::byte value : decoded.controlWindow)
    {
        if (value != std::byte{0})
        {
            nonZeroBytes++;
        }
    }
    for (const std::byte value : decoded.data)
    {
        if (value != std::byte{0})
        {
            nonZeroBytes++;
        }
    }
    if (nonZeroBytes != 0)
    {
        return 7;
    }

    return 0;
}
