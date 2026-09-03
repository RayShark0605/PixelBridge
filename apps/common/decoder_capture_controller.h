#pragma once

#include "application_model.h"
#include "pbcapturenormalize/screen_capture_frame.h"

#include <functional>
#include <memory>

namespace pbapp
{

// OS-only seam. Production uses the existing normalized WGC/DXGI owner, inbox
// and lease ring; deterministic tests replace only this session interface.
class DecoderCaptureSession
{
public:
    virtual ~DecoderCaptureSession() = default;
    [[nodiscard]] virtual pbcapturenormalize::CaptureStatus Start(const pbcapturenormalize::CaptureNormalizeConfig& config,
                                                                  std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> consumer) noexcept = 0;
    [[nodiscard]] virtual pbcapturenormalize::CaptureSnapshot GetSnapshot() const noexcept = 0;
    [[nodiscard]] virtual pbcapturenormalize::CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept = 0;
    virtual void RequestStop() noexcept = 0;
    [[nodiscard]] virtual pbcapturenormalize::CaptureStatus Stop() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<DecoderCaptureSession> MakeNativeDecoderCaptureSession(CaptureBackend backend);
[[nodiscard]] pbcapturenormalize::CaptureStatus ValidateDecoderCapturePolicy(CaptureBackend requested,
                                                                             const pbcapturenormalize::CaptureConfig& config) noexcept;

enum class CaptureFallbackReason : std::uint8_t
{
    None,
    InitializationFailure,
    AccessLost,
    DeviceRebuildFailure
};

[[nodiscard]] const char* GetCaptureFallbackReasonName(CaptureFallbackReason reason) noexcept;

// All policy/admission calls run on the Receiver controller thread. A switch
// cannot race Receiver processing. Backend callbacks retain their sole GPU
// owner thread; consumers are rebuilt, not migrated between those threads.
class DecoderCaptureController
{
public:
    using UnixClock = std::function<std::uint64_t()>;
    using SessionFactory = std::function<std::unique_ptr<DecoderCaptureSession>(CaptureBackend)>;
    using ConsumerFactory = std::function<pbcapturenormalize::CaptureStatus(std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer>&)>;

    DecoderCaptureController(CaptureBackend requested, pbcapturenormalize::CaptureNormalizeConfig config, ConsumerFactory consumerFactory,
                             SessionFactory sessionFactory = MakeNativeDecoderCaptureSession, UnixClock unixClock = {});
    ~DecoderCaptureController();
    [[nodiscard]] pbcapturenormalize::CaptureStatus Start();
    [[nodiscard]] pbcapturenormalize::CaptureStatus Poll();
    [[nodiscard]] pbcapturenormalize::CaptureSnapshot GetSnapshot() const noexcept;
    [[nodiscard]] pbcapturenormalize::CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept;
    // Last admission check immediately before Receiver processing, including
    // source identity and age, not merely a numerically equal CaptureEpoch.
    [[nodiscard]] bool CanAdmit(const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata, std::int64_t now100ns) noexcept;
    void ApplyBinding(DecoderSnapshot& snapshot) const;
    void RequestStop() noexcept;
    [[nodiscard]] pbcapturenormalize::CaptureStatus Stop() noexcept;

private:
    [[nodiscard]] pbcapturenormalize::CaptureStatus Attempt(CaptureBackend backend);
    [[nodiscard]] pbcapturenormalize::CaptureStatus Fallback(CaptureFallbackReason reason);

    CaptureBackend requested_;
    pbcapturenormalize::CaptureNormalizeConfig config_;
    ConsumerFactory consumerFactory_;
    SessionFactory sessionFactory_;
    UnixClock unixClock_;
    std::unique_ptr<DecoderCaptureSession> session_;
    std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> consumer_;
    std::optional<CaptureBackend> actual_;
    std::optional<CaptureBackend> attempted_;
    pbcapturenormalize::CaptureStatus terminalStatus_;
    pbcapturenormalize::CaptureStatus wgcFailure_;
    pbcapturenormalize::CaptureStatus dxgiFailure_;
    CaptureFallbackReason fallbackReason_ = CaptureFallbackReason::None;
    std::optional<std::uint64_t> fallbackUnixMilliseconds_;
    std::uint64_t fallbackEpoch_ = 0;
    std::uint64_t admissionDrops_ = 0;
    std::uint32_t attempts_ = 0;
    bool started_ = false;
    bool stopRequested_ = false;
};

} // namespace pbapp
