#include "capture_runtime.h"
#include "pbprotocol/checked_integer.h"

namespace pbcapturenormalize::detail
{
namespace
{
[[nodiscard]] std::uint64_t PixelBytes(const DXGI_FORMAT format) noexcept
{
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        return 8;
    }
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        return 4;
    }
    return 0;
}
}

CaptureStatus ValidateLayout(const CaptureConfig& config, const CaptureEnvironment& environment, CaptureLayout& layout) noexcept
{
    const std::uint64_t outputPixelBytes = PixelBytes(config.pixelFormat);
    const std::uint64_t sourcePixelBytes = PixelBytes(environment.pixelFormat);
    if (config.queuedFrameLimit == 0 || config.queuedFrameLimit > maximumQueuedFrames || config.roiTextureCount < 2 ||
        config.roiTextureCount > maximumRoiTextures ||
        outputPixelBytes == 0 || sourcePixelBytes == 0 ||
        (environment.backendKind != CaptureBackendKind::Wgc && environment.backendKind != CaptureBackendKind::Dxgi))
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    const auto& region = environment.region;
    const auto& rectangle = region.physicalRect;
    const auto& monitor = region.monitorPhysicalRect;
    const auto width = static_cast<std::int64_t>(rectangle.right) - rectangle.left;
    const auto height = static_cast<std::int64_t>(rectangle.bottom) - rectangle.top;
    const auto monitorWidth = static_cast<std::int64_t>(monitor.right) - monitor.left;
    const auto monitorHeight = static_cast<std::int64_t>(monitor.bottom) - monitor.top;
    if (region.monitor == nullptr || width <= 0 || height <= 0 || monitorWidth <= 0 || monitorHeight <= 0 || monitorWidth > 16384 || monitorHeight > 16384 ||
        rectangle.left < monitor.left || rectangle.top < monitor.top || rectangle.right > monitor.right || rectangle.bottom > monitor.bottom ||
        environment.contentSize.width != monitorWidth || environment.contentSize.height != monitorHeight || region.dpiX == 0 || region.dpiY == 0 ||
        region.rotation < DXGI_MODE_ROTATION_IDENTITY || region.rotation > DXGI_MODE_ROTATION_ROTATE270 ||
        environment.sourceRotation < DXGI_MODE_ROTATION_IDENTITY || environment.sourceRotation > DXGI_MODE_ROTATION_ROTATE270)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Region);
    }
    const bool swapsAxes = environment.sourceRotation == DXGI_MODE_ROTATION_ROTATE90 || environment.sourceRotation == DXGI_MODE_ROTATION_ROTATE270;
    const CaptureSize sourceSize = environment.sourceSize == CaptureSize{} ? environment.contentSize : environment.sourceSize;
    if (sourceSize.width != (swapsAxes ? monitorHeight : monitorWidth) || sourceSize.height != (swapsAxes ? monitorWidth : monitorHeight))
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Surface);
    }
    const bool transformRequired = environment.sourceRotation != DXGI_MODE_ROTATION_IDENTITY || environment.pixelFormat != config.pixelFormat;
    const auto roiPixels = pbprotocol::CheckedMultiplyUint64(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height));
    const auto outputRoiBytes = roiPixels ? pbprotocol::CheckedMultiplyUint64(roiPixels.Value(), outputPixelBytes) : roiPixels;
    const auto sourceRoiBytes = roiPixels ? pbprotocol::CheckedMultiplyUint64(roiPixels.Value(), sourcePixelBytes) : roiPixels;
    const auto outputRingBytes = outputRoiBytes ? pbprotocol::CheckedMultiplyUint64(outputRoiBytes.Value(), config.roiTextureCount) : outputRoiBytes;
    // Rotation or format conversion needs one shader-readable source-format
    // ROI scratch per owned output slot. Acquired desktop surfaces need not
    // expose SRV bind flags, so they are never sampled directly.
    const auto scratchRingBytes = sourceRoiBytes ? pbprotocol::CheckedMultiplyUint64(sourceRoiBytes.Value(),
        transformRequired ? config.roiTextureCount : 0U) : sourceRoiBytes;
    const auto ringBytes = outputRingBytes && scratchRingBytes ?
        pbprotocol::CheckedAddUint64(outputRingBytes.Value(), scratchRingBytes.Value()) : outputRingBytes;
    const auto poolBuffers = environment.backendKind == CaptureBackendKind::Dxgi ? 1u : config.queuedFrameLimit + config.roiTextureCount + 1;
    const auto surfacePixels = pbprotocol::CheckedMultiplyUint64(static_cast<std::uint64_t>(monitorWidth), static_cast<std::uint64_t>(monitorHeight));
    const auto surfaceBytes = surfacePixels ? pbprotocol::CheckedMultiplyUint64(surfacePixels.Value(), sourcePixelBytes) : surfacePixels;
    const auto poolBytes = surfaceBytes ? pbprotocol::CheckedMultiplyUint64(surfaceBytes.Value(), poolBuffers) : surfaceBytes;
    const auto totalBytes = poolBytes && ringBytes ? pbprotocol::CheckedAddUint64(poolBytes.Value(), ringBytes.Value()) : poolBytes;
    if (!outputRoiBytes || !sourceRoiBytes || !outputRingBytes || !scratchRingBytes || !ringBytes || !poolBytes || !totalBytes ||
        ringBytes.Value() > config.maximumRoiBytes || totalBytes.Value() > config.maximumCaptureBytes ||
        !pbprotocol::CheckedUint64ToSize(totalBytes.Value()))
    {
        return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration);
    }
    CaptureLayout candidate;
    candidate.sourceBox = {static_cast<UINT>(static_cast<std::int64_t>(rectangle.left) - monitor.left),
                           static_cast<UINT>(static_cast<std::int64_t>(rectangle.top) - monitor.top), 0,
                           static_cast<UINT>(static_cast<std::int64_t>(rectangle.right) - monitor.left),
                           static_cast<UINT>(static_cast<std::int64_t>(rectangle.bottom) - monitor.top), 1};
    const auto visualBox = candidate.sourceBox;
    // sourceRotation is the transform from the raw duplication surface to the
    // visible desktop. Transform the half-open physical ROI in the opposite
    // direction; no DPI scaling, clipping, interpolation, or unsigned origins.
    switch (environment.sourceRotation)
    {
    case DXGI_MODE_ROTATION_ROTATE90:
        candidate.sourceBox = {visualBox.top, static_cast<UINT>(monitorWidth) - visualBox.right, 0,
                               visualBox.bottom, static_cast<UINT>(monitorWidth) - visualBox.left, 1};
        break;
    case DXGI_MODE_ROTATION_ROTATE180:
        candidate.sourceBox = {static_cast<UINT>(monitorWidth) - visualBox.right, static_cast<UINT>(monitorHeight) - visualBox.bottom, 0,
                               static_cast<UINT>(monitorWidth) - visualBox.left, static_cast<UINT>(monitorHeight) - visualBox.top, 1};
        break;
    case DXGI_MODE_ROTATION_ROTATE270:
        candidate.sourceBox = {static_cast<UINT>(monitorHeight) - visualBox.bottom, visualBox.left, 0,
                               static_cast<UINT>(monitorHeight) - visualBox.top, visualBox.right, 1};
        break;
    default:
        break;
    }
    candidate.roiWidth = static_cast<std::uint32_t>(width);
    candidate.roiHeight = static_cast<std::uint32_t>(height);
    candidate.poolBufferCount = poolBuffers;
    candidate.totalBytes = totalBytes.Value();
    layout = candidate;
    return {};
}

}
