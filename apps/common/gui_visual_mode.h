#pragma once

#include "local_desktop_runtime.h"
#include "pbmodulation/experimental_pam4_wide.h"

#include <array>
#include <optional>

namespace pbapp
{

// Persisted GUI indices, not the certified protocol catalog. Keep the legacy
// 0/1 meanings; invalid values never select an experimental identity.
[[nodiscard]] inline std::optional<VisualProfile> GetGuiVisualProfile(const int index) noexcept
{
    constexpr std::array profiles{VisualProfile::UnifiedLc4, VisualProfile::UnifiedGrayFast,
        VisualProfile::ExperimentalPam4, VisualProfile::ExperimentalPam4Wide};
    if (index < 0 || static_cast<std::size_t>(index) >= profiles.size())
    {
        return std::nullopt;
    }
    return profiles[static_cast<std::size_t>(index)];
}

// Called on a fresh MakeUnifiedEncoderConfig after target enumeration. The
// current GUI choices cannot mutate a running Session or the saved FPS value.
[[nodiscard]] inline RuntimeStatus ApplyGuiEncoderVisualMode(EncoderConfig& config, const int index, const bool formalMeasurement)
{
    const auto profile = GetGuiVisualProfile(index);
    if (!profile)
    {
        return RuntimeStatus::Failure("传输模式无效；请重新选择，不会自动切换其它模式。");
    }
    if (!IsExperimentalPam4Family(*profile))
    {
        config.visualProfile = *profile;
        return {};
    }
    if (formalMeasurement || config.measurement)
    {
        return RuntimeStatus::Failure("PAM4 模式不支持正式测量入口；请使用普通启动方式，或明确选择标准模式。");
    }
    if (!config.singleMonitorFullscreen || config.monitorSafety || config.fullscreenNativeSize ||
        config.fullscreenSampling != FullscreenSamplingMode::Point || config.grayFastSpatialInterleave ||
        config.grayFastShortInitialAirtime || config.grayFastExtendedVisitBudget)
    {
        return RuntimeStatus::Failure("PAM4 需要明确的一块发送屏幕与固定点呈现，不能混用其它呈现方式。");
    }
    const bool wide = *profile == VisualProfile::ExperimentalPam4Wide;
    const auto& monitor = *config.singleMonitorFullscreen;
    const std::int64_t width = static_cast<std::int64_t>(monitor.physicalRect.right) - monitor.physicalRect.left;
    const std::int64_t height = static_cast<std::int64_t>(monitor.physicalRect.bottom) - monitor.physicalRect.top;
    const std::uint32_t minimumWidth = wide ? pbmodulation::kExperimentalPam4WidePresentationWidth : phase1CanvasWidth;
    const std::uint32_t minimumHeight = wide ? pbmodulation::kExperimentalPam4WidePresentationHeight : phase1CanvasHeight;
    if (monitor.monitor == nullptr || monitor.rotation != DXGI_MODE_ROTATION_IDENTITY || monitor.dpiX == 0 || monitor.dpiY == 0 ||
        width < minimumWidth || height < minimumHeight || width > 3840 || height > 2160)
    {
        return RuntimeStatus::Failure(wide ?
            "PAM4 Wide 需要发送端至少 2560×1440、至多 3840×2160 的未旋转物理屏幕；1080P 请明确选择 PAM4。程序不会修改分辨率或缩小 Wide。" :
            "PAM4 需要发送端至少 1920×1080、至多 3840×2160 的未旋转物理屏幕；程序不会修改分辨率。");
    }
    const std::uint32_t rasterWidth = wide ? pbmodulation::kExperimentalPam4WidePresentationWidth : 0;
    if (config.fullscreenRasterWidth != 0 && config.fullscreenRasterWidth != rasterWidth)
    {
        return RuntimeStatus::Failure("PAM4 呈现宽度与所选模式冲突，不会静默重采样。");
    }
    config.visualProfile = *profile;
    config.fullscreenRasterWidth = rasterWidth;
    config.remoteMetadata.channelType = ChannelType::RemoteVisual;
    if (config.remoteMetadata.remoteProvider.empty())
    {
        // The GUI has not observed a remote product or its codec/settings.
        config.remoteMetadata.remoteProvider = "Unspecified";
    }
    if (config.segmentTargetBytes == 0)
    {
        config.segmentTargetBytes = 7U * 1024U * 1024U;
    }
    return {};
}

} // namespace pbapp
