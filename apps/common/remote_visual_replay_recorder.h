#pragma once

#include "pbcapturenormalize/diagnostic_readback.h"
#include "pbrealcapturereplay/replay_v2.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace pbapp
{

inline constexpr std::uint32_t remoteVisualReplayDefaultStopWaitMilliseconds = 30000;

struct RemoteVisualReplayRecorderConfig
{
    std::filesystem::path outputPath;
    pbrealcapturereplay::ReplayV2FileDescriptor descriptor;
    pbrealcapturereplay::ReplayV2Limits limits;
    std::uint32_t roiWidth = 0;
    std::uint32_t roiHeight = 0;
    DXGI_FORMAT pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    std::string displayIdentityUtf8;
    std::uint32_t dpiX = 0;
    std::uint32_t dpiY = 0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    std::uint32_t queueCapacity = 2;
};

struct RemoteVisualReplayRecorderSnapshot
{
    bool enabled = false;
    bool stopRequested = false;
    bool workerStopped = false;
    bool finalized = false;
    bool evidenceValid = true;
    std::uint64_t processingReservedBytes = 0;
    std::uint64_t analyzedFrames = 0;
    std::uint64_t enqueuedFrames = 0;
    std::uint64_t writtenFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t queuedDemodObservations = 0;
    std::uint64_t writtenDemodObservations = 0;
    std::uint64_t droppedDemodObservations = 0;
    std::uint32_t queueDepth = 0;
    std::uint32_t queueHighWater = 0;
    std::uint64_t fileBytes = 0;
    pbrealcapturereplay::ReplayStatus lastError;
};

// Diagnostic-only GPU->CPU fan-out processor. Analyze copies into one of a
// fixed number of preallocated slots; a private bounded worker performs file
// I/O. A full recorder queue drops only replay evidence and never blocks or
// changes production demodulation/Receiver acceptance.
class RemoteVisualReplayRecorder final : public pbcapturenormalize::CpuFrameProcessor
{
public:
    struct Implementation;

    [[nodiscard]] static pbcapturenormalize::CaptureStatus Create(
        const RemoteVisualReplayRecorderConfig& config,
        std::shared_ptr<RemoteVisualReplayRecorder>& output) noexcept;
    ~RemoteVisualReplayRecorder() override;

    [[nodiscard]] std::uint64_t ProcessingReservedBytes() const noexcept override;
    void Reset(std::optional<pbcapturenormalize::ScreenCaptureDomain> domain) noexcept override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus Analyze(
        const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata,
        std::span<const std::byte> pixels, std::size_t rowPitch) override;
    void Commit(const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata) noexcept override;
    void Discard() noexcept override;

    void RecordDemodObservation(
        const pbrealcapturereplay::ReplayV2DemodObservationView& observation) noexcept;
    void RequestStop() noexcept;
    [[nodiscard]] pbcapturenormalize::CaptureStatus Stop(
        std::uint32_t maximumWaitMilliseconds = remoteVisualReplayDefaultStopWaitMilliseconds) noexcept;
    [[nodiscard]] RemoteVisualReplayRecorderSnapshot GetSnapshot() const noexcept;

    RemoteVisualReplayRecorder(const RemoteVisualReplayRecorder&) = delete;
    RemoteVisualReplayRecorder& operator=(const RemoteVisualReplayRecorder&) = delete;

private:
    explicit RemoteVisualReplayRecorder(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace pbapp
