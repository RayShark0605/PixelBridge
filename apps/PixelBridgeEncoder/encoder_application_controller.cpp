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
    log_.Observe(runtime_.GetSnapshot(), true);
}

QString EncoderApplicationController::Start(const pbapp::EncoderConfig& config)
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

QString EncoderApplicationController::SetLogicalVisualFps(const std::uint32_t logicalVisualFps) noexcept
{
    try
    {
        if (IsActive())
        {
            return QStringLiteral("传输期间不能调整刷新率；请先按 Esc 停止传输");
        }
        const pbapp::RuntimeStatus status = runtime_.SetLogicalVisualFps(logicalVisualFps);
        PollSnapshot();
        return status ? QString() : QString::fromUtf8(status.message.data(), static_cast<int>(status.message.size()));
    }
    catch (...)
    {
        return QStringLiteral("无法刷新 Encoder FPS 状态");
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

pbapp::EncoderSnapshot EncoderApplicationController::StopAndGetSnapshot()
{
    runtime_.Stop();
    log_.Observe(runtime_.GetSnapshot(), true);
    return runtime_.GetSnapshot();
}

bool EncoderApplicationController::IsActive() const
{
    return pbapp::IsEncoderStateActive(runtime_.GetSnapshot().state);
}

void EncoderApplicationController::PollSnapshot()
{
    pbapp::EncoderSnapshot current = runtime_.GetSnapshot();
    if (IsTerminal(current.state) && (current.runGeneration != lastSnapshot_.runGeneration || !IsTerminal(lastSnapshot_.state)))
    {
        // A published snapshot can precede capture cleanup. Seal diagnostics
        // only after the worker is joined, preserving post-publish warnings.
        runtime_.Stop();
        current = runtime_.GetSnapshot();
    }
    log_.Observe(current, IsTerminal(current.state));
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

QString EncoderApplicationController::LogStatusText() const
{
    return log_.StatusText();
}

QString EncoderApplicationController::LogDirectory() const
{
    return log_.Directory();
}
