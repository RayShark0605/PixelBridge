#pragma once

#include "pbcapturenormalize/screen_capture_frame.h"

namespace pbcapturenormalize::detail
{
class CaptureRuntime;
class NormalizeConsumer;
}

namespace pbscreencapturedxgi
{

using CaptureStatus = pbcapturenormalize::CaptureStatus;
using CaptureError = pbcapturenormalize::CaptureError;
using CaptureStage = pbcapturenormalize::CaptureStage;
using DxgiCaptureConfig = pbcapturenormalize::CaptureConfig;
using DxgiCaptureSnapshot = pbcapturenormalize::CaptureSnapshot;
using pbcapturenormalize::GetCaptureErrorName;

// Before format negotiation, admission reserves the largest advertised format
// (8 bytes/pixel), every rotated slot's raw scratch, and the bounded pointer data.
[[nodiscard]] CaptureStatus ValidateDxgiCaptureConfig(const DxgiCaptureConfig& config) noexcept;

class DxgiCaptureTestAccess;

// Owns the shared capture runtime, not a second queue or GPU submission thread.
// The native duplication lease remains owned until source GPU reads retire.
// RequestStop/GetSnapshot may be used in callbacks; Stop/destruction may not.
class DxgiCapture
{
public:
    // Optional failure evidence is sampled after Stop joins the owner. A
    // deferred/failed drain is not authorization to create another backend.
    [[nodiscard]] static CaptureStatus Create(const pbcapturenormalize::CaptureNormalizeConfig& config,
                                              std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> consumer,
                                              std::unique_ptr<DxgiCapture>& output,
                                                        pbcapturenormalize::CaptureSnapshot* failedStartSnapshot = nullptr) noexcept;
    ~DxgiCapture();
    DxgiCapture(const DxgiCapture&) = delete;
    DxgiCapture& operator=(const DxgiCapture&) = delete;
    [[nodiscard]] DxgiCaptureSnapshot GetSnapshot() const noexcept;
    [[nodiscard]] pbcapturenormalize::CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept;
    void RequestStop() noexcept;
    [[nodiscard]] CaptureStatus Stop() noexcept;

private:
    friend class DxgiCaptureTestAccess;
    explicit DxgiCapture(std::unique_ptr<pbcapturenormalize::detail::CaptureRuntime> runtime,
                         std::shared_ptr<pbcapturenormalize::detail::NormalizeConsumer> normalizer = {}) noexcept;
    std::unique_ptr<pbcapturenormalize::detail::CaptureRuntime> runtime_;
    std::shared_ptr<pbcapturenormalize::detail::NormalizeConsumer> normalizer_;
};

} // namespace pbscreencapturedxgi
