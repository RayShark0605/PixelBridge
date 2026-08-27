#include "pbscreencapturewgc/wgc_capture.h"

namespace
{
class Consumer final : public pbscreencapturewgc::RoiConsumer
{
public:
    pbscreencapturewgc::CaptureStatus EpochStarted(std::uint64_t, const pbscreencapturewgc::CaptureEnvironment&, ID3D11Device*) override
    {
        return pbscreencapturewgc::CaptureStatus::Failure(pbscreencapturewgc::CaptureError::InternalError, pbscreencapturewgc::CaptureStage::Consumer);
    }
    pbscreencapturewgc::CaptureStatus Submit(const pbscreencapturewgc::RoiFrameMetadata&, ID3D11Texture2D*, ID3D11DeviceContext*) override
    {
        return pbscreencapturewgc::CaptureStatus::Failure(pbscreencapturewgc::CaptureError::InternalError, pbscreencapturewgc::CaptureStage::Consumer);
    }
};
}

int main()
{
    using namespace pbscreencapturewgc;
    WgcCaptureConfig config;
    config.region.monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(config.region.monitor, &info))
    {
        return 2;
    }
    config.region.monitorPhysicalRect = info.rcMonitor;
    config.region.physicalRect = {info.rcMonitor.left + 1, info.rcMonitor.top + 1, info.rcMonitor.left + 3, info.rcMonitor.top + 3};
    config.region.dpiX = 96;
    config.region.dpiY = 96;
    config.region.rotation = DXGI_MODE_ROTATION_IDENTITY;
    std::unique_ptr<WgcCapture> capture;
    const auto status = WgcCapture::Create(config, std::make_shared<Consumer>(), capture);
    return status.code == CaptureError::DpiAwarenessRequired && status.stage == CaptureStage::Region && !capture ? 0 : 1;
}
