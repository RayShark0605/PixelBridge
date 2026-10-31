#pragma once

#include "local_desktop_runtime.h"

#include <QObject>
#include <memory>

namespace pbencoder
{

class QtRasterPresentationTestAccess;

// The GUI owns this host through worker shutdown. RequestStop cancels worker
// waits; FinalizeStop releases GUI objects only after the worker has joined.
class QtRasterPresentationHost final : public QObject
{
    Q_OBJECT

public:
    explicit QtRasterPresentationHost(QObject* parent = nullptr);
    ~QtRasterPresentationHost() override;
    void BeginRun();
    [[nodiscard]] std::unique_ptr<pbapp::EncoderPresentation> CreatePresentation(const pbrenderd3d::DataWindowConfig& config);
    void RequestStop() noexcept;
    void FinalizeStop() noexcept;
    [[nodiscard]] bool HasPendingWork() const noexcept;

signals:
    void StopRequested();

private:
    friend class QtRasterPresentationTestAccess;
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}
