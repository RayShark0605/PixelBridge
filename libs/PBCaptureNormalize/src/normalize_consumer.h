#pragma once

#include "pbcapturenormalize/screen_capture_frame.h"

namespace pbcapturenormalize::detail
{

// Sole raw-to-normalized boundary, shared by both platform facades. Raw WGC
// consumers bypass this adapter by explicit API choice; strict APIs never do.
class NormalizeConsumer final : public RawRoiConsumer
{
public:
    [[nodiscard]] static CaptureStatus Create(const CaptureNormalizeConfig& config, CaptureBackendKind backend,
                                              std::shared_ptr<ScreenCaptureConsumer> consumer, std::shared_ptr<NormalizeConsumer>& output) noexcept;
    ~NormalizeConsumer() override;
    // Immutable, factory-only configuration; the runtime copies it before
    // starting its thread. The reference never conveys any GPU slot lifetime.
    [[nodiscard]] const CaptureConfig& GetRuntimeConfig() const noexcept;
    [[nodiscard]] CaptureNormalizeSnapshot GetSnapshot() const noexcept;
    [[nodiscard]] CaptureStatus EpochStarted(std::uint64_t epoch, const CaptureEnvironment& environment, ID3D11Device* device) override;
    void EpochInvalidated(std::uint64_t epoch) noexcept override;
    [[nodiscard]] CaptureStatus Submit(const RawRoiFrameMetadata& metadata, ID3D11Texture2D* texture, ID3D11DeviceContext* context) override;
    [[nodiscard]] CaptureStatus Completed(const RawRoiFrameMetadata& metadata, ID3D11DeviceContext* context, bool cancelled) override;
private:
    struct Implementation;
    explicit NormalizeConsumer(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace pbcapturenormalize::detail
