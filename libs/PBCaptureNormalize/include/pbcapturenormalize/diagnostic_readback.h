#pragma once

#include "pbcapturenormalize/screen_capture_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace pbcapturenormalize
{

namespace detail
{
struct DiagnosticReadbackTestAccess;
}

inline constexpr std::uint32_t diagnosticCpuBufferCount = 3;

struct DiagnosticReadbackConfig
{
    CaptureSize maximumRoiSize;
    std::uint32_t stagingTextureCount = 3;
    std::uint32_t maximumFrameAgeMilliseconds = 250;
    std::uint64_t maximumReadbackBytes = 256ull * 1024 * 1024;
};

struct DiagnosticReadbackBudget
{
    std::uint64_t bytesPerCpuBuffer = 0;
    std::uint64_t cpuBytes = 0;
    std::uint64_t stagingBytes = 0;
    std::uint64_t totalBytes = 0;
    bool operator==(const DiagnosticReadbackBudget&) const = default;
};

// Fixed allocation reservation at eight bytes per pixel, including all CPU
// buffers and all staging textures. Failure leaves the output unchanged.
[[nodiscard]] CaptureStatus CalculateDiagnosticReadbackBudget(const DiagnosticReadbackConfig& config, DiagnosticReadbackBudget& output) noexcept;

class CpuFrameProcessor
{
public:
    virtual ~CpuFrameProcessor() = default;
    // All four methods run on ONE CPU worker. Reset is also dispatched after
    // invalidation with no following frame. A null domain clears temporal state.
    // Reset/Commit/Discard must be bounded, noexcept, and perform no I/O, waits,
    // GPU access, or callbacks into capture/readback. Commit runs under the short
    // admission mutex, making invalidation and result publication linearizable.
    virtual void Reset(std::optional<ScreenCaptureDomain> domain) noexcept = 0;
    // Pixels are tightly row-packed, format-preserving bytes. The span is valid
    // ONLY during Analyze. Analyze must terminate in bounded time and may only
    // build a candidate, never advance accepted geometry/calibration/duplicates.
    // The processor must not own GPU objects or capture/readback references.
    [[nodiscard]] virtual CaptureStatus Analyze(const ScreenCaptureFrameMetadata& metadata, std::span<const std::byte> pixels, std::size_t rowPitch) = 0;
    virtual void Commit(const ScreenCaptureFrameMetadata& metadata) noexcept = 0;
    virtual void Discard() noexcept = 0;
};

enum class DiagnosticReadbackDropReason : std::uint8_t
{
    None, InactiveDomain, StaleObservation, ExpiredBeforeReadback, ExpiredAfterReadback,
    ExpiredBeforeAnalyze, ExpiredBeforeCommit, InvalidTimestamp, NoCpuBuffer, QueueReplaced,
    CancelledCompletion, StaleCompletion, MapNotReady, MapFailure, ProcessorFailure, Stopping, Count
};

struct DiagnosticReadbackSnapshot
{
    // This path is explicitly GPU -> CPU reference processing, not the fast path.
    bool diagnosticCpuReadback = true;
    bool active = false;
    bool resetPending = false;
    bool workerBusy = false;
    bool workerStopped = false;
    bool stopRequested = false;
    ScreenCaptureDomain domain;
    CaptureStatus error;
    // A graphics device removal invalidates the old domain but is not a fatal
    // CPU-worker error. The capture runtime owns the finite recovery policy.
    CaptureStatus lastGraphicsFailure;
    std::uint64_t deviceLossEvents = 0;
    DiagnosticReadbackBudget reservation;
    std::uint64_t residentStagingBytes = 0;
    std::uint64_t domainStarts = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t processorResets = 0;
    std::uint64_t submittedCopies = 0;
    std::uint64_t mappedFrames = 0;
    std::uint64_t readbackBytes = 0;
    std::uint64_t mapCalls = 0;
    std::uint64_t mapBlockingRetries = 0;
    std::uint64_t analyzedFrames = 0;
    std::uint64_t committedFrames = 0;
    std::uint64_t discardedCandidates = 0;
    std::uint64_t captureErasures = 0;
    std::uint64_t dropEvents = 0;
    std::uint64_t frameAgeHighWater100ns = 0;
    std::uint64_t readbackLatencyHighWater100ns = 0;
    std::uint64_t analysisLatencyHighWater100ns = 0;
    std::uint32_t pendingStagingFrames = 0;
    std::uint32_t stagingHighWater = 0;
    std::uint32_t queuedFrames = 0;
    std::uint32_t queueHighWater = 0;
    std::uint32_t cpuBuffersInUse = 0;
    std::uint32_t cpuBufferHighWater = 0;
    std::uint32_t lastMappedRowPitch = 0;
    DiagnosticReadbackDropReason lastDrop = DiagnosticReadbackDropReason::None;
    std::array<std::uint64_t, static_cast<std::size_t>(DiagnosticReadbackDropReason::Count)> drops{};
};

class DiagnosticCpuReadback final : public ScreenCaptureConsumer
{
public:
    [[nodiscard]] static CaptureStatus Create(const DiagnosticReadbackConfig& config, std::shared_ptr<CpuFrameProcessor> processor,
                                               std::shared_ptr<DiagnosticCpuReadback>& output) noexcept;
    ~DiagnosticCpuReadback() override;
    [[nodiscard]] std::uint64_t ReservedBytes() const noexcept override;
    [[nodiscard]] CaptureStatus ValidateConfiguration(const CaptureConfig& config) const noexcept override;
    [[nodiscard]] CaptureStatus DomainStarted(const ScreenCaptureDomain& domain, const CaptureEnvironment& environment, ID3D11Device* device) override;
    void DomainInvalidated(const ScreenCaptureDomain& domain) noexcept override;
    [[nodiscard]] CaptureStatus Submit(const ScreenCaptureFrame& frame, ID3D11DeviceContext* context) override;
    [[nodiscard]] CaptureStatus Completed(const ScreenCaptureFrameMetadata& metadata, ID3D11DeviceContext* context, bool cancelled) override;
    void Erased(const CaptureErasure& erasure) noexcept override;
    [[nodiscard]] DiagnosticReadbackSnapshot GetSnapshot() const noexcept;

    // Capture-owner/deferred-cleanup destruction only cancels, never joins a
    // slow Analyze. The worker retains bounded CPU buffers/processor, NOT GPU
    // resources. Applications may wait explicitly from their lifecycle thread.
    void RequestStop() noexcept;
    [[nodiscard]] CaptureStatus Stop(std::uint32_t maximumWaitMilliseconds = 3000) noexcept;
    DiagnosticCpuReadback(const DiagnosticCpuReadback&) = delete;
    DiagnosticCpuReadback& operator=(const DiagnosticCpuReadback&) = delete;

private:
    friend struct detail::DiagnosticReadbackTestAccess;
    struct Implementation;
    explicit DiagnosticCpuReadback(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

[[nodiscard]] const char* GetDiagnosticReadbackDropName(DiagnosticReadbackDropReason reason) noexcept;

} // namespace pbcapturenormalize
