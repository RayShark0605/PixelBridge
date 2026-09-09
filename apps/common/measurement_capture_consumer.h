#pragma once

#include "run_measurement.h"
#include "pbcapturenormalize/screen_capture_frame.h"

#include <memory>
#include <utility>

namespace pbapp::detail
{
// Transparent metadata-only observation. It neither retains the borrowed
// texture nor inserts a GPU stage, readback, admission rule or lease extension.
class MeasurementCaptureConsumer final : public pbcapturenormalize::ScreenCaptureConsumer
{
  public:
    MeasurementCaptureConsumer(std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> primary,
                               std::shared_ptr<RunMeasurementRecorder> recorder)
        : primary_(std::move(primary)), recorder_(std::move(recorder))
    {
    }
    [[nodiscard]] std::uint64_t ReservedBytes() const noexcept override
    {
        return primary_->ReservedBytes();
    }
    [[nodiscard]] pbcapturenormalize::CaptureStatus ValidateConfiguration(const pbcapturenormalize::CaptureConfig &config) const noexcept override
    {
        return primary_->ValidateConfiguration(config);
    }
    [[nodiscard]] pbcapturenormalize::CaptureStatus DomainStarted(const pbcapturenormalize::ScreenCaptureDomain &domain,
                                                                  const pbcapturenormalize::CaptureEnvironment &environment, ID3D11Device *device) override
    {
        return primary_->DomainStarted(domain, environment, device);
    }
    void DomainInvalidated(const pbcapturenormalize::ScreenCaptureDomain &domain) noexcept override
    {
        primary_->DomainInvalidated(domain);
    }
    [[nodiscard]] pbcapturenormalize::CaptureStatus Submit(const pbcapturenormalize::ScreenCaptureFrame &frame, ID3D11DeviceContext *context) override
    {
        recorder_->Record(RunMilestone::FirstVisualObservation);
        return primary_->Submit(frame, context);
    }
    [[nodiscard]] pbcapturenormalize::CaptureConsumerCompletion CompleteStage(const pbcapturenormalize::ScreenCaptureFrameMetadata &metadata,
                                                                              ID3D11Texture2D *texture, ID3D11DeviceContext *context, const bool cancelled) override
    {
        return primary_->CompleteStage(metadata, texture, context, cancelled);
    }
    [[nodiscard]] pbcapturenormalize::CaptureStatus Completed(const pbcapturenormalize::ScreenCaptureFrameMetadata &metadata,
                                                              ID3D11DeviceContext *context, const bool cancelled) override
    {
        return primary_->Completed(metadata, context, cancelled);
    }
    void Erased(const pbcapturenormalize::CaptureErasure &erasure) noexcept override
    {
        primary_->Erased(erasure);
    }

  private:
    std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> primary_;
    std::shared_ptr<RunMeasurementRecorder> recorder_;
};
} // namespace pbapp::detail
