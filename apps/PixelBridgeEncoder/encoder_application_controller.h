#pragma once

#include "application_model.h"
#include "local_desktop_runtime.h"

#include <QObject>
#include <QTimer>

class EncoderApplicationController final : public QObject
{
    Q_OBJECT

public:
    explicit EncoderApplicationController(QObject* parent = nullptr, pbapp::EncoderPresentationFactory presentationFactory = {});
    ~EncoderApplicationController() override;

    [[nodiscard]] QString Start(const pbapp::EncoderConfig& config);
    [[nodiscard]] QString SetLogicalVisualFps(std::uint32_t logicalVisualFps) noexcept;
    [[nodiscard]] QString EndAndDeleteSession(std::uint64_t expectedRunGeneration);
    void RequestStop() noexcept;
    [[nodiscard]] pbapp::EncoderSnapshot GetSnapshot() const;
    [[nodiscard]] bool IsActive() const;

signals:
    void SnapshotChanged();
    void TerminalStateReached();

private slots:
    void PollSnapshot();

private:
    pbapp::EncoderRuntime runtime_;
    QTimer pollTimer_;
    pbapp::EncoderSnapshot lastSnapshot_;
};
