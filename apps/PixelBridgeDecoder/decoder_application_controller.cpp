#include "decoder_application_controller.h"

#include <QString>

namespace
{

[[nodiscard]] bool IsTerminal(const pbapp::DecoderState state) noexcept
{
    return state == pbapp::DecoderState::Stopped || state == pbapp::DecoderState::Completed ||
        state == pbapp::DecoderState::Failed;
}

} // namespace

DecoderApplicationController::DecoderApplicationController(QObject* parent, pbapp::DecoderRuntimeServices services) :
    QObject(parent), runtime_(std::move(services))
{
    pollTimer_.setInterval(100);
    pollTimer_.setTimerType(Qt::CoarseTimer);
    connect(&pollTimer_, &QTimer::timeout, this, &DecoderApplicationController::PollSnapshot);
    pollTimer_.start();
}

DecoderApplicationController::~DecoderApplicationController()
{
    pollTimer_.stop();
    runtime_.Stop();
    log_.Observe(runtime_.GetSnapshot(), true);
}

QString DecoderApplicationController::Start(const pbapp::DecoderConfig& config)
{
    if (!IsActive())
    {
        log_.Begin();
    }
    const pbapp::RuntimeStatus status = runtime_.Start(config);
    if (!status && !IsActive())
    {
        log_.StartRejected(QString::fromStdString(status.message));
    }
    PollSnapshot();
    return status ? QString() : QString::fromUtf8(status.message.data(), static_cast<int>(status.message.size()));
}

void DecoderApplicationController::RequestStop() noexcept
{
    runtime_.RequestStop();
    try
    {
        PollSnapshot();
    }
    catch (...)
    {
    }
}

QString DecoderApplicationController::ResolveLargeOutputConfirmation(const std::uint64_t runGeneration,
    const std::uint64_t requestId, const bool accepted)
{
    const pbapp::RuntimeStatus status = runtime_.ResolveLargeOutputConfirmation(runGeneration, requestId, accepted);
    PollSnapshot();
    return status ? QString() : QString::fromUtf8(status.message.data(), static_cast<int>(status.message.size()));
}

pbapp::DecoderSnapshot DecoderApplicationController::GetSnapshot() const
{
    return runtime_.GetSnapshot();
}

pbapp::DecoderSnapshot DecoderApplicationController::StopAndGetSnapshot()
{
    runtime_.Stop();
    log_.Observe(runtime_.GetSnapshot(), true);
    return runtime_.GetSnapshot();
}

bool DecoderApplicationController::IsActive() const
{
    return pbapp::IsDecoderStateActive(runtime_.GetSnapshot().state);
}

bool DecoderApplicationController::SetStatusRefreshMilliseconds(const int milliseconds)
{
    if (milliseconds < 100 || milliseconds > 2000)
    {
        return false;
    }
    pollTimer_.setInterval(milliseconds);
    return true;
}

int DecoderApplicationController::GetStatusRefreshMilliseconds() const
{
    return pollTimer_.interval();
}

void DecoderApplicationController::PollSnapshot()
{
    pbapp::DecoderSnapshot current = runtime_.GetSnapshot();
    if (IsTerminal(current.state) && (current.runGeneration != lastSnapshot_.runGeneration || !IsTerminal(lastSnapshot_.state)))
    {
        // A published snapshot can precede capture cleanup. Seal diagnostics
        // only after the worker is joined, preserving post-publish warnings.
        runtime_.Stop();
        current = runtime_.GetSnapshot();
    }
    log_.Observe(current, IsTerminal(current.state));
    const bool changed = current.runGeneration != lastSnapshot_.runGeneration || current.state != lastSnapshot_.state ||
        current.largeOutputConfirmationRequestId != lastSnapshot_.largeOutputConfirmationRequestId ||
        current.largeOutputConfirmationState != lastSnapshot_.largeOutputConfirmationState ||
        current.verifiedSegmentCount != lastSnapshot_.verifiedSegmentCount || current.resumeStateGeneration != lastSnapshot_.resumeStateGeneration ||
        current.captureStallActive != lastSnapshot_.captureStallActive || current.visualStallActive != lastSnapshot_.visualStallActive ||
        current.captureDeliveredFrames != lastSnapshot_.captureDeliveredFrames ||
        current.actualBackend != lastSnapshot_.actualBackend || current.backendReason != lastSnapshot_.backendReason ||
        current.verifiedRawBytes != lastSnapshot_.verifiedRawBytes || current.captureEpoch != lastSnapshot_.captureEpoch ||
        current.statusMessage != lastSnapshot_.statusMessage || current.errorDetail != lastSnapshot_.errorDetail;
    const bool becameTerminal = IsTerminal(current.state) && !IsTerminal(lastSnapshot_.state);
    lastSnapshot_ = current;
    if (changed || pbapp::IsDecoderStateActive(current.state))
    {
        emit SnapshotChanged();
    }
    if (becameTerminal)
    {
        emit TerminalStateReached();
    }
}

QString DecoderApplicationController::LogStatusText() const
{
    return log_.StatusText();
}

QString DecoderApplicationController::LogDirectory() const
{
    return log_.Directory();
}

pbapp::DecoderActivity DecoderApplicationController::GetActivity() const
{
    return log_.GetDecoderActivity();
}
