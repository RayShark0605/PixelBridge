#pragma once

#include "pbscreencapturewgc/wgc_capture.h"
#include <winrt/Windows.Graphics.Capture.h>

namespace pbscreencapturewgc::detail
{

void ProbeCursorAndCadence(const winrt::Windows::Graphics::Capture::GraphicsCaptureSession& session,
                           const WgcCaptureConfig& config, CaptureCapabilities& capabilities) noexcept;
void ApplyBorderlessPermission(const winrt::Windows::Graphics::Capture::GraphicsCaptureSession& session,
                               bool granted, CaptureCapabilities& capabilities) noexcept;

}
