#include "encoder_application_controller.h"

#include <QString>

namespace
{

[[nodiscard]] bool IsTerminal(const pbapp::EncoderState state) noexcept
{
    return state == pbapp::EncoderState::Stopped || state == pbapp::EncoderState::Failed;
}

} // namespace

EncoderApplicationController::EncoderApplicationController(QObject* parent, pbapp::EncoderPresentationFactory presentationFactory) :
    QObject(parent), runtime_(std::move(presentationFactory))
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

QString EncoderApplicationController::SetLogicalVisualFps(const std::uint32_t logicalVisualFps) noexcept
{
    const pbapp::RuntimeStatus status = runtime_.SetLogicalVisualFps(logicalVisualFps);
    try
    {
        PollSnapshot();
        return status ? QString() : QString::fromUtf8(status.message.data(), static_cast<int>(status.message.size()));
    }
    catch (...)
    {
        return status ? QStringLiteral("无法刷新 Encoder FPS 状态") : QString::fromUtf8(status.message.data(), static_cast<int>(status.message.size()));
    }
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

QString EncoderApplicationController::EndAndDeleteSession(const std::uint64_t expectedRunGeneration)
{
    const pbapp::RuntimeStatus status = runtime_.EndAndDeleteSession(expectedRunGeneration);
    PollSnapshot();
    return status ? QString() : QString::fromUtf8(status.message.data(), static_cast<int>(status.message.size()));
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
        current.preparedSourceBytes != lastSnapshot_.preparedSourceBytes ||
        current.preparationComplete != lastSnapshot_.preparationComplete || current.sessionDeleted != lastSnapshot_.sessionDeleted ||
        current.sessionStateGeneration != lastSnapshot_.sessionStateGeneration ||
        current.frameSequence != lastSnapshot_.frameSequence || current.presentationEpoch != lastSnapshot_.presentationEpoch ||
        current.configuredLogicalVisualFps != lastSnapshot_.configuredLogicalVisualFps ||
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
