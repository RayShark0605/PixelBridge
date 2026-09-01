#pragma once

#include "application_model.h"
#include "monitor_catalog.h"

#include "pbcompression/segment_compression.h"
#include "pbrenderd3d/data_window.h"
#include "pbscreenregion/screen_region.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace pbapp
{

struct EncoderConfig
{
    std::wstring sourcePath;
    bool compressionEnabled = false;
    int compressionLevel = 3;
    VisualProfile visualProfile = VisualProfile::DirectLevels2x2;
    std::optional<pbrenderd3d::PhysicalPoint> monitorClientOrigin;
    std::string runId;
    RemoteRunMetadata remoteMetadata;
    // Zero preserves the historical presentation-driven cadence for local
    // profiles only. RemoteVisual requires 1..5 and therefore establishes at
    // least 200 ms stable dwell; it never changes the encoded bytes.
    std::uint32_t logicalVisualFps = 0;
    std::uint32_t controlRepetitions = 4;
};

struct DecoderConfig
{
    std::wstring outputDirectory;
    CaptureBackend captureBackend = CaptureBackend::Wgc;
    VisualProfile visualProfile = VisualProfile::DirectLevels2x2;
    pbscreenregion::ScreenCaptureRegion region;
    std::string runId;
    RemoteRunMetadata remoteMetadata;
    std::optional<MonitorSafetySelection> monitorSafety;
    // Empty disables replay. Enabling is diagnostic-only and excludes the run
    // from main goodput baselines; it never changes demod/Receiver acceptance.
    std::wstring replayOutputPath;
    // Allows a non-Phase-1 ROI only for bounded receiver-side replay capture.
    // No Bootstrap, demodulation, FEC, Receiver mutation, or publish is run.
    bool diagnosticCaptureOnly = false;
    // Optional descriptor identity for capture-only evidence. This only tags
    // the sealed receiver-side raster; it never selects a production demodulator.
    std::optional<std::uint64_t> replayEvidenceVisualProfileId;
    // Nonempty selects bounded offline Replay v2 input instead of live WGC or
    // DXGI capture. The replay adapter only reconstructs the PB-owned BGRA ROI
    // texture and then enters the same CaptureDemodulator/Receiver pipeline.
    std::wstring replayInputPath;
    std::uint32_t replayMaximumCaptureFrames = 256;
    std::uint64_t replayMaximumFileBytes = 2ULL * 1024 * 1024 * 1024;
};

struct RuntimeStatus
{
    bool success = true;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return success;
    }

    [[nodiscard]] static RuntimeStatus Failure(std::string message)
    {
        return {false, std::move(message)};
    }
};

[[nodiscard]] RuntimeStatus ValidateEncoderConfig(const EncoderConfig& config);
[[nodiscard]] RuntimeStatus ValidateDecoderConfig(const DecoderConfig& config);
[[nodiscard]] pbcompression::CompressionResult<pbcompression::EncodedSegment> PrepareEncodedSegment(
    std::span<const std::byte> rawBytes, bool compressionEnabled, int compressionLevel);

// Qt-free application controller. Start launches one bounded worker and
// returns immediately. UI code reads immutable snapshot copies; it never owns
// DataWindow/FEC resources. Stop is idempotent and joins before destruction.
class EncoderRuntime
{
public:
    EncoderRuntime() = default;
    ~EncoderRuntime();
    EncoderRuntime(const EncoderRuntime&) = delete;
    EncoderRuntime& operator=(const EncoderRuntime&) = delete;

    [[nodiscard]] RuntimeStatus Start(const EncoderConfig& config);
    void RequestStop() noexcept;
    void Stop() noexcept;
    [[nodiscard]] EncoderSnapshot GetSnapshot() const;

private:
    void Run(EncoderConfig config, std::uint64_t runGeneration) noexcept;
    mutable std::mutex lifecycleMutex_;
    SnapshotStore<EncoderSnapshot> snapshot_;
    std::thread worker_;
    std::atomic<bool> stopRequested_ = false;
    std::atomic<bool> workerRunning_ = false;
    std::uint64_t nextRunGeneration_ = 1;
};

class DecoderRuntime
{
public:
    DecoderRuntime() = default;
    ~DecoderRuntime();
    DecoderRuntime(const DecoderRuntime&) = delete;
    DecoderRuntime& operator=(const DecoderRuntime&) = delete;

    [[nodiscard]] RuntimeStatus Start(const DecoderConfig& config);
    void RequestStop() noexcept;
    void Stop() noexcept;
    [[nodiscard]] DecoderSnapshot GetSnapshot() const;

private:
    void Run(const DecoderConfig& config, std::uint64_t runGeneration) noexcept;
    mutable std::mutex lifecycleMutex_;
    SnapshotStore<DecoderSnapshot> snapshot_;
    std::thread worker_;
    std::atomic<bool> stopRequested_ = false;
    std::atomic<bool> workerRunning_ = false;
    std::uint64_t nextRunGeneration_ = 1;
};

} // namespace pbapp
