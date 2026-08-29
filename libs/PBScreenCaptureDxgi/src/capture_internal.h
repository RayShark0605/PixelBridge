#pragma once

#include "pbscreencapturedxgi/dxgi_capture.h"
#include "capture_runtime.h"

namespace pbscreencapturedxgi::detail
{
using namespace pbcapturenormalize;
using namespace pbcapturenormalize::detail;

struct NativeDxgiOptions
{
    bool debugLayer = false;
    bool forceQuery = false;
    bool holdCompletionPolling = false;
    bool normalizeConfiguredFormat = false;
    CaptureStage failAfterStage = CaptureStage::None;
};

[[nodiscard]] std::unique_ptr<CaptureBackend> MakeNativeDxgiBackend(const NativeDxgiOptions& options = {});

} // namespace pbscreencapturedxgi::detail

namespace pbscreencapturedxgi
{

// Deliberately private to tests. Production consumers must use normalization;
// a raw DXGI surface cannot promise exclusion of a composited/unknown pointer.
class DxgiCaptureTestAccess
{
public:
    [[nodiscard]] static CaptureStatus CreateRaw(const DxgiCaptureConfig& config, std::shared_ptr<pbcapturenormalize::RawRoiConsumer> consumer,
                                                 std::unique_ptr<DxgiCapture>& output, const detail::NativeDxgiOptions& options = {}) noexcept;
    [[nodiscard]] static CaptureStatus CreateWithBackend(const DxgiCaptureConfig& config, std::shared_ptr<pbcapturenormalize::RawRoiConsumer> consumer,
                                                         std::unique_ptr<pbcapturenormalize::detail::CaptureBackend> backend,
                                                         std::unique_ptr<DxgiCapture>& output) noexcept;
    [[nodiscard]] static CaptureStatus CreateNormalized(const pbcapturenormalize::CaptureNormalizeConfig& config,
                                                        std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> consumer,
                                                        std::unique_ptr<pbcapturenormalize::detail::CaptureBackend> backend,
                                                        std::unique_ptr<DxgiCapture>& output) noexcept;
    static void RequestRecreate(DxgiCapture& capture) noexcept;
};

} // namespace pbscreencapturedxgi
