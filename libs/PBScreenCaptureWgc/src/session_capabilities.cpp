#include "session_capabilities.h"
#include <winrt/Windows.Foundation.h>

namespace pbscreencapturewgc::detail
{
using namespace winrt::Windows::Graphics::Capture;

void ProbeCursorAndCadence(const GraphicsCaptureSession& session, const WgcCaptureConfig& config, CaptureCapabilities& capabilities) noexcept
{
    const auto cursor = session.try_as<IGraphicsCaptureSession2>();
    capabilities.cursorDisableAvailable = static_cast<bool>(cursor);
    capabilities.cursorExcluded = false;
    capabilities.cursorNativeError = 0;
    if (cursor)
    {
        try
        {
            cursor.IsCursorCaptureEnabled(false);
            capabilities.cursorExcluded = !cursor.IsCursorCaptureEnabled();
        }
        catch (...)
        {
            capabilities.cursorNativeError = winrt::to_hresult();
        }
    }
    capabilities.borderlessAvailable = static_cast<bool>(session.try_as<IGraphicsCaptureSession3>());
    const auto cadence = session.try_as<IGraphicsCaptureSession5>();
    capabilities.minUpdateIntervalAvailable = static_cast<bool>(cadence);
    capabilities.minUpdateIntervalApplied = false;
    capabilities.minUpdateIntervalNativeError = 0;
    capabilities.actualMinUpdateInterval100ns = 0;
    if (cadence && config.minUpdateInterval100ns)
    {
        try
        {
            cadence.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan(*config.minUpdateInterval100ns));
            capabilities.actualMinUpdateInterval100ns = cadence.MinUpdateInterval().count();
            capabilities.minUpdateIntervalApplied = capabilities.actualMinUpdateInterval100ns == *config.minUpdateInterval100ns;
        }
        catch (...)
        {
            capabilities.minUpdateIntervalNativeError = winrt::to_hresult();
        }
    }
}

void ApplyBorderlessPermission(const GraphicsCaptureSession& session, const bool granted, CaptureCapabilities& capabilities) noexcept
{
    capabilities.borderlessAccessGranted = granted;
    capabilities.borderlessSettingApplied = false;
    if (!granted)
    {
        return;
    }
    try
    {
        const auto border = session.try_as<IGraphicsCaptureSession3>();
        if (border)
        {
            border.IsBorderRequired(false);
            capabilities.borderlessSettingApplied = !border.IsBorderRequired();
        }
    }
    catch (...)
    {
        capabilities.borderlessNativeError = winrt::to_hresult();
    }
}

}
