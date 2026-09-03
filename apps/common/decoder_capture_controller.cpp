#include "decoder_capture_controller.h"

#include "pbscreencapturedxgi/dxgi_capture.h"
#include "pbscreencapturewgc/wgc_capture.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>

namespace pbapp
{
namespace
{
using namespace pbcapturenormalize;

class NativeDecoderCaptureSession final : public DecoderCaptureSession
{
public:
    explicit NativeDecoderCaptureSession(const CaptureBackend backend) : backend_(backend)
    {
    }
    CaptureStatus Start(const CaptureNormalizeConfig& config, std::shared_ptr<ScreenCaptureConsumer> consumer) noexcept override
    {
        const auto status = backend_ == CaptureBackend::Wgc ? pbscreencapturewgc::WgcCapture::CreateNormalized(config, std::move(consumer), wgc_, &failedStart_)
                                                            : pbscreencapturedxgi::DxgiCapture::Create(config, std::move(consumer), dxgi_, &failedStart_);
        if (!status)
        {
            failedStart_.error = status;
            failedStart_.state = CaptureState::Failed;
        }
        return status;
    }

    CaptureSnapshot GetSnapshot() const noexcept override
    {
        return wgc_ ? wgc_->GetSnapshot() : dxgi_ ? dxgi_->GetSnapshot() : failedStart_;
    }
    CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept override
    {
        return wgc_ ? wgc_->GetNormalizationSnapshot() : dxgi_ ? dxgi_->GetNormalizationSnapshot() : CaptureNormalizeSnapshot{};
    }

    void RequestStop() noexcept override
    {
        if (wgc_)
        {
            wgc_->RequestStop();
        }
        if (dxgi_)
        {
            dxgi_->RequestStop();
        }
    }

    CaptureStatus Stop() noexcept override
    {
        return wgc_ ? wgc_->Stop() : dxgi_ ? dxgi_->Stop() : failedStart_.error;
    }

private:
    CaptureBackend backend_;
    CaptureSnapshot failedStart_;
    std::unique_ptr<pbscreencapturewgc::WgcCapture> wgc_;
    std::unique_ptr<pbscreencapturedxgi::DxgiCapture> dxgi_;
};

bool DeviceRemovalStatus(const CaptureStatus status) noexcept
{
    return status.code == CaptureError::DeviceLost ||
           (status.code == CaptureError::NativeFailure &&
            (status.nativeError == DXGI_ERROR_DEVICE_REMOVED || status.nativeError == DXGI_ERROR_DEVICE_RESET || status.nativeError == DXGI_ERROR_DEVICE_HUNG));
}

std::string DescribeStatus(const CaptureStatus status)
{
    return std::string(GetCaptureErrorName(status.code)) + " stage=" + std::to_string(static_cast<unsigned int>(status.stage)) +
           " native=" + std::to_string(status.nativeError);
}
} // namespace

std::unique_ptr<DecoderCaptureSession> MakeNativeDecoderCaptureSession(const CaptureBackend backend)
{
    if (backend != CaptureBackend::Wgc && backend != CaptureBackend::Dxgi)
    {
        return {};
    }
    return std::make_unique<NativeDecoderCaptureSession>(backend);
}

const char* GetCaptureFallbackReasonName(const CaptureFallbackReason reason) noexcept
{
    switch (reason)
    {
    case CaptureFallbackReason::None:
        return "None";
    case CaptureFallbackReason::InitializationFailure:
        return "InitializationFailure";
    case CaptureFallbackReason::AccessLost:
        return "AccessLost";
    case CaptureFallbackReason::DeviceRebuildFailure:
        return "DeviceRebuildFailure";
    }
    return "Unknown";
}

CaptureStatus ValidateDecoderCapturePolicy(const CaptureBackend requested, const CaptureConfig& config) noexcept
{
    if (requested == CaptureBackend::Auto)
    {
        const auto wgc = pbscreencapturewgc::ValidateWgcCaptureConfig(config);
        return wgc ? pbscreencapturedxgi::ValidateDxgiCaptureConfig(config) : wgc;
    }
    if (requested == CaptureBackend::Wgc)
    {
        return pbscreencapturewgc::ValidateWgcCaptureConfig(config);
    }
    if (requested == CaptureBackend::Dxgi)
    {
        return pbscreencapturedxgi::ValidateDxgiCaptureConfig(config);
    }
    return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
}

DecoderCaptureController::DecoderCaptureController(const CaptureBackend requested, CaptureNormalizeConfig config, ConsumerFactory consumerFactory,
                                                   SessionFactory sessionFactory, UnixClock unixClock)
    : requested_(requested), config_(std::move(config)), consumerFactory_(std::move(consumerFactory)), sessionFactory_(std::move(sessionFactory)),
      unixClock_(std::move(unixClock))
{
    // Product capture never inherits the Encoder cadence or Replay sampling
    // rate. Diagnostic readback may sample independently downstream.
    config_.capture.minUpdateInterval100ns = 0;
    if (!unixClock_)
    {
        unixClock_ = []
        {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            return now > 0 ? static_cast<std::uint64_t>(now) : std::uint64_t{0};
        };
    }
}

DecoderCaptureController::~DecoderCaptureController()
{
    static_cast<void>(Stop());
}
CaptureStatus DecoderCaptureController::Attempt(const CaptureBackend backend)
{
    // Release the old consumer before reserving its replacement. In particular,
    // a CaptureDemodulator's owner-thread binding must never be reused here.
    consumer_.reset();
    const auto consumerStatus = consumerFactory_(consumer_);
    if (!consumerStatus || !consumer_)
    {
        return consumerStatus ? CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Consumer) : consumerStatus;
    }
    session_ = sessionFactory_(backend);
    if (!session_)
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Configuration);
    }
    attempted_ = backend;
    attempts_++;
    const auto status = session_->Start(config_, consumer_);
    if (status)
    {
        actual_ = backend;
    }
    else if (backend == CaptureBackend::Wgc)
    {
        wgcFailure_ = status;
    }
    else
    {
        dxgiFailure_ = status;
    }
    return status;
}

