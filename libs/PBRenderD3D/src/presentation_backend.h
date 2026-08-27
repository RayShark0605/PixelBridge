#pragma once

#include "pbrenderd3d/data_window.h"

namespace pbrenderd3d
{

enum class BackendWake : std::uint8_t
{
    Message,
    FramePermit,
    Timeout,
    Failed
};

struct BackendWaitResult
{
    BackendWake wake = BackendWake::Message;
    PresentationStatus error;
};

struct BackendPresentResult
{
    PresentationStatus error;
    pbpresenttiming::PresentOutcome outcome = pbpresenttiming::PresentOutcome::Success;
    std::optional<std::uint32_t> presentId;
    std::int32_t presentIdNativeStatus = 0;
};

struct BackendStatistics
{
    pbpresenttiming::StatisticsStatus status = pbpresenttiming::StatisticsStatus::Unavailable;
    pbpresenttiming::FrameStatistics statistics;
    std::int32_t nativeStatus = 0;
    PresentationStatus error;
};

struct BackendDiagnostics
{
    std::uint64_t swapChainGeneration = 0;
    std::uint64_t bufferGeneration = 0;
    std::uint64_t verifiedUploads = 0;
    std::uint64_t debugErrors = 0;
    std::uint64_t framePermits = 0;
    std::uint64_t presentCalls = 0;
    std::uint64_t liveGraphicsObjects = 0;
    std::uint64_t liveOwnedHandles = 0;
    bool warp = false;
};

// Private OS seam. The production and fake backends execute the same owner
// state machine; tests do not substitute a second renderer/scheduler.
class PresentationBackend
{
public:
    virtual ~PresentationBackend() = default;
    [[nodiscard]] virtual std::int64_t GetQpcFrequency() const noexcept = 0;
    [[nodiscard]] virtual std::int64_t NowQpc() const noexcept = 0;
    [[nodiscard]] virtual PresentationStatus Initialize(const DataWindowConfig& config) noexcept = 0;
    [[nodiscard]] virtual PresentationResult<WindowEnvironment> PollEnvironment() noexcept = 0;
    [[nodiscard]] virtual PresentationStatus Reconfigure(const WindowEnvironment& environment) noexcept = 0;
    [[nodiscard]] virtual PresentationContract GetContract() const noexcept = 0;
    [[nodiscard]] virtual BackendDiagnostics GetDiagnostics() const noexcept = 0;
    [[nodiscard]] virtual BackendWaitResult Wait(bool requestFramePermit, std::uint32_t timeoutMilliseconds) noexcept = 0;
    [[nodiscard]] virtual PresentationStatus Upload(std::span<const std::byte> pixels) noexcept = 0;
    [[nodiscard]] virtual BackendPresentResult Present() noexcept = 0;
    [[nodiscard]] virtual BackendStatistics GetStatistics() noexcept = 0;
    // Wake/Cancel are the only backend calls made off-owner; their event
    // remains alive until the owner is joined and the backend is destroyed.
    virtual void Wake() noexcept = 0;
    virtual void Cancel() noexcept = 0;
    virtual void Shutdown() noexcept = 0;
    [[nodiscard]] virtual std::uintptr_t GetWindowToken() const noexcept = 0;
};

struct NativeBackendTestOptions
{
    bool warp = false;
    bool debugLayer = false;
    bool verifyUploads = false;
    PresentationStage failAfterStage = PresentationStage::None;
    BackendDiagnostics* shutdownDiagnostics = nullptr;
};

[[nodiscard]] std::unique_ptr<PresentationBackend> MakeNativeBackend(const NativeBackendTestOptions& options = {});

class DataWindowTestAccess
{
public:
    [[nodiscard]] static PresentationResult<std::unique_ptr<DataWindow>> Create(const DataWindowConfig& config,
                                                                                std::unique_ptr<PresentationBackend> backend) noexcept;
    [[nodiscard]] static std::uintptr_t GetWindowToken(const DataWindow& window) noexcept;
    [[nodiscard]] static BackendDiagnostics GetDiagnostics(const DataWindow& window) noexcept;
};

}
