#include "capture_runtime.h"
#include "pbprotocol/checked_integer.h"

namespace pbcapturenormalize::detail
{
CaptureStatus ValidateLayout(const CaptureConfig& config, const CaptureEnvironment& environment, CaptureLayout& layout) noexcept
{
    if (config.queuedFrameLimit == 0 || config.queuedFrameLimit > maximumQueuedFrames || config.roiTextureCount < 2 ||
        config.roiTextureCount > maximumRoiTextures ||
        (config.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM && config.pixelFormat != DXGI_FORMAT_R10G10B10A2_UNORM &&
         config.pixelFormat != DXGI_FORMAT_R16G16B16A16_FLOAT) ||
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
    const bool rotated = environment.sourceRotation != DXGI_MODE_ROTATION_IDENTITY;
    const std::uint64_t pixelBytes = config.pixelFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    const auto roiPixels = pbprotocol::CheckedMultiplyUint64(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height));
    const auto roiBytes = roiPixels ? pbprotocol::CheckedMultiplyUint64(roiPixels.Value(), pixelBytes) : roiPixels;
    // Rotation needs a shader-readable raw ROI scratch for every owned output
    // slot: acquired desktop surfaces are not required to support SRV binding.
    const auto outputRingBytes = roiBytes ? pbprotocol::CheckedMultiplyUint64(roiBytes.Value(), config.roiTextureCount) : roiBytes;
    const auto ringBytes = outputRingBytes ? pbprotocol::CheckedMultiplyUint64(outputRingBytes.Value(), rotated ? 2u : 1u) : outputRingBytes;
    const auto poolBuffers = environment.backendKind == CaptureBackendKind::Dxgi ? 1u : config.queuedFrameLimit + config.roiTextureCount + 1;
    const auto surfacePixels = pbprotocol::CheckedMultiplyUint64(static_cast<std::uint64_t>(monitorWidth), static_cast<std::uint64_t>(monitorHeight));
    const auto surfaceBytes = surfacePixels ? pbprotocol::CheckedMultiplyUint64(surfacePixels.Value(), pixelBytes) : surfacePixels;
    const auto poolBytes = surfaceBytes ? pbprotocol::CheckedMultiplyUint64(surfaceBytes.Value(), poolBuffers) : surfaceBytes;
    const auto totalBytes = poolBytes && ringBytes ? pbprotocol::CheckedAddUint64(poolBytes.Value(), ringBytes.Value()) : poolBytes;
    if (!roiBytes || !ringBytes || !poolBytes || !totalBytes || ringBytes.Value() > config.maximumRoiBytes || totalBytes.Value() > config.maximumCaptureBytes ||
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