CaptureStatus DecoderCaptureController::Start()
{
    if (started_ || stopRequested_ || !consumerFactory_ || !sessionFactory_ ||
        (requested_ != CaptureBackend::Auto && requested_ != CaptureBackend::Wgc && requested_ != CaptureBackend::Dxgi))
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    started_ = true;
    terminalStatus_ = ValidateDecoderCapturePolicy(requested_, config_.capture);
    if (!terminalStatus_)
    {
        return terminalStatus_;
    }
    terminalStatus_ = Attempt(requested_ == CaptureBackend::Auto ? CaptureBackend::Wgc : requested_);
    if (!terminalStatus_ && requested_ == CaptureBackend::Auto && attempted_ == CaptureBackend::Wgc)
    {
        terminalStatus_ = Fallback(CaptureFallbackReason::InitializationFailure);
    }
    return terminalStatus_;
}

CaptureStatus DecoderCaptureController::Fallback(const CaptureFallbackReason reason)
{
    static_cast<void>(session_->Stop());
    const auto retired = session_->GetSnapshot();
    const auto normalized = session_->GetNormalizationSnapshot();
    if (!retired.shutdownComplete || retired.deferredCleanup || retired.liveFrameLeases != 0 || retired.busyRoiTextures != 0 || normalized.active ||
        (!retired.shutdownStatus && !DeviceRemovalStatus(retired.shutdownStatus)))
    {
        // A lifetime/resource failure is not another reason to switch. Keep
        // the bounded old session alive and fail closed instead of overlapping.
        return retired.shutdownStatus ? CaptureStatus::Failure(CaptureError::Timeout, CaptureStage::Shutdown) : retired.shutdownStatus;
    }
    const std::uint64_t previousEpoch = (std::max)(config_.capture.initialCaptureEpoch, retired.captureEpoch);
    if (previousEpoch >= (std::numeric_limits<std::uint64_t>::max)() - 1)
    {
        return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Recreate);
    }
    session_.reset();
    actual_.reset();
    fallbackReason_ = reason;
    // Timestamp the handoff after retirement, not the earlier failure poll.
    fallbackUnixMilliseconds_ = unixClock_();
    fallbackEpoch_ = previousEpoch + 1;
    config_.capture.initialCaptureEpoch = fallbackEpoch_;
    return Attempt(CaptureBackend::Dxgi);
}

