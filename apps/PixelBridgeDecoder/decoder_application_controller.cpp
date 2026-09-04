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
}

QString DecoderApplicationController::Start(const pbapp::DecoderConfig& config)
{
    const pbapp::RuntimeStatus status = runtime_.Start(config);
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

bool DecoderApplicationController::IsActive() const
{
    return pbapp::IsDecoderStateActive(runtime_.GetSnapshot().state);
}

void DecoderApplicationController::PollSnapshot()
{
    const pbapp::DecoderSnapshot current = runtime_.GetSnapshot();
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
    if (changed)
    {
        emit SnapshotChanged();
    }
    if (becameTerminal)
    {
        emit TerminalStateReached();
    }
}
