#pragma once
#include "pbscreencapturewgc/wgc_capture.h"
#include "../../PBCaptureNormalize/src/capture_runtime.h"

namespace pbscreencapturewgc::detail
{
using namespace pbcapturenormalize::detail;
struct NativeCaptureOptions
{
    bool forceQuery = false;
    bool debugLayer = false;
    bool holdCompletionPolling = false;
    CaptureStage failAfterStage = CaptureStage::None;
};

[[nodiscard]] std::unique_ptr<CaptureBackend> MakeNativeCaptureBackend(const NativeCaptureOptions& options = {});

} // namespace pbscreencapturewgc::detail

namespace pbscreencapturewgc
{

class WgcCaptureTestAccess
{
public:
    [[nodiscard]] static CaptureStatus Create(const WgcCaptureConfig& config, std::shared_ptr<RoiConsumer> consumer,
                                              std::unique_ptr<detail::CaptureBackend> backend, std::unique_ptr<WgcCapture>& output) noexcept;
    [[nodiscard]] static CaptureStatus CreateNormalized(const pbcapturenormalize::CaptureNormalizeConfig& config,
                                                        std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> consumer,
                                                        std::unique_ptr<detail::CaptureBackend> backend, std::unique_ptr<WgcCapture>& output) noexcept;
    static void RequestRecreate(WgcCapture& capture) noexcept;
};

}
