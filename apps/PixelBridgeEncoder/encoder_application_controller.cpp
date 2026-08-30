#include "encoder_application_controller.h"

#include <QString>

namespace
{

[[nodiscard]] bool IsTerminal(const pbapp::EncoderState state) noexcept
{
    return state == pbapp::EncoderState::Stopped || state == pbapp::EncoderState::Failed;
}

} // namespace

EncoderApplicationController::EncoderApplicationController(QObject* parent) : QObject(parent)
{
    pollTimer_.setInterval(100);
    pollTimer_.setTimerType(Qt::CoarseTimer);
    connect(&pollTimer_, &QTimer::timeout, this, &EncoderApplicationController::PollSnapshot);
    pollTimer_.start();
}

EncoderApplicationController::~EncoderApplicationController()
{
    pollTimer_.stop();
    runtime_.Stop();
}

QString EncoderApplicationController::Start(const pbapp::EncoderConfig& config)
{
    const pbapp::RuntimeStatus status = runtime_.Start(config);
    PollSnapshot();
    return status ? QString() : QString::fromUtf8(status.message.data(), static_cast<int>(status.message.size()));
}

void EncoderApplicationController::RequestStop() noexcept
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

pbapp::EncoderSnapshot EncoderApplicationController::GetSnapshot() const
{
    return runtime_.GetSnapshot();
}

bool EncoderApplicationController::IsActive() const
{
    return pbapp::IsEncoderStateActive(runtime_.GetSnapshot().state);
}

void EncoderApplicationController::PollSnapshot()
{
    const pbapp::EncoderSnapshot current = runtime_.GetSnapshot();
    const bool changed = current.runGeneration != lastSnapshot_.runGeneration || current.state != lastSnapshot_.state ||
        current.frameSequence != lastSnapshot_.frameSequence || current.presentationEpoch != lastSnapshot_.presentationEpoch ||
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
