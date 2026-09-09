#pragma once

#include "application_model.h"
#include "local_desktop_runtime.h"

#include <QObject>
#include <QTimer>

class DecoderApplicationController final : public QObject
{
    Q_OBJECT

public:
    explicit DecoderApplicationController(QObject* parent = nullptr, pbapp::DecoderRuntimeServices services = {});
    ~DecoderApplicationController() override;

    [[nodiscard]] QString Start(const pbapp::DecoderConfig& config);
    [[nodiscard]] QString ResolveLargeOutputConfirmation(std::uint64_t runGeneration, std::uint64_t requestId, bool accepted);
    void RequestStop() noexcept;
    [[nodiscard]] pbapp::DecoderSnapshot GetSnapshot() const;
    [[nodiscard]] pbapp::DecoderSnapshot StopAndGetSnapshot();
    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] bool SetStatusRefreshMilliseconds(int milliseconds);
    [[nodiscard]] int GetStatusRefreshMilliseconds() const;

signals:
    void SnapshotChanged();
    void TerminalStateReached();

private slots:
    void PollSnapshot();

private:
    pbapp::DecoderRuntime runtime_;
    QTimer pollTimer_;
    pbapp::DecoderSnapshot lastSnapshot_;
};
