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

DecoderApplicationController::DecoderApplicationController(QObject* parent) : QObject(parent)
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
        current.captureDeliveredFrames != lastSnapshot_.captureDeliveredFrames ||
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
