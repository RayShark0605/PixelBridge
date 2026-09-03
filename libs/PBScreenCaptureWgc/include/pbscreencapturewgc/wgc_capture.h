#pragma once

#include "pbcapturenormalize/screen_capture_frame.h"

namespace pbscreencapturewgc
{

using CaptureError = pbcapturenormalize::CaptureError;
using CaptureStage = pbcapturenormalize::CaptureStage;
using CaptureStatus = pbcapturenormalize::CaptureStatus;
using CaptureConsumerCompletion = pbcapturenormalize::CaptureConsumerCompletion;
using CaptureSize = pbcapturenormalize::CaptureSize;
using WgcCaptureConfig = pbcapturenormalize::CaptureConfig;
using CaptureCapabilities = pbcapturenormalize::CaptureCapabilities;
using CaptureEnvironment = pbcapturenormalize::CaptureEnvironment;
using RoiFrameMetadata = pbcapturenormalize::RawRoiFrameMetadata;
using RoiConsumer = pbcapturenormalize::RawRoiConsumer;
using CaptureState = pbcapturenormalize::CaptureState;
using WgcCaptureSnapshot = pbcapturenormalize::CaptureSnapshot;
using pbcapturenormalize::GetCaptureErrorName;
[[nodiscard]] CaptureStatus ValidateWgcCaptureConfig(const WgcCaptureConfig& config) noexcept;

class WgcCaptureTestAccess;

// The host declares PMv2 before creating windows. The module owns its MTA and
// capture-adapter D3D11 device. Consumer ownership lasts through GPU retirement.
// Create leaves output unchanged on failure. RequestStop/GetSnapshot are safe
// from the consumer; Stop is idempotent but must not run on the owner thread.
// Destruction must not race other calls or run inside a consumer callback.
// A permanently wedged OS/GPU can retain
// at most this instance's bounded deferred resources; it is never called success.
class WgcCapture
{
public:
    [[nodiscard]] static CaptureStatus Create(const WgcCaptureConfig& config, std::shared_ptr<RoiConsumer> consumer,
                                              std::unique_ptr<WgcCapture>& output) noexcept;
    // Explicit strict boundary. Cursor ambiguity/age are observable erasures;
    // this overload never falls back to raw ROI delivery or another backend.
    // Optional failure evidence is sampled after Stop joins the owner. A
    // deferred/failed drain is not authorization to create another backend.
    [[nodiscard]] static CaptureStatus CreateNormalized(const pbcapturenormalize::CaptureNormalizeConfig& config,
                                                        std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> consumer,
                                                        std::unique_ptr<WgcCapture>& output,
                                                        pbcapturenormalize::CaptureSnapshot* failedStartSnapshot = nullptr) noexcept;
    ~WgcCapture();
    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;
    [[nodiscard]] WgcCaptureSnapshot GetSnapshot() const noexcept;
    [[nodiscard]] pbcapturenormalize::CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept;
    void RequestStop() noexcept;
    [[nodiscard]] CaptureStatus Stop() noexcept;

private:
    friend class WgcCaptureTestAccess;
    struct Implementation;
    explicit WgcCapture(std::shared_ptr<Implementation> implementation) noexcept;
    std::shared_ptr<Implementation> implementation_;
};

} // namespace pbscreencapturewgc
