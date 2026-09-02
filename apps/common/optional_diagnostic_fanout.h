#pragma once

#include "pbcapturenormalize/screen_capture_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace pbapp::detail
{

// The primary consumer remains authoritative for capture success and protocol
// admission. The diagnostic branch is resource-charged and may extend the ROI
// lease for one bounded GPU stage, but its failures never replace primary truth.
class OptionalDiagnosticFanout final : public pbcapturenormalize::ScreenCaptureConsumer
{
public:
    OptionalDiagnosticFanout(std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> primary,
        std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> diagnostic);

    [[nodiscard]] std::uint64_t ReservedBytes() const noexcept override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus ValidateConfiguration(
        const pbcapturenormalize::CaptureConfig& config) const noexcept override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus DomainStarted(
        const pbcapturenormalize::ScreenCaptureDomain& domain,
        const pbcapturenormalize::CaptureEnvironment& environment, ID3D11Device* device) override;
    void DomainInvalidated(const pbcapturenormalize::ScreenCaptureDomain& domain) noexcept override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus Submit(
        const pbcapturenormalize::ScreenCaptureFrame& frame, ID3D11DeviceContext* context) override;
    [[nodiscard]] pbcapturenormalize::CaptureConsumerCompletion CompleteStage(
        const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata, ID3D11Texture2D* texture,
        ID3D11DeviceContext* context, bool cancelled) override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus Completed(
        const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata,
        ID3D11DeviceContext* context, bool cancelled) override;
    void Erased(const pbcapturenormalize::CaptureErasure& erasure) noexcept override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus GetDiagnosticStatus() const noexcept;

    OptionalDiagnosticFanout(const OptionalDiagnosticFanout&) = delete;
    OptionalDiagnosticFanout& operator=(const OptionalDiagnosticFanout&) = delete;

private:
    struct Pending
    {
        bool active = false;
        bool primaryPending = false;
        bool diagnosticPending = false;
        bool diagnosticCancelled = false;
        pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
    };

    void RecordDiagnosticStatus(pbcapturenormalize::CaptureStatus status) noexcept;
    [[nodiscard]] static bool SameCompletion(const pbcapturenormalize::ScreenCaptureFrameMetadata& left,
        const pbcapturenormalize::ScreenCaptureFrameMetadata& right) noexcept;

    static constexpr std::size_t maximumSlots = 8;
    std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> primary_;
    std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> diagnostic_;
    mutable std::mutex pendingMutex_;
    std::array<Pending, maximumSlots> pending_;
    bool diagnosticDomainActive_ = false;
    mutable std::mutex diagnosticStatusMutex_;
    pbcapturenormalize::CaptureStatus diagnosticStatus_;
};

} // namespace pbapp::detail