CaptureStatus DecoderCaptureController::Poll()
{
    if (!started_ || stopRequested_ || !terminalStatus_ || !session_)
    {
        return terminalStatus_;
    }
    const auto current = session_->GetSnapshot();
    if (current.state != CaptureState::Failed && !current.deferredCleanup)
    {
        return {};
    }
    terminalStatus_ = current.error ? CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Session) : current.error;
    if (attempted_ == CaptureBackend::Wgc)
    {
        wgcFailure_ = terminalStatus_;
    }
    else
    {
        dxgiFailure_ = terminalStatus_;
    }
    if (requested_ == CaptureBackend::Auto && attempted_ == CaptureBackend::Wgc && fallbackReason_ == CaptureFallbackReason::None)
    {
        const auto reason = current.deviceRebuildFailed                      ? CaptureFallbackReason::DeviceRebuildFailure
                            : current.error.code == CaptureError::AccessLost ? CaptureFallbackReason::AccessLost
                                                                             : CaptureFallbackReason::None;
        if (reason != CaptureFallbackReason::None)
        {
            terminalStatus_ = Fallback(reason);
        }
    }
    return terminalStatus_;
}

CaptureSnapshot DecoderCaptureController::GetSnapshot() const noexcept
{
    return session_ ? session_->GetSnapshot() : CaptureSnapshot{};
}

CaptureNormalizeSnapshot DecoderCaptureController::GetNormalizationSnapshot() const noexcept
{
    return session_ ? session_->GetNormalizationSnapshot() : CaptureNormalizeSnapshot{};
}

bool DecoderCaptureController::CanAdmit(const ScreenCaptureFrameMetadata& metadata, const std::int64_t now100ns) noexcept
{
    const auto normalized = GetNormalizationSnapshot();
    const auto effectiveTime = ResolveEffectiveCaptureTime100ns(metadata.timestamp.monotonic100ns, metadata.timestamp.arrivalQpc100ns);
    const bool current =
        !stopRequested_ && terminalStatus_ && normalized.active && normalized.domain == metadata.domain && actual_ &&
        metadata.backend == (*actual_ == CaptureBackend::Wgc ? CaptureBackendKind::Wgc : CaptureBackendKind::Dxgi) && effectiveTime >= 0 &&
        ClassifyFrameAge(now100ns, effectiveTime, config_.capture.maximumFrameAgeMilliseconds).disposition == CaptureFrameAgeDisposition::Current;
    if (!current && admissionDrops_ != (std::numeric_limits<std::uint64_t>::max)())
    {
        admissionDrops_++;
    }
    return current;
}

void DecoderCaptureController::ApplyBinding(DecoderSnapshot& snapshot) const
{
    snapshot.requestedBackend = requested_;
    snapshot.actualBackend = actual_;
    snapshot.captureBackendAttempts = attempts_;
    snapshot.captureFallbackReason = GetCaptureFallbackReasonName(fallbackReason_);
    snapshot.captureFallbackUnixMilliseconds = fallbackUnixMilliseconds_;
    snapshot.captureFallbackEpoch = fallbackEpoch_;
    snapshot.captureAdmissionDrops = admissionDrops_;
    snapshot.backendReason = requested_ == CaptureBackend::Auto ? "Auto: WGC preferred; no FPS throttle" : "Explicit backend; automatic fallback disabled";
    if (fallbackReason_ != CaptureFallbackReason::None)
    {
        snapshot.backendReason += "; DXGI fallback=" + snapshot.captureFallbackReason + " epoch=" + std::to_string(fallbackEpoch_) +
                                  " unixMs=" + std::to_string(*fallbackUnixMilliseconds_);
    }
    if (!wgcFailure_)
    {
        snapshot.backendReason += "; WGC=" + DescribeStatus(wgcFailure_);
    }
    if (!dxgiFailure_)
    {
        snapshot.backendReason += "; DXGI=" + DescribeStatus(dxgiFailure_);
    }
    if (!terminalStatus_)
    {
        snapshot.backendReason += "; stopped=" + DescribeStatus(terminalStatus_);
    }
}

void DecoderCaptureController::RequestStop() noexcept
{
    stopRequested_ = true;
    if (session_)
    {
        session_->RequestStop();
    }
}

CaptureStatus DecoderCaptureController::Stop() noexcept
{
    RequestStop();
    return session_ ? session_->Stop() : terminalStatus_;
}

} // namespace pbapp
