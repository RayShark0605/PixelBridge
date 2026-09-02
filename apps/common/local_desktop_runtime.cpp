#include "local_desktop_runtime.h"

#include "optional_diagnostic_fanout.h"
#include "remote_visual_replay_recorder.h"
#include "run_report.h"

#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/local_desktop_bootstrap.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/remote_visual.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbmodulation/shape_chroma.h"
#include "pbouterfec/direct_repeat.h"
#include "pbouterfec/wirehair_v2.h"
#include "pbreceiver/receiver_ingress.h"
#include "pbrealcapturereplay/replay_v2.h"
#include "pbscreencapturedxgi/dxgi_capture.h"
#include "pbscreencapturewgc/wgc_capture.h"
#include "pbdemodd3d11/capture_demodulator.h"
#include "pbstorage/output_file.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/byte_io.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/session_random.h"
#include "pbprotocol/transport_block_codec.h"
#include "pbtelemetry/telemetry.h"
#include "pbcapturenormalize/diagnostic_readback.h"

#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace pbapp
{
namespace
{

using detail::OptionalDiagnosticFanout;

inline constexpr std::uint32_t outerBlockBytes = 1314;
inline constexpr std::size_t informationBytes = 1350;
inline constexpr std::size_t codewordBytes = 2025;
inline constexpr std::uint32_t minimumControlRepetitions = 1;
inline constexpr std::uint32_t maximumControlRepetitions = 64;
inline constexpr std::uint32_t maximumLogicalVisualFps = 240;
inline constexpr std::uint32_t maximumRemoteVisualLogicalFps = 5;
inline constexpr std::uint32_t maximumReplayCaptureFramesPerSecond = 60;
inline constexpr std::uint64_t timeUnitsPerSecond100ns = 10000000ULL;
inline constexpr std::uint32_t captureQueuedFrameLimit = 4;
inline constexpr std::uint32_t captureDemodulatorSlotCount = 4;
inline constexpr std::uint32_t captureResultQueueCapacity = 128;
inline constexpr std::uint64_t maximumDemodulatorResidentBytes = 128ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t maximumRemoteVisualLowFpsDemodulatorResidentBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t maximumReplayDemodulatorResidentBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
inline constexpr std::uint64_t maximumCaptureResidentBytes = 512ULL * mebibyte;
inline constexpr std::uint64_t maximumRemoteVisualLowFpsCaptureResidentBytes = 1024ULL * mebibyte;
inline constexpr std::uint64_t maximumRemoteVisualLowFpsReplayCaptureResidentBytes = 2048ULL * mebibyte;
inline constexpr std::uint64_t maximumProductionReplayReadbackResidentBytes = 768ULL * mebibyte;
inline constexpr std::uint64_t maximumProductionReplayProcessorResidentBytes = 128ULL * mebibyte;
inline constexpr std::uint64_t maximumRoiResidentBytes = 128ULL * mebibyte;
inline constexpr std::uint32_t remoteVisualReplayRecorderQueueCapacity = 2;
inline constexpr std::uint32_t remoteVisualReplayPostCompletionTailMilliseconds = 2000;
inline constexpr std::uint32_t minimumRemoteVisualLowFpsRoiWidth = phase1CanvasWidth / 2;
inline constexpr std::uint32_t minimumRemoteVisualLowFpsRoiHeight = phase1CanvasHeight / 2;
inline constexpr std::uint32_t maximumRemoteVisualLowFpsRoiWidth = phase1CanvasWidth * 2;
inline constexpr std::uint32_t maximumRemoteVisualLowFpsRoiHeight = phase1CanvasHeight * 2;
static_assert(pbdesktoplevels::kPayloadBytes == outerBlockBytes);
static_assert(pbdesktoplevels::kInfoBytes == informationBytes);
static_assert(pbdesktoplevels::kCodewordBytes == codewordBytes);
static_assert(pbmodulation::kRemoteVisualLowFpsDataBytes ==
    pbmodulation::kRemoteVisualLowFpsCodewords * codewordBytes);

[[nodiscard]] bool IsReplayEvidenceVisualProfileId(const std::uint64_t visualProfileId) noexcept
{
    return visualProfileId == pbmodulation::kDesktopLevels2ProfileId ||
        visualProfileId == pbmodulation::kShapeChromaProfileId ||
        visualProfileId == pbmodulation::kRemoteVisualLowFpsProfileId;
}

[[nodiscard]] std::uint64_t CalculateReplaySamplingInterval100ns(const std::uint32_t maximumFramesPerSecond) noexcept
{
    return maximumFramesPerSecond == 0 ? 0 :
        (timeUnitsPerSecond100ns + maximumFramesPerSecond - 1) / maximumFramesPerSecond;
}

class RuntimeFailure final : public std::runtime_error
{
public:
    explicit RuntimeFailure(const std::string& message) : std::runtime_error(message)
    {
    }
};

class WorkerRunningGuard
{
public:
    explicit WorkerRunningGuard(std::atomic<bool>& running) noexcept : running_(running)
    {
    }

    ~WorkerRunningGuard()
    {
        running_ = false;
    }

    WorkerRunningGuard(const WorkerRunningGuard&) = delete;
    WorkerRunningGuard& operator=(const WorkerRunningGuard&) = delete;

private:
    std::atomic<bool>& running_;
};

void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw RuntimeFailure(message);
    }
}

template <typename ResultType>
void RequireResult(const ResultType& result, const std::string& message)
{
    Require(static_cast<bool>(result), message);
}

[[nodiscard]] std::string Utf8FromWide(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    Require(value.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()),
        "UTF-16 path exceeds the Win32 conversion limit");
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    Require(count > 0, "UTF-16 path cannot be represented as UTF-8");
    std::string output(static_cast<std::size_t>(count), '\0');
    Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), output.data(), count, nullptr, nullptr) == count,
        "UTF-16 path conversion changed between passes");
    return output;
}

[[nodiscard]] std::string DigestHex(const std::array<std::byte, pbprotocol::kDigestBytes>& digest)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const std::byte value : digest)
    {
        stream << std::setw(2) << static_cast<unsigned int>(std::to_integer<std::uint8_t>(value));
    }
    return stream.str();
}

[[nodiscard]] std::string SessionIdHex(const pbprotocol::SessionId& sessionId)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const std::byte value : sessionId.bytes)
    {
        stream << std::setw(2) << static_cast<unsigned int>(std::to_integer<std::uint8_t>(value));
    }
    return stream.str();
}

[[nodiscard]] std::string GenerateRunId()
{
    const auto sessionId = pbprotocol::GenerateRandomSessionId();
    RequireResult(sessionId, "OS CSPRNG RunId generation failed");
    return SessionIdHex(sessionId.Value());
}

[[nodiscard]] bool IsValidRunId(const std::string_view runId) noexcept
{
    if (runId.empty())
    {
        return true;
    }
    return runId.size() == 32 && std::ranges::all_of(runId, [](const char character)
    {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

[[nodiscard]] bool IsValidMetadataNumber(const std::optional<double>& value, const double maximum,
    const bool allowZero = false) noexcept
{
    return !value || (std::isfinite(*value) && (allowZero ? *value >= 0 : *value > 0) && *value <= maximum);
}

[[nodiscard]] bool IsValidMetadataRect(const std::optional<MetadataPhysicalRect>& rectangle) noexcept
{
    if (!rectangle)
    {
        return true;
    }
    const auto width = static_cast<std::int64_t>(rectangle->right) - rectangle->left;
    const auto height = static_cast<std::int64_t>(rectangle->bottom) - rectangle->top;
    return width > 0 && height > 0 && width <= 32768 && height <= 32768;
}

[[nodiscard]] bool IsValidRemoteMetadata(const RemoteRunMetadata& metadata) noexcept
{
    const std::array<std::string_view, 14> strings{metadata.runId, metadata.remoteProvider, metadata.providerVersion,
        metadata.remoteMode, metadata.computerBDisplayResolution, metadata.computerADisplayResolution,
        metadata.remoteResolution, metadata.letterboxStatus, metadata.cropStatus, metadata.geometryStatus,
        metadata.networkType, metadata.protectedMonitorIdentity, metadata.experimentMonitorIdentity,
        metadata.notes};
    std::size_t totalBytes = 0;
    for (const std::string_view value : strings)
    {
        if (value.size() > 1024 || totalBytes > 8192 - value.size() || value.find('\0') != std::string_view::npos ||
            !pbprotocol::ValidateUtf8(value))
        {
            return false;
        }
        totalBytes += value.size();
    }
    const bool validChannel = metadata.channelType == ChannelType::LocalDesktop ||
        metadata.channelType == ChannelType::RemoteVisual || metadata.channelType == ChannelType::Other;
    const bool validChroma = metadata.chromaMode == ChromaMode::Unknown || metadata.chromaMode == ChromaMode::Chroma444 ||
        metadata.chromaMode == ChromaMode::Chroma420;
    const auto ValidProvenance = [](const MetadataProvenance provenance) noexcept
    {
        return provenance == MetadataProvenance::NotProvided || provenance == MetadataProvenance::Manual ||
            provenance == MetadataProvenance::PixelBridgeObserved || provenance == MetadataProvenance::RemoteUiVisible;
    };
    const bool validProvider = metadata.channelType != ChannelType::RemoteVisual ||
        std::ranges::any_of(metadata.remoteProvider, [](const unsigned char character)
        {
            return character > 0x20U && character != 0x7FU;
        });
    return IsValidRunId(metadata.runId) && validChannel && validProvider && validChroma &&
        ValidProvenance(metadata.remoteUiProvenance) &&
        ValidProvenance(metadata.geometryProvenance) && ValidProvenance(metadata.networkProvenance) &&
        IsValidMetadataNumber(metadata.targetFps, 1000) && IsValidMetadataNumber(metadata.observedFps, 1000, true) &&
        IsValidMetadataNumber(metadata.computerBRefreshRate, 1000) &&
        IsValidMetadataNumber(metadata.computerARefreshRate, 1000) &&
        IsValidMetadataNumber(metadata.estimatedScaleX, 16) && IsValidMetadataNumber(metadata.estimatedScaleY, 16) &&
        IsValidMetadataNumber(metadata.observedBandwidthMbps, 1000000, true) &&
        IsValidMetadataNumber(metadata.observedLatencyMilliseconds, 10000000, true) &&
        IsValidMetadataRect(metadata.remoteWindowPhysicalRect) && IsValidMetadataRect(metadata.selectedRoiPhysicalRect);
}

[[nodiscard]] bool TryMakePhysicalRect(const pbrenderd3d::PhysicalPoint origin, const std::uint32_t width,
    const std::uint32_t height, RECT& output) noexcept
{
    if (origin.x == (std::numeric_limits<std::int32_t>::min)() ||
        origin.y == (std::numeric_limits<std::int32_t>::min)() || width == 0 || height == 0)
    {
        return false;
    }
    const std::int64_t right = static_cast<std::int64_t>(origin.x) + width;
    const std::int64_t bottom = static_cast<std::int64_t>(origin.y) + height;
    if (right > (std::numeric_limits<LONG>::max)() || bottom > (std::numeric_limits<LONG>::max)())
    {
        return false;
    }
    output = {origin.x, origin.y, static_cast<LONG>(right), static_cast<LONG>(bottom)};
    return true;
}

[[nodiscard]] bool TryGetMonitorDimensions(const MonitorInfo& monitor, std::uint32_t& width,
    std::uint32_t& height) noexcept
{
    const std::int64_t signedWidth = static_cast<std::int64_t>(monitor.physicalRect.right) - monitor.physicalRect.left;
    const std::int64_t signedHeight = static_cast<std::int64_t>(monitor.physicalRect.bottom) - monitor.physicalRect.top;
    if (signedWidth <= 0 || signedHeight <= 0 ||
        signedWidth > (std::numeric_limits<std::uint32_t>::max)() ||
        signedHeight > (std::numeric_limits<std::uint32_t>::max)())
    {
        return false;
    }
    width = static_cast<std::uint32_t>(signedWidth);
    height = static_cast<std::uint32_t>(signedHeight);
    return true;
}

[[nodiscard]] bool IsSupportedRemoteVisualFullscreenMonitor(const MonitorInfo& monitor) noexcept
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    return TryGetMonitorDimensions(monitor, width, height) &&
        width >= phase1CanvasWidth && width <= maximumRemoteVisualLowFpsRoiWidth &&
        height >= phase1CanvasHeight && height <= maximumRemoteVisualLowFpsRoiHeight &&
        monitor.rotation == DXGI_MODE_ROTATION_IDENTITY;
}

[[nodiscard]] MonitorSafetyStatus RevalidateSingleMonitorFullscreen(const MonitorInfo& expected) noexcept
{
    std::vector<MonitorInfo> monitors;
    const MonitorCatalogStatus catalog = EnumerateMonitors(monitors);
    if (!catalog)
    {
        return {MonitorSafetyError::CatalogFailure, catalog};
    }
    const auto current = std::find_if(monitors.begin(), monitors.end(), [&expected](const MonitorInfo& monitor)
    {
        return monitor.deviceName == expected.deviceName;
    });
    return current != monitors.end() && SameMonitorIdentity(expected, *current) ? MonitorSafetyStatus{} :
        MonitorSafetyStatus{MonitorSafetyError::TopologyChanged, {}};
}

void ComposeRemoteVisualFullscreenBgra(const std::span<const std::byte> source, const std::uint32_t destinationWidth,
    const std::uint32_t destinationHeight, std::span<std::byte> destination)
{
    Require(destinationWidth >= phase1CanvasWidth && destinationWidth <= maximumRemoteVisualLowFpsRoiWidth &&
        destinationHeight >= phase1CanvasHeight && destinationHeight <= maximumRemoteVisualLowFpsRoiHeight,
        "remote-lf4 fullscreen dimensions are outside the bounded composition contract");
    const std::size_t sourceRowBytes = static_cast<std::size_t>(phase1CanvasWidth) * 4U;
    const std::size_t destinationRowBytes = static_cast<std::size_t>(destinationWidth) * 4U;
    const std::size_t expectedSourceBytes = sourceRowBytes * phase1CanvasHeight;
    const std::size_t expectedDestinationBytes = destinationRowBytes * destinationHeight;
    Require(source.size() == expectedSourceBytes && destination.size() == expectedDestinationBytes,
        "remote-lf4 fullscreen raster size mismatch");
    for (std::size_t offset = 0; offset < destination.size(); offset += 4U)
    {
        destination[offset + 0U] = std::byte{0x80};
        destination[offset + 1U] = std::byte{0x80};
        destination[offset + 2U] = std::byte{0x80};
        destination[offset + 3U] = std::byte{0xFF};
    }
    const std::uint32_t canvasLeft = (destinationWidth - phase1CanvasWidth) / 2U;
    const std::uint32_t canvasTop = (destinationHeight - phase1CanvasHeight) / 2U;
    for (std::uint32_t sourceY = 0; sourceY < phase1CanvasHeight; sourceY++)
    {
        const std::byte* const sourceRow = source.data() + static_cast<std::size_t>(sourceY) * sourceRowBytes;
        std::byte* const destinationRow = destination.data() +
            static_cast<std::size_t>(canvasTop + sourceY) * destinationRowBytes +
            static_cast<std::size_t>(canvasLeft) * 4U;
        std::copy_n(sourceRow, sourceRowBytes, destinationRow);
    }
}

[[nodiscard]] std::uint64_t ElapsedMilliseconds(const std::chrono::steady_clock::time_point started) noexcept
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    return elapsed < 0 ? 0 : static_cast<std::uint64_t>(elapsed);
}

[[nodiscard]] std::uint64_t GetUnixTimeMilliseconds() noexcept
{
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    constexpr std::uint64_t windowsToUnixEpoch100ns = 116444736000000000ULL;
    return ticks.QuadPart < windowsToUnixEpoch100ns ? 0 :
        (ticks.QuadPart - windowsToUnixEpoch100ns) / 10000ULL;
}

struct ProcessResourceSample
{
    std::optional<double> cpuAveragePercent;
    std::optional<double> cpuPeakPercent;
    std::optional<double> cpuEquivalentCores;
    std::string cpuUnavailableReason;
    std::optional<double> gpuAveragePercent;
    std::optional<double> gpuPeakPercent;
    std::string gpuUnavailableReason = "PID-scoped GPU Engine counter unavailable in current sampler";
};

class ProcessResourceSampler
{
public:
    ProcessResourceSampler() noexcept
    {
        processorCount_ = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (processorCount_ == 0)
        {
            snapshot_.cpuUnavailableReason = "GetActiveProcessorCount failed";
            return;
        }
        std::uint64_t cpu100ns = 0;
        if (!ReadCpu100ns(cpu100ns))
        {
            snapshot_.cpuUnavailableReason = "GetProcessTimes failed";
            return;
        }
        firstCpu100ns_ = cpu100ns;
        lastCpu100ns_ = cpu100ns;
        initialized_ = true;
    }

    void Sample(const std::uint64_t monotonicMilliseconds) noexcept
    {
        if (!initialized_)
        {
            return;
        }
        if (!hasWallBaseline_)
        {
            firstWallMilliseconds_ = monotonicMilliseconds;
            lastWallMilliseconds_ = monotonicMilliseconds;
            hasWallBaseline_ = true;
            return;
        }
        if (monotonicMilliseconds < lastWallMilliseconds_ + 1000)
        {
            return;
        }
        std::uint64_t cpu100ns = 0;
        if (!ReadCpu100ns(cpu100ns) || cpu100ns < lastCpu100ns_)
        {
            snapshot_.cpuAveragePercent.reset();
            snapshot_.cpuPeakPercent.reset();
            snapshot_.cpuEquivalentCores.reset();
            snapshot_.cpuUnavailableReason = "Process CPU counter regressed or became unavailable";
            initialized_ = false;
            return;
        }
        // The 1 s gate above guarantees intervalMilliseconds >= 1000, so the divisor below is non-zero.
        const std::uint64_t intervalMilliseconds = monotonicMilliseconds - lastWallMilliseconds_;
        const double intervalCores = static_cast<double>(cpu100ns - lastCpu100ns_) /
            (static_cast<double>(intervalMilliseconds) * 10000.0);
        const double intervalPercent = intervalCores * 100.0 / static_cast<double>(processorCount_);
        peakPercent_ = std::max(peakPercent_, intervalPercent);
        const std::uint64_t totalMilliseconds = monotonicMilliseconds - firstWallMilliseconds_;
        const double averageCores = totalMilliseconds == 0 ? 0 : static_cast<double>(cpu100ns - firstCpu100ns_) /
            (static_cast<double>(totalMilliseconds) * 10000.0);
        snapshot_.cpuEquivalentCores = averageCores;
        snapshot_.cpuAveragePercent = averageCores * 100.0 / static_cast<double>(processorCount_);
        snapshot_.cpuPeakPercent = peakPercent_;
        snapshot_.cpuUnavailableReason.clear();
        lastCpu100ns_ = cpu100ns;
        lastWallMilliseconds_ = monotonicMilliseconds;
    }

    [[nodiscard]] const ProcessResourceSample& GetSnapshot() const noexcept
    {
        return snapshot_;
    }

private:
    [[nodiscard]] static bool ReadCpu100ns(std::uint64_t& output) noexcept
    {
        FILETIME created{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == FALSE)
        {
            return false;
        }
        ULARGE_INTEGER kernelTicks{};
        kernelTicks.LowPart = kernel.dwLowDateTime;
        kernelTicks.HighPart = kernel.dwHighDateTime;
        ULARGE_INTEGER userTicks{};
        userTicks.LowPart = user.dwLowDateTime;
        userTicks.HighPart = user.dwHighDateTime;
        if (userTicks.QuadPart > (std::numeric_limits<std::uint64_t>::max)() - kernelTicks.QuadPart)
        {
            return false;
        }
        output = kernelTicks.QuadPart + userTicks.QuadPart;
        return true;
    }

    ProcessResourceSample snapshot_;
    std::uint64_t firstCpu100ns_ = 0;
    std::uint64_t lastCpu100ns_ = 0;
    std::uint64_t firstWallMilliseconds_ = 0;
    std::uint64_t lastWallMilliseconds_ = 0;
    std::uint32_t processorCount_ = 0;
    double peakPercent_ = 0;
    bool initialized_ = false;
    bool hasWallBaseline_ = false;
};

void ApplyProcessResourceSample(const ProcessResourceSample& sample, EncoderSnapshot& snapshot)
{
    snapshot.processCpuAveragePercent = sample.cpuAveragePercent;
    snapshot.processCpuPeakPercent = sample.cpuPeakPercent;
    snapshot.processCpuEquivalentCores = sample.cpuEquivalentCores;
    snapshot.processCpuUnavailableReason = sample.cpuUnavailableReason;
    snapshot.processGpuEngineAveragePercent = sample.gpuAveragePercent;
    snapshot.processGpuEnginePeakPercent = sample.gpuPeakPercent;
    snapshot.processGpuUnavailableReason = sample.gpuUnavailableReason;
}

void ApplyProcessResourceSample(const ProcessResourceSample& sample, DecoderSnapshot& snapshot)
{
    snapshot.processCpuAveragePercent = sample.cpuAveragePercent;
    snapshot.processCpuPeakPercent = sample.cpuPeakPercent;
    snapshot.processCpuEquivalentCores = sample.cpuEquivalentCores;
    snapshot.processCpuUnavailableReason = sample.cpuUnavailableReason;
    snapshot.processGpuEngineAveragePercent = sample.gpuAveragePercent;
    snapshot.processGpuEnginePeakPercent = sample.gpuPeakPercent;
    snapshot.processGpuUnavailableReason = sample.gpuUnavailableReason;
}

void ApplyCaptureComponentSnapshot(const pbcapturenormalize::CaptureSnapshot& capture,
    DecoderSnapshot& snapshot) noexcept
{
    snapshot.captureEpoch = capture.captureEpoch;
    snapshot.captureArrivedFrames = capture.arrivedFrames;
    snapshot.captureCopiedFrames = capture.copiedFrames;
    snapshot.captureDeliveredFrames = capture.deliveredFrames;
    snapshot.captureDroppedFrames = capture.droppedFrames;
    snapshot.captureAcquireTimeouts = capture.acquireTimeouts;
    snapshot.capturePointerOnlyFrames = capture.pointerOnlyFrames;
    snapshot.captureAccumulatedFrames = capture.accumulatedFrames;
    snapshot.captureAccessLostEvents = capture.accessLostEvents;
    snapshot.captureExpiredFrames = capture.expiredFrames;
    snapshot.captureStaleFrames = capture.staleFrames;
    snapshot.captureCursorErasures = capture.cursorErasures;
    snapshot.captureFrameAgeHighWater100ns = capture.frameAgeHighWater100ns;
    snapshot.captureRecreates = capture.recreates;
    snapshot.captureDeviceRecoveries = capture.deviceRecoveries;
    snapshot.frameLeaseHighWater = capture.frameLeaseHighWater;
    snapshot.roiGpuTimeTotal100ns = capture.roiCopyTimeTotal100ns;
}

[[nodiscard]] std::string DescribePresentationStatus(const pbrenderd3d::PresentationStatus& status)
{
    std::ostringstream stream;
    stream << pbrenderd3d::GetPresentationErrorName(status.code) << " stage="
           << pbrenderd3d::GetPresentationStageName(status.stage) << " native=" << status.nativeError;
    return stream.str();
}

[[nodiscard]] std::string DescribeCaptureStatus(const pbcapturenormalize::CaptureStatus& status)
{
    std::ostringstream stream;
    stream << pbcapturenormalize::GetCaptureErrorName(status.code) << " stage="
           << static_cast<unsigned int>(status.stage) << " native=" << status.nativeError;
    return stream.str();
}

[[nodiscard]] std::string DescribeStorageStatus(const pbstorage::StorageStatus& status)
{
    std::ostringstream stream;
    stream << pbstorage::GetStorageErrorName(status.code) << " stage="
           << pbstorage::GetStorageStageName(status.stage) << " native=" << status.nativeError;
    return stream.str();
}

[[nodiscard]] std::string DescribeReceiverError(const pbreceiver::ReceiverError& error)
{
    std::ostringstream stream;
    if (const auto* protocol = std::get_if<pbprotocol::ProtocolError>(&error))
    {
        stream << "protocol code=" << static_cast<unsigned int>(protocol->code)
               << " offset=" << protocol->offset;
    }
    else if (const auto* outerFec = std::get_if<pbouterfec::OuterFecError>(&error))
    {
        stream << "outer-fec code=" << static_cast<unsigned int>(outerFec->code)
               << " detail=" << outerFec->detail;
    }
    else
    {
        const auto& compression = std::get<pbcompression::CompressionError>(error);
        stream << "compression code=" << static_cast<unsigned int>(compression.code)
               << " detail=" << compression.detail;
    }
    return stream.str();
}

[[nodiscard]] bool IsOuterConflictError(const pbreceiver::ReceiverError& error) noexcept
{
    if (const auto* protocol = std::get_if<pbprotocol::ProtocolError>(&error))
    {
        return protocol->code == pbprotocol::ProtocolErrorCode::OrphanPayloadConflict;
    }
    if (const auto* outerFec = std::get_if<pbouterfec::OuterFecError>(&error))
    {
        return outerFec->code == pbouterfec::OuterFecErrorCode::OuterBlockConflict;
    }
    return false;
}

[[nodiscard]] bool IsOuterResourceError(const pbreceiver::ReceiverError& error) noexcept
{
    if (const auto* protocol = std::get_if<pbprotocol::ProtocolError>(&error))
    {
        return protocol->code == pbprotocol::ProtocolErrorCode::ResourceLimitExceeded ||
            protocol->code == pbprotocol::ProtocolErrorCode::ResourceExhausted ||
            protocol->code == pbprotocol::ProtocolErrorCode::ControlReassemblyQuotaExceeded;
    }
    if (const auto* outerFec = std::get_if<pbouterfec::OuterFecError>(&error))
    {
        return outerFec->code == pbouterfec::OuterFecErrorCode::OutOfMemory ||
            outerFec->code == pbouterfec::OuterFecErrorCode::OuterFecDecoderQuotaExceeded ||
            outerFec->code == pbouterfec::OuterFecErrorCode::ExtraInsufficient;
    }
    return false;
}

class UniqueHandle
{
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : handle_(handle)
    {
    }
    ~UniqueHandle()
    {
        Reset();
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE))
    {
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE Get() const noexcept
    {
        return handle_;
    }
    void Reset(const HANDLE handle = INVALID_HANDLE_VALUE) noexcept
    {
        if (*this)
        {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

struct SourceFile
{
    UniqueHandle handle;
    BY_HANDLE_FILE_INFORMATION identity{};
    std::vector<std::byte> bytes;
};

[[nodiscard]] SourceFile ReadSourceFile(const std::wstring& path)
{
    SourceFile source;
    source.handle.Reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    Require(static_cast<bool>(source.handle), "无法以只读方式打开源文件，Win32=" + std::to_string(GetLastError()));
    Require(GetFileInformationByHandle(source.handle.Get(), &source.identity) != FALSE,
        "无法读取源文件身份，Win32=" + std::to_string(GetLastError()));
    const std::uint64_t fileBytes = (static_cast<std::uint64_t>(source.identity.nFileSizeHigh) << 32U) |
        source.identity.nFileSizeLow;
    Require(fileBytes >= minimumInstantFileBytes && fileBytes <= maximumInstantFileBytes,
        "当前单 Segment production path 仅支持 1 byte 到 8 MiB，源文件不会被截断");
    source.bytes.resize(static_cast<std::size_t>(fileBytes));
    std::size_t offset = 0;
    while (offset < source.bytes.size())
    {
        const DWORD chunk = static_cast<DWORD>((std::min)(source.bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD readBytes = 0;
        Require(ReadFile(source.handle.Get(), source.bytes.data() + offset, chunk, &readBytes, nullptr) != FALSE,
            "读取源文件失败，Win32=" + std::to_string(GetLastError()));
        Require(readBytes != 0 && readBytes <= chunk, "源文件在 Session 准备期间提前结束");
        offset += readBytes;
    }
    std::byte extra{};
    DWORD extraBytes = 0;
    Require(ReadFile(source.handle.Get(), &extra, 1, &extraBytes, nullptr) != FALSE && extraBytes == 0,
        "源文件大小在 Session 准备期间发生变化");
    return source;
}

[[nodiscard]] bool IsSourceStable(const SourceFile& source) noexcept
{
    BY_HANDLE_FILE_INFORMATION current{};
    if (GetFileInformationByHandle(source.handle.Get(), &current) == FALSE)
    {
        return false;
    }
    return current.dwVolumeSerialNumber == source.identity.dwVolumeSerialNumber &&
        current.nFileIndexHigh == source.identity.nFileIndexHigh &&
        current.nFileIndexLow == source.identity.nFileIndexLow &&
        current.nFileSizeHigh == source.identity.nFileSizeHigh &&
        current.nFileSizeLow == source.identity.nFileSizeLow &&
        CompareFileTime(&current.ftLastWriteTime, &source.identity.ftLastWriteTime) == 0;
}

struct ProfileBinding
{
    VisualProfile profile = VisualProfile::DirectLevels2x2;
    std::uint64_t visualProfileId = 0;
    std::uint8_t layoutVersion = 0;
    std::uint32_t dataBytes = 0;
    std::uint32_t codewords = 0;
};

[[nodiscard]] ProfileBinding GetProfileBinding(const VisualProfile profile)
{
    if (profile == VisualProfile::ShapeChroma)
    {
        return {profile, pbmodulation::kShapeChromaProfileId, pbmodulation::kShapeChromaLayoutVersion,
            pbmodulation::kShapeChromaDataBytes, pbmodulation::kShapeChromaCodewords};
    }
    if (profile == VisualProfile::RemoteVisualResilient)
    {
        return {profile, pbmodulation::kRemoteVisualProfileId, pbmodulation::kRemoteVisualLayoutVersion,
            pbmodulation::kRemoteVisualDataBytes, pbmodulation::kRemoteVisualCodewords};
    }
    if (profile == VisualProfile::RemoteVisualLowFps)
    {
        return {profile, pbmodulation::kRemoteVisualLowFpsProfileId, pbmodulation::kRemoteVisualLowFpsLayoutVersion,
            pbmodulation::kRemoteVisualLowFpsDataBytes, pbmodulation::kRemoteVisualLowFpsCodewords};
    }
    const auto* const direct = pbmodulation::GetDesktopLevelsProfile(pbmodulation::kDesktopLevels2ProfileId);
    Require(direct != nullptr, "Direct-Level 2x2 profile is unavailable");
    return {profile, direct->visualProfileId, pbmodulation::kDesktopLevelsLayoutVersion,
        direct->dataBytes, direct->codewords};
}

struct TransferDescription
{
    pbprotocol::SessionDescriptor session;
    pbprotocol::SegmentDescriptor segment;
    pbprotocol::FinalManifest manifest;
    pbcompression::EncodedSegment encoded;
    std::vector<std::byte> sessionControl;
    std::vector<std::byte> manifestControl;
    std::vector<std::byte> segmentControl;
};

[[nodiscard]] std::vector<std::byte> WrapControlRecord(const pbprotocol::ControlRecordType recordType,
    const std::uint64_t sequence, const pbprotocol::SessionTag sessionTag,
    const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView record{pbprotocol::kControlVersion, recordType, sequence,
        sessionTag, payload};
    const auto size = pbprotocol::GetSerializedSize(record);
    RequireResult(size, "ControlRecord size calculation failed");
    Require(size.Value() <= pbmodulation::kReferenceControlWindowBytes,
        "ControlRecord does not fit the fixed Control window");
    std::vector<std::byte> bytes(size.Value());
    RequireResult(pbprotocol::SerializeControlRecord(record, bytes), "ControlRecord serialization failed");
    return bytes;
}

[[nodiscard]] TransferDescription DescribeSource(const std::span<const std::byte> rawBytes,
    const bool compressionEnabled, const int compressionLevel)
{
    const auto sessionId = pbprotocol::GenerateRandomSessionId();
    RequireResult(sessionId, "OS CSPRNG SessionId generation failed");
    const pbprotocol::SessionDescriptor session{pbprotocol::GetProtocolVersion(), sessionId.Value(),
        rawBytes.size(), 1, pbprotocol::DigestAlgorithm::Blake3_256};
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(session.sessionId);
    auto prepared = PrepareEncodedSegment(rawBytes, compressionEnabled, compressionLevel);
    RequireResult(prepared, "Segment compression/RAW preparation failed");
    pbcompression::EncodedSegment encoded = std::move(prepared).Value();
    const auto selectedMode = pbouterfec::ChooseOuterFecMode(encoded.bytes.size(), outerBlockBytes);
    RequireResult(selectedMode, "Outer FEC mode selection failed");
    pbprotocol::SegmentDescriptor segment;
    segment.sessionTag = sessionTag;
    segment.segmentOrdinal = 0;
    segment.rawOffset = 0;
    segment.rawSize = rawBytes.size();
    segment.encodedSize = encoded.bytes.size();
    segment.compressionCodec = encoded.codec;
    segment.outerFecMode = selectedMode.Value();
    segment.outerBlockBytes = outerBlockBytes;
    segment.rawDigest = pbprotocol::RawDigest{pbprotocol::ComputeBlake3Digest(rawBytes)};
    segment.encodedDigest = pbprotocol::EncodedDigest{pbprotocol::ComputeBlake3Digest(encoded.bytes)};
    if (segment.outerFecMode == pbprotocol::OuterFecMode::WirehairV2)
    {
        const auto encoder = pbouterfec::WirehairV2Encoder::Create(encoded.bytes, outerBlockBytes);
        RequireResult(encoder, "Wirehair V2 descriptor creation failed");
        segment.wirehairV2SerializedProfile = encoder.Value().GetSerializedProfile();
    }
    else
    {
        const auto encoder = pbouterfec::DirectRepeatEncoder::Create(encoded.bytes, outerBlockBytes);
        RequireResult(encoder, "DirectRepeat descriptor validation failed");
    }
    const pbprotocol::FinalManifest manifest{session.sessionId, rawBytes.size(), 1,
        pbprotocol::WholeFileDigest{pbprotocol::ComputeBlake3Digest(rawBytes)},
        pbprotocol::DigestAlgorithm::Blake3_256};
    const pbprotocol::ReceiverResourcePolicy policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> sessionPayload{};
    RequireResult(pbprotocol::SerializeSessionDescriptor(session, policy, sessionPayload),
        "SessionDescriptor serialization failed");
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> manifestPayload{};
    RequireResult(pbprotocol::SerializeFinalManifest(manifest, session, policy, manifestPayload),
        "FinalManifest serialization failed");
    const auto segmentSize = pbprotocol::GetSerializedSize(segment);
    RequireResult(segmentSize, "SegmentDescriptor size calculation failed");
    std::vector<std::byte> segmentPayload(segmentSize.Value());
    RequireResult(pbprotocol::SerializeSegmentDescriptor(segment, session, policy, segmentPayload),
        "SegmentDescriptor serialization failed");
    TransferDescription description;
    description.session = session;
    description.segment = segment;
    description.manifest = manifest;
    description.encoded = std::move(encoded);
    description.sessionControl = WrapControlRecord(pbprotocol::ControlRecordType::SessionDescriptor, 1,
        sessionTag, sessionPayload);
    description.manifestControl = WrapControlRecord(pbprotocol::ControlRecordType::FinalManifest, 2,
        sessionTag, manifestPayload);
    description.segmentControl = WrapControlRecord(pbprotocol::ControlRecordType::SegmentDescriptor, 3,
        sessionTag, segmentPayload);
    return description;
}

enum class FrameKind : std::uint8_t
{
    SessionControl,
    ManifestControl,
    SegmentControl,
    Data
};

class SenderFrameBuilder
{
public:
    SenderFrameBuilder(ProfileBinding profile, const TransferDescription& description, const std::uint32_t controlRepetitions)
        : profile_(profile), description_(description), data_(profile.dataBytes),
          pixels_(pbmodulation::kLocalDesktopFrameBgraBytes), controlRepetitions_(controlRepetitions)
    {
        if (description_.segment.outerFecMode == pbprotocol::OuterFecMode::WirehairV2)
        {
            auto encoder = pbouterfec::WirehairV2Encoder::Recreate(description_.encoded.bytes,
                description_.segment);
            RequireResult(encoder, "Wirehair V2 sender recreation failed");
            wirehair_ = std::make_unique<pbouterfec::WirehairV2Encoder>(std::move(encoder).Value());
            blockCount_ = wirehair_->GetBlockCount();
            const std::uint64_t repairAllowance = (std::max)(4096ULL,
                static_cast<std::uint64_t>(blockCount_) / 2ULL);
            const auto blocksPerCycle = pbprotocol::CheckedAddUint64(blockCount_, repairAllowance);
            RequireResult(blocksPerCycle, "Wirehair carousel block count overflow");
            targetBlocksPerCycle_ = blocksPerCycle.Value();
        }
        else
        {
            auto encoder = pbouterfec::DirectRepeatEncoder::Recreate(description_.encoded.bytes,
                description_.segment);
            RequireResult(encoder, "DirectRepeat sender recreation failed");
            directRepeat_ = std::make_unique<pbouterfec::DirectRepeatEncoder>(std::move(encoder).Value());
            Require(directRepeat_->GetBlockCount() > 0 &&
                directRepeat_->GetBlockCount() <= (std::numeric_limits<std::uint32_t>::max)(),
                "DirectRepeat block count is invalid");
            blockCount_ = static_cast<std::uint32_t>(directRepeat_->GetBlockCount());
            targetBlocksPerCycle_ = blockCount_;
        }
        Require(profile_.codewords != 0 && targetBlocksPerCycle_ != 0,
            "Profile or outer FEC produced an empty carousel");
        const auto rounded = pbprotocol::CheckedAddUint64(targetBlocksPerCycle_, profile_.codewords - 1U);
        RequireResult(rounded, "Carousel frame count overflow");
        const std::uint64_t dataFrames = rounded.Value() / profile_.codewords;
        const std::uint32_t controlFramesPerCycle = 3 * controlRepetitions_;
        Require(dataFrames <= (std::numeric_limits<std::uint32_t>::max)() - controlFramesPerCycle,
            "Carousel frame count exceeds the bounded UI model");
        cycleFrameCount_ = controlFramesPerCycle + static_cast<std::uint32_t>(dataFrames);
        Require(carousel_.Reset(cycleFrameCount_), "Carousel counter initialization failed");
    }

    [[nodiscard]] FrameKind GetCurrentKind() const noexcept
    {
        const std::uint32_t position = carousel_.GetSnapshot().cyclePosition;
        if (position < controlRepetitions_)
        {
            return FrameKind::SessionControl;
        }
        if (position < 2U * controlRepetitions_)
        {
            return FrameKind::ManifestControl;
        }
        if (position < 3U * controlRepetitions_)
        {
            return FrameKind::SegmentControl;
        }
        return FrameKind::Data;
    }

    [[nodiscard]] const std::vector<std::byte>& Build(const std::uint64_t frameSequence)
    {
        const FrameKind kind = GetCurrentKind();
        const auto bootstrap = MakeBootstrap(frameSequence);
        if (kind != FrameKind::Data)
        {
            const std::vector<std::byte>& control = kind == FrameKind::SessionControl ?
                description_.sessionControl : kind == FrameKind::ManifestControl ?
                description_.manifestControl : description_.segmentControl;
            if (profile_.profile == VisualProfile::RemoteVisualLowFps)
            {
                Require(control.size() <= pbmodulation::kReferenceControlWindowBytes,
                    "RemoteVisual Control exceeds the bounded physical carrier");
                std::fill(information_.begin(), information_.end(), std::byte{0});
                std::copy(control.begin(), control.end(), information_.begin());
                const auto firstCodeword = std::span(data_).first(codewordBytes);
                RequireResult(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information_, firstCodeword),
                    "RemoteVisual Control inner FEC generation failed");
                for (std::uint32_t slot = 1; slot < profile_.codewords; slot++)
                {
                    std::copy(firstCodeword.begin(), firstCodeword.end(),
                        data_.begin() + static_cast<std::size_t>(slot) * codewordBytes);
                }
                RequireResult(pbmodulation::EncodeRemoteVisualLowFpsFrame(bootstrap, data_, pixels_),
                    "RemoteVisual LF4 Control raster generation failed");
                generatedPayloadBytesInFrame_ = 0;
                return pixels_;
            }
            if (profile_.profile == VisualProfile::RemoteVisualResilient)
            {
                Require(control.size() <= pbmodulation::kReferenceControlWindowBytes,
                    "RemoteVisual Control exceeds the bounded physical carrier");
                std::fill(information_.begin(), information_.end(), std::byte{0});
                std::copy(control.begin(), control.end(), information_.begin());
                RequireResult(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information_, data_),
                    "RemoteVisual Control inner FEC generation failed");
                RequireResult(pbmodulation::EncodeRemoteVisualFrame(bootstrap, data_, pixels_),
                    "RemoteVisual Control raster generation failed");
                generatedPayloadBytesInFrame_ = 0;
                return pixels_;
            }
            std::fill(controlWindow_.begin(), controlWindow_.end(), std::byte{0});
            std::copy(control.begin(), control.end(), controlWindow_.begin());
            std::fill(referenceData_.begin(), referenceData_.end(), std::byte{0});
            RequireResult(pbmodulation::EncodeReferenceFrame({bootstrap, controlWindow_, referenceData_}, pixels_),
                "fixed Control raster generation failed");
            generatedPayloadBytesInFrame_ = 0;
            return pixels_;
        }
        BuildTransportData();
        const auto status = profile_.profile == VisualProfile::ShapeChroma ?
            pbmodulation::EncodeShapeChromaFrame(bootstrap, data_, pixels_) :
            profile_.profile == VisualProfile::RemoteVisualLowFps ?
            pbmodulation::EncodeRemoteVisualLowFpsFrame(bootstrap, data_, pixels_) :
            profile_.profile == VisualProfile::RemoteVisualResilient ?
            pbmodulation::EncodeRemoteVisualFrame(bootstrap, data_, pixels_) :
            pbmodulation::EncodeDesktopLevelsFrame(bootstrap, data_, pixels_);
        RequireResult(status, "physical data raster generation failed");
        return pixels_;
    }

    void Advance()
    {
        if (GetCurrentKind() == FrameKind::Data)
        {
            if (wirehair_)
            {
                const std::uint64_t next = static_cast<std::uint64_t>(nextOuterBlockId_) + profile_.codewords;
                Require(next <= (std::numeric_limits<std::uint32_t>::max)(),
                    "Wirehair repair ID space exhausted; start a new Session");
                nextOuterBlockId_ = static_cast<std::uint32_t>(next);
            }
            else
            {
                nextOuterBlockId_ = static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(nextOuterBlockId_) + profile_.codewords) % blockCount_);
            }
        }
        Require(carousel_.Advance(), "Carousel counter overflow");
    }

    [[nodiscard]] CarouselSnapshot GetCarouselSnapshot() const noexcept
    {
        return carousel_.GetSnapshot();
    }
    [[nodiscard]] const std::vector<std::byte>& GetBuiltPixels() const noexcept
    {
        return pixels_;
    }
    [[nodiscard]] std::uint32_t GetCurrentOuterBlockId() const noexcept
    {
        return nextOuterBlockId_;
    }
    [[nodiscard]] std::uint32_t GetBlockCount() const noexcept
    {
        return blockCount_;
    }
    [[nodiscard]] std::uint64_t GetGeneratedPayloadBytesInFrame() const noexcept
    {
        return generatedPayloadBytesInFrame_;
    }

private:
    [[nodiscard]] std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeBootstrap(
        const std::uint64_t frameSequence) const
    {
        const pbprotocol::BootstrapRecord record{pbprotocol::kBootstrapVersion,
            pbprotocol::GetProtocolVersion(), profile_.layoutVersion, profile_.visualProfileId,
            description_.segment.sessionTag, frameSequence, 0, 0};
        std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bytes{};
        RequireResult(pbprotocol::SerializeBootstrapRecord(record, bytes), "Bootstrap serialization failed");
        return bytes;
    }

    void BuildTransportData()
    {
        std::fill(data_.begin(), data_.end(), std::byte{0});
        generatedPayloadBytesInFrame_ = 0;
        for (std::uint32_t slot = 0; slot < profile_.codewords; slot++)
        {
            const std::uint64_t candidate = static_cast<std::uint64_t>(nextOuterBlockId_) + slot;
            Require(!wirehair_ || candidate <= (std::numeric_limits<std::uint32_t>::max)(),
                "Wirehair repair ID space exhausted inside a visual frame");
            const std::uint32_t blockId = wirehair_ ? static_cast<std::uint32_t>(candidate) :
                static_cast<std::uint32_t>(candidate % blockCount_);
            std::fill(outerPayload_.begin(), outerPayload_.end(), std::byte{0});
            const auto encoded = wirehair_ ? wirehair_->EncodeBlock(blockId, outerPayload_) :
                directRepeat_->EncodeBlock(blockId, outerPayload_);
            RequireResult(encoded, "Outer FEC block encoding failed");
            Require(encoded.Value() > 0 && encoded.Value() <= outerPayload_.size() &&
                encoded.Value() <= (std::numeric_limits<std::uint16_t>::max)(),
                "Outer FEC produced an invalid payload length");
            const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData,
                pbprotocol::kTransportProtocolMinor, 0, description_.segment.sessionTag, 0, blockId,
                static_cast<std::uint16_t>(encoded.Value())};
            const std::size_t serializedBytes = pbprotocol::GetTransportSerializedSize(header);
            Require(serializedBytes <= transport_.size(), "Transport block exceeds the robust information block");
            RequireResult(pbprotocol::SerializeTransportBlock(header,
                std::span(outerPayload_).first(encoded.Value()), std::span(transport_).first(serializedBytes)),
                "Transport serialization failed");
            RequireResult(pbprotocol::FrameTransportBlockIntoInfoBlock(std::span(transport_).first(serializedBytes),
                information_.size(), information_), "Transport information framing failed");
            RequireResult(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information_,
                std::span(data_).subspan(static_cast<std::size_t>(slot) * codewordBytes, codewordBytes)),
                "Robust QC-LDPC encoding failed");
            generatedPayloadBytesInFrame_ += encoded.Value();
        }
    }

    ProfileBinding profile_;
    const TransferDescription& description_;
    std::unique_ptr<pbouterfec::WirehairV2Encoder> wirehair_;
    std::unique_ptr<pbouterfec::DirectRepeatEncoder> directRepeat_;
    std::vector<std::byte> data_;
    std::vector<std::byte> pixels_;
    std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> controlWindow_{};
    std::array<std::byte, pbmodulation::kReferenceDataRegionBytes> referenceData_{};
    std::array<std::byte, outerBlockBytes> outerPayload_{};
    std::array<std::byte, informationBytes> transport_{};
    std::array<std::byte, informationBytes> information_{};
    CarouselCounter carousel_;
    std::uint32_t blockCount_ = 0;
    std::uint64_t targetBlocksPerCycle_ = 0;
    std::uint32_t cycleFrameCount_ = 0;
    std::uint32_t nextOuterBlockId_ = 0;
    std::uint64_t generatedPayloadBytesInFrame_ = 0;
    const std::uint32_t controlRepetitions_;
};

[[nodiscard]] bool HasStablePresentationContract(const pbrenderd3d::DataWindowSnapshot& snapshot,
    const std::uint32_t expectedWidth, const std::uint32_t expectedHeight) noexcept
{
    return snapshot.state == pbrenderd3d::WindowState::Running && snapshot.candidateContractSatisfied &&
        snapshot.contract.bufferWidth == expectedWidth && snapshot.contract.bufferHeight == expectedHeight &&
        snapshot.contract.bufferCount == 2 && snapshot.contract.maximumFrameLatency == 1 &&
        snapshot.contract.flipEffect == pbrenderd3d::FlipEffect::Discard && snapshot.contract.bgraUnorm &&
        snapshot.contract.noMsaa && snapshot.contract.alphaIgnored && snapshot.contract.scalingNone &&
        snapshot.contract.tearingDisabled && snapshot.contract.latencyWaitable && snapshot.contract.perMonitorV2 &&
        !snapshot.softwareRasterizer;
}

void ApplyEncoderPresentationSnapshot(const pbrenderd3d::DataWindowSnapshot& source, EncoderSnapshot& destination) noexcept
{
    destination.presentationEpoch = source.timing.presentationEpoch;
    destination.presentedVisualFps = source.timing.presentedVisualFps;
    destination.presentCallFps = source.timing.presentCallFps;
    destination.submittedFrames = source.submittedFrames;
    destination.replacedPendingFrames = source.replacedPendingFrames;
    destination.sourceTextureReplacements = source.sourceTextureReplacements;
    destination.repeatedPresentCalls = source.repeatedPresentCalls;
    destination.invalidatedActiveFrames = source.invalidatedActiveFrames;
    destination.pendingFrames = source.pendingFrame ? 1U : 0U;
    destination.pendingHighWater = source.pendingFrame ? 1U : destination.pendingHighWater;
    destination.activeFrame = source.activeFrame;
    destination.activeFrameSequence = source.activeFrameSequence;
    destination.candidateContractSatisfied = source.candidateContractSatisfied;
}

class NativeCaptureSession
{
public:
    NativeCaptureSession(const CaptureBackend backend,
        const pbcapturenormalize::CaptureNormalizeConfig& config,
        const std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer>& consumer)
        : backend_(backend)
    {
        const pbcapturenormalize::CaptureStatus status = backend == CaptureBackend::Wgc ?
            pbscreencapturewgc::WgcCapture::CreateNormalized(config, consumer, wgc_) :
            pbscreencapturedxgi::DxgiCapture::Create(config, consumer, dxgi_);
        Require(static_cast<bool>(status), std::string(GetCaptureBackendName(backend)) +
            " capture creation failed: " + DescribeCaptureStatus(status));
    }

    ~NativeCaptureSession()
    {
        static_cast<void>(Stop());
    }
    NativeCaptureSession(const NativeCaptureSession&) = delete;
    NativeCaptureSession& operator=(const NativeCaptureSession&) = delete;

    [[nodiscard]] pbcapturenormalize::CaptureSnapshot GetSnapshot() const noexcept
    {
        return wgc_ ? wgc_->GetSnapshot() : dxgi_->GetSnapshot();
    }
    [[nodiscard]] pbcapturenormalize::CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept
    {
        return wgc_ ? wgc_->GetNormalizationSnapshot() : dxgi_->GetNormalizationSnapshot();
    }
    void RequestStop() noexcept
    {
        if (wgc_)
        {
            wgc_->RequestStop();
        }
        else if (dxgi_)
        {
            dxgi_->RequestStop();
        }
    }
    [[nodiscard]] pbcapturenormalize::CaptureStatus Stop() noexcept
    {
        if (stopped_)
        {
            return stopStatus_;
        }
        stopStatus_ = wgc_ ? wgc_->Stop() : dxgi_->Stop();
        stopped_ = true;
        return stopStatus_;
    }

private:
    CaptureBackend backend_ = CaptureBackend::Wgc;
    std::unique_ptr<pbscreencapturewgc::WgcCapture> wgc_;
    std::unique_ptr<pbscreencapturedxgi::DxgiCapture> dxgi_;
    pbcapturenormalize::CaptureStatus stopStatus_;
    bool stopped_ = false;
};

[[nodiscard]] std::int64_t GetUtcFileTime100ns() noexcept
{
    FILETIME fileTime{};
    GetSystemTimePreciseAsFileTime(&fileTime);
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ?
        static_cast<std::int64_t>(value.QuadPart) : 0;
}

void ApplyReplaySnapshot(const RemoteVisualReplayRecorderSnapshot& replay,
    DecoderSnapshot& snapshot)
{
    snapshot.replayEvidenceValid = replay.evidenceValid;
    snapshot.replayFinalized = replay.finalized;
    snapshot.replayWrittenFrames = replay.writtenFrames;
    snapshot.replayDroppedFrames = replay.droppedFrames;
    snapshot.replayWrittenDemodObservations = replay.writtenDemodObservations;
    snapshot.replayDroppedDemodObservations = replay.droppedDemodObservations;
    snapshot.replayQueueHighWater = replay.queueHighWater;
    snapshot.replayFileBytes = replay.fileBytes;
    if (!replay.lastError)
    {
        snapshot.replayError = std::string(pbrealcapturereplay::GetReplayErrorName(replay.lastError.code)) +
            " at stage " + std::to_string(static_cast<unsigned int>(replay.lastError.stage));
    }
}

struct ReceiverProcessResult
{
    bool carrierAccepted = false;
    bool uniqueAdmission = false;
};

[[nodiscard]] pbrealcapturereplay::ReplayV2DemodResultKind ToReplayResultKind(
    const pbdemodd3d11::CaptureDemodulatorResultKind kind) noexcept
{
    switch (kind)
    {
    case pbdemodd3d11::CaptureDemodulatorResultKind::Transport:
        return pbrealcapturereplay::ReplayV2DemodResultKind::Transport;
    case pbdemodd3d11::CaptureDemodulatorResultKind::ControlRecord:
        return pbrealcapturereplay::ReplayV2DemodResultKind::ControlRecord;
    case pbdemodd3d11::CaptureDemodulatorResultKind::ControlFragment:
        return pbrealcapturereplay::ReplayV2DemodResultKind::ControlFragment;
    case pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly:
        return pbrealcapturereplay::ReplayV2DemodResultKind::TelemetryOnly;
    }
    return pbrealcapturereplay::ReplayV2DemodResultKind::Unavailable;
}

[[nodiscard]] pbrealcapturereplay::ReplayV2GeometryStatus ToReplayGeometryStatus(
    const pbdemodd3d11::CaptureDemodulatorGeometryStatus status) noexcept
{
    switch (status)
    {
    case pbdemodd3d11::CaptureDemodulatorGeometryStatus::NotApplicable:
        return pbrealcapturereplay::ReplayV2GeometryStatus::NotApplicable;
    case pbdemodd3d11::CaptureDemodulatorGeometryStatus::ExactCanvas:
        return pbrealcapturereplay::ReplayV2GeometryStatus::ExactCanvas;
    case pbdemodd3d11::CaptureDemodulatorGeometryStatus::Scaled:
        return pbrealcapturereplay::ReplayV2GeometryStatus::Scaled;
    case pbdemodd3d11::CaptureDemodulatorGeometryStatus::Letterboxed:
        return pbrealcapturereplay::ReplayV2GeometryStatus::Letterboxed;
    case pbdemodd3d11::CaptureDemodulatorGeometryStatus::Rejected:
        return pbrealcapturereplay::ReplayV2GeometryStatus::Rejected;
    }
    return pbrealcapturereplay::ReplayV2GeometryStatus::Unavailable;
}

[[nodiscard]] pbrealcapturereplay::ReplayV2TemporalDisposition ToReplayTemporalDisposition(
    const pbdemodd3d11::CaptureDemodulatorTemporalDisposition disposition) noexcept
{
    switch (disposition)
    {
    case pbdemodd3d11::CaptureDemodulatorTemporalDisposition::NotApplicable:
        return pbrealcapturereplay::ReplayV2TemporalDisposition::NotApplicable;
    case pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique:
        return pbrealcapturereplay::ReplayV2TemporalDisposition::Unique;
    case pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateRefinement:
        return pbrealcapturereplay::ReplayV2TemporalDisposition::DuplicateRefinement;
    case pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateSuppressed:
        return pbrealcapturereplay::ReplayV2TemporalDisposition::DuplicateSuppressed;
    case pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Reordered:
        return pbrealcapturereplay::ReplayV2TemporalDisposition::Reordered;
    case pbdemodd3d11::CaptureDemodulatorTemporalDisposition::StaleCompletion:
        return pbrealcapturereplay::ReplayV2TemporalDisposition::StaleCompletion;
    }
    return pbrealcapturereplay::ReplayV2TemporalDisposition::Unavailable;
}

[[nodiscard]] pbrealcapturereplay::ReplayV2DemodObservationView MakeReplayDemodObservation(
    const pbdemodd3d11::CaptureDemodulatorResult& result, const std::uint64_t visualProfileId,
    const ReceiverProcessResult& receiverResult)
{
    pbrealcapturereplay::ReplayV2DemodObservationView observation;
    observation.captureEpoch = result.metadata.domain.captureEpoch;
    observation.captureObservation = result.metadata.captureObservation;
    observation.visualProfileId = visualProfileId;
    observation.bootstrapAttempted = true;
    observation.bootstrapSucceeded = result.bootstrap.IsAccepted();
    observation.diagnosticCode = static_cast<std::uint32_t>(result.bootstrap.erasure);
    if (result.bootstrap.IsAccepted())
    {
        const auto bootstrap = pbprotocol::ParseBootstrapRecord(result.bootstrapRecord);
        if (bootstrap)
        {
            observation.frameSequenceAvailable = true;
            observation.frameSequence = bootstrap.Value().frameSequence;
            observation.layoutAvailable = true;
            observation.visualLayoutVersion = bootstrap.Value().visualLayoutVersion;
        }
    }
    observation.transportProduced = result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport &&
        result.demodulation.acceptedTransportBlockCount != 0;
    observation.receiverAdmitted = observation.transportProduced && receiverResult.uniqueAdmission;
    observation.disposition = !observation.bootstrapSucceeded ?
        pbrealcapturereplay::ReplayV2DemodDisposition::Erasure :
        observation.receiverAdmitted ? pbrealcapturereplay::ReplayV2DemodDisposition::Accepted :
        pbrealcapturereplay::ReplayV2DemodDisposition::Rejected;
    observation.productionDetailAvailable = true;
    observation.resultKind = ToReplayResultKind(result.kind);
    observation.geometryStatus = ToReplayGeometryStatus(result.geometryStatus);
    observation.geometryAvailable = result.bootstrap.IsAccepted() &&
        (result.geometryStatus == pbdemodd3d11::CaptureDemodulatorGeometryStatus::ExactCanvas ||
         result.geometryStatus == pbdemodd3d11::CaptureDemodulatorGeometryStatus::Scaled ||
         result.geometryStatus == pbdemodd3d11::CaptureDemodulatorGeometryStatus::Letterboxed);
    if (observation.geometryAvailable)
    {
        observation.geometryOriginX = result.bootstrap.geometry.originX;
        observation.geometryOriginY = result.bootstrap.geometry.originY;
        observation.geometryScaleX = result.bootstrap.geometry.scaleX;
        observation.geometryScaleY = result.bootstrap.geometry.scaleY;
    }
    observation.temporalDisposition = ToReplayTemporalDisposition(result.temporalDisposition);
    const auto& evaluation = result.demodulation.evaluation;
    observation.evaluationAvailable = evaluation.evaluated;
    if (observation.evaluationAvailable)
    {
        observation.paddingValid = evaluation.paddingValid;
        observation.codewords = evaluation.codewords;
        observation.fecFailures = evaluation.fecFailures;
        observation.crcFailures = evaluation.crcFailures;
        observation.identityFailures = evaluation.identityFailures;
        // Production Replay is receiver-only. Diagnostic truth counters and
        // compared bits remain absent even if a test source exists elsewhere.
        observation.senderTruthAvailable = false;
        observation.falseAcceptedCodewords = 0;
        observation.acceptedTransportBlocks = result.demodulation.acceptedTransportBlockCount;
        observation.acceptedRemoteControlBlocks = result.demodulation.acceptedRemoteControlBlockCount;
        observation.admittedTransportBlocks = result.admittedTransportBlockCount;
        observation.admittedRemoteControlBlocks = result.admittedRemoteControlBlockCount;
        observation.iterationsTotal = evaluation.iterationsTotal;
        observation.iterationsMaximum = evaluation.iterationsMaximum;
    }
    observation.metricSummaryAvailable = result.demodulation.remoteMetricSummaryAvailable;
    if (observation.metricSummaryAvailable)
    {
        observation.metricSamples = result.demodulation.remoteMetricSamples;
        observation.zeroMagnitudeMetrics = result.demodulation.remoteZeroMagnitudeMetrics;
        observation.minimumAbsoluteMetric = result.demodulation.remoteMinimumAbsoluteMetric;
        observation.meanAbsoluteMetric = result.demodulation.remoteMeanAbsoluteMetric;
        observation.freshnessRegions = result.demodulation.remoteFreshnessRegions;
        observation.staleRegions = result.demodulation.remoteStaleRegions;
        observation.freshnessTagMismatches = result.demodulation.remoteFreshnessTagMismatches;
        observation.freshnessTagErasures = result.demodulation.remoteFreshnessTagErasures;
        observation.freshnessErasedDataMetrics = result.demodulation.remoteFreshnessErasedDataMetrics;
        observation.unreliableSymbols = result.demodulation.remoteUnreliableSymbols;
    }
    observation.metricReadbackBytes = result.demodulation.metricReadbackBytes;
    observation.gpuTimingAvailable = result.demodulation.gpuTimingValid;
    observation.gpuTime100ns = observation.gpuTimingAvailable ? result.demodulation.gpuTime100ns : 0;
    observation.carrierAccepted = receiverResult.carrierAccepted;
    observation.receiverStateAdvanced = receiverResult.uniqueAdmission;
    return observation;
}

[[nodiscard]] std::string DescribeReplayStatus(const pbrealcapturereplay::ReplayStatus& status)
{
    std::ostringstream stream;
    stream << pbrealcapturereplay::GetReplayErrorName(status.code) << " stage="
           << static_cast<unsigned int>(status.stage) << " native=" << status.nativeError
           << " offset=" << status.offset;
    return stream.str();
}

[[nodiscard]] bool SameAdapterLuid(const LUID& left, const LUID& right) noexcept
{
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

[[nodiscard]] bool NonzeroAdapterLuid(const LUID& value) noexcept
{
    return value.HighPart != 0 || value.LowPart != 0;
}

struct ReplayD3dDevice
{
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    LUID adapterLuid{};
};

[[nodiscard]] ReplayD3dDevice CreateWarpReplayD3dDevice()
{
    constexpr std::array<D3D_FEATURE_LEVEL, 2> requestedFeatureLevels{
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ReplayD3dDevice output;
    D3D_FEATURE_LEVEL actualFeatureLevel = D3D_FEATURE_LEVEL_9_1;
    HRESULT deviceResult = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, requestedFeatureLevels.data(),
        static_cast<UINT>(requestedFeatureLevels.size()), D3D11_SDK_VERSION, &output.device,
        &actualFeatureLevel, &output.context);
    if (deviceResult == E_INVALIDARG)
    {
        const D3D_FEATURE_LEVEL fallbackFeatureLevel = D3D_FEATURE_LEVEL_11_0;
        deviceResult = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fallbackFeatureLevel, 1, D3D11_SDK_VERSION,
            &output.device, &actualFeatureLevel, &output.context);
    }
    Require(SUCCEEDED(deviceResult) && output.device && output.context && actualFeatureLevel >= D3D_FEATURE_LEVEL_11_0,
        "Replay WARP D3D11 device creation failed, HRESULT=" + std::to_string(deviceResult));
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT queryResult = output.device.As(&dxgiDevice);
    if (SUCCEEDED(queryResult))
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        queryResult = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(queryResult))
        {
            queryResult = adapter.As(&output.adapter);
        }
    }
    DXGI_ADAPTER_DESC1 description{};
    if (SUCCEEDED(queryResult) && output.adapter)
    {
        queryResult = output.adapter->GetDesc1(&description);
    }
    Require(SUCCEEDED(queryResult) && output.adapter && NonzeroAdapterLuid(description.AdapterLuid),
        "Replay WARP adapter identity acquisition failed, HRESULT=" + std::to_string(queryResult));
    output.adapterLuid = description.AdapterLuid;
    return output;
}

[[nodiscard]] ReplayD3dDevice CreateReplayD3dDevice(const LUID& requiredAdapterLuid)
{
    Require(NonzeroAdapterLuid(requiredAdapterLuid), "Replay capture does not identify a D3D adapter LUID");
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    const HRESULT factoryResult = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    Require(SUCCEEDED(factoryResult) && factory, "Replay DXGI factory creation failed, HRESULT=" +
        std::to_string(factoryResult));
    Microsoft::WRL::ComPtr<IDXGIAdapter1> matchingAdapter;
    for (UINT adapterIndex = 0;; adapterIndex++)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumResult = factory->EnumAdapters1(adapterIndex, &adapter);
        if (enumResult == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        Require(SUCCEEDED(enumResult) && adapter, "Replay DXGI adapter enumeration failed, HRESULT=" +
            std::to_string(enumResult));
        DXGI_ADAPTER_DESC1 description{};
        const HRESULT descriptionResult = adapter->GetDesc1(&description);
        Require(SUCCEEDED(descriptionResult), "Replay DXGI adapter description failed, HRESULT=" +
            std::to_string(descriptionResult));
        if (SameAdapterLuid(description.AdapterLuid, requiredAdapterLuid))
        {
            matchingAdapter = std::move(adapter);
            break;
        }
    }
    if (!matchingAdapter)
    {
        ReplayD3dDevice warp = CreateWarpReplayD3dDevice();
        Require(SameAdapterLuid(warp.adapterLuid, requiredAdapterLuid),
            "Replay adapter LUID is unavailable on this machine; no different-adapter fallback is allowed");
        return warp;
    }

    constexpr std::array<D3D_FEATURE_LEVEL, 2> requestedFeatureLevels{
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ReplayD3dDevice output;
    output.adapter = matchingAdapter;
    output.adapterLuid = requiredAdapterLuid;
    D3D_FEATURE_LEVEL actualFeatureLevel = D3D_FEATURE_LEVEL_9_1;
    HRESULT deviceResult = D3D11CreateDevice(output.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, requestedFeatureLevels.data(),
        static_cast<UINT>(requestedFeatureLevels.size()), D3D11_SDK_VERSION, &output.device,
        &actualFeatureLevel, &output.context);
    if (deviceResult == E_INVALIDARG)
    {
        const D3D_FEATURE_LEVEL fallbackFeatureLevel = D3D_FEATURE_LEVEL_11_0;
        deviceResult = D3D11CreateDevice(output.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fallbackFeatureLevel, 1, D3D11_SDK_VERSION,
            &output.device, &actualFeatureLevel, &output.context);
    }
    Require(SUCCEEDED(deviceResult) && output.device && output.context && actualFeatureLevel >= D3D_FEATURE_LEVEL_11_0,
        "Replay D3D11 device creation failed, HRESULT=" + std::to_string(deviceResult));
    return output;
}

[[nodiscard]] std::int64_t CurrentQpc100ns()
{
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    std::int64_t converted = -1;
    Require(QueryPerformanceCounter(&counter) != FALSE && QueryPerformanceFrequency(&frequency) != FALSE &&
        frequency.QuadPart > 0 && pbcapturenormalize::ConvertQpcTo100ns(
            counter.QuadPart, frequency.QuadPart, converted), "Replay QPC timestamp acquisition failed");
    return converted;
}

enum class ReplayGpuCompletion : std::uint8_t
{
    Completed, DeviceRemoved, Deferred
};

// A replay frame bypasses CaptureRuntime, but it must preserve the same source
// lifetime contract. Flush1 supplies an OS completion notification for all work
// submitted before the marker. If the bounded owner-thread wait expires, this
// object retains the texture, demodulator, device and context until that OS
// notification (or registered device removal) proves external retirement. It
// never polls on a detached thread and never treats Flush as completion.
class ReplayGpuRetirement final : public std::enable_shared_from_this<ReplayGpuRetirement>
{
public:
    ~ReplayGpuRetirement()
    {
        Reset();
    }

    ReplayGpuRetirement(const ReplayGpuRetirement&) = delete;
    ReplayGpuRetirement& operator=(const ReplayGpuRetirement&) = delete;

    [[nodiscard]] static std::shared_ptr<ReplayGpuRetirement> Create(ID3D11Device* const device,
        ID3D11DeviceContext* const context)
    {
        Require(device != nullptr && context != nullptr &&
            context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE,
            "Replay GPU retirement received an invalid D3D11 immediate context");
        auto output = std::shared_ptr<ReplayGpuRetirement>(new ReplayGpuRetirement());
        HRESULT result = device->QueryInterface(IID_PPV_ARGS(&output->device_));
        if (SUCCEEDED(result))
        {
            result = context->QueryInterface(IID_PPV_ARGS(&output->context_));
        }
        Require(SUCCEEDED(result) && output->device_ && output->context_,
            "Replay requires D3D11 Flush1 retirement support, HRESULT=" + std::to_string(result));
        output->CreateSignal();
        return output;
    }

    void BindPending(std::shared_ptr<pbdemodd3d11::CaptureDemodulator> demodulator,
        const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata,
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture) noexcept
    {
        demodulator_ = std::move(demodulator);
        metadata_ = metadata;
        texture_ = std::move(texture);
    }

    void Mark()
    {
        if (marked_)
        {
            // Never Reset an event also registered for device removal: doing
            // so could erase a removal signal racing the second LF4 stage.
            // A fresh event plus fresh registration makes each marker one-shot.
            ReleaseSignal();
            CreateSignal();
            marked_ = false;
        }
        context_->Flush1(D3D11_CONTEXT_TYPE_ALL, event_);
        marked_ = true;
    }

    [[nodiscard]] ReplayGpuCompletion Wait(const DWORD timeoutMilliseconds) noexcept
    {
        if (!marked_)
        {
            return ReplayGpuCompletion::Deferred;
        }
        const DWORD waitResult = WaitForSingleObject(event_, timeoutMilliseconds);
        if (waitResult == WAIT_OBJECT_0)
        {
            return FAILED(device_->GetDeviceRemovedReason()) ?
                ReplayGpuCompletion::DeviceRemoved : ReplayGpuCompletion::Completed;
        }
        return ReplayGpuCompletion::Deferred;
    }

    void Defer() noexcept
    {
        deferredOwner_ = shared_from_this();
        SetThreadpoolWait(wait_, event_, nullptr);
    }

private:
    ReplayGpuRetirement() = default;

    void CreateSignal()
    {
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Require(event_ != nullptr,
            "Replay GPU retirement event creation failed, Win32=" + std::to_string(GetLastError()));
        const HRESULT registerResult = device_->RegisterDeviceRemovedEvent(event_, &removedCookie_);
        Require(SUCCEEDED(registerResult),
            "Replay device-removal event registration failed, HRESULT=" + std::to_string(registerResult));
        registered_ = true;
        wait_ = CreateThreadpoolWait(OnRetired, this, nullptr);
        Require(wait_ != nullptr,
            "Replay deferred GPU wait creation failed, Win32=" + std::to_string(GetLastError()));
    }

    void ReleaseSignal() noexcept
    {
        if (registered_)
        {
            device_->UnregisterDeviceRemoved(removedCookie_);
            registered_ = false;
            removedCookie_ = 0;
        }
        if (wait_ != nullptr)
        {
            CloseThreadpoolWait(std::exchange(wait_, nullptr));
        }
        if (event_ != nullptr)
        {
            CloseHandle(std::exchange(event_, nullptr));
        }
    }

    void Reset() noexcept
    {
        ReleaseSignal();
        texture_.Reset();
        demodulator_.reset();
        context_.Reset();
        device_.Reset();
    }

    static void CALLBACK OnRetired(PTP_CALLBACK_INSTANCE, void* const context, PTP_WAIT,
        const TP_WAIT_RESULT result) noexcept
    {
        auto* const retirement = static_cast<ReplayGpuRetirement*>(context);
        if (result != WAIT_OBJECT_0)
        {
            return;
        }
        const auto lifetime = retirement->deferredOwner_;
        if (!lifetime)
        {
            return;
        }
        if (retirement->demodulator_)
        {
            static_cast<void>(retirement->demodulator_->Completed(retirement->metadata_, nullptr, true));
        }
        retirement->texture_.Reset();
        retirement->demodulator_.reset();
        retirement->deferredOwner_.reset();
        // The local lifetime may destroy retirement here. Do not access it again.
    }

    Microsoft::WRL::ComPtr<ID3D11Device4> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext3> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> demodulator_;
    pbcapturenormalize::ScreenCaptureFrameMetadata metadata_{};
    HANDLE event_ = nullptr;
    PTP_WAIT wait_ = nullptr;
    DWORD removedCookie_ = 0;
    bool registered_ = false;
    bool marked_ = false;
    std::shared_ptr<ReplayGpuRetirement> deferredOwner_;
};

[[nodiscard]] bool EquivalentReplayDemodObservation(
    const pbrealcapturereplay::ReplayV2DemodObservationView& left,
    const pbrealcapturereplay::ReplayV2DemodObservationView& right) noexcept
{
    const bool baseEquivalent = left.captureEpoch == right.captureEpoch &&
        left.captureObservation == right.captureObservation &&
        left.visualProfileId == right.visualProfileId && left.disposition == right.disposition &&
        left.bootstrapAttempted == right.bootstrapAttempted &&
        left.bootstrapSucceeded == right.bootstrapSucceeded &&
        left.frameSequenceAvailable == right.frameSequenceAvailable &&
        left.frameSequence == right.frameSequence && left.transportProduced == right.transportProduced &&
        left.receiverAdmitted == right.receiverAdmitted && left.diagnosticCode == right.diagnosticCode;
    if (!baseEquivalent || !right.productionDetailAvailable)
    {
        return baseEquivalent;
    }
    return left.productionDetailAvailable && left.layoutAvailable == right.layoutAvailable &&
        left.visualLayoutVersion == right.visualLayoutVersion && left.resultKind == right.resultKind &&
        left.geometryAvailable == right.geometryAvailable && left.geometryStatus == right.geometryStatus &&
        left.geometryOriginX == right.geometryOriginX && left.geometryOriginY == right.geometryOriginY &&
        left.geometryScaleX == right.geometryScaleX && left.geometryScaleY == right.geometryScaleY &&
        left.temporalDisposition == right.temporalDisposition &&
        left.evaluationAvailable == right.evaluationAvailable && left.paddingValid == right.paddingValid &&
        left.senderTruthAvailable == right.senderTruthAvailable && left.codewords == right.codewords &&
        left.fecFailures == right.fecFailures && left.crcFailures == right.crcFailures &&
        left.identityFailures == right.identityFailures &&
        left.falseAcceptedCodewords == right.falseAcceptedCodewords &&
        left.acceptedTransportBlocks == right.acceptedTransportBlocks &&
        left.acceptedRemoteControlBlocks == right.acceptedRemoteControlBlocks &&
        left.admittedTransportBlocks == right.admittedTransportBlocks &&
        left.admittedRemoteControlBlocks == right.admittedRemoteControlBlocks &&
        left.iterationsTotal == right.iterationsTotal && left.iterationsMaximum == right.iterationsMaximum &&
        left.comparedCodedBits == right.comparedCodedBits && left.erroneousCodedBits == right.erroneousCodedBits &&
        left.metricSummaryAvailable == right.metricSummaryAvailable && left.metricSamples == right.metricSamples &&
        left.zeroMagnitudeMetrics == right.zeroMagnitudeMetrics &&
        left.minimumAbsoluteMetric == right.minimumAbsoluteMetric && left.meanAbsoluteMetric == right.meanAbsoluteMetric &&
        left.freshnessRegions == right.freshnessRegions && left.staleRegions == right.staleRegions &&
        left.freshnessTagMismatches == right.freshnessTagMismatches &&
        left.freshnessTagErasures == right.freshnessTagErasures &&
        left.freshnessErasedDataMetrics == right.freshnessErasedDataMetrics &&
        left.unreliableSymbols == right.unreliableSymbols &&
        left.metricReadbackBytes == right.metricReadbackBytes &&
        // GPU duration is retained as live evidence but is not a deterministic
        // channel result. Offline consistency compares timing availability,
        // not a second execution's wall-clock duration.
        left.gpuTimingAvailable == right.gpuTimingAvailable &&
        left.carrierAccepted == right.carrierAccepted &&
        left.receiverStateAdvanced == right.receiverStateAdvanced;
}

[[nodiscard]] pbcapturenormalize::CaptureNormalizeConfig MakeCaptureConfig(const DecoderConfig& config)
{
    pbcapturenormalize::CaptureNormalizeConfig captureConfig;
    captureConfig.capture.region = config.region;
    captureConfig.capture.initialCaptureEpoch = 1;
    captureConfig.capture.queuedFrameLimit = captureQueuedFrameLimit;
    captureConfig.capture.roiTextureCount = captureDemodulatorSlotCount;
    captureConfig.capture.maximumCaptureBytes = config.visualProfile == VisualProfile::RemoteVisualLowFps ?
        config.replayOutputPath.empty() ? maximumRemoteVisualLowFpsCaptureResidentBytes :
            maximumRemoteVisualLowFpsReplayCaptureResidentBytes : maximumCaptureResidentBytes;
    captureConfig.capture.maximumRoiBytes = maximumRoiResidentBytes;
    captureConfig.capture.gpuTimeoutMilliseconds = 3000;
    captureConfig.capture.maximumDeviceRecoveries = 1;
    captureConfig.capture.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    captureConfig.capture.maximumFrameAgeMilliseconds = 250;
    captureConfig.capture.requestBorderless = true;
    return captureConfig;
}

[[nodiscard]] pbdemodd3d11::CaptureDemodulatorConfig MakeCaptureDemodulatorConfig(
    const DecoderConfig& config, const ProfileBinding& profile, const bool offlineReplay)
{
    pbdemodd3d11::CaptureDemodulatorConfig demodConfig;
    demodConfig.visualProfileId = profile.visualProfileId;
    demodConfig.slotCount = captureDemodulatorSlotCount;
    // A sealed Replay has no live capture backlog. Keep the same bounded
    // production decoder while allowing slow WARP/offline execution to finish
    // without misclassifying compute time as capture staleness.
    demodConfig.maximumFrameAgeMilliseconds = offlineReplay ? 60000 : 250;
    demodConfig.resultQueueCapacity = captureResultQueueCapacity;
    if (offlineReplay && config.visualProfile == VisualProfile::RemoteVisualLowFps)
    {
        demodConfig.maximumResidentBytes = maximumReplayDemodulatorResidentBytes;
    }
    else if (config.visualProfile == VisualProfile::RemoteVisualLowFps)
    {
        demodConfig.maximumResidentBytes = maximumRemoteVisualLowFpsDemodulatorResidentBytes;
    }
    else
    {
        demodConfig.maximumResidentBytes = maximumDemodulatorResidentBytes;
    }
    if (!offlineReplay && config.visualProfile == VisualProfile::RemoteVisualLowFps)
    {
        demodConfig.maximumRoiWidth = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(config.region.physicalRect.right) - config.region.physicalRect.left);
        demodConfig.maximumRoiHeight = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(config.region.physicalRect.bottom) - config.region.physicalRect.top);
    }
    demodConfig.evaluationMode = pbdesktoplevels::EvaluationMode::Transport;
    return demodConfig;
}

[[nodiscard]] pbcapturenormalize::DiagnosticReadbackConfig MakeProductionReplayReadbackConfig(
    const DecoderConfig& config, const pbcapturenormalize::CaptureNormalizeConfig& captureConfig,
    const std::uint64_t processingReservedBytes)
{
    const std::int64_t roiWidth = static_cast<std::int64_t>(config.region.physicalRect.right) -
        config.region.physicalRect.left;
    const std::int64_t roiHeight = static_cast<std::int64_t>(config.region.physicalRect.bottom) -
        config.region.physicalRect.top;
    pbcapturenormalize::DiagnosticReadbackConfig readbackConfig;
    readbackConfig.maximumRoiSize = {static_cast<std::int32_t>(roiWidth), static_cast<std::int32_t>(roiHeight)};
    // DiagnosticCpuReadback indexes its staging ring with the capture slot.
    // The two counts therefore form one lifetime contract and must never drift.
    readbackConfig.stagingTextureCount = captureConfig.capture.roiTextureCount;
    readbackConfig.maximumFrameAgeMilliseconds = captureConfig.capture.maximumFrameAgeMilliseconds;
    readbackConfig.maximumReadbackBytes = maximumProductionReplayReadbackResidentBytes;
    readbackConfig.processingReservedBytes = processingReservedBytes;
    readbackConfig.minimumSubmissionInterval100ns = CalculateReplaySamplingInterval100ns(
        config.replayMaximumCaptureFramesPerSecond);
    return readbackConfig;
}

[[nodiscard]] RuntimeStatus ValidateProductionReplayResourceContract(const DecoderConfig& config,
    const ProfileBinding& profile, const pbcapturenormalize::CaptureNormalizeConfig& captureConfig)
{
    if (config.replayOutputPath.empty() || config.diagnosticCaptureOnly)
    {
        return {};
    }
    const std::uint64_t roiWidth = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(config.region.physicalRect.right) - config.region.physicalRect.left);
    const std::uint64_t roiHeight = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(config.region.physicalRect.bottom) - config.region.physicalRect.top);
    const auto pixels = pbprotocol::CheckedMultiplyUint64(roiWidth, roiHeight);
    const auto frameBytes = pixels ? pbprotocol::CheckedMultiplyUint64(pixels.Value(), 4) : pixels;
    const auto recorderSlots = pbprotocol::CheckedAddUint64(remoteVisualReplayRecorderQueueCapacity, 1);
    const auto recorderPixelReservation = frameBytes && recorderSlots ?
        pbprotocol::CheckedMultiplyUint64(frameBytes.Value(), recorderSlots.Value()) : frameBytes;
    if (!pixels || !frameBytes || !recorderSlots || !recorderPixelReservation ||
        recorderPixelReservation.Value() > config.replayMaximumFileBytes)
    {
        return RuntimeStatus::Failure(
            "RemoteVisual production Replay 的 file budget 小于 bounded recorder slot reservation");
    }

    pbdemodd3d11::CaptureDemodulatorBudget demodBudget;
    const auto demodStatus = pbdemodd3d11::CalculateCaptureDemodulatorBudget(
        MakeCaptureDemodulatorConfig(config, profile, false), demodBudget);
    if (!demodStatus)
    {
        return RuntimeStatus::Failure("RemoteVisual production Replay demod resource contract invalid: " +
            DescribeCaptureStatus(demodStatus));
    }
    const auto readbackConfig = MakeProductionReplayReadbackConfig(config, captureConfig,
        maximumProductionReplayProcessorResidentBytes);
    pbcapturenormalize::DiagnosticReadbackBudget readbackBudget;
    const auto readbackStatus = pbcapturenormalize::CalculateDiagnosticReadbackBudget(readbackConfig, readbackBudget);
    if (!readbackStatus)
    {
        return RuntimeStatus::Failure("RemoteVisual production Replay readback resource contract invalid: " +
            DescribeCaptureStatus(readbackStatus));
    }
    const auto consumerReservation = pbprotocol::CheckedAddUint64(demodBudget.totalBytes, readbackBudget.totalBytes);
    if (!consumerReservation || consumerReservation.Value() >= captureConfig.capture.maximumCaptureBytes)
    {
        return RuntimeStatus::Failure("RemoteVisual production Replay combined consumer reservation overflow");
    }
    auto remainingCaptureConfig = captureConfig.capture;
    remainingCaptureConfig.maximumCaptureBytes -= consumerReservation.Value();
    const auto nativeStatus = config.captureBackend == CaptureBackend::Wgc ?
        pbscreencapturewgc::ValidateWgcCaptureConfig(remainingCaptureConfig) :
        pbscreencapturedxgi::ValidateDxgiCaptureConfig(remainingCaptureConfig);
    if (!nativeStatus)
    {
        return RuntimeStatus::Failure("RemoteVisual production Replay leaves insufficient bounded native capture budget: " +
            DescribeCaptureStatus(nativeStatus));
    }
    return {};
}

void RunRemoteVisualDiagnosticCapture(const DecoderConfig& config, const ProfileBinding& profile,
    const std::string& runId, SnapshotStore<DecoderSnapshot>& snapshot, const std::uint64_t runGeneration,
    const std::chrono::steady_clock::time_point started, ProcessResourceSampler& resourceSampler,
    std::atomic<bool>& stopRequested)
{
    Require(config.diagnosticCaptureOnly && !config.replayOutputPath.empty() && config.monitorSafety.has_value(),
        "diagnostic capture-only entered without its validated replay/monitor contract");
    const std::int64_t roiWidth = static_cast<std::int64_t>(config.region.physicalRect.right) -
        config.region.physicalRect.left;
    const std::int64_t roiHeight = static_cast<std::int64_t>(config.region.physicalRect.bottom) -
        config.region.physicalRect.top;
    Require(roiWidth > 0 && roiHeight > 0 && roiWidth <= 16384 && roiHeight <= 16384,
        "diagnostic capture-only ROI exceeds the bounded capture inventory");

    const auto datasetId = pbprotocol::GenerateRandomSessionId();
    RequireResult(datasetId, "Replay dataset CSPRNG failed");
    pbrealcapturereplay::ReplayV2FileDescriptor replayDescriptor;
    replayDescriptor.datasetClass = pbrealcapturereplay::ReplayDatasetClass::RemoteVisual;
    replayDescriptor.datasetId = datasetId.Value().bytes;
    replayDescriptor.runId = runId;
    replayDescriptor.visualProfileId = config.replayEvidenceVisualProfileId.value_or(profile.visualProfileId);
    replayDescriptor.createdUtc100ns = GetUtcFileTime100ns();
    replayDescriptor.remoteMetadataJsonUtf8 = BuildRemoteVisualRunMetadataJson(config.remoteMetadata);
    Require(replayDescriptor.createdUtc100ns > 0, "Replay UTC timestamp acquisition failed");

    RemoteVisualReplayRecorderConfig recorderConfig;
    recorderConfig.outputPath = config.replayOutputPath;
    recorderConfig.descriptor = std::move(replayDescriptor);
    recorderConfig.limits.maximumFileBytes = config.replayMaximumFileBytes;
    recorderConfig.limits.maximumTotalRasterBytes = config.replayMaximumFileBytes;
    recorderConfig.limits.maximumCaptureFrames = config.replayMaximumCaptureFrames;
    recorderConfig.roiWidth = static_cast<std::uint32_t>(roiWidth);
    recorderConfig.roiHeight = static_cast<std::uint32_t>(roiHeight);
    recorderConfig.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    recorderConfig.displayIdentityUtf8 = !config.remoteMetadata.experimentMonitorIdentity.empty() ?
        config.remoteMetadata.experimentMonitorIdentity :
        Utf8FromWide(config.monitorSafety->experimentMonitor.deviceName);
    recorderConfig.dpiX = config.region.dpiX;
    recorderConfig.dpiY = config.region.dpiY;
    recorderConfig.scaleX = config.remoteMetadata.estimatedScaleX.value_or(
        static_cast<double>(roiWidth) / phase1CanvasWidth);
    recorderConfig.scaleY = config.remoteMetadata.estimatedScaleY.value_or(
        static_cast<double>(roiHeight) / phase1CanvasHeight);
    std::shared_ptr<RemoteVisualReplayRecorder> replayRecorder;
    const auto recorderStatus = RemoteVisualReplayRecorder::Create(recorderConfig, replayRecorder);
    Require(static_cast<bool>(recorderStatus), "Replay recorder creation failed: " +
        DescribeCaptureStatus(recorderStatus));

    auto captureConfig = MakeCaptureConfig(config);
    const std::uint64_t samplingInterval100ns = CalculateReplaySamplingInterval100ns(
        config.replayMaximumCaptureFramesPerSecond);
    if (samplingInterval100ns != 0 && config.captureBackend == CaptureBackend::Wgc)
    {
        captureConfig.capture.minUpdateInterval100ns = static_cast<std::int64_t>(samplingInterval100ns);
    }
    pbcapturenormalize::DiagnosticReadbackConfig readbackConfig;
    readbackConfig.maximumRoiSize = {static_cast<std::int32_t>(roiWidth), static_cast<std::int32_t>(roiHeight)};
    readbackConfig.stagingTextureCount = captureConfig.capture.roiTextureCount;
    readbackConfig.maximumFrameAgeMilliseconds = 250;
    readbackConfig.maximumReadbackBytes = 256ULL * mebibyte;
    readbackConfig.processingReservedBytes = replayRecorder->ProcessingReservedBytes();
    readbackConfig.minimumSubmissionInterval100ns = samplingInterval100ns;
    std::shared_ptr<pbcapturenormalize::DiagnosticCpuReadback> replayReadback;
    const auto readbackStatus = pbcapturenormalize::DiagnosticCpuReadback::Create(
        readbackConfig, replayRecorder, replayReadback);
    Require(static_cast<bool>(readbackStatus), "Replay diagnostic readback creation failed: " +
        DescribeCaptureStatus(readbackStatus));

    NativeCaptureSession capture(config.captureBackend, captureConfig, replayReadback);
    snapshot.Update([&](DecoderSnapshot& value)
    {
        if (value.runGeneration == runGeneration)
        {
            value.actualBackend = config.captureBackend;
            value.backendReason = "Explicit backend selected for bounded replay capture-only; no demodulation or fallback";
        }
    });
    auto nextMonitorSafetyCheck = started;
    ChannelStallTracker captureOnlyStalls;
    bool captureLimitReached = false;
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextMonitorSafetyCheck)
        {
            const MonitorSafetyStatus monitorSafety = RevalidateMonitorSafetySelection(*config.monitorSafety);
            Require(static_cast<bool>(monitorSafety),
                std::string("display topology changed during diagnostic capture-only: ") +
                    GetMonitorSafetyErrorName(monitorSafety.code));
            snapshot.Update([runGeneration](DecoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.monitorSafetyPreflightPassed = true;
                    value.monitorSafetyRevalidationCount++;
                    value.monitorSafetyStatus = "PASS";
                }
            });
            nextMonitorSafetyCheck = now + std::chrono::seconds(1);
        }
        resourceSampler.Sample(ElapsedMilliseconds(started));
        const pbcapturenormalize::CaptureSnapshot captureSnapshot = capture.GetSnapshot();
        const pbcapturenormalize::DiagnosticReadbackSnapshot readbackSnapshot = replayReadback->GetSnapshot();
        const RemoteVisualReplayRecorderSnapshot recorderSnapshot = replayRecorder->GetSnapshot();
        const std::uint64_t elapsedMilliseconds = ElapsedMilliseconds(started);
        captureOnlyStalls.Observe(elapsedMilliseconds, captureSnapshot.arrivedFrames,
            captureSnapshot.arrivedFrames);
        const ChannelStallSnapshot captureOnlyStallSnapshot = captureOnlyStalls.GetSnapshot();
        if (captureSnapshot.state == pbcapturenormalize::CaptureState::Failed || !readbackSnapshot.error)
        {
            throw RuntimeFailure("diagnostic capture-only entered terminal failure: capture=" +
                DescribeCaptureStatus(captureSnapshot.error) + " readback=" +
                DescribeCaptureStatus(readbackSnapshot.error));
        }
        snapshot.Update([&](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration)
            {
                return;
            }
            ApplyCaptureComponentSnapshot(captureSnapshot, value);
            value.captureReadbackDropEvents = readbackSnapshot.dropEvents;
            value.replaySampledOutFrames = readbackSnapshot.sampledOutFrames;
            value.captureFps = elapsedMilliseconds == 0 ? std::optional<double>{} :
                std::optional<double>{static_cast<double>(captureSnapshot.deliveredFrames) * 1000.0 /
                    static_cast<double>(elapsedMilliseconds)};
            value.telemetryCapturedFrames = captureSnapshot.deliveredFrames;
            value.telemetryDroppedFrames = pbprotocol::SaturatingAddUnsigned(
                captureSnapshot.droppedFrames, readbackSnapshot.dropEvents);
            value.recoveryRuntimeMilliseconds = elapsedMilliseconds;
            value.captureStallCount = captureOnlyStallSnapshot.capture.count;
            value.captureStallTotalMilliseconds = pbprotocol::SaturatingAddUnsigned(
                captureOnlyStallSnapshot.capture.totalMilliseconds,
                captureOnlyStallSnapshot.capture.currentMilliseconds);
            value.captureStallMaximumMilliseconds = std::max(
                captureOnlyStallSnapshot.capture.maximumMilliseconds,
                captureOnlyStallSnapshot.capture.currentMilliseconds);
            value.captureStallActive = captureOnlyStallSnapshot.capture.active;
            ApplyReplaySnapshot(recorderSnapshot, value);
            value.replayDroppedFrames = pbprotocol::SaturatingAddUnsigned(value.replayDroppedFrames,
                readbackSnapshot.dropEvents);
            ApplyProcessResourceSample(resourceSampler.GetSnapshot(), value);
        });
        captureLimitReached = recorderSnapshot.enqueuedFrames >= config.replayMaximumCaptureFrames;
        if (stopRequested || captureLimitReached || captureSnapshot.state == pbcapturenormalize::CaptureState::Stopped)
        {
            capture.RequestStop();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto captureStopStatus = capture.Stop();
    captureOnlyStalls.Finish(ElapsedMilliseconds(started));
    const auto readbackStopStatus = replayReadback->Stop();
    const auto recorderStopStatus = replayRecorder->Stop();
    const pbcapturenormalize::CaptureSnapshot finalCaptureSnapshot = capture.GetSnapshot();
    const RemoteVisualReplayRecorderSnapshot finalRecorderSnapshot = replayRecorder->GetSnapshot();
    const pbcapturenormalize::DiagnosticReadbackSnapshot finalReadbackSnapshot = replayReadback->GetSnapshot();
    snapshot.Update([&](DecoderSnapshot& value)
    {
        if (value.runGeneration != runGeneration)
        {
            return;
        }
        ApplyCaptureComponentSnapshot(finalCaptureSnapshot, value);
        ApplyReplaySnapshot(finalRecorderSnapshot, value);
        value.replayDroppedFrames = pbprotocol::SaturatingAddUnsigned(value.replayDroppedFrames,
            finalReadbackSnapshot.dropEvents);
        value.captureReadbackDropEvents = finalReadbackSnapshot.dropEvents;
        value.replaySampledOutFrames = finalReadbackSnapshot.sampledOutFrames;
        const ChannelStallSnapshot finalStalls = captureOnlyStalls.GetSnapshot();
        value.captureStallCount = finalStalls.capture.count;
        value.captureStallTotalMilliseconds = finalStalls.capture.totalMilliseconds;
        value.captureStallMaximumMilliseconds = finalStalls.capture.maximumMilliseconds;
        value.captureStallActive = false;
        value.state = DecoderState::Stopped;
        value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
        value.recoveryRuntimeMilliseconds = ElapsedMilliseconds(started);
        value.statusMessage = captureLimitReached ?
            "Diagnostic replay capture-only reached its explicit frame limit; no decode or publish was attempted" :
            "Diagnostic replay capture-only stopped by user; no decode or publish was attempted";
        ApplyProcessResourceSample(resourceSampler.GetSnapshot(), value);
    });
    Require(static_cast<bool>(captureStopStatus), "diagnostic capture-only shutdown failed: " +
        DescribeCaptureStatus(captureStopStatus));
    Require(static_cast<bool>(readbackStopStatus), "diagnostic readback shutdown failed: " +
        DescribeCaptureStatus(readbackStopStatus));
    Require(static_cast<bool>(recorderStopStatus) && finalRecorderSnapshot.finalized &&
        finalRecorderSnapshot.evidenceValid && finalRecorderSnapshot.writtenFrames != 0,
        "diagnostic replay capture-only did not produce a finalized nonempty evidence file");
}

struct AuthoritativeCompletion
{
    bool published = false;
    std::string outputPath;
    std::string wholeFileDigestHex;
    std::uint64_t recoveryRuntimeMilliseconds = 0;
};

class ReceiverPipeline
{
public:
    ReceiverPipeline(pbreceiver::ReceiverIngress& receiver, std::wstring outputDirectory,
        const pbprotocol::ReceiverResourcePolicy& policy, SnapshotStore<DecoderSnapshot>& snapshot,
        AuthoritativeCompletion& completion, const std::uint64_t runGeneration,
        const std::chrono::steady_clock::time_point started, const VisualProfile visualProfile,
        const bool deferCompletedState = false)
        : receiver_(receiver), outputDirectory_(std::move(outputDirectory)), policy_(policy), snapshot_(snapshot),
          completion_(completion), runGeneration_(runGeneration), started_(started), visualProfile_(visualProfile),
          deferCompletedState_(deferCompletedState)
    {
    }

    [[nodiscard]] ReceiverProcessResult Process(const pbdemodd3d11::CaptureDemodulatorResult& result)
    {
        RecordCaptureTelemetry(result);
        RecordRemoteMetricTelemetry(result);
        if (!result.bootstrap.IsAccepted())
        {
            UpdateTelemetrySnapshot();
            return {};
        }
        snapshot_.Update([this, &result](DecoderSnapshot& value)
        {
            if (value.runGeneration == runGeneration_ && visualProfile_ == VisualProfile::RemoteVisualResilient)
            {
                value.remoteMetadata.estimatedScaleX = result.bootstrap.geometry.scaleX;
                value.remoteMetadata.estimatedScaleY = result.bootstrap.geometry.scaleY;
                value.remoteMetadata.geometryStatus = "CompatibleStrict1:1";
                value.remoteMetadata.geometryProvenance = MetadataProvenance::PixelBridgeObserved;
            }
        });
        const auto parsedBootstrap = pbprotocol::ParseBootstrapRecord(result.bootstrapRecord);
        RequireResult(parsedBootstrap, "CaptureDemodulator published an invalid Bootstrap");
        const pbprotocol::BootstrapRecord& bootstrap = parsedBootstrap.Value();
        if (session_ && bootstrap.sessionTag != pbprotocol::DeriveSessionTag(session_->sessionId))
        {
            UpdateTelemetrySnapshot();
            return {};
        }

        const VisualIdentityDisposition identityDisposition = visualRate_.Observe(bootstrap.frameSequence,
            result.metadata.domain.captureEpoch, result.metadata.timestamp.monotonic100ns,
            bootstrap.sessionTag.value);
        if (identityDisposition == VisualIdentityDisposition::Invalid)
        {
            UpdateVisualSnapshot();
            UpdateTelemetrySnapshot();
            return {};
        }
        const bool lowFps = visualProfile_ == VisualProfile::RemoteVisualLowFps;
        if (lowFps)
        {
            Require(result.temporalDisposition != pbdemodd3d11::CaptureDemodulatorTemporalDisposition::NotApplicable,
                "LF4 CaptureDemodulator result bypassed temporal admission");
            Require((result.kind != pbdemodd3d11::CaptureDemodulatorResultKind::ControlRecord ||
                        result.admittedRemoteControlBlockCount == 1) &&
                    (result.kind != pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly ||
                        (result.admittedTransportBlockCount == 0 && result.admittedRemoteControlBlockCount == 0)),
                "LF4 CaptureDemodulator result kind disagrees with temporal carrier admission");
        }
        if (!lowFps && identityDisposition == VisualIdentityDisposition::Unique)
        {
            remoteRefinement_.StartSequence(result.metadata.domain.captureEpoch, bootstrap.frameSequence);
        }
        const bool uniqueAdmissionFrame = lowFps ?
            result.temporalDisposition == pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique :
            identityDisposition == VisualIdentityDisposition::Unique;
        if (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport &&
            uniqueAdmissionFrame)
        {
            RecordFecTelemetry(result);
        }

        const bool temporallySuppressed = lowFps &&
            (result.temporalDisposition == pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateSuppressed ||
             result.temporalDisposition == pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Reordered ||
             result.temporalDisposition == pbdemodd3d11::CaptureDemodulatorTemporalDisposition::StaleCompletion);
        if ((!lowFps && identityDisposition == VisualIdentityDisposition::Reordered) || temporallySuppressed)
        {
            UpdateVisualSnapshot();
            UpdateTelemetrySnapshot();
            return {};
        }

        const bool acceptedCarrier = (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::ControlRecord &&
                (!lowFps || result.admittedRemoteControlBlockCount == 1)) ||
            result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::ControlFragment ||
            (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport &&
             result.admittedTransportBlockCount != 0);
        bool duplicateRefinement = false;
        const bool isDuplicateRefinement = lowFps ?
            result.temporalDisposition == pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateRefinement :
            identityDisposition == VisualIdentityDisposition::Duplicate;
        if (isDuplicateRefinement)
        {
            if ((!lowFps && visualProfile_ != VisualProfile::RemoteVisualResilient) ||
                (!lowFps &&
                !remoteRefinement_.ShouldAttemptDuplicate(result.metadata.domain.captureEpoch,
                    bootstrap.frameSequence, acceptedCarrier)) || (lowFps && !acceptedCarrier))
            {
                UpdateVisualSnapshot();
                UpdateTelemetrySnapshot();
                return {};
            }
            duplicateRefinement = true;
        }

        if (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly)
        {
            UpdateVisualSnapshot();
            UpdateTelemetrySnapshot();
            return {};
        }
        bool carrierAccepted = false;
        bool receiverAdmission = false;
        if (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::ControlRecord)
        {
            Require(result.controlByteCount <= result.controlBytes.size(),
                "CaptureDemodulator ControlRecord byte count is out of bounds");
            carrierAccepted = ProcessControlRecord(std::span(result.controlBytes).first(result.controlByteCount),
                bootstrap.sessionTag, result.metadata.timestamp.monotonic100ns);
            receiverAdmission = carrierAccepted;
        }
        else if (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::ControlFragment)
        {
            Require(result.controlByteCount <= result.controlBytes.size(),
                "CaptureDemodulator ControlFragment byte count is out of bounds");
            carrierAccepted = ProcessControlFragment(std::span(result.controlBytes).first(result.controlByteCount),
                result.metadata.captureObservation);
            receiverAdmission = carrierAccepted;
        }
        else
        {
            const ReceiverProcessResult transport = ProcessTransport(result, uniqueAdmissionFrame);
            carrierAccepted = transport.carrierAccepted;
            receiverAdmission = transport.uniqueAdmission;
        }
        if (carrierAccepted && !lowFps)
        {
            static_cast<void>(remoteRefinement_.MarkAdmission(result.metadata.domain.captureEpoch,
                bootstrap.frameSequence, duplicateRefinement));
        }
        if (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport && receiverAdmission)
        {
            static_cast<void>(endToEndRate_.Observe(bootstrap.frameSequence, result.metadata.domain.captureEpoch,
                result.metadata.timestamp.monotonic100ns, bootstrap.sessionTag.value));
        }
        UpdateVisualSnapshot();
        UpdateTelemetrySnapshot();
        return {carrierAccepted, receiverAdmission};
    }

    void CaptureEpochReset(const std::uint64_t monotonicMilliseconds)
    {
        telemetry_.EndCaptureEpoch();
        receiverSessionBound_ = false;
        storage_.reset();
        session_.reset();
        segment_.reset();
        manifest_.reset();
        stored_ = false;
        storedEncodedBytes_ = 0;
        acceptedTransportBlocks_ = 0;
        identityFailures_ = 0;
        falseAcceptedCodewords_ = 0;
        outerUniqueSymbols_ = 0;
        outerIdenticalDuplicateSymbols_ = 0;
        outerRecoveryAlreadyReadySymbols_ = 0;
        outerAlreadyCompletedSymbols_ = 0;
        outerRecoveryReadyEvents_ = 0;
        outerResourceRejections_ = 0;
        outerConflictRejections_ = 0;
        remoteRefinement_.ResetEpoch();
        progress_.ResetForCaptureEpoch(monotonicMilliseconds);
        channelStalls_.ResetDomain(monotonicMilliseconds, lastCaptureObservationsForStall_,
            visualRate_.GetSnapshot().uniqueFrames);
        snapshot_.Update([this](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            pbprotocol::SaturatingIncrementUnsigned(value.captureEpochResets);
            if (value.state != DecoderState::Stopping)
            {
                value.state = DecoderState::WaitingForBootstrap;
            }
            value.descriptorKnown = false;
            value.originalFileBytes = 0;
            value.verifiedRawBytes = 0;
            value.remainingRawBytes = 0;
            value.recoveryProgress.reset();
            value.instantVerifiedRawGoodputBytesPerSecond = 0;
            value.smoothedVerifiedRawGoodputBytesPerSecond = 0;
            value.averageVerifiedRawGoodputBytesPerSecond = 0;
            value.verifiedEncodedBytes = 0;
            value.verifiedEncodedGoodputBitsPerSecond.reset();
            value.etaMilliseconds.reset();
            value.sessionIdHex.clear();
            value.sessionTag = 0;
            value.segmentCount = 0;
            value.currentSegmentOrdinal = 0;
            value.outputPath.clear();
            value.wholeFileDigestHex.clear();
            value.wholeFileDigestVerified = false;
            value.finalPublishSucceeded = false;
            value.telemetryCapturedFrames = 0;
            value.telemetryDroppedFrames = 0;
            value.fingerprintedFrames = 0;
            value.captureFps.reset();
            value.roiPixelDigestUniqueVisualFps.reset();
            value.uniqueVisualFps.reset();
            value.telemetryBootstrapAttempts = 0;
            value.telemetryBootstrapSuccesses = 0;
            value.bootstrapSuccessRate.reset();
            value.evaluatedDataFrames = 0;
            value.evaluatedCodewords = 0;
            value.postFecFailedFrames = 0;
            value.acceptedTransportBlocks = 0;
            value.temporallyAdmittedTransportBlocks = 0;
            value.fecAcceptedTransportBlocks = 0;
            value.fecAcceptedTransportBlockRate.reset();
            value.comparedCodedBits = 0;
            value.erroneousCodedBits = 0;
            value.fecFailures = 0;
            value.crcFailures = 0;
            value.identityFailures = 0;
            value.falseAcceptedCodewords = 0;
            value.outerUniqueSymbols = 0;
            value.outerIdenticalDuplicateSymbols = 0;
            value.outerRecoveryAlreadyReadySymbols = 0;
            value.outerAlreadyCompletedSymbols = 0;
            value.outerRecoveryReadyEvents = 0;
            value.outerResourceRejections = 0;
            value.outerConflictRejections = 0;
            value.preFecBerEstimate.reset();
            value.fecFrameErrorRate.reset();
            value.fecCodewordFailureRate.reset();
            value.remoteMetricFrames = 0;
            value.remoteMetricSamples = 0;
            value.remoteZeroMagnitudeMetrics = 0;
            value.remoteZeroMagnitudeMetricRate.reset();
            value.remoteMinimumAbsoluteMetric.reset();
            value.remoteMeanAbsoluteMetric.reset();
            value.remoteSymbolSamples = 0;
            value.remoteUnreliableSymbols = 0;
            value.remoteUnreliableSymbolRate.reset();
            value.remoteVerifiedMetricFrames = 0;
            value.remoteRejectedMetricFrames = 0;
            value.remoteVerifiedMeanAbsoluteMetric.reset();
            value.remoteRejectedMeanAbsoluteMetric.reset();
            value.remoteRejectedZeroMagnitudeMetricRate.reset();
            value.remoteFreshnessRegions = 0;
            value.remoteFreshRegions = 0;
            value.remoteStaleRegions = 0;
            value.remoteStaleRegionRate.reset();
            value.remoteFramesWithStaleRegions = 0;
            value.remoteFreshnessTagMismatches = 0;
            value.remoteFreshnessTagErasures = 0;
            value.remoteFreshnessErasedDataMetrics = 0;
            value.remoteFreshnessErasedDataMetricRate.reset();
            value.statusMessage = "CaptureEpoch changed; discarded unpublished state and waiting for authoritative descriptor rebind";
        });
    }

    void ObserveDroppedFrames(const std::uint64_t captureDroppedFrames,
        const std::uint64_t resultQueueDrops, const std::uint64_t staleResultDrops)
    {
        const auto Delta = [](const std::uint64_t current, const std::uint64_t previous) noexcept
        {
            return current >= previous ? current - previous : current;
        };
        const std::uint64_t captureDelta = Delta(captureDroppedFrames, lastCaptureDroppedFrames_);
        const std::uint64_t queueDelta = Delta(resultQueueDrops, lastResultQueueDrops_);
        const std::uint64_t staleDelta = Delta(staleResultDrops, lastStaleResultDrops_);
        lastCaptureDroppedFrames_ = captureDroppedFrames;
        lastResultQueueDrops_ = resultQueueDrops;
        lastStaleResultDrops_ = staleResultDrops;
        const auto captureAndQueueDelta = pbprotocol::CheckedAddUint64(captureDelta, queueDelta);
        RequireResult(captureAndQueueDelta, "capture/result dropped-frame delta overflow");
        const auto totalDelta = pbprotocol::CheckedAddUint64(captureAndQueueDelta.Value(), staleDelta);
        RequireResult(totalDelta, "capture/result/stale dropped-frame delta overflow");
        const std::uint64_t delta = totalDelta.Value();
        if (delta == 0)
        {
            return;
        }
        if (telemetry_.GetSnapshot().active)
        {
            telemetry_.RecordDroppedFrames(delta);
        }
        else
        {
            const auto pending = pbprotocol::CheckedAddUint64(pendingDroppedFrames_, delta);
            RequireResult(pending, "pending PBTelemetry dropped-frame counter overflow");
            pendingDroppedFrames_ = pending.Value();
        }
        Require(!telemetry_.GetSnapshot().counterSaturated,
            "PBTelemetry dropped-frame counter overflow");
        UpdateTelemetrySnapshot();
    }

    void EndCaptureTelemetry(const std::uint64_t monotonicMilliseconds)
    {
        telemetry_.EndCaptureEpoch();
        channelStalls_.Finish(monotonicMilliseconds);
        ApplyChannelStalls();
        UpdateTelemetrySnapshot();
    }

    void ObserveStall(const std::uint64_t monotonicMilliseconds, const std::uint64_t captureObservations)
    {
        progress_.ObserveStall(monotonicMilliseconds);
        lastCaptureObservationsForStall_ = captureObservations;
        channelStalls_.Observe(monotonicMilliseconds, captureObservations, visualRate_.GetSnapshot().uniqueFrames);
        ApplyProgress();
        ApplyChannelStalls();
    }

    [[nodiscard]] bool IsCompleted() const noexcept
    {
        return published_;
    }

private:
    static void RequireTelemetry(const pbtelemetry::TelemetryStatus status, const char* const operation)
    {
        if (!status)
        {
            throw RuntimeFailure(std::string(operation) + " failed: " +
                pbtelemetry::GetTelemetryErrorName(status.code));
        }
    }

    void RecordCaptureTelemetry(const pbdemodd3d11::CaptureDemodulatorResult& result)
    {
        pbtelemetry::TelemetrySnapshot telemetry = telemetry_.GetSnapshot();
        if (!telemetry.active)
        {
            RequireTelemetry(telemetry_.BeginCaptureEpoch(result.metadata.domain,
                result.metadata.timestamp.monotonic100ns), "PBTelemetry BeginCaptureEpoch");
            if (pendingDroppedFrames_ != 0)
            {
                telemetry_.RecordDroppedFrames(pendingDroppedFrames_);
                pendingDroppedFrames_ = 0;
            }
        }
        else
        {
            Require(telemetry.domain == result.metadata.domain,
                "PBTelemetry received a capture domain without an explicit CaptureEpoch reset");
        }

        const pbtelemetry::CaptureSample captureSample{result.metadata.domain,
            result.metadata.captureObservation, result.metadata.timestamp.monotonic100ns,
            result.metadata.roiCopyTime100ns, std::nullopt};
        RequireTelemetry(telemetry_.RecordCapture(captureSample), "PBTelemetry RecordCapture");

        pbtelemetry::BootstrapSample bootstrapSample;
        bootstrapSample.domain = result.metadata.domain;
        bootstrapSample.captureObservation = result.metadata.captureObservation;
        bootstrapSample.success = result.bootstrap.IsAccepted();
        if (bootstrapSample.success)
        {
            bootstrapSample.scaleX = result.bootstrap.geometry.scaleX;
            bootstrapSample.scaleY = result.bootstrap.geometry.scaleY;
            bootstrapSample.phaseX = result.bootstrap.geometry.originX - std::floor(result.bootstrap.geometry.originX);
            bootstrapSample.phaseY = result.bootstrap.geometry.originY - std::floor(result.bootstrap.geometry.originY);
        }
        RequireTelemetry(telemetry_.RecordBootstrap(bootstrapSample), "PBTelemetry RecordBootstrap");
    }

    void RecordFecTelemetry(const pbdemodd3d11::CaptureDemodulatorResult& result)
    {
        Require(result.demodulation.evaluation.evaluated,
            "CaptureDemodulator Transport result did not carry a FEC evaluation");
        const pbtelemetry::FecSample fecSample{result.metadata.domain,
            result.metadata.captureObservation, result.demodulation.evaluation};
        RequireTelemetry(telemetry_.RecordFec(fecSample), "PBTelemetry RecordFec");
    }

    void RecordRemoteMetricTelemetry(const pbdemodd3d11::CaptureDemodulatorResult& result)
    {
        const auto& demodulation = result.demodulation;
        if (!demodulation.remoteMetricSummaryAvailable)
        {
            return;
        }
        const bool lowFps = visualProfile_ == VisualProfile::RemoteVisualLowFps;
        const std::uint32_t expectedMetricSamples = lowFps ? pbmodulation::kRemoteVisualLowFpsCodedBits :
            pbmodulation::kRemoteVisualCodedBits;
        const std::uint32_t expectedSymbolSamples = lowFps ? pbmodulation::kRemoteVisualDataTileCount : 0;
        Require((lowFps || visualProfile_ == VisualProfile::RemoteVisualResilient) &&
            demodulation.remoteMetricSamples == expectedMetricSamples &&
            demodulation.remoteZeroMagnitudeMetrics <= demodulation.remoteMetricSamples &&
            demodulation.remoteFreshnessRegions == pbmodulation::kRemoteVisualEligibleFreshnessRegions &&
            demodulation.remoteStaleRegions <= demodulation.remoteFreshnessRegions &&
            demodulation.remoteFreshnessErasedDataMetrics <= demodulation.remoteMetricSamples &&
            demodulation.remoteUnreliableSymbols <= expectedSymbolSamples &&
            (lowFps || demodulation.remoteUnreliableSymbols == 0) &&
            std::isfinite(demodulation.remoteMinimumAbsoluteMetric) &&
            std::isfinite(demodulation.remoteMeanAbsoluteMetric) &&
            demodulation.remoteMinimumAbsoluteMetric >= 0 && demodulation.remoteMeanAbsoluteMetric >= 0,
            "CaptureDemodulator emitted an invalid RemoteVisual metric summary");
        pbtelemetry::RemoteMetricFrameClass frameClass = pbtelemetry::RemoteMetricFrameClass::Other;
        if (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport &&
            demodulation.evaluation.evaluated)
        {
            frameClass = demodulation.evaluation.IsVerified() ? pbtelemetry::RemoteMetricFrameClass::TransportVerified :
                pbtelemetry::RemoteMetricFrameClass::TransportRejected;
        }
        const pbtelemetry::RemoteMetricSample sample{result.metadata.domain, result.metadata.captureObservation,
            demodulation.remoteMetricSamples, demodulation.remoteZeroMagnitudeMetrics,
            demodulation.remoteMinimumAbsoluteMetric, demodulation.remoteMeanAbsoluteMetric,
            expectedSymbolSamples, demodulation.remoteUnreliableSymbols, demodulation.remoteFreshnessRegions,
            demodulation.remoteStaleRegions, demodulation.remoteFreshnessTagMismatches,
            demodulation.remoteFreshnessTagErasures, demodulation.remoteFreshnessErasedDataMetrics, frameClass};
        RequireTelemetry(telemetry_.RecordRemoteMetric(sample), "PBTelemetry RecordRemoteMetric");
    }

    [[nodiscard]] bool IsRetryableUnknownSession(const pbreceiver::ReceiverError& error) const noexcept
    {
        const auto* protocol = std::get_if<pbprotocol::ProtocolError>(&error);
        return !receiverSessionBound_ && protocol != nullptr &&
            protocol->code == pbprotocol::ProtocolErrorCode::UnknownSession;
    }

    [[nodiscard]] bool ProcessControlRecord(const std::span<const std::byte> bytes,
        const pbprotocol::SessionTag bootstrapSessionTag, const std::int64_t timestamp100ns)
    {
        const auto parsedRecord = pbprotocol::ParseControlRecord(bytes);
        RequireResult(parsedRecord, "captured ControlRecord failed an independent parse");
        const pbprotocol::ControlRecordView& record = parsedRecord.Value();
        Require(record.sessionTag == bootstrapSessionTag,
            "captured ControlRecord SessionTag disagrees with the same-frame Bootstrap");
        auto admission = receiver_.ReceiveControlRecord(bytes);
        if (!admission)
        {
            if (record.recordType != pbprotocol::ControlRecordType::SessionDescriptor &&
                IsRetryableUnknownSession(admission.Error()))
            {
                return false;
            }
            throw RuntimeFailure("ReceiverIngress rejected ControlRecord: " +
                DescribeReceiverError(admission.Error()));
        }
        HandleControlAdmission(admission.Value(), record, timestamp100ns);
        return true;
    }

    [[nodiscard]] bool ProcessControlFragment(const std::span<const std::byte> bytes,
        const std::uint64_t captureObservation)
    {
        auto fragment = receiver_.ReceiveControlFragment(bytes, captureObservation);
        if (!fragment)
        {
            if (IsRetryableUnknownSession(fragment.Error()))
            {
                return false;
            }
            throw RuntimeFailure("ReceiverIngress rejected ControlFragment: " +
                DescribeReceiverError(fragment.Error()));
        }
        if (fragment.Value().admission)
        {
            throw RuntimeFailure(
                "completed fragmented ControlRecord is outside the current fixed-Control-window application inventory");
        }
        return true;
    }

    void HandleControlAdmission(pbreceiver::ReceiverControlAdmission& admission,
        const std::optional<pbprotocol::ControlRecordView> record, const std::int64_t timestamp100ns)
    {
        if (record)
        {
            if (record->recordType == pbprotocol::ControlRecordType::SessionDescriptor)
            {
                const auto parsed = pbprotocol::ParseSessionDescriptor(record->payload, policy_);
                RequireResult(parsed, "SessionDescriptor independent parse failed");
                Require(pbprotocol::DeriveSessionTag(parsed.Value().sessionId) == record->sessionTag,
                    "SessionDescriptor tag mismatch");
                Require(admission.outputReservationDecision &&
                    *admission.outputReservationDecision == pbprotocol::OutputReservationDecision::AutoAccept,
                    "current bounded output reservation was not AutoAccept");
                if (!session_)
                {
                    session_ = parsed.Value();
                    Require(session_->segmentCount == 1 &&
                        session_->originalFileSize >= minimumInstantFileBytes &&
                        session_->originalFileSize <= maximumInstantFileBytes,
                        "received Session is outside the current one-Segment 1 byte..8 MiB product boundary");
                    Require(progress_.BindDescriptor(session_->originalFileSize, ElapsedMilliseconds(started_)),
                        "Decoder progress descriptor binding failed");
                    pbstorage::OutputFileConfig storageConfig;
                    storageConfig.outputDirectory = outputDirectory_;
                    storageConfig.sessionTag = record->sessionTag;
                    storageConfig.fileBytes = session_->originalFileSize;
                    storageConfig.maximumFileBytes = maximumInstantFileBytes;
                    const auto storageStatus = pbstorage::OutputFile::Create(storageConfig, storage_);
                    Require(static_cast<bool>(storageStatus), "PBStorage output reservation failed: " +
                        DescribeStorageStatus(storageStatus));
                    const auto storageSnapshot = storage_->GetSnapshot();
                    snapshot_.Update([this, &storageSnapshot](DecoderSnapshot& value)
                    {
                        if (value.runGeneration != runGeneration_)
                        {
                            return;
                        }
                        value.state = DecoderState::ReceivingControl;
                        value.descriptorKnown = true;
                        value.originalFileBytes = session_->originalFileSize;
                        value.remainingRawBytes = session_->originalFileSize;
                        value.recoveryProgress = 0.0;
                        value.sessionIdHex = SessionIdHex(session_->sessionId);
                        value.sessionTag = pbprotocol::DeriveSessionTag(session_->sessionId).value;
                        value.segmentCount = session_->segmentCount;
                        value.outputPath = Utf8FromWide(storageSnapshot.finalPath);
                        value.statusMessage = "Session Descriptor accepted; receiving Control";
                    });
                }
                else
                {
                    Require(*session_ == parsed.Value(), "conflicting repeated SessionDescriptor");
                }
                receiverSessionBound_ = true;
            }
            else if (record->recordType == pbprotocol::ControlRecordType::SegmentDescriptor)
            {
                Require(session_.has_value(), "SegmentDescriptor arrived before authoritative Session binding");
                const auto parsed = pbprotocol::ParseSegmentDescriptor(record->payload, *session_, policy_);
                RequireResult(parsed, "SegmentDescriptor independent parse failed");
                Require(admission.controlAdmission.boundSegmentDescriptor &&
                    admission.controlAdmission.boundSegmentDescriptor->GetDescriptor() == parsed.Value(),
                    "Receiver binding differs from independently parsed SegmentDescriptor");
                if (!segment_)
                {
                    segment_ = parsed.Value();
                    snapshot_.Update([this](DecoderSnapshot& value)
                    {
                        if (value.runGeneration != runGeneration_)
                        {
                            return;
                        }
                        value.state = DecoderState::Receiving;
                        value.currentSegmentOrdinal = segment_->segmentOrdinal;
                        value.compressionCodec = segment_->compressionCodec;
                        value.outerFecMode = segment_->outerFecMode;
                        value.statusMessage = "Segment Descriptor accepted; receiving verified Transport blocks";
                    });
                }
                else
                {
                    Require(*segment_ == parsed.Value(), "conflicting repeated SegmentDescriptor");
                }
            }
            else if (record->recordType == pbprotocol::ControlRecordType::FinalManifest)
            {
                Require(session_.has_value(), "FinalManifest arrived before authoritative Session binding");
                const auto parsed = pbprotocol::ParseFinalManifest(record->payload, *session_, policy_);
                RequireResult(parsed, "FinalManifest independent parse failed");
                if (!manifest_)
                {
                    manifest_ = parsed.Value();
                }
                else
                {
                    Require(*manifest_ == parsed.Value(), "conflicting repeated FinalManifest");
                }
                const std::string wholeFileDigestHex = DigestHex(parsed.Value().wholeFileDigest.bytes);
                snapshot_.Update([this, &wholeFileDigestHex](DecoderSnapshot& value)
                {
                    if (value.runGeneration == runGeneration_)
                    {
                        value.wholeFileDigestHex = wholeFileDigestHex;
                    }
                });
            }
        }
        if (admission.completedSegment)
        {
            StoreCompleted(std::move(*admission.completedSegment), timestamp100ns);
        }
        TryPublish(timestamp100ns);
    }

    [[nodiscard]] ReceiverProcessResult ProcessTransport(
        const pbdemodd3d11::CaptureDemodulatorResult& result, const bool countEvaluation)
    {
        Require(result.demodulation.acceptedTransportBlockCount <=
            result.demodulation.acceptedTransportBlocks.size(),
            "CaptureDemodulator accepted Transport block count is out of bounds");
        Require(result.admittedTransportBlockCount <= result.admittedTransportBlockIndices.size() &&
            result.admittedTransportBlockCount <= result.demodulation.acceptedTransportBlockCount,
            "CaptureDemodulator temporal Transport admission count is out of bounds");
        acceptedTransportBlocks_ = pbprotocol::SaturatingAddUnsigned(acceptedTransportBlocks_,
            static_cast<std::uint64_t>(result.admittedTransportBlockCount));
        const pbdesktoplevels::FrameEvaluation& evaluation = result.demodulation.evaluation;
        if (countEvaluation)
        {
            identityFailures_ = pbprotocol::SaturatingAddUnsigned(identityFailures_,
                static_cast<std::uint64_t>(evaluation.identityFailures));
            falseAcceptedCodewords_ = pbprotocol::SaturatingAddUnsigned(falseAcceptedCodewords_,
                static_cast<std::uint64_t>(evaluation.falseAcceptedCodewords));
        }
        ReceiverProcessResult processing;
        std::array<bool, pbdesktoplevels::kMaximumCodewords> processedIndices{};
        for (std::uint32_t admissionIndex = 0; admissionIndex < result.admittedTransportBlockCount; admissionIndex++)
        {
            const std::uint32_t index = result.admittedTransportBlockIndices[admissionIndex];
            Require(index < result.demodulation.acceptedTransportBlockCount && !processedIndices[index],
                "CaptureDemodulator temporal Transport admission index is invalid or repeated");
            processedIndices[index] = true;
            const auto& accepted = result.demodulation.acceptedTransportBlocks[index];
            Require(accepted.byteCount >= pbprotocol::kTransportMinimumBlockBytes &&
                accepted.byteCount <= accepted.bytes.size(), "accepted Transport byte count is invalid");
            const auto parsed = pbprotocol::ParseTransportBlock(
                std::span(accepted.bytes).first(accepted.byteCount));
            RequireResult(parsed, "accepted Transport failed an independent parse");
            const auto& transport = parsed.Value();
            if (session_ && transport.header.sessionTag != pbprotocol::DeriveSessionTag(session_->sessionId))
            {
                continue;
            }
            Require(transport.header.payloadBytes == transport.payload.size() &&
                transport.payload.size() <= paddedPayload_.size(), "Transport payload is out of bounds");
            std::fill(paddedPayload_.begin(), paddedPayload_.end(), std::byte{0});
            std::copy(transport.payload.begin(), transport.payload.end(), paddedPayload_.begin());
            const pbreceiver::ReceivedTransportBlock block{transport.header.sessionTag,
                transport.header.segmentOrdinal, transport.header.outerBlockId,
                transport.header.payloadBytes, paddedPayload_};
            auto admission = receiver_.ReceiveDataBlock(block, result.metadata.captureObservation);
            if (!admission)
            {
                if (IsOuterConflictError(admission.Error()))
                {
                    pbprotocol::SaturatingIncrementUnsigned(outerConflictRejections_);
                }
                if (IsOuterResourceError(admission.Error()))
                {
                    pbprotocol::SaturatingIncrementUnsigned(outerResourceRejections_);
                }
                UpdateOuterAdmissionSnapshot();
                const auto* protocol = std::get_if<pbprotocol::ProtocolError>(&admission.Error());
                if (protocol != nullptr &&
                    (protocol->code == pbprotocol::ProtocolErrorCode::ResourceLimitExceeded ||
                     protocol->code == pbprotocol::ProtocolErrorCode::UnknownSession))
                {
                    continue;
                }
                throw RuntimeFailure("ReceiverIngress rejected Transport block: " +
                    DescribeReceiverError(admission.Error()));
            }
            const pbreceiver::ReceiverDataAdmission& acceptedAdmission = admission.Value();
            processing.carrierAccepted = true;
            switch (acceptedAdmission.outerSymbolAdmission)
            {
            case pbreceiver::ReceiverOuterSymbolAdmission::Unique:
                pbprotocol::SaturatingIncrementUnsigned(outerUniqueSymbols_);
                processing.uniqueAdmission = true;
                break;
            case pbreceiver::ReceiverOuterSymbolAdmission::IdenticalDuplicate:
                pbprotocol::SaturatingIncrementUnsigned(outerIdenticalDuplicateSymbols_);
                break;
            case pbreceiver::ReceiverOuterSymbolAdmission::RecoveryAlreadyReady:
                pbprotocol::SaturatingIncrementUnsigned(outerRecoveryAlreadyReadySymbols_);
                break;
            case pbreceiver::ReceiverOuterSymbolAdmission::AlreadyCompleted:
                pbprotocol::SaturatingIncrementUnsigned(outerAlreadyCompletedSymbols_);
                break;
            case pbreceiver::ReceiverOuterSymbolAdmission::NotApplicable:
                throw RuntimeFailure("ReceiverIngress returned a successful Transport admission without Outer identity classification");
            }
            if (acceptedAdmission.disposition == pbreceiver::ReceiverDataDisposition::EncodedSegmentReady)
            {
                pbprotocol::SaturatingIncrementUnsigned(outerRecoveryReadyEvents_);
            }
            if (admission.Value().completedSegment)
            {
                StoreCompleted(std::move(*admission.Value().completedSegment),
                    result.metadata.timestamp.monotonic100ns);
            }
        }
        snapshot_.Update([this, &processing](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            if (processing.uniqueAdmission && value.state == DecoderState::Receiving)
            {
                value.state = DecoderState::Recovering;
                value.statusMessage = "Outer FEC is converging; progress advances only after Segment verification";
            }
            value.acceptedTransportBlocks = acceptedTransportBlocks_;
            value.temporallyAdmittedTransportBlocks = acceptedTransportBlocks_;
            value.identityFailures = identityFailures_;
            value.falseAcceptedCodewords = falseAcceptedCodewords_;
            ApplyOuterAdmissionSnapshot(value);
        });
        return processing;
    }

    void ApplyOuterAdmissionSnapshot(DecoderSnapshot& value) const noexcept
    {
        value.outerUniqueSymbols = outerUniqueSymbols_;
        value.outerIdenticalDuplicateSymbols = outerIdenticalDuplicateSymbols_;
        value.outerRecoveryAlreadyReadySymbols = outerRecoveryAlreadyReadySymbols_;
        value.outerAlreadyCompletedSymbols = outerAlreadyCompletedSymbols_;
        value.outerRecoveryReadyEvents = outerRecoveryReadyEvents_;
        value.outerResourceRejections = outerResourceRejections_;
        value.outerConflictRejections = outerConflictRejections_;
    }

    void UpdateOuterAdmissionSnapshot()
    {
        snapshot_.Update([this](DecoderSnapshot& value)
        {
            if (value.runGeneration == runGeneration_)
            {
                ApplyOuterAdmissionSnapshot(value);
            }
        });
    }

    void StoreCompleted(pbreceiver::ReceiverCompletedSegment&& completed, const std::int64_t timestamp100ns)
    {
        Require(!stored_, "Receiver emitted a duplicate completed Segment after authoritative storage");
        const std::uint64_t encodedBytes = completed.encodedBytes.size();
        snapshot_.Update([this](DecoderSnapshot& value)
        {
            if (value.runGeneration == runGeneration_)
            {
                value.state = DecoderState::Verifying;
                value.statusMessage = "Verifying EncodedDigest, bounded decompression, and RawDigest";
            }
        });
        auto verified = receiver_.VerifyRecoveredSegment(std::move(completed));
        RequireResult(verified, "encoded digest, decompression, or raw digest verification failed");
        pbreceiver::ReceiverVerifiedSegment verifiedSegment = std::move(verified).Value();
        const pbprotocol::SegmentDescriptor& descriptor =
            verifiedSegment.GetBoundSegmentDescriptor().GetDescriptor();
        Require(storage_ && session_ && descriptor.rawOffset == 0 &&
            descriptor.rawSize == verifiedSegment.GetRawBytes().size() &&
            descriptor.rawSize == session_->originalFileSize && encodedBytes == descriptor.encodedSize,
            "verified Segment does not match the bounded output reservation");
        const auto writeStatus = storage_->Write(descriptor.rawOffset, verifiedSegment.GetRawBytes());
        Require(static_cast<bool>(writeStatus), "PBStorage Segment write failed: " +
            DescribeStorageStatus(writeStatus));
        const auto committed = receiver_.CommitStoredSegment(std::move(verifiedSegment));
        RequireResult(committed, "Receiver stored Segment commit failed");
        Require(committed.Value() == pbreceiver::ReceiverSegmentCommitDisposition::Committed,
            "first stored Segment commit was not Committed");
        stored_ = true;
        storedEncodedBytes_ = encodedBytes;
        Require(progress_.ObserveVerifiedRawBytes(descriptor.rawSize, ElapsedMilliseconds(started_)),
            "verified raw-byte progress update failed");
        ApplyProgress();
        TryPublish(timestamp100ns);
    }

    void TryPublish(const std::int64_t timestamp100ns)
    {
        if (published_ || !stored_ || !segment_ || !manifest_ || !storage_)
        {
            return;
        }
        const auto finalized = receiver_.PrepareFinalization(segment_->sessionTag);
        RequireResult(finalized, "Receiver finalization was not authoritative after stored commit");
        Require(finalized.Value() == *manifest_, "Receiver returned a different FinalManifest");
        snapshot_.Update([this](DecoderSnapshot& value)
        {
            if (value.runGeneration == runGeneration_)
            {
                value.state = DecoderState::Publishing;
                value.statusMessage = "WholeFileDigest verification and same-directory final publish";
            }
        });
        const auto storageSnapshot = storage_->GetSnapshot();
        std::string outputPath = Utf8FromWide(storageSnapshot.finalPath);
        std::string wholeFileDigestHex = DigestHex(manifest_->wholeFileDigest.bytes);
        const auto publishStatus = storage_->Publish(manifest_->wholeFileDigest);
        Require(static_cast<bool>(publishStatus), "WholeFileDigest/final publish failed: " +
            DescribeStorageStatus(publishStatus));
        completion_.outputPath = std::move(outputPath);
        completion_.wholeFileDigestHex = std::move(wholeFileDigestHex);
        completion_.recoveryRuntimeMilliseconds = ElapsedMilliseconds(started_);
        completion_.published = true;
        published_ = true;
        Require(storedEncodedBytes_ != 0, "published Segment has no verified encoded-byte accounting");
        RequireTelemetry(telemetry_.RecordVerifiedEncodedBytes(storedEncodedBytes_, timestamp100ns),
            "PBTelemetry RecordVerifiedEncodedBytes");
        UpdateTelemetrySnapshot();
        snapshot_.Update([this](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            value.state = deferCompletedState_ ? DecoderState::Publishing : DecoderState::Completed;
            value.runEndedUnixMilliseconds = deferCompletedState_ ? 0 : GetUnixTimeMilliseconds();
            value.wholeFileDigestVerified = true;
            value.finalPublishSucceeded = true;
            value.wholeFileDigestHex = completion_.wholeFileDigestHex;
            value.outputPath = completion_.outputPath;
            value.recoveryRuntimeMilliseconds = completion_.recoveryRuntimeMilliseconds;
            value.statusMessage = deferCompletedState_ ?
                "文件已发布；正在消费 Replay 尾部并验证完整证据" :
                "文件接收完成：WholeFileDigest PASS 且 final publish 成功";
        });
    }

    void ApplyProgress()
    {
        const ProgressSnapshot progress = progress_.GetSnapshot();
        snapshot_.Update([this, &progress](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            value.descriptorKnown = progress.descriptorKnown;
            value.originalFileBytes = progress.totalRawBytes;
            value.verifiedRawBytes = progress.verifiedRawBytes;
            value.remainingRawBytes = progress.remainingRawBytes;
            value.recoveryProgress = progress.progress;
            value.instantVerifiedRawGoodputBytesPerSecond = progress.instantBytesPerSecond;
            value.smoothedVerifiedRawGoodputBytesPerSecond = progress.smoothedBytesPerSecond;
            value.averageVerifiedRawGoodputBytesPerSecond = progress.averageBytesPerSecond;
            value.etaMilliseconds = progress.etaMilliseconds;
        });
    }

    void UpdateVisualSnapshot()
    {
        const VisualIdentitySnapshot visual = visualRate_.GetSnapshot();
        const VisualIdentitySnapshot endToEnd = endToEndRate_.GetSnapshot();
        const RemoteDuplicateRefinementSnapshot refinement = remoteRefinement_.GetSnapshot();
        snapshot_.Update([this, &visual, &endToEnd, &refinement](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            value.uniqueVisualFps = visual.framesPerSecond;
            value.admittedFrameSequenceFps = visual.framesPerSecond;
            value.duplicateFrameSequences = visual.duplicateFrames;
            value.reorderedFrameSequences = visual.reorderedFrames;
            value.frameSequenceGapEvents = visual.gapEvents;
            value.skippedFrameSequences = visual.skippedSequences;
            value.endToEndUniqueFrameSequences = endToEnd.uniqueFrames;
            value.endToEndUniqueVisualFps = endToEnd.framesPerSecond;
            value.remoteDuplicateRefinementAttempts = refinement.attempts;
            value.remoteDuplicateRefinementRecoveries = refinement.recoveries;
        });
    }

    void ApplyChannelStalls()
    {
        const ChannelStallSnapshot stalls = channelStalls_.GetSnapshot();
        snapshot_.Update([this, &stalls](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            value.captureStallCount = stalls.capture.count;
            value.captureStallTotalMilliseconds = pbprotocol::SaturatingAddUnsigned(
                stalls.capture.totalMilliseconds, stalls.capture.currentMilliseconds);
            value.captureStallMaximumMilliseconds = std::max(stalls.capture.maximumMilliseconds,
                stalls.capture.currentMilliseconds);
            value.captureStallActive = stalls.capture.active;
            value.visualStallCount = stalls.visual.count;
            value.visualStallTotalMilliseconds = pbprotocol::SaturatingAddUnsigned(
                stalls.visual.totalMilliseconds, stalls.visual.currentMilliseconds);
            value.visualStallMaximumMilliseconds = std::max(stalls.visual.maximumMilliseconds,
                stalls.visual.currentMilliseconds);
            value.visualStallActive = stalls.visual.active;
        });
    }

    void UpdateTelemetrySnapshot()
    {
        const pbtelemetry::TelemetrySnapshot telemetry = telemetry_.GetSnapshot();
        snapshot_.Update([this, &telemetry](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            value.telemetryCapturedFrames = telemetry.capturedFrames;
            value.telemetryDroppedFrames = telemetry.droppedFrames;
            value.fingerprintedFrames = telemetry.fingerprintedFrames;
            value.captureFps = telemetry.captureFps;
            value.roiPixelDigestUniqueVisualFps = telemetry.uniqueVisualFps;
            value.telemetryBootstrapAttempts = telemetry.bootstrapAttempts;
            value.telemetryBootstrapSuccesses = telemetry.bootstrapSuccesses;
            value.bootstrapSuccessRate = telemetry.bootstrapSuccessRate;
            value.evaluatedDataFrames = telemetry.fecEvaluatedFrames;
            value.evaluatedCodewords = telemetry.fecCodewords;
            value.postFecFailedFrames = telemetry.postFecFailedFrames;
            value.comparedCodedBits = telemetry.comparedCodedBits;
            value.erroneousCodedBits = telemetry.erroneousCodedBits;
            value.fecFailures = telemetry.fecFailures;
            value.crcFailures = telemetry.crcFailures;
            value.identityFailures = telemetry.identityFailures;
            value.preFecBerEstimate = telemetry.preFecBerEstimate;
            value.fecFrameErrorRate = telemetry.fecFrameErrorRate;
            value.fecCodewordFailureRate = telemetry.fecCodewordFailureRate;
            value.fecAcceptedTransportBlocks = telemetry.acceptedTransportCodewords;
            value.fecAcceptedTransportBlockRate = telemetry.acceptedTransportCodewordRate;
            value.remoteMetricFrames = telemetry.remoteMetricFrames;
            value.remoteMetricSamples = telemetry.remoteMetricSamples;
            value.remoteZeroMagnitudeMetrics = telemetry.remoteZeroMagnitudeMetrics;
            value.remoteZeroMagnitudeMetricRate = telemetry.remoteZeroMagnitudeMetricRate;
            value.remoteMinimumAbsoluteMetric = telemetry.remoteMinimumAbsoluteMetric;
            value.remoteMeanAbsoluteMetric = telemetry.remoteMeanAbsoluteMetric;
            value.remoteSymbolSamples = telemetry.remoteSymbolSamples;
            value.remoteUnreliableSymbols = telemetry.remoteUnreliableSymbols;
            value.remoteUnreliableSymbolRate = telemetry.remoteUnreliableSymbolRate;
            value.remoteVerifiedMetricFrames = telemetry.remoteTransportVerifiedMetricFrames;
            value.remoteRejectedMetricFrames = telemetry.remoteTransportRejectedMetricFrames;
            value.remoteVerifiedMeanAbsoluteMetric = telemetry.remoteVerifiedMeanAbsoluteMetric;
            value.remoteRejectedMeanAbsoluteMetric = telemetry.remoteRejectedMeanAbsoluteMetric;
            value.remoteRejectedZeroMagnitudeMetricRate = telemetry.remoteRejectedZeroMagnitudeMetricRate;
            value.remoteFreshnessRegions = telemetry.remoteFreshnessRegions;
            value.remoteFreshRegions = telemetry.remoteFreshRegions;
            value.remoteStaleRegions = telemetry.remoteStaleRegions;
            value.remoteStaleRegionRate = telemetry.remoteStaleRegionRate;
            value.remoteFramesWithStaleRegions = telemetry.remoteFramesWithStaleRegions;
            value.remoteFreshnessTagMismatches = telemetry.remoteFreshnessTagMismatches;
            value.remoteFreshnessTagErasures = telemetry.remoteFreshnessTagErasures;
            value.remoteFreshnessErasedDataMetrics = telemetry.remoteFreshnessErasedDataMetrics;
            value.remoteFreshnessErasedDataMetricRate = telemetry.remoteFreshnessErasedDataMetricRate;
            value.verifiedEncodedBytes = telemetry.verifiedEncodedBytes;
            value.verifiedEncodedGoodputBitsPerSecond = telemetry.verifiedEncodedGoodputBitsPerSecond;
        });
    }

    pbreceiver::ReceiverIngress& receiver_;
    std::wstring outputDirectory_;
    pbprotocol::ReceiverResourcePolicy policy_;
    SnapshotStore<DecoderSnapshot>& snapshot_;
    AuthoritativeCompletion& completion_;
    std::uint64_t runGeneration_ = 0;
    std::chrono::steady_clock::time_point started_;
    VisualProfile visualProfile_ = VisualProfile::DirectLevels2x2;
    bool deferCompletedState_ = false;
    std::unique_ptr<pbstorage::OutputFile> storage_;
    std::optional<pbprotocol::SessionDescriptor> session_;
    std::optional<pbprotocol::SegmentDescriptor> segment_;
    std::optional<pbprotocol::FinalManifest> manifest_;
    std::array<std::byte, outerBlockBytes> paddedPayload_{};
    DecoderProgressTracker progress_;
    VisualIdentityTracker visualRate_;
    VisualIdentityTracker endToEndRate_;
    ChannelStallTracker channelStalls_;
    RemoteDuplicateRefinementGate remoteRefinement_;
    pbtelemetry::TelemetryAccumulator telemetry_;
    std::uint64_t storedEncodedBytes_ = 0;
    std::uint64_t pendingDroppedFrames_ = 0;
    std::uint64_t lastCaptureDroppedFrames_ = 0;
    std::uint64_t lastResultQueueDrops_ = 0;
    std::uint64_t lastStaleResultDrops_ = 0;
    std::uint64_t lastCaptureObservationsForStall_ = 0;
    std::uint64_t acceptedTransportBlocks_ = 0;
    std::uint64_t identityFailures_ = 0;
    std::uint64_t falseAcceptedCodewords_ = 0;
    std::uint64_t outerUniqueSymbols_ = 0;
    std::uint64_t outerIdenticalDuplicateSymbols_ = 0;
    std::uint64_t outerRecoveryAlreadyReadySymbols_ = 0;
    std::uint64_t outerAlreadyCompletedSymbols_ = 0;
    std::uint64_t outerRecoveryReadyEvents_ = 0;
    std::uint64_t outerResourceRejections_ = 0;
    std::uint64_t outerConflictRejections_ = 0;
    bool receiverSessionBound_ = false;
    bool stored_ = false;
    bool published_ = false;
};

[[nodiscard]] pbcapturenormalize::CaptureEnvironment MakeReplayCaptureEnvironment(
    const pbrealcapturereplay::ReplayV2Capture& replayCapture)
{
    pbcapturenormalize::CaptureEnvironment environment;
    environment.region.physicalRect = replayCapture.capture.physicalRoi;
    environment.region.monitorPhysicalRect = replayCapture.capture.physicalRoi;
    environment.region.dpiX = replayCapture.dpiX;
    environment.region.dpiY = replayCapture.dpiY;
    environment.region.rotation = replayCapture.capture.displayRotation;
    environment.contentSize = replayCapture.capture.sourceContentSize;
    environment.pixelFormat = replayCapture.capturedRoi.pixelFormat;
    environment.adapterLuid = replayCapture.capture.adapterLuid;
    environment.bitsPerColor = replayCapture.capture.bitsPerColor;
    environment.outputColorSpace = replayCapture.capture.outputColorSpace;
    environment.hdr = replayCapture.capture.hdr;
    environment.backendKind = replayCapture.capture.backend;
    environment.sourceSize = replayCapture.capture.sourceExtent;
    environment.sourceRotation = replayCapture.capture.sourceTransform;
    return environment;
}

void ValidateReplayProductionCapture(const pbrealcapturereplay::ReplayV2Capture& replayCapture,
    const std::uint64_t expectedVisualProfileId)
{
    const auto& metadata = replayCapture.capture;
    const auto& raster = replayCapture.capturedRoi;
    const std::int64_t physicalWidth = static_cast<std::int64_t>(metadata.physicalRoi.right) -
        metadata.physicalRoi.left;
    const std::int64_t physicalHeight = static_cast<std::int64_t>(metadata.physicalRoi.bottom) -
        metadata.physicalRoi.top;
    const bool nonzeroSourceId = std::ranges::any_of(metadata.domain.sourceId,
        [](const std::byte value) { return value != std::byte{0}; });
    const auto rasterBytes = pbprotocol::CheckedMultiplyUint64(raster.rowPitch, raster.height);
    const bool lowFps = expectedVisualProfileId == pbmodulation::kRemoteVisualLowFpsProfileId;
    const bool strictProfile = expectedVisualProfileId == pbmodulation::kDesktopLevels2ProfileId ||
        expectedVisualProfileId == pbmodulation::kShapeChromaProfileId ||
        expectedVisualProfileId == pbmodulation::kRemoteVisualProfileId;
    Require(strictProfile || lowFps, "Offline replay attempted an unknown production profile binding");
    Require(nonzeroSourceId && metadata.domain.captureEpoch != 0 && metadata.captureObservation != 0,
        "Replay capture identity is incomplete");
    Require(physicalWidth == metadata.roiSize.width && physicalHeight == metadata.roiSize.height &&
        metadata.roiSize.width == static_cast<std::int32_t>(raster.width) &&
        metadata.roiSize.height == static_cast<std::int32_t>(raster.height),
        "Replay capture ROI metadata and selected raster dimensions disagree");
    if (strictProfile)
    {
        Require(raster.width == phase1CanvasWidth && raster.height == phase1CanvasHeight &&
            replayCapture.scaleX == 1.0 && replayCapture.scaleY == 1.0,
            "Geometry incompatible with strict production profile: replay ROI is not exact 1920x1080 at 1:1");
    }
    else
    {
        Require(raster.width <= 3840 && raster.height <= 2160,
            "LF4 Replay selected ROI exceeds the production continuous-geometry reservation");
    }
    Require(replayCapture.dpiX != 0 && replayCapture.dpiY != 0 &&
        metadata.displayRotation == DXGI_MODE_ROTATION_IDENTITY &&
        metadata.sourceTransform == DXGI_MODE_ROTATION_IDENTITY,
        "Geometry incompatible with current production Replay rotation contract");
    Require(raster.pixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM &&
        metadata.pixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM &&
        raster.rowPitch >= raster.width * 4 && rasterBytes &&
        rasterBytes.Value() == raster.pixels.size(),
        "Replay captured ROI is not a bounded BGRA8 production raster");
    Require(!metadata.hdr && metadata.outputColorSpace == 0 &&
        metadata.signalEncoding == pbcapturenormalize::CaptureSignalEncoding::SdrRgb,
        "Replay capture is not the current strict SDR RGB profile");
    Require(NonzeroAdapterLuid(metadata.adapterLuid), "Replay capture adapter LUID is unavailable");
}

[[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateReplayTexture(
    ID3D11Device* const device, const pbrealcapturereplay::ReplayRasterView& raster)
{
    Require(device != nullptr && !raster.pixels.empty(), "Replay texture creation received invalid input");
    D3D11_TEXTURE2D_DESC description{};
    description.Width = raster.width;
    description.Height = raster.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = raster.pixelFormat;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = raster.pixels.data();
    initialData.SysMemPitch = raster.rowPitch;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    const HRESULT result = device->CreateTexture2D(&description, &initialData, &texture);
    Require(SUCCEEDED(result) && texture, "Replay D3D11 ROI texture creation failed, HRESULT=" +
        std::to_string(result));
    return texture;
}

[[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateReplayTexture(
    ID3D11Device* const device, const pbrealcapturereplay::ReplayRaster& raster)
{
    return CreateReplayTexture(device, pbrealcapturereplay::ReplayRasterView{raster.width, raster.height,
        raster.rowPitch, raster.pixelFormat, raster.pixels});
}

[[nodiscard]] pbdemodd3d11::CaptureDemodulatorResult RunReplayProductionDemod(
    const std::shared_ptr<pbdemodd3d11::CaptureDemodulator>& demodulator,
    ID3D11Device* const device, ID3D11DeviceContext* const context,
    const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata,
    const Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture)
{
    Require(demodulator != nullptr && device != nullptr && context != nullptr && texture != nullptr,
        "Offline Replay demod received an invalid production binding");
    const auto retirement = ReplayGpuRetirement::Create(device, context);
    retirement->BindPending(demodulator, metadata, texture);
    const pbcapturenormalize::ScreenCaptureFrame frame{metadata, texture.Get()};
    const auto submitStatus = demodulator->Submit(frame, context);
    Require(static_cast<bool>(submitStatus), "Offline Replay demod submit failed: " +
        DescribeCaptureStatus(submitStatus));

    for (std::uint32_t stage = 0; stage < 2; stage++)
    {
        retirement->Mark();
        const ReplayGpuCompletion gpuCompletion = retirement->Wait(3000);
        if (gpuCompletion == ReplayGpuCompletion::Deferred)
        {
            demodulator->DomainInvalidated(metadata.domain);
            retirement->Defer();
            throw RuntimeFailure("Offline Replay GPU completion timed out; bounded resources were handed to OS-notified deferred retirement");
        }
        if (gpuCompletion == ReplayGpuCompletion::DeviceRemoved)
        {
            demodulator->DomainInvalidated(metadata.domain);
            const auto cancelled = demodulator->CompleteStage(metadata, nullptr, nullptr, true);
            Require(static_cast<bool>(cancelled.status), "Offline Replay device-loss retirement failed: " +
                DescribeCaptureStatus(cancelled.status));
            throw RuntimeFailure("Offline Replay D3D11 device was removed during demodulation");
        }
        const auto completion = demodulator->CompleteStage(metadata, texture.Get(), context, false);
        Require(static_cast<bool>(completion.status), "Offline Replay demod completion failed: " +
            DescribeCaptureStatus(completion.status));
        if (!completion.gpuWorkSubmitted)
        {
            pbdemodd3d11::CaptureDemodulatorResult result;
            Require(demodulator->TakeResult(result),
                "Offline Replay production CaptureDemodulator emitted no result for a completed capture");
            pbdemodd3d11::CaptureDemodulatorResult unexpectedResult;
            Require(!demodulator->TakeResult(unexpectedResult),
                "Offline Replay production CaptureDemodulator emitted multiple results for one capture");
            return result;
        }
    }

    // A future demodulator must not silently grow an unbounded staged chain.
    // The unexpected third GPU stage is externally retired before cancellation.
    retirement->Mark();
    const ReplayGpuCompletion finalCompletion = retirement->Wait(3000);
    if (finalCompletion == ReplayGpuCompletion::Deferred)
    {
        demodulator->DomainInvalidated(metadata.domain);
        retirement->Defer();
        throw RuntimeFailure("Offline Replay exceeded the two-stage completion contract and deferred retirement timed out");
    }
    demodulator->DomainInvalidated(metadata.domain);
    const auto cancelled = demodulator->CompleteStage(metadata, nullptr, nullptr, true);
    Require(static_cast<bool>(cancelled.status), "Offline Replay over-stage cancellation failed: " +
        DescribeCaptureStatus(cancelled.status));
    throw RuntimeFailure("Offline Replay production demodulator exceeded the two-stage completion contract");
}

void ApplyOfflineDemodSnapshot(const pbdemodd3d11::CaptureDemodulatorSnapshot& demod,
    DecoderSnapshot& snapshot)
{
    snapshot.bootstrapAcceptedFrames = demod.bootstrapAcceptedFrames;
    snapshot.bootstrapRejectedFrames = demod.bootstrapRejectedFrames;
    snapshot.bootstrapMismatchFrames = demod.bootstrapErasures[
        static_cast<std::size_t>(pbmodulation::LocalDesktopErasureReason::BootstrapMismatch)];
    snapshot.bootstrapControlFrameFailures = demod.controlFrameFailures;
    snapshot.demodPendingHighWater = demod.pendingHighWater;
    snapshot.resultQueueHighWater = demod.resultQueueHighWater;
    snapshot.staleResultDrops = demod.staleResultDrops;
    snapshot.demodGpuTimeTotal100ns = demod.demodulator.gpuTimeTotal100ns;
    snapshot.bootstrapCpuTimeTotal100ns = demod.bootstrapCpuTimeTotal100ns;
    snapshot.postGpuFecCpuTimeTotal100ns = demod.demodulationCpuTimeTotal100ns;
}

void RunOfflineReplayDataset(const DecoderConfig& config, pbrealcapturereplay::ReplayV2Reader& reader,
    const ProfileBinding& profile, pbreceiver::ReceiverIngress& receiver, ReceiverPipeline& pipeline,
    const std::shared_ptr<pbdemodd3d11::CaptureDemodulator>& demodulator,
    SnapshotStore<DecoderSnapshot>& snapshot, const std::uint64_t runGeneration,
    const std::chrono::steady_clock::time_point started, ProcessResourceSampler& resourceSampler,
    const std::atomic<bool>& stopRequested)
{
    std::optional<ReplayD3dDevice> replayDevice;
    std::optional<pbcapturenormalize::ScreenCaptureDomain> activeDomain;
    std::vector<pbrealcapturereplay::ReplayV2DemodObservationView> actualObservations;
    actualObservations.reserve(config.replayMaximumCaptureFrames);
    std::uint64_t receiverCaptureEpoch = 0;
    std::uint64_t captureFrames = 0;
    std::uint64_t demodResults = 0;
    std::uint64_t observationComparisons = 0;
    std::uint64_t observationMismatches = 0;
    bool stoppedEarly = false;
    for (;;)
    {
        if (stopRequested)
        {
            stoppedEarly = true;
            break;
        }
        pbrealcapturereplay::ReplayV2Record record;
        const auto readStatus = reader.ReadNext(record);
        if (!readStatus)
        {
            Require(readStatus.code == pbrealcapturereplay::ReplayError::EndOfFile,
                "Replay v2 record read failed: " + DescribeReplayStatus(readStatus));
            break;
        }
        resourceSampler.Sample(ElapsedMilliseconds(started));
        if (record.type == pbrealcapturereplay::ReplayV2RecordType::DemodObservation)
        {
            const auto actual = std::ranges::find_if(actualObservations,
                [&](const pbrealcapturereplay::ReplayV2DemodObservationView& value)
                {
                    return value.captureEpoch == record.demodObservation.captureEpoch &&
                        value.captureObservation == record.demodObservation.captureObservation;
                });
            observationComparisons++;
            if (actual == actualObservations.end() ||
                !EquivalentReplayDemodObservation(*actual, record.demodObservation))
            {
                observationMismatches++;
            }
            snapshot.Update([&](DecoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.replayOfflineObservationComparisons = observationComparisons;
                    value.replayOfflineObservationMismatches = observationMismatches;
                    ApplyProcessResourceSample(resourceSampler.GetSnapshot(), value);
                }
            });
            continue;
        }

        ValidateReplayProductionCapture(record.capture, profile.visualProfileId);
        captureFrames++;
        Require(captureFrames <= config.replayMaximumCaptureFrames,
            "Replay input exceeded the configured capture-frame bound");
        if (!replayDevice)
        {
            replayDevice.emplace(CreateReplayD3dDevice(record.capture.capture.adapterLuid));
        }
        Require(SameAdapterLuid(replayDevice->adapterLuid, record.capture.capture.adapterLuid),
            "Replay changes adapter LUID; cross-adapter fallback is forbidden");

        const auto& recordedDomain = record.capture.capture.domain;
        if (!activeDomain || *activeDomain != recordedDomain)
        {
            if (activeDomain)
            {
                Require(recordedDomain.captureEpoch > activeDomain->captureEpoch,
                    "Replay capture domain changed without a strictly newer CaptureEpoch");
                demodulator->DomainInvalidated(*activeDomain);
                const auto reset = receiver.ResetCaptureEpoch(outerBlockBytes);
                RequireResult(reset, "ReceiverIngress offline Replay CaptureEpoch reset failed");
                Require(reset.Value(), "ReceiverIngress offline Replay CaptureEpoch reset made no state transition");
                pipeline.CaptureEpochReset(ElapsedMilliseconds(started));
            }
            const auto environment = MakeReplayCaptureEnvironment(record.capture);
            const auto domainStatus = demodulator->DomainStarted(recordedDomain, environment,
                replayDevice->device.Get());
            Require(static_cast<bool>(domainStatus), "Offline Replay demod domain start failed: " +
                DescribeCaptureStatus(domainStatus));
            activeDomain = recordedDomain;
            receiverCaptureEpoch = recordedDomain.captureEpoch;
        }
        Require(recordedDomain.captureEpoch == receiverCaptureEpoch,
            "Offline Replay attempted to combine different CaptureEpoch values");

        pbcapturenormalize::ScreenCaptureFrameMetadata metadata = record.capture.capture;
        metadata.slotIndex = static_cast<std::uint32_t>((captureFrames - 1) % captureDemodulatorSlotCount);
        metadata.slotGeneration = captureFrames;
        metadata.timestamp.domain = pbcapturenormalize::CaptureTimestampDomain::DxgiQpcTicks;
        metadata.timestamp.rawValue = 0;
        metadata.timestamp.rawFrequency = 10000000;
        metadata.timestamp.monotonic100ns = CurrentQpc100ns();
        metadata.timestamp.arrivalQpc100ns = metadata.timestamp.monotonic100ns;
        auto texture = CreateReplayTexture(replayDevice->device.Get(), record.capture.capturedRoi);
        const pbdemodd3d11::CaptureDemodulatorResult result = RunReplayProductionDemod(demodulator,
            replayDevice->device.Get(), replayDevice->context.Get(), metadata, texture);
        const ReceiverProcessResult receiverResult = pipeline.Process(result);
        actualObservations.push_back(MakeReplayDemodObservation(result, profile.visualProfileId, receiverResult));
        demodResults++;
        const auto demodSnapshot = demodulator->GetSnapshot();
        snapshot.Update([&](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration)
            {
                return;
            }
            value.captureEpoch = recordedDomain.captureEpoch;
            value.captureArrivedFrames = captureFrames;
            value.captureDeliveredFrames = captureFrames;
            value.replayOfflineCaptureFrames = captureFrames;
            value.replayOfflineDemodResults = demodResults;
            value.roiLeft = metadata.physicalRoi.left;
            value.roiTop = metadata.physicalRoi.top;
            value.roiWidth = record.capture.capturedRoi.width;
            value.roiHeight = record.capture.capturedRoi.height;
            value.monitorLeft = metadata.physicalRoi.left;
            value.monitorTop = metadata.physicalRoi.top;
            value.monitorWidth = record.capture.capturedRoi.width;
            value.monitorHeight = record.capture.capturedRoi.height;
            value.dpiX = record.capture.dpiX;
            value.dpiY = record.capture.dpiY;
            value.rotation = static_cast<std::uint32_t>(metadata.displayRotation);
            value.recoveryRuntimeMilliseconds = ElapsedMilliseconds(started);
            value.remoteMetadata.selectedRoiPhysicalRect = MetadataPhysicalRect{metadata.physicalRoi.left,
                metadata.physicalRoi.top, metadata.physicalRoi.right, metadata.physicalRoi.bottom};
            value.remoteMetadata.estimatedScaleX = record.capture.scaleX;
            value.remoteMetadata.estimatedScaleY = record.capture.scaleY;
            value.remoteMetadata.geometryStatus = profile.profile == VisualProfile::RemoteVisualLowFps ?
                "ProductionLF4ContinuousGeometry (sealed Replay v2 ROI)" :
                "CompatibleStrict1:1 (sealed Replay v2 ROI)";
            value.remoteMetadata.geometryProvenance = MetadataProvenance::PixelBridgeObserved;
            ApplyOfflineDemodSnapshot(demodSnapshot, value);
            ApplyProcessResourceSample(resourceSampler.GetSnapshot(), value);
        });
    }

    if (activeDomain)
    {
        demodulator->DomainInvalidated(*activeDomain);
    }
    const auto demodSnapshot = demodulator->GetSnapshot();
    Require(demodSnapshot.pendingFrames == 0 && demodSnapshot.queuedResults == 0 &&
        demodSnapshot.demodulator.shutdown,
        "Offline Replay demod shutdown did not retire all bounded resources");
    const auto readerSnapshot = reader.GetSnapshot();
    Require(stoppedEarly || readerSnapshot.complete,
        "Offline Replay reader did not reach its validated footer record count");
    pipeline.EndCaptureTelemetry(ElapsedMilliseconds(started));
    snapshot.Update([&](DecoderSnapshot& value)
    {
        if (value.runGeneration != runGeneration)
        {
            return;
        }
        value.replayFinalized = readerSnapshot.complete;
        value.replayFileBytes = readerSnapshot.fileBytes;
        value.replayOfflineCaptureFrames = captureFrames;
        value.replayOfflineDemodResults = demodResults;
        value.replayOfflineObservationComparisons = observationComparisons;
        value.replayOfflineObservationMismatches = observationMismatches;
        ApplyOfflineDemodSnapshot(demodSnapshot, value);
        ApplyProcessResourceSample(resourceSampler.GetSnapshot(), value);
        if (pipeline.IsCompleted() && !stoppedEarly && readerSnapshot.complete)
        {
            value.state = DecoderState::Completed;
            value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
            value.statusMessage = "Offline Replay 完整消费并复现 WholeFileDigest 与 final publish";
        }
        else if (pipeline.IsCompleted())
        {
            value.state = DecoderState::Stopped;
            value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
            value.statusMessage = "文件已发布，但 Offline Replay 在完整证据验证前被停止";
        }
        else
        {
            value.state = DecoderState::Stopped;
            value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
            value.recoveryRuntimeMilliseconds = ElapsedMilliseconds(started);
            value.statusMessage = stoppedEarly ?
                "Offline Replay stopped by user; no final file was published" :
                "Offline Replay exhausted; no final file was published";
        }
    });
}

} // namespace

RuntimeStatus EncoderRuntimeTestAccess::ProbeRemoteVisualLowFpsCarousel(const std::span<const std::byte> rawBytes,
    const std::uint32_t controlRepetitions, const std::uint32_t completedCyclesBeforeMarker,
    EncoderCarouselProbeSnapshot& output) noexcept
{
    if (rawBytes.empty() || rawBytes.size() > maximumInstantFileBytes ||
        controlRepetitions < minimumControlRepetitions || controlRepetitions > maximumControlRepetitions ||
        completedCyclesBeforeMarker == 0 || completedCyclesBeforeMarker > 4)
    {
        return RuntimeStatus::Failure("RemoteVisual LF4 carousel probe input is outside its bounded contract");
    }
    try
    {
        const TransferDescription description = DescribeSource(rawBytes, false, 3);
        const ProfileBinding profile = GetProfileBinding(VisualProfile::RemoteVisualLowFps);
        SenderFrameBuilder builder(profile, description, controlRepetitions);
        const CarouselSnapshot initial = builder.GetCarouselSnapshot();
        const auto markerFrameCount = pbprotocol::CheckedMultiplyUint64(initial.cycleFrameCount,
            completedCyclesBeforeMarker);
        RequireResult(markerFrameCount, "RemoteVisual LF4 carousel probe frame count overflow");
        const auto totalFrameCount = pbprotocol::CheckedAddUint64(markerFrameCount.Value(), 1);
        RequireResult(totalFrameCount, "RemoteVisual LF4 carousel probe continuation overflow");
        auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
        RequireResult(channelResult, "RemoteVisual LF4 carousel probe channel creation failed");
        auto channel = std::move(channelResult).Value();
        EncoderCarouselProbeSnapshot result;
        result.cycleFrameCount = initial.cycleFrameCount;
        result.visualProfileId = profile.visualProfileId;
        result.layoutVersion = profile.layoutVersion;
        result.codedDataBytes = profile.dataBytes;
        result.codewords = profile.codewords;
        for (std::uint64_t frameSequence = 0; frameSequence < totalFrameCount.Value(); frameSequence++)
        {
            const FrameKind kind = builder.GetCurrentKind();
            const auto& pixels = builder.Build(frameSequence);
            const pbmodulation::LumaView view{pixels, phase1CanvasWidth, phase1CanvasHeight,
                static_cast<std::size_t>(phase1CanvasWidth) * 4, pbmodulation::LumaPixelFormat::Bgra8};
            const auto observation = channel.DecodeRemoteVisualLowFps(view, {},
                pbdesktoplevels::EvaluationMode::Transport);
            Require(observation.modulation.IsAccepted(), "production LF4 sender raster failed exact CPU demodulation");
            if (kind == FrameKind::Data)
            {
                Require(observation.evaluation.IsVerified() &&
                    observation.evaluation.acceptedTransportBlocks == profile.codewords &&
                    observation.evaluation.acceptedRemoteControlBlocks == 0,
                    "production LF4 sender data raster failed Transport verification");
                result.dataFrames++;
                result.acceptedTransportBlocks += observation.evaluation.acceptedTransportBlocks;
            }
            else
            {
                const std::vector<std::byte>& expected = kind == FrameKind::SessionControl ?
                    description.sessionControl : kind == FrameKind::ManifestControl ?
                    description.manifestControl : description.segmentControl;
                const auto acceptedControls = channel.GetAcceptedRemoteControlBlocks();
                Require(observation.evaluation.acceptedRemoteControlBlocks == profile.codewords &&
                    acceptedControls.size() == profile.codewords && observation.evaluation.fecFailures == 0 &&
                    observation.evaluation.crcFailures == 0 && observation.evaluation.identityFailures == 0,
                    "production LF4 sender Control raster did not preserve all four Robust copies");
                for (const auto& accepted : acceptedControls)
                {
                    Require(accepted.byteCount == expected.size() &&
                        std::ranges::equal(expected, std::span(accepted.bytes).first(accepted.byteCount)),
                        "production LF4 sender Control copy differs from canonical PB-Control-1 bytes");
                }
                result.controlFrames++;
                result.acceptedRemoteControlCopies += acceptedControls.size();
            }
            builder.Advance();
            result.framesBuilt++;
            if (frameSequence >= markerFrameCount.Value())
            {
                result.framesBuiltAfterExternalCompletionMarker++;
            }
        }
        result.completedCarouselCycles = builder.GetCarouselSnapshot().cycleCount;
        output = result;
        return {};
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(exception.what());
    }
    catch (...)
    {
        return RuntimeStatus::Failure("RemoteVisual LF4 carousel probe failed with an unknown error");
    }
}

RuntimeStatus EncoderRuntimeTestAccess::ProbeRemoteVisualFullscreenComposition(const std::span<const std::byte> source,
    const std::uint32_t destinationWidth, const std::uint32_t destinationHeight,
    std::vector<std::byte>& output) noexcept
{
    constexpr std::size_t expectedSourceBytes =
        static_cast<std::size_t>(phase1CanvasWidth) * phase1CanvasHeight * 4U;
    if (source.size() != expectedSourceBytes || destinationWidth < phase1CanvasWidth ||
        destinationWidth > maximumRemoteVisualLowFpsRoiWidth || destinationHeight < phase1CanvasHeight ||
        destinationHeight > maximumRemoteVisualLowFpsRoiHeight)
    {
        return RuntimeStatus::Failure("RemoteVisual fullscreen composition probe input is outside its bounded contract");
    }
    try
    {
        const auto pixelCount = pbprotocol::CheckedMultiplyUnsigned(
            static_cast<std::size_t>(destinationWidth), static_cast<std::size_t>(destinationHeight));
        RequireResult(pixelCount, "RemoteVisual fullscreen composition probe pixel count overflow");
        const auto byteCount = pbprotocol::CheckedMultiplyUnsigned(pixelCount.Value(), std::size_t{4});
        RequireResult(byteCount, "RemoteVisual fullscreen composition probe byte count overflow");
        std::vector<std::byte> result(byteCount.Value());
        ComposeRemoteVisualFullscreenBgra(source, destinationWidth, destinationHeight, result);
        output = std::move(result);
        return {};
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(exception.what());
    }
    catch (...)
    {
        return RuntimeStatus::Failure("RemoteVisual fullscreen composition probe failed with an unknown error");
    }
}

RuntimeStatus DecoderRuntimeTestAccess::ProbeRemoteVisualLowFpsReceiver(const std::span<const std::byte> rawBytes,
    const std::wstring& outputDirectory, const std::uint32_t suppressedDuplicateResults,
    DecoderAdmissionProbeSnapshot& output) noexcept
{
    if (rawBytes.empty() || rawBytes.size() > 64ULL * 1024ULL || outputDirectory.empty() ||
        suppressedDuplicateResults == 0 || suppressedDuplicateResults > 256)
    {
        return RuntimeStatus::Failure("RemoteVisual LF4 Receiver probe input is outside its bounded contract");
    }
    try
    {
        std::error_code directoryError;
        Require(std::filesystem::is_directory(outputDirectory, directoryError) && !directoryError,
            "RemoteVisual LF4 Receiver probe output directory is unavailable");
        const TransferDescription description = DescribeSource(rawBytes, false, 3);
        const ProfileBinding profile = GetProfileBinding(VisualProfile::RemoteVisualLowFps);
        SenderFrameBuilder builder(profile, description, 1);
        auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
        RequireResult(channelResult, "RemoteVisual LF4 Receiver probe channel creation failed");
        auto channel = std::move(channelResult).Value();
        const pbprotocol::ReceiverResourcePolicy policy = pbprotocol::GetDefaultReceiverResourcePolicy();
        auto receiverResult = pbreceiver::ReceiverIngress::Create(policy, outerBlockBytes);
        RequireResult(receiverResult, "RemoteVisual LF4 Receiver probe ingress creation failed");
        pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
        constexpr std::uint64_t runGeneration = 1;
        SnapshotStore<DecoderSnapshot> snapshot;
        snapshot.Update([&](DecoderSnapshot& value)
        {
            value.state = DecoderState::WaitingForBootstrap;
            value.runGeneration = runGeneration;
            value.visualProfile = VisualProfile::RemoteVisualLowFps;
            value.visualProfileId = profile.visualProfileId;
            value.visualLayoutVersion = profile.layoutVersion;
            value.codedDataBytesPerFrame = profile.dataBytes;
            value.codewordsPerFrame = profile.codewords;
        });
        AuthoritativeCompletion completion;
        const auto started = std::chrono::steady_clock::now();
        ReceiverPipeline pipeline(receiver, outputDirectory, policy, snapshot, completion, runGeneration,
            started, VisualProfile::RemoteVisualLowFps);
        pbcapturenormalize::ScreenCaptureDomain domain;
        domain.sourceId[0] = std::byte{0x6F};
        domain.captureEpoch = 1;
        DecoderAdmissionProbeSnapshot probe;
        probe.outerFecMode = description.segment.outerFecMode;
        std::uint64_t captureObservation = 0;
        const auto Stamp = [&](pbdemodd3d11::CaptureDemodulatorResult& result)
        {
            captureObservation++;
            result.metadata.domain = domain;
            result.metadata.captureObservation = captureObservation;
            result.metadata.timestamp.rawFrequency = 10000000;
            result.metadata.timestamp.rawValue = static_cast<std::int64_t>(captureObservation * 2000000ULL);
            result.metadata.timestamp.monotonic100ns = result.metadata.timestamp.rawValue;
            result.metadata.timestamp.arrivalQpc100ns = result.metadata.timestamp.rawValue;
            result.demodulation.metadata = result.metadata;
        };
        const auto BuildResult = [&](const std::uint64_t frameSequence, const FrameKind kind)
        {
            const auto& pixels = builder.Build(frameSequence);
            const pbmodulation::LumaView view{pixels, phase1CanvasWidth, phase1CanvasHeight,
                static_cast<std::size_t>(phase1CanvasWidth) * 4, pbmodulation::LumaPixelFormat::Bgra8};
            const auto observation = channel.DecodeRemoteVisualLowFps(view, {},
                pbdesktoplevels::EvaluationMode::Transport);
            Require(observation.modulation.IsAccepted(),
                "production LF4 Receiver probe raster failed exact reference demodulation");
            pbdemodd3d11::CaptureDemodulatorResult result;
            result.bootstrap = observation.modulation.bootstrap;
            result.bootstrapRecord = observation.modulation.bootstrap.canonical44;
            result.geometryStatus = pbdemodd3d11::CaptureDemodulatorGeometryStatus::ExactCanvas;
            result.temporalDisposition = pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique;
            result.demodulation.visualProfileId = profile.visualProfileId;
            result.demodulation.evaluation = observation.evaluation;
            const auto acceptedTransport = channel.GetAcceptedTransportBlocks();
            const auto acceptedControl = channel.GetAcceptedRemoteControlBlocks();
            Require(acceptedTransport.size() <= result.demodulation.acceptedTransportBlocks.size() &&
                acceptedControl.size() <= result.demodulation.acceptedRemoteControlBlocks.size(),
                "production LF4 Receiver probe exceeded fixed accepted-block capacity");
            result.demodulation.acceptedTransportBlockCount = static_cast<std::uint32_t>(acceptedTransport.size());
            std::copy(acceptedTransport.begin(), acceptedTransport.end(),
                result.demodulation.acceptedTransportBlocks.begin());
            result.demodulation.acceptedRemoteControlBlockCount = static_cast<std::uint32_t>(acceptedControl.size());
            std::copy(acceptedControl.begin(), acceptedControl.end(),
                result.demodulation.acceptedRemoteControlBlocks.begin());
            if (kind == FrameKind::Data)
            {
                Require(acceptedTransport.size() == profile.codewords && acceptedControl.empty(),
                    "production LF4 Receiver probe data frame did not yield four Transport candidates");
                result.kind = pbdemodd3d11::CaptureDemodulatorResultKind::Transport;
                result.admittedTransportBlockCount = static_cast<std::uint32_t>(acceptedTransport.size());
                for (std::uint32_t index = 0; index < result.admittedTransportBlockCount; index++)
                {
                    result.admittedTransportBlockIndices[index] = index;
                }
            }
            else
            {
                Require(acceptedControl.size() == profile.codewords && acceptedTransport.empty(),
                    "production LF4 Receiver probe control frame did not yield four Robust copies");
                for (std::size_t index = 1; index < acceptedControl.size(); index++)
                {
                    Require(acceptedControl[index] == acceptedControl[0],
                        "production LF4 Receiver probe control copies conflict");
                }
                result.kind = pbdemodd3d11::CaptureDemodulatorResultKind::ControlRecord;
                result.admittedRemoteControlBlockCount = 1;
                result.admittedRemoteControlBlockIndices[0] = 0;
                result.controlByteCount = acceptedControl[0].byteCount;
                Require(result.controlByteCount <= result.controlBytes.size(),
                    "production LF4 Receiver probe control bytes exceed fixed result capacity");
                std::copy_n(acceptedControl[0].bytes.begin(), result.controlByteCount, result.controlBytes.begin());
            }
            Stamp(result);
            return result;
        };

        bool duplicateBurstInjected = false;
        bool duplicateRefinementInjected = false;
        constexpr std::uint64_t maximumFrames = 4096;
        for (std::uint64_t frameSequence = 0; frameSequence < maximumFrames && !pipeline.IsCompleted(); frameSequence++)
        {
            const FrameKind kind = builder.GetCurrentKind();
            auto result = BuildResult(frameSequence, kind);
            if (kind == FrameKind::Data && !duplicateRefinementInjected)
            {
                Require(result.demodulation.acceptedTransportBlockCount == profile.codewords,
                    "production LF4 Receiver probe cannot split an incomplete source observation");
                auto uniquePartial = result;
                uniquePartial.demodulation.acceptedTransportBlockCount = 2;
                uniquePartial.demodulation.evaluation.acceptedTransportBlocks = 2;
                uniquePartial.demodulation.evaluation.fecFailures = 2;
                uniquePartial.admittedTransportBlockCount = 2;
                uniquePartial.admittedTransportBlockIndices[0] = 0;
                uniquePartial.admittedTransportBlockIndices[1] = 1;
                static_cast<void>(pipeline.Process(uniquePartial));
                probe.processedResults++;
                probe.rawAcceptedTransportBlocks += uniquePartial.demodulation.acceptedTransportBlockCount;
                probe.temporallyAdmittedTransportBlocks += uniquePartial.admittedTransportBlockCount;

                result.temporalDisposition = pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateRefinement;
                result.admittedTransportBlockCount = 2;
                result.admittedTransportBlockIndices[0] = 2;
                result.admittedTransportBlockIndices[1] = 3;
                Stamp(result);
                static_cast<void>(pipeline.Process(result));
                probe.processedResults++;
                probe.duplicateRefinementResults++;
                probe.rawAcceptedTransportBlocks += result.demodulation.acceptedTransportBlockCount;
                probe.temporallyAdmittedTransportBlocks += result.admittedTransportBlockCount;
                duplicateRefinementInjected = true;
            }
            else
            {
                static_cast<void>(pipeline.Process(result));
                probe.processedResults++;
            }
            if (!duplicateBurstInjected)
            {
                Require(kind == FrameKind::SessionControl,
                    "production LF4 Receiver probe first frame is not Session Control");
                for (std::uint32_t duplicate = 0; duplicate < suppressedDuplicateResults; duplicate++)
                {
                    auto suppressed = result;
                    suppressed.kind = pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly;
                    suppressed.temporalDisposition =
                        pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateSuppressed;
                    suppressed.admittedTransportBlockCount = 0;
                    suppressed.admittedRemoteControlBlockCount = 0;
                    suppressed.controlByteCount = 0;
                    Stamp(suppressed);
                    static_cast<void>(pipeline.Process(suppressed));
                    probe.processedResults++;
                    probe.suppressedDuplicateResults++;
                }
                duplicateBurstInjected = true;
            }
            builder.Advance();
        }
        Require(duplicateBurstInjected && duplicateRefinementInjected,
            "production LF4 Receiver probe did not exercise both temporal branches");
        Require(pipeline.IsCompleted() && completion.published,
            "production LF4 Receiver probe did not reach authoritative publish");
        pipeline.EndCaptureTelemetry(ElapsedMilliseconds(started));
        probe.decoder = snapshot.Get();
        Require(probe.decoder.state == DecoderState::Completed && probe.decoder.wholeFileDigestVerified &&
            probe.decoder.finalPublishSucceeded && probe.decoder.verifiedRawBytes == rawBytes.size() &&
            probe.decoder.acceptedTransportBlocks == probe.temporallyAdmittedTransportBlocks,
            "production LF4 Receiver probe final snapshot is not authoritative");
        output = std::move(probe);
        return {};
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(exception.what());
    }
    catch (...)
    {
        return RuntimeStatus::Failure("RemoteVisual LF4 Receiver probe failed with an unknown error");
    }
}

RuntimeStatus DecoderRuntimeTestAccess::ProbeRemoteVisualLowFpsReplay(const std::span<const std::byte> rawBytes,
    const std::wstring& replayPath, const std::wstring& outputDirectory, const std::uint32_t duplicateFrames,
    DecoderReplayProbeSnapshot& output) noexcept
{
    if (rawBytes.empty() || rawBytes.size() > 64ULL * 1024ULL || replayPath.empty() || outputDirectory.empty() ||
        duplicateFrames == 0 || duplicateFrames > 4)
    {
        return RuntimeStatus::Failure("RemoteVisual LF4 Replay probe input is outside its bounded contract");
    }
    try
    {
        std::error_code directoryError;
        Require(std::filesystem::is_directory(outputDirectory, directoryError) && !directoryError,
            "RemoteVisual LF4 Replay probe output directory is unavailable");
        const TransferDescription description = DescribeSource(rawBytes, false, 3);
        const ProfileBinding profile = GetProfileBinding(VisualProfile::RemoteVisualLowFps);
        SenderFrameBuilder builder(profile, description, 1);
        const std::uint32_t maximumCaptureFrames = builder.GetCarouselSnapshot().cycleFrameCount + duplicateFrames;
        Require(maximumCaptureFrames > duplicateFrames && maximumCaptureFrames <= 12,
            "RemoteVisual LF4 Replay probe would exceed its 12-frame corpus bound");
        const auto maximumRasterBytes = pbprotocol::CheckedMultiplyUint64(
            static_cast<std::uint64_t>(maximumCaptureFrames),
            static_cast<std::uint64_t>(phase1CanvasWidth) * phase1CanvasHeight * 4);
        RequireResult(maximumRasterBytes, "RemoteVisual LF4 Replay probe raster budget overflow");
        Require(maximumRasterBytes.Value() < 128ULL * mebibyte,
            "RemoteVisual LF4 Replay probe exceeds its 128 MiB file contract");

        ReplayD3dDevice replayDevice = CreateWarpReplayD3dDevice();
        const auto datasetId = pbprotocol::GenerateRandomSessionId();
        RequireResult(datasetId, "RemoteVisual LF4 Replay probe dataset CSPRNG failed");
        const std::string runId = GenerateRunId();
        pbrealcapturereplay::ReplayV2FileDescriptor replayDescriptor;
        replayDescriptor.datasetClass = pbrealcapturereplay::ReplayDatasetClass::RemoteVisual;
        replayDescriptor.datasetId = datasetId.Value().bytes;
        replayDescriptor.runId = runId;
        replayDescriptor.visualProfileId = profile.visualProfileId;
        replayDescriptor.createdUtc100ns = GetUtcFileTime100ns();
        replayDescriptor.remoteMetadataJsonUtf8 =
            R"({"schema":"PixelBridge.RemoteVisualRunMetadata.1","remoteProvider":"Step15HeadlessWARP","senderTruthAvailable":false})";
        Require(replayDescriptor.createdUtc100ns > 0, "RemoteVisual LF4 Replay probe UTC timestamp failed");

        RemoteVisualReplayRecorderConfig recorderConfig;
        recorderConfig.outputPath = replayPath;
        recorderConfig.descriptor = replayDescriptor;
        recorderConfig.limits.maximumFileBytes = 128ULL * mebibyte;
        recorderConfig.limits.maximumTotalRasterBytes = 128ULL * mebibyte;
        recorderConfig.limits.maximumCaptureFrames = maximumCaptureFrames;
        recorderConfig.roiWidth = phase1CanvasWidth;
        recorderConfig.roiHeight = phase1CanvasHeight;
        recorderConfig.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        recorderConfig.displayIdentityUtf8 = "WARP|Step15HeadlessProductionReplay";
        recorderConfig.dpiX = 96;
        recorderConfig.dpiY = 96;
        recorderConfig.scaleX = 1.0;
        recorderConfig.scaleY = 1.0;
        recorderConfig.queueCapacity = 2;
        std::shared_ptr<RemoteVisualReplayRecorder> recorder;
        const auto recorderCreateStatus = RemoteVisualReplayRecorder::Create(recorderConfig, recorder);
        Require(static_cast<bool>(recorderCreateStatus), "RemoteVisual LF4 Replay recorder creation failed: " +
            DescribeCaptureStatus(recorderCreateStatus));

        const pbprotocol::ReceiverResourcePolicy policy = pbprotocol::GetDefaultReceiverResourcePolicy();
        auto receiverResult = pbreceiver::ReceiverIngress::Create(policy, outerBlockBytes);
        RequireResult(receiverResult, "RemoteVisual LF4 Replay probe ingress creation failed");
        pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
        constexpr std::uint64_t runGeneration = 1;
        SnapshotStore<DecoderSnapshot> snapshot;
        snapshot.Update([&](DecoderSnapshot& value)
        {
            value.state = DecoderState::WaitingForBootstrap;
            value.runGeneration = runGeneration;
            value.runId = runId;
            value.visualProfile = VisualProfile::RemoteVisualLowFps;
            value.visualProfileId = profile.visualProfileId;
            value.visualLayoutVersion = profile.layoutVersion;
            value.codedDataBytesPerFrame = profile.dataBytes;
            value.codewordsPerFrame = profile.codewords;
            value.replayEnabled = true;
            value.replayDiagnosticOnly = true;
        });
        AuthoritativeCompletion authoritativeCompletion;
        const auto started = std::chrono::steady_clock::now();
        ReceiverPipeline pipeline(receiver, outputDirectory, policy, snapshot, authoritativeCompletion,
            runGeneration, started, VisualProfile::RemoteVisualLowFps);

        pbdemodd3d11::CaptureDemodulatorConfig demodConfig;
        demodConfig.visualProfileId = profile.visualProfileId;
        demodConfig.slotCount = captureDemodulatorSlotCount;
        demodConfig.maximumFrameAgeMilliseconds = 60000;
        demodConfig.resultQueueCapacity = captureResultQueueCapacity;
        demodConfig.maximumResidentBytes = maximumReplayDemodulatorResidentBytes;
        demodConfig.evaluationMode = pbdesktoplevels::EvaluationMode::Transport;
        std::shared_ptr<pbdemodd3d11::CaptureDemodulator> demodulator;
        const auto demodCreateStatus = pbdemodd3d11::CaptureDemodulator::Create(demodConfig, demodulator);
        Require(static_cast<bool>(demodCreateStatus), "RemoteVisual LF4 Replay demod creation failed: " +
            DescribeCaptureStatus(demodCreateStatus));

        pbcapturenormalize::ScreenCaptureDomain domain;
        domain.sourceId[0] = std::byte{0x15};
        domain.sourceId[15] = std::byte{0x4F};
        domain.captureEpoch = 1;
        pbcapturenormalize::CaptureEnvironment environment;
        environment.region.physicalRect = {0, 0, static_cast<LONG>(phase1CanvasWidth),
            static_cast<LONG>(phase1CanvasHeight)};
        environment.region.monitorPhysicalRect = environment.region.physicalRect;
        environment.region.dpiX = 96;
        environment.region.dpiY = 96;
        environment.region.rotation = DXGI_MODE_ROTATION_IDENTITY;
        environment.contentSize = {static_cast<std::int32_t>(phase1CanvasWidth),
            static_cast<std::int32_t>(phase1CanvasHeight)};
        environment.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        environment.adapterLuid = replayDevice.adapterLuid;
        environment.bitsPerColor = 8;
        environment.outputColorSpace = 0;
        environment.hdr = false;
        environment.backendKind = pbcapturenormalize::CaptureBackendKind::Dxgi;
        environment.sourceSize = environment.contentSize;
        environment.sourceRotation = DXGI_MODE_ROTATION_IDENTITY;
        const auto domainStatus = demodulator->DomainStarted(domain, environment, replayDevice.device.Get());
        Require(static_cast<bool>(domainStatus), "RemoteVisual LF4 Replay demod domain start failed: " +
            DescribeCaptureStatus(domainStatus));

        std::uint64_t captureObservation = 0;
        std::uint64_t duplicateSuppressedResults = 0;
        const auto ProcessFrame = [&](const std::span<const std::byte> pixels)
        {
            captureObservation++;
            pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
            metadata.domain = domain;
            metadata.backend = pbcapturenormalize::CaptureBackendKind::Dxgi;
            metadata.captureObservation = captureObservation;
            metadata.sourceGeneration = 1;
            metadata.slotGeneration = captureObservation;
            metadata.slotIndex = static_cast<std::uint32_t>((captureObservation - 1) % captureDemodulatorSlotCount);
            metadata.physicalRoi = environment.region.physicalRect;
            metadata.sourceContentSize = environment.contentSize;
            metadata.sourceExtent = environment.sourceSize;
            metadata.roiSize = environment.contentSize;
            metadata.displayRotation = DXGI_MODE_ROTATION_IDENTITY;
            metadata.sourceTransform = DXGI_MODE_ROTATION_IDENTITY;
            metadata.sourcePixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            metadata.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            metadata.adapterLuid = replayDevice.adapterLuid;
            metadata.bitsPerColor = 8;
            metadata.outputColorSpace = 0;
            metadata.signalEncoding = pbcapturenormalize::CaptureSignalEncoding::SdrRgb;
            metadata.timestamp.domain = pbcapturenormalize::CaptureTimestampDomain::DxgiQpcTicks;
            metadata.timestamp.rawValue = CurrentQpc100ns();
            metadata.timestamp.rawFrequency = 10000000;
            metadata.timestamp.monotonic100ns = metadata.timestamp.rawValue;
            metadata.timestamp.arrivalQpc100ns = metadata.timestamp.rawValue;
            metadata.roiCopyTime100ns = 0;
            metadata.isCursorExcluded = true;
            metadata.sourceCursorState = pbcapturenormalize::CursorState::Excluded;

            const auto analyzeStatus = recorder->Analyze(metadata, pixels,
                static_cast<std::size_t>(phase1CanvasWidth) * 4);
            Require(static_cast<bool>(analyzeStatus), "RemoteVisual LF4 Replay capture copy failed: " +
                DescribeCaptureStatus(analyzeStatus));
            recorder->Commit(metadata);
            const pbrealcapturereplay::ReplayRasterView raster{phase1CanvasWidth, phase1CanvasHeight,
                phase1CanvasWidth * 4, DXGI_FORMAT_B8G8R8A8_UNORM, pixels};
            const auto texture = CreateReplayTexture(replayDevice.device.Get(), raster);
            const pbdemodd3d11::CaptureDemodulatorResult demodResult = RunReplayProductionDemod(demodulator,
                replayDevice.device.Get(), replayDevice.context.Get(), metadata, texture);
            const ReceiverProcessResult processResult = pipeline.Process(demodResult);
            recorder->RecordDemodObservation(MakeReplayDemodObservation(
                demodResult, profile.visualProfileId, processResult));
            if (demodResult.temporalDisposition ==
                pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateSuppressed)
            {
                duplicateSuppressedResults++;
            }
            const auto writerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            for (;;)
            {
                const auto recorderSnapshot = recorder->GetSnapshot();
                Require(recorderSnapshot.evidenceValid && recorderSnapshot.droppedFrames == 0,
                    "RemoteVisual LF4 Replay recorder dropped or invalidated a selected ROI sample");
                if (recorderSnapshot.writtenFrames >= captureObservation)
                {
                    break;
                }
                if (std::chrono::steady_clock::now() >= writerDeadline)
                {
                    throw RuntimeFailure("RemoteVisual LF4 Replay writer did not drain within its bounded wait");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };

        std::uint64_t frameSequence = 0;
        while (!pipeline.IsCompleted() && frameSequence < builder.GetCarouselSnapshot().cycleFrameCount)
        {
            const auto& pixels = builder.Build(frameSequence);
            ProcessFrame(pixels);
            if (frameSequence == 0)
            {
                for (std::uint32_t duplicate = 0; duplicate < duplicateFrames; duplicate++)
                {
                    ProcessFrame(pixels);
                }
            }
            builder.Advance();
            frameSequence++;
        }
        Require(pipeline.IsCompleted(),
            "RemoteVisual LF4 Replay probe did not reach WholeFileDigest and final publish within one carousel");
        Require(duplicateSuppressedResults == duplicateFrames,
            "RemoteVisual LF4 Replay duplicate corpus did not remain temporally suppressed");
        demodulator->DomainInvalidated(domain);
        const auto demodSnapshot = demodulator->GetSnapshot();
        Require(demodSnapshot.pendingFrames == 0 && demodSnapshot.queuedResults == 0 &&
            demodSnapshot.demodulator.shutdown,
            "RemoteVisual LF4 Replay demod shutdown left bounded resources pending");
        pipeline.EndCaptureTelemetry(ElapsedMilliseconds(started));
        const auto stopStatus = recorder->Stop();
        Require(static_cast<bool>(stopStatus), "RemoteVisual LF4 Replay recorder finalization failed: " +
            DescribeCaptureStatus(stopStatus));
        const RemoteVisualReplayRecorderSnapshot recorderSnapshot = recorder->GetSnapshot();
        Require(recorderSnapshot.finalized && recorderSnapshot.evidenceValid &&
            recorderSnapshot.writtenFrames == captureObservation &&
            recorderSnapshot.writtenDemodObservations == captureObservation &&
            recorderSnapshot.droppedFrames == 0 && recorderSnapshot.droppedDemodObservations == 0,
            "RemoteVisual LF4 Replay recorder counts did not close exactly");

        DecoderReplayProbeSnapshot probe;
        probe.decoder = snapshot.Get();
        Require(probe.decoder.state == DecoderState::Completed && probe.decoder.wholeFileDigestVerified &&
            probe.decoder.finalPublishSucceeded && probe.decoder.verifiedRawBytes == rawBytes.size(),
            "RemoteVisual LF4 Replay live production result is not authoritative");
        probe.replayFileBytes = recorderSnapshot.fileBytes;
        probe.captureFrames = recorderSnapshot.writtenFrames;
        probe.demodObservations = recorderSnapshot.writtenDemodObservations;
        probe.droppedCaptureFrames = recorderSnapshot.droppedFrames;
        probe.droppedDemodObservations = recorderSnapshot.droppedDemodObservations;
        probe.duplicateSuppressedResults = duplicateSuppressedResults;
        probe.recorderQueueHighWater = recorderSnapshot.queueHighWater;
        output = std::move(probe);
        return {};
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(exception.what());
    }
    catch (...)
    {
        return RuntimeStatus::Failure("RemoteVisual LF4 Replay probe failed with an unknown error");
    }
}

RuntimeStatus ValidateEncoderConfig(const EncoderConfig& config)
{
    if (!IsValidRunId(config.runId))
    {
        return RuntimeStatus::Failure("RunId 必须为空或 128-bit lowercase hex（32 个字符）");
    }
    if (!IsValidRemoteMetadata(config.remoteMetadata))
    {
        return RuntimeStatus::Failure("RemoteVisual metadata 含无效 UTF-8、非有限数值、越界矩形或超长字段");
    }
    if (!config.runId.empty() && !config.remoteMetadata.runId.empty() && config.runId != config.remoteMetadata.runId)
    {
        return RuntimeStatus::Failure("RunId 与 RemoteVisual metadata preset 不一致");
    }
    if (config.sourcePath.empty())
    {
        return RuntimeStatus::Failure("请选择源文件");
    }
    if (config.compressionLevel < 1 || config.compressionLevel > 22)
    {
        return RuntimeStatus::Failure("Compression level 必须位于当前支持范围 1..22");
    }
    if (FindVisualProfileOption(config.visualProfile) == nullptr || !config.monitorClientOrigin)
    {
        return RuntimeStatus::Failure("请选择当前真实存在的 Visual Profile 和目标 monitor");
    }
    if (config.logicalVisualFps > maximumLogicalVisualFps || config.controlRepetitions < minimumControlRepetitions ||
        config.controlRepetitions > maximumControlRepetitions)
    {
        return RuntimeStatus::Failure("Logical Visual FPS 必须为 0 或 1..240，Control repetitions 必须为 1..64");
    }
    if (IsRemoteVisualProfile(config.visualProfile) &&
        (config.logicalVisualFps == 0 || config.logicalVisualFps > maximumRemoteVisualLogicalFps))
    {
        return RuntimeStatus::Failure("RemoteVisual Logical Visual FPS 必须为 1..5；0 会恢复高频 presentation-driven 更新，已禁止");
    }
    if (config.visualProfile != VisualProfile::RemoteVisualLowFps && config.singleMonitorFullscreen)
    {
        return RuntimeStatus::Failure("single-monitor fullscreen 仅允许 remote-lf4 profile");
    }
    if (config.visualProfile == VisualProfile::RemoteVisualLowFps)
    {
        if (config.remoteMetadata.channelType != ChannelType::RemoteVisual)
        {
            return RuntimeStatus::Failure("remote-lf4 profile 必须显式使用 RemoteVisual channel");
        }
        const bool singleMonitorFullscreen = config.singleMonitorFullscreen.has_value();
        if (singleMonitorFullscreen == config.monitorSafety.has_value())
        {
            return RuntimeStatus::Failure(
                "remote-lf4 必须且只能选择 dual-monitor safety 或显式 single-monitor fullscreen authority");
        }
        std::uint32_t targetWidth = phase1CanvasWidth;
        std::uint32_t targetHeight = phase1CanvasHeight;
        if (singleMonitorFullscreen &&
            !TryGetMonitorDimensions(*config.singleMonitorFullscreen, targetWidth, targetHeight))
        {
            return RuntimeStatus::Failure("single-monitor fullscreen 物理显示器尺寸无效");
        }
        RECT target{};
        if (!TryMakePhysicalRect(*config.monitorClientOrigin, targetWidth, targetHeight, target))
        {
            return RuntimeStatus::Failure("remote-lf4 Data Window physical RECT 溢出或无效");
        }
        if (singleMonitorFullscreen)
        {
            const MonitorInfo& monitor = *config.singleMonitorFullscreen;
            if (!IsSupportedRemoteVisualFullscreenMonitor(monitor) ||
                !EqualRect(&target, &monitor.physicalRect) || monitor.monitor == nullptr)
            {
                return RuntimeStatus::Failure(
                    "single-monitor fullscreen 必须精确覆盖一块可容纳 1920x1080 LF4 canvas、且不超过 3840x2160 的未旋转物理显示器");
            }
            try
            {
                const std::string experimentIdentity = Utf8FromWide(monitor.deviceName);
                if (!config.remoteMetadata.protectedMonitorIdentity.empty() ||
                    config.remoteMetadata.experimentMonitorIdentity != experimentIdentity)
                {
                    return RuntimeStatus::Failure(
                        "single-monitor fullscreen metadata 必须不声明 ProtectedMonitor，并精确绑定目标显示器 identity");
                }
            }
            catch (const std::exception&)
            {
                return RuntimeStatus::Failure("single-monitor fullscreen monitor identity 不是有效 UTF-16");
            }
        }
        else
        {
            const MonitorInfo& experimentMonitor = config.monitorSafety->experimentMonitor;
            const MonitorSafetyStatus monitorSafety = ValidateMonitorSafetyTarget(*config.monitorSafety, target,
                experimentMonitor.monitor);
            if (!monitorSafety || experimentMonitor.rotation != DXGI_MODE_ROTATION_IDENTITY ||
                !CanHostPhase1Canvas(experimentMonitor.physicalRect))
            {
                return RuntimeStatus::Failure(std::string("remote-lf4 Data Window 不满足屏幕安全约束：") +
                    GetMonitorSafetyErrorName(monitorSafety.code));
            }
            try
            {
                const std::string protectedIdentity = Utf8FromWide(config.monitorSafety->protectedMonitor.deviceName);
                const std::string experimentIdentity = Utf8FromWide(experimentMonitor.deviceName);
                if (config.remoteMetadata.protectedMonitorIdentity != protectedIdentity ||
                    config.remoteMetadata.experimentMonitorIdentity != experimentIdentity)
                {
                    return RuntimeStatus::Failure(
                        "remote-lf4 metadata 的 ProtectedMonitor/ExperimentMonitor identity 必须与安全选择完全一致");
                }
            }
            catch (const std::exception&)
            {
                return RuntimeStatus::Failure("remote-lf4 monitor identity 不是有效 UTF-16");
            }
        }
    }
    try
    {
        std::error_code error;
        const std::filesystem::path path(config.sourcePath);
        if (!std::filesystem::is_regular_file(path, error) || error)
        {
            return RuntimeStatus::Failure("源文件不存在或不是常规文件");
        }
        const std::uint64_t fileBytes = std::filesystem::file_size(path, error);
        if (error || fileBytes < minimumInstantFileBytes || fileBytes > maximumInstantFileBytes)
        {
            return RuntimeStatus::Failure("当前单 Segment production path 仅支持 1 byte 到 8 MiB");
        }
    }
    catch (...)
    {
        return RuntimeStatus::Failure("源文件路径无效");
    }
    return {};
}

RuntimeStatus ValidateDecoderConfig(const DecoderConfig& config)
{
    if (!IsValidRunId(config.runId))
    {
        return RuntimeStatus::Failure("RunId 必须为空或 128-bit lowercase hex（32 个字符）");
    }
    if (!IsValidRemoteMetadata(config.remoteMetadata))
    {
        return RuntimeStatus::Failure("RemoteVisual metadata 含无效 UTF-8、非有限数值、越界矩形或超长字段");
    }
    if (!config.runId.empty() && !config.remoteMetadata.runId.empty() && config.runId != config.remoteMetadata.runId)
    {
        return RuntimeStatus::Failure("RunId 与 RemoteVisual metadata preset 不一致");
    }
    if (config.outputDirectory.empty())
    {
        return RuntimeStatus::Failure("请选择输出目录");
    }
    try
    {
        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::path(config.outputDirectory), error) || error)
        {
            return RuntimeStatus::Failure("输出目录不存在或不可用");
        }
    }
    catch (...)
    {
        return RuntimeStatus::Failure("输出目录路径无效");
    }
    const bool offlineReplay = !config.replayInputPath.empty();
    const bool sampledProductionReplay = !config.replayOutputPath.empty() &&
        config.remoteMetadata.channelType == ChannelType::RemoteVisual && !config.diagnosticCaptureOnly &&
        FindVisualProfileOption(config.visualProfile) != nullptr;
    if (config.replayMaximumCaptureFramesPerSecond > maximumReplayCaptureFramesPerSecond ||
        (config.replayMaximumCaptureFramesPerSecond != 0 && !config.diagnosticCaptureOnly &&
         !sampledProductionReplay))
    {
        return RuntimeStatus::Failure(
            "Replay time sampler 仅允许 diagnostic capture-only 或 RemoteVisual production diagnostic fan-out，且必须处于 1..60 FPS 的显式上限内");
    }
    if (config.replayEvidenceVisualProfileId &&
        (!config.diagnosticCaptureOnly || !IsReplayEvidenceVisualProfileId(*config.replayEvidenceVisualProfileId)))
    {
        return RuntimeStatus::Failure(
            "Replay evidence profile 只允许 Direct、Shape 或 LF4 capture-only descriptor 标记；不得选择 production demodulator");
    }
    if (offlineReplay && !config.replayOutputPath.empty())
    {
        return RuntimeStatus::Failure("Replay input 与 live replay recorder output 互斥");
    }
    if (offlineReplay)
    {
        const bool supportedReplayProfile = config.visualProfile == VisualProfile::DirectLevels2x2 ||
            config.visualProfile == VisualProfile::ShapeChroma ||
            config.visualProfile == VisualProfile::RemoteVisualResilient ||
            config.visualProfile == VisualProfile::RemoteVisualLowFps;
        if (!supportedReplayProfile ||
            config.replayMaximumCaptureFrames == 0 ||
            config.replayMaximumCaptureFrames > pbrealcapturereplay::kReplayV2HardMaximumCaptureFrames ||
            config.replayMaximumFileBytes < 16ULL * mebibyte ||
            config.replayMaximumFileBytes > pbrealcapturereplay::kReplayV2HardMaximumFileBytes)
        {
            return RuntimeStatus::Failure("Offline Replay v2 仅复用 Direct、Shape、RemoteVisual 或 LF4 production profile，且必须满足 1..2048 帧、16 MiB..16 GiB 的有界配置");
        }
        try
        {
            const std::filesystem::path replayPath(config.replayInputPath);
            std::error_code error;
            if (replayPath.empty() || !std::filesystem::is_regular_file(replayPath, error) || error)
            {
                return RuntimeStatus::Failure("Replay input 不存在或不是常规文件");
            }
            const std::uint64_t replayBytes = std::filesystem::file_size(replayPath, error);
            if (error || replayBytes < pbrealcapturereplay::kReplayV2FileHeaderBytes +
                pbrealcapturereplay::kReplayV2FileFooterBytes || replayBytes > config.replayMaximumFileBytes)
            {
                return RuntimeStatus::Failure("Replay input 大小超出当前显式 resource contract");
            }
        }
        catch (...)
        {
            return RuntimeStatus::Failure("Replay input 路径无效");
        }
        return {};
    }
    if ((config.captureBackend != CaptureBackend::Wgc && config.captureBackend != CaptureBackend::Dxgi) ||
        (config.visualProfile != VisualProfile::DirectLevels2x2 && config.visualProfile != VisualProfile::ShapeChroma &&
         config.visualProfile != VisualProfile::RemoteVisualResilient &&
         config.visualProfile != VisualProfile::RemoteVisualLowFps))
    {
        return RuntimeStatus::Failure("Capture backend 或 Visual Profile 不在当前 Runtime Option Inventory 中");
    }
    const RECT& rect = config.region.physicalRect;
    const std::int64_t width = static_cast<std::int64_t>(rect.right) - rect.left;
    const std::int64_t height = static_cast<std::int64_t>(rect.bottom) - rect.top;
    const RECT& monitorRect = config.region.monitorPhysicalRect;
    const std::int64_t monitorWidth = static_cast<std::int64_t>(monitorRect.right) - monitorRect.left;
    const std::int64_t monitorHeight = static_cast<std::int64_t>(monitorRect.bottom) - monitorRect.top;
    if (config.region.monitor == nullptr || width <= 0 || height <= 0 ||
        config.region.dpiX == 0 || config.region.dpiY == 0 ||
        config.region.rotation != DXGI_MODE_ROTATION_IDENTITY || monitorWidth <= 0 || monitorHeight <= 0 ||
        rect.left < monitorRect.left || rect.top < monitorRect.top || rect.right > monitorRect.right ||
        rect.bottom > monitorRect.bottom)
    {
        return RuntimeStatus::Failure("ROI 必须是单显示器内、identity rotation、非空的物理像素矩形");
    }
    const bool strictGeometry = width == phase1CanvasWidth && height == phase1CanvasHeight;
    const bool lowFpsGeometry = config.visualProfile == VisualProfile::RemoteVisualLowFps &&
        width >= minimumRemoteVisualLowFpsRoiWidth && width <= maximumRemoteVisualLowFpsRoiWidth &&
        height >= minimumRemoteVisualLowFpsRoiHeight && height <= maximumRemoteVisualLowFpsRoiHeight;
    const bool diagnosticCaptureOnlyAllowed = config.diagnosticCaptureOnly &&
        !config.replayOutputPath.empty() && config.remoteMetadata.channelType == ChannelType::RemoteVisual &&
        config.visualProfile == VisualProfile::RemoteVisualResilient;
    if (!strictGeometry && !lowFpsGeometry && !diagnosticCaptureOnlyAllowed)
    {
        return RuntimeStatus::Failure(
            "Geometry incompatible with selected profile; remote-lf4 requires a bounded 0.5x..2.0x physical ROI");
    }
    if (config.diagnosticCaptureOnly && !diagnosticCaptureOnlyAllowed)
    {
        return RuntimeStatus::Failure(
            "Replay capture-only 必须显式启用 RemoteVisual、RemoteVisual Resilient profile 和 create-only replay output");
    }
    if (config.visualProfile == VisualProfile::RemoteVisualLowFps &&
        config.remoteMetadata.channelType != ChannelType::RemoteVisual)
    {
        return RuntimeStatus::Failure("remote-lf4 profile 必须显式使用 RemoteVisual channel");
    }
    if (config.remoteMetadata.channelType == ChannelType::RemoteVisual)
    {
        if (!config.monitorSafety)
        {
            return RuntimeStatus::Failure("RemoteVisual 必须显式选择 ProtectedMonitor 与 ExperimentMonitor");
        }
        const MonitorSafetyStatus monitorSafety = ValidateMonitorSafetyTarget(*config.monitorSafety, rect,
            config.region.monitor);
        const MonitorInfo& experimentMonitor = config.monitorSafety->experimentMonitor;
        if (!monitorSafety || EqualRect(&experimentMonitor.physicalRect, &monitorRect) == FALSE ||
            experimentMonitor.dpiX != config.region.dpiX || experimentMonitor.dpiY != config.region.dpiY ||
            experimentMonitor.rotation != config.region.rotation)
        {
            return RuntimeStatus::Failure(std::string("Geometry incompatible with current experimental profile: ") +
                GetMonitorSafetyErrorName(monitorSafety.code));
        }
        if (config.visualProfile == VisualProfile::RemoteVisualLowFps)
        {
            try
            {
                const std::string protectedIdentity = Utf8FromWide(config.monitorSafety->protectedMonitor.deviceName);
                const std::string experimentIdentity = Utf8FromWide(experimentMonitor.deviceName);
                if (config.remoteMetadata.protectedMonitorIdentity != protectedIdentity ||
                    config.remoteMetadata.experimentMonitorIdentity != experimentIdentity)
                {
                    return RuntimeStatus::Failure(
                        "remote-lf4 metadata 的 ProtectedMonitor/ExperimentMonitor identity 必须与安全选择完全一致");
                }
            }
            catch (const std::exception&)
            {
                return RuntimeStatus::Failure("remote-lf4 monitor identity 不是有效 UTF-16");
            }
        }
    }
    if (!config.replayOutputPath.empty())
    {
        const bool supportedReplayMode = config.diagnosticCaptureOnly ?
            config.visualProfile == VisualProfile::RemoteVisualResilient :
            FindVisualProfileOption(config.visualProfile) != nullptr;
        if (config.remoteMetadata.channelType != ChannelType::RemoteVisual || !supportedReplayMode ||
            config.replayMaximumCaptureFrames == 0 ||
            config.replayMaximumCaptureFrames > pbrealcapturereplay::kReplayV2HardMaximumCaptureFrames ||
            config.replayMaximumFileBytes < 16ULL * mebibyte ||
            config.replayMaximumFileBytes > pbrealcapturereplay::kReplayV2HardMaximumFileBytes)
        {
            return RuntimeStatus::Failure("Replay v2 仅用于旧 RemoteVisual capture-only 或 RemoteVisual production diagnostic fan-out，且必须满足 1..2048 帧、16 MiB..16 GiB 的有界配置");
        }
        try
        {
            const std::filesystem::path replayPath(config.replayOutputPath);
            const std::filesystem::path parent = replayPath.parent_path();
            std::error_code error;
            if (replayPath.empty() || parent.empty() || !std::filesystem::is_directory(parent, error) || error ||
                std::filesystem::exists(replayPath, error) || error)
            {
                return RuntimeStatus::Failure("Replay 输出目录无效，或最终文件已存在（禁止覆盖）");
            }
            auto partialPath = replayPath;
            partialPath += L".partial";
            error.clear();
            if (std::filesystem::exists(partialPath, error) || error)
            {
                return RuntimeStatus::Failure("Replay .partial 已存在；请先核对其来源，不自动覆盖或删除");
            }
        }
        catch (...)
        {
            return RuntimeStatus::Failure("Replay 输出路径无效");
        }
    }
    else if (config.diagnosticCaptureOnly)
    {
        return RuntimeStatus::Failure("Replay capture-only 缺少 create-only replay output path");
    }
    const auto captureConfig = MakeCaptureConfig(config);
    const RuntimeStatus replayResourceStatus = ValidateProductionReplayResourceContract(
        config, GetProfileBinding(config.visualProfile), captureConfig);
    if (!replayResourceStatus)
    {
        return replayResourceStatus;
    }
    const auto captureStatus = config.captureBackend == CaptureBackend::Wgc ?
        pbscreencapturewgc::ValidateWgcCaptureConfig(captureConfig.capture) :
        pbscreencapturedxgi::ValidateDxgiCaptureConfig(captureConfig.capture);
    if (!captureStatus)
    {
        return RuntimeStatus::Failure(std::string(GetCaptureBackendName(config.captureBackend)) +
            " capture resource contract invalid: " + DescribeCaptureStatus(captureStatus));
    }
    return {};
}

pbcompression::CompressionResult<pbcompression::EncodedSegment> PrepareEncodedSegment(
    const std::span<const std::byte> rawBytes, const bool compressionEnabled,
    const int compressionLevel)
{
    if (!compressionEnabled)
    {
        pbcompression::EncodedSegment encoded;
        encoded.codec = pbprotocol::CompressionCodec::Raw;
        encoded.bytes.assign(rawBytes.begin(), rawBytes.end());
        return pbcompression::CompressionResult<pbcompression::EncodedSegment>::Success(std::move(encoded));
    }
    pbcompression::CompressionSettings settings;
    settings.compressionLevel = compressionLevel;
    settings.maxOutputBytes = 16ULL * mebibyte;
    return pbcompression::CompressSegment(rawBytes, settings);
}

EncoderRuntime::~EncoderRuntime()
{
    Stop();
}

RuntimeStatus EncoderRuntime::Start(const EncoderConfig& config)
{
    const RuntimeStatus validation = ValidateEncoderConfig(config);
    if (!validation)
    {
        return validation;
    }
    std::string sourcePathUtf8;
    try
    {
        sourcePathUtf8 = Utf8FromWide(config.sourcePath);
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(std::string("源文件路径编码无效：") + exception.what());
    }
    std::unique_lock lock(lifecycleMutex_);
    if (workerRunning_)
    {
        return RuntimeStatus::Failure("Encoder 已处于 Preparing/Broadcasting/Stopping，拒绝重复 Start");
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
    if (nextRunGeneration_ == 0)
    {
        return RuntimeStatus::Failure("run generation exhausted");
    }
    const std::uint64_t runGeneration = nextRunGeneration_++;
    stopRequested_ = false;
    EncoderSnapshot initial;
    initial.state = EncoderState::Preparing;
    initial.runGeneration = runGeneration;
    initial.runId = config.runId.empty() ? "pending" : config.runId;
    initial.runStartedUnixMilliseconds = GetUnixTimeMilliseconds();
    initial.sourcePath = std::move(sourcePathUtf8);
    initial.visualProfile = config.visualProfile;
    initial.dataWindowLeft = config.monitorClientOrigin->x;
    initial.dataWindowTop = config.monitorClientOrigin->y;
    initial.singleMonitorFullscreen = config.singleMonitorFullscreen.has_value();
    if (config.singleMonitorFullscreen)
    {
        static_cast<void>(TryGetMonitorDimensions(*config.singleMonitorFullscreen,
            initial.dataWindowWidth, initial.dataWindowHeight));
    }
    initial.monitorSafetyPreflightPassed = false;
    initial.monitorSafetyStatus = config.singleMonitorFullscreen ? "NotApplicableSingleMonitorFullscreen" :
        config.visualProfile == VisualProfile::RemoteVisualLowFps ? "Pending" : "NotRequired";
    initial.statusMessage = "Preparing source, descriptors, compression, and outer FEC";
    initial.remoteMetadata = config.remoteMetadata;
    initial.configuredLogicalVisualFps = config.logicalVisualFps;
    if (config.logicalVisualFps != 0)
    {
        initial.configuredLogicalDwellMilliseconds = 1000.0 / config.logicalVisualFps;
    }
    initial.configuredControlRepetitions = config.controlRepetitions;
    try
    {
        snapshot_.Replace(std::move(initial));
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(std::string("无法初始化 Encoder snapshot：") + exception.what());
    }
    catch (...)
    {
        return RuntimeStatus::Failure("无法初始化 Encoder snapshot");
    }
    workerRunning_ = true;
    try
    {
        worker_ = std::thread(&EncoderRuntime::Run, this, config, runGeneration);
    }
    catch (const std::exception& exception)
    {
        workerRunning_ = false;
        try
        {
            snapshot_.Update([runGeneration, &exception](EncoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = EncoderState::Failed;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.errorDetail = exception.what();
                }
            });
        }
        catch (...)
        {
        }
        return RuntimeStatus::Failure("无法创建 Encoder worker thread");
    }
    return {};
}

void EncoderRuntime::RequestStop() noexcept
{
    stopRequested_ = true;
    try
    {
        const std::scoped_lock lock(lifecycleMutex_);
        stopRequested_ = true;
        snapshot_.Update([](EncoderSnapshot& value)
        {
            if (value.state == EncoderState::Preparing || value.state == EncoderState::Broadcasting)
            {
                value.state = EncoderState::Stopping;
            }
        });
    }
    catch (...)
    {
    }
}

void EncoderRuntime::Stop() noexcept
{
    RequestStop();
    try
    {
        const std::scoped_lock lock(lifecycleMutex_);
        stopRequested_ = true;
        if (worker_.joinable())
        {
            worker_.join();
        }
    }
    catch (...)
    {
        std::terminate();
    }
}

EncoderSnapshot EncoderRuntime::GetSnapshot() const
{
    return snapshot_.Get();
}

void EncoderRuntime::Run(EncoderConfig config, const std::uint64_t runGeneration) noexcept
{
    WorkerRunningGuard runningGuard(workerRunning_);
    bool sourceStable = true;
    try
    {
        const auto workerStarted = std::chrono::steady_clock::now();
        ProcessResourceSampler resourceSampler;
        if (config.visualProfile == VisualProfile::RemoteVisualLowFps)
        {
            Require(config.monitorSafety.has_value() != config.singleMonitorFullscreen.has_value(),
                "remote-lf4 worker requires exactly one display authority");
            if (config.singleMonitorFullscreen)
            {
                const MonitorSafetyStatus topology = RevalidateSingleMonitorFullscreen(
                    *config.singleMonitorFullscreen);
                Require(static_cast<bool>(topology),
                    std::string("single-monitor fullscreen topology changed before startup: ") +
                    GetMonitorSafetyErrorName(topology.code));
                snapshot_.Update([runGeneration](EncoderSnapshot& value)
                {
                    if (value.runGeneration == runGeneration)
                    {
                        value.monitorSafetyRevalidationCount++;
                        value.monitorSafetyStatus = "NotApplicableSingleMonitorFullscreen";
                    }
                });
            }
            else
            {
                const MonitorSafetyStatus monitorSafety = RevalidateMonitorSafetySelection(*config.monitorSafety);
                Require(static_cast<bool>(monitorSafety),
                    std::string("display topology changed before remote-lf4 startup: ") +
                    GetMonitorSafetyErrorName(monitorSafety.code));
                snapshot_.Update([runGeneration](EncoderSnapshot& value)
                {
                    if (value.runGeneration == runGeneration)
                    {
                        value.monitorSafetyPreflightPassed = true;
                        value.monitorSafetyRevalidationCount++;
                        value.monitorSafetyStatus = "PASS";
                    }
                });
            }
        }
        SourceFile source = ReadSourceFile(config.sourcePath);
        if (stopRequested_)
        {
            snapshot_.Update([runGeneration](EncoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = EncoderState::Stopped;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.statusMessage = "Stopped during source preparation";
                }
            });
            return;
        }
        const TransferDescription description = DescribeSource(source.bytes, config.compressionEnabled,
            config.compressionLevel);
        if (stopRequested_)
        {
            snapshot_.Update([runGeneration](EncoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = EncoderState::Stopped;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.statusMessage = "Stopped during descriptor/FEC preparation";
                }
            });
            return;
        }
        const ProfileBinding profile = GetProfileBinding(config.visualProfile);
        SenderFrameBuilder builder(profile, description, config.controlRepetitions);
        const std::string runId = config.runId.empty() ? GenerateRunId() : config.runId;
        const CarouselSnapshot initialCarousel = builder.GetCarouselSnapshot();
        const auto rawVisualBits = pbprotocol::CheckedMultiplyUint64(profile.dataBytes, 8);
        const auto informationBytesPerFrame = pbprotocol::CheckedMultiplyUint64(profile.codewords, informationBytes);
        const auto transportPayloadCeiling = pbprotocol::CheckedMultiplyUint64(profile.codewords, outerBlockBytes);
        RequireResult(rawVisualBits, "profile raw visual capacity overflow");
        RequireResult(informationBytesPerFrame, "profile Inner-FEC information capacity overflow");
        RequireResult(transportPayloadCeiling, "profile Transport payload ceiling overflow");
        std::optional<std::uint64_t> configuredTransportPayloadCeiling;
        if (config.logicalVisualFps != 0)
        {
            const auto perSecond = pbprotocol::CheckedMultiplyUint64(transportPayloadCeiling.Value(),
                config.logicalVisualFps);
            RequireResult(perSecond, "configured Transport payload ceiling overflow");
            configuredTransportPayloadCeiling = perSecond.Value();
        }
        snapshot_.Update([&](EncoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration)
            {
                return;
            }
            value.runId = runId;
            value.remoteMetadata.runId = runId;
            value.sourceBytes = source.bytes.size();
            value.visualProfileId = profile.visualProfileId;
            value.visualLayoutVersion = profile.layoutVersion;
            value.codedDataBytesPerFrame = profile.dataBytes;
            value.codewordsPerFrame = profile.codewords;
            value.rawVisualBitsPerLogicalFrame = rawVisualBits.Value();
            value.innerFecInformationBytesPerLogicalFrame = informationBytesPerFrame.Value();
            value.transportPayloadCeilingBytesPerLogicalFrame = transportPayloadCeiling.Value();
            value.configuredTransportPayloadCeilingBytesPerSecond = configuredTransportPayloadCeiling;
            value.sessionIdHex = SessionIdHex(description.session.sessionId);
            value.sessionTag = description.segment.sessionTag.value;
            value.wholeFileDigestHex = DigestHex(description.manifest.wholeFileDigest.bytes);
            value.compressionCodec = description.segment.compressionCodec;
            value.outerFecMode = description.segment.outerFecMode;
            value.outerBlockCount = builder.GetBlockCount();
            value.cycleFrameCount = initialCarousel.cycleFrameCount;
            value.segmentCount = 1;
            value.statusMessage = "Creating the production D3D11 Data Window";
        });
        if (stopRequested_)
        {
            snapshot_.Update([runGeneration](EncoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = EncoderState::Stopped;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.statusMessage = "Stopped during preparation";
                }
            });
            return;
        }
        std::uint32_t presentationWidth = phase1CanvasWidth;
        std::uint32_t presentationHeight = phase1CanvasHeight;
        if (config.singleMonitorFullscreen)
        {
            Require(TryGetMonitorDimensions(*config.singleMonitorFullscreen, presentationWidth, presentationHeight),
                "single-monitor fullscreen dimensions became invalid");
        }
        const auto presentationPixelsCount = pbprotocol::CheckedMultiplyUnsigned(
            static_cast<std::size_t>(presentationWidth), static_cast<std::size_t>(presentationHeight));
        const auto presentationBytes = presentationPixelsCount ? pbprotocol::CheckedMultiplyUnsigned(
            presentationPixelsCount.Value(), std::size_t{4}) : presentationPixelsCount;
        RequireResult(presentationBytes, "single-monitor fullscreen presentation size overflow");
        std::vector<std::byte> fullscreenPixels;
        if (config.singleMonitorFullscreen)
        {
            fullscreenPixels.resize(presentationBytes.Value());
        }
        pbrenderd3d::DataWindowConfig windowConfig;
        windowConfig.width = presentationWidth;
        windowConfig.height = presentationHeight;
        windowConfig.clientOrigin = config.monitorClientOrigin;
        windowConfig.repeatActiveFrame = config.visualProfile == VisualProfile::RemoteVisualLowFps;
        windowConfig.topmost = config.singleMonitorFullscreen.has_value();
        auto created = pbrenderd3d::DataWindow::Create(windowConfig);
        if (!created)
        {
            throw RuntimeFailure("DataWindow creation failed: " + DescribePresentationStatus(created.Error()));
        }
        std::unique_ptr<pbrenderd3d::DataWindow> window = std::move(created).Value();
        std::uint64_t frameSequence = 0;
        std::uint64_t generatedPayloadBytes = 0;
        bool frameBuilt = false;
        bool broadcastStarted = false;
        auto nextStabilityCheck = workerStarted;
        auto nextMonitorSafetyCheck = workerStarted;
        const auto logicalFrameInterval = config.logicalVisualFps == 0 ? std::chrono::steady_clock::duration::zero() :
            std::chrono::ceil<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / config.logicalVisualFps));
        const std::optional<double> configuredLogicalDwellMilliseconds = config.logicalVisualFps == 0 ?
            std::nullopt : std::optional<double>{1000.0 / config.logicalVisualFps};
        std::optional<std::chrono::steady_clock::time_point> firstLogicalFrameAt;
        std::optional<std::chrono::steady_clock::time_point> previousLogicalFrameAt;
        std::optional<double> minimumObservedLogicalDwellMilliseconds;
        std::uint64_t logicalDwellViolationCount = 0;
        auto nextLogicalFrameAt = workerStarted;
        std::optional<std::chrono::steady_clock::time_point> broadcastStartedAt;
        for (;;)
        {
            const auto now = std::chrono::steady_clock::now();
            resourceSampler.Sample(ElapsedMilliseconds(workerStarted));
            snapshot_.Update([&](EncoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    ApplyProcessResourceSample(resourceSampler.GetSnapshot(), value);
                }
            });
            const pbrenderd3d::DataWindowSnapshot windowSnapshot = window->GetSnapshot();
            if (windowSnapshot.state == pbrenderd3d::WindowState::Failed)
            {
                throw RuntimeFailure("DataWindow failed: " + DescribePresentationStatus(windowSnapshot.error));
            }
            if (stopRequested_)
            {
                window->RequestStop();
            }
            if (windowSnapshot.state == pbrenderd3d::WindowState::Stopped)
            {
                break;
            }
            if (now >= nextStabilityCheck)
            {
                sourceStable = IsSourceStable(source);
                Require(sourceStable, "源文件在 Session 广播期间发生变化，Session 已中止");
                nextStabilityCheck = now + std::chrono::seconds(1);
            }
            if (config.visualProfile == VisualProfile::RemoteVisualLowFps && now >= nextMonitorSafetyCheck)
            {
                RECT actualTarget{};
                Require(TryMakePhysicalRect(windowSnapshot.environment.clientOrigin,
                    windowSnapshot.environment.clientWidth, windowSnapshot.environment.clientHeight, actualTarget),
                    "remote-lf4 Data Window has no valid physical client RECT");
                const HMONITOR actualMonitor = MonitorFromRect(&actualTarget, MONITOR_DEFAULTTONULL);
                if (config.singleMonitorFullscreen)
                {
                    const MonitorSafetyStatus topology = RevalidateSingleMonitorFullscreen(
                        *config.singleMonitorFullscreen);
                    Require(static_cast<bool>(topology),
                        std::string("single-monitor fullscreen topology changed: ") +
                        GetMonitorSafetyErrorName(topology.code));
                    Require(EqualRect(&actualTarget, &config.singleMonitorFullscreen->physicalRect) != FALSE &&
                        actualMonitor == config.singleMonitorFullscreen->monitor,
                        "single-monitor fullscreen no longer exactly covers its selected monitor");
                    Require(windowSnapshot.environment.clientWidth == presentationWidth &&
                        windowSnapshot.environment.clientHeight == presentationHeight,
                        "single-monitor fullscreen client geometry changed");
                    snapshot_.Update([runGeneration](EncoderSnapshot& value)
                    {
                        if (value.runGeneration == runGeneration)
                        {
                            value.monitorSafetyRevalidationCount++;
                            value.monitorSafetyStatus = "NotApplicableSingleMonitorFullscreen";
                        }
                    });
                }
                else
                {
                    const MonitorSafetyStatus topology = RevalidateMonitorSafetySelection(*config.monitorSafety);
                    Require(static_cast<bool>(topology),
                        std::string("display topology changed during remote-lf4 broadcast: ") +
                        GetMonitorSafetyErrorName(topology.code));
                    const MonitorSafetyStatus targetSafety = ValidateMonitorSafetyTarget(*config.monitorSafety,
                        actualTarget, actualMonitor);
                    Require(static_cast<bool>(targetSafety),
                        std::string("remote-lf4 Data Window left its ExperimentMonitor: ") +
                        GetMonitorSafetyErrorName(targetSafety.code));
                    Require(windowSnapshot.environment.clientWidth == phase1CanvasWidth &&
                        windowSnapshot.environment.clientHeight == phase1CanvasHeight,
                        "remote-lf4 Data Window client geometry changed");
                    snapshot_.Update([runGeneration](EncoderSnapshot& value)
                    {
                        if (value.runGeneration == runGeneration)
                        {
                            value.monitorSafetyPreflightPassed = true;
                            value.monitorSafetyRevalidationCount++;
                            value.monitorSafetyStatus = "PASS";
                        }
                    });
                }
                nextMonitorSafetyCheck = now + std::chrono::seconds(1);
            }
            const bool presentationStable = HasStablePresentationContract(windowSnapshot,
                presentationWidth, presentationHeight);
            if (!broadcastStarted && presentationStable)
            {
                broadcastStarted = true;
                broadcastStartedAt = now;
                snapshot_.Update([runGeneration](EncoderSnapshot& value)
                {
                    if (value.runGeneration == runGeneration && value.state == EncoderState::Preparing)
                    {
                        value.state = EncoderState::Broadcasting;
                        value.statusMessage = "Broadcasting continuously; receiver completion is visible only on Decoder";
                    }
                });
            }
            const bool canBuild = presentationStable && windowSnapshot.state == pbrenderd3d::WindowState::Running &&
                !frameBuilt;
            if (canBuild)
            {
                static_cast<void>(builder.Build(frameSequence));
                if (config.singleMonitorFullscreen)
                {
                    ComposeRemoteVisualFullscreenBgra(builder.GetBuiltPixels(), presentationWidth,
                        presentationHeight, fullscreenPixels);
                }
                frameBuilt = true;
            }
            const bool logicalFrameReady = logicalFrameInterval == std::chrono::steady_clock::duration::zero() || now >= nextLogicalFrameAt;
            if (presentationStable && windowSnapshot.state == pbrenderd3d::WindowState::Running && frameBuilt && logicalFrameReady &&
                !windowSnapshot.pendingFrame && !stopRequested_)
            {
                const std::uint32_t outerBlockId = builder.GetCurrentOuterBlockId();
                const std::span<const std::byte> pixels = config.singleMonitorFullscreen ?
                    std::span<const std::byte>(fullscreenPixels) : std::span<const std::byte>(builder.GetBuiltPixels());
                const auto submit = window->SubmitFrame({pixels, presentationWidth, presentationHeight,
                    static_cast<std::size_t>(presentationWidth) * 4U, frameSequence,
                    windowSnapshot.timing.presentationEpoch});
                if (submit)
                {
                    if (previousLogicalFrameAt)
                    {
                        const double observedDwellMilliseconds =
                            std::chrono::duration<double, std::milli>(now - *previousLogicalFrameAt).count();
                        minimumObservedLogicalDwellMilliseconds = minimumObservedLogicalDwellMilliseconds ?
                            std::min(*minimumObservedLogicalDwellMilliseconds, observedDwellMilliseconds) : observedDwellMilliseconds;
                        if (configuredLogicalDwellMilliseconds && observedDwellMilliseconds < *configuredLogicalDwellMilliseconds)
                        {
                            pbprotocol::SaturatingIncrementUnsigned(logicalDwellViolationCount);
                        }
                    }
                    if (!firstLogicalFrameAt)
                    {
                        firstLogicalFrameAt = now;
                    }
                    previousLogicalFrameAt = now;
                    const auto nextGeneratedPayloadBytes = pbprotocol::CheckedAddUint64(generatedPayloadBytes,
                        builder.GetGeneratedPayloadBytesInFrame());
                    RequireResult(nextGeneratedPayloadBytes, "generated payload telemetry overflow");
                    generatedPayloadBytes = nextGeneratedPayloadBytes.Value();
                    Require(frameSequence != (std::numeric_limits<std::uint64_t>::max)(),
                        "FrameSequence exhausted");
                    builder.Advance();
                    frameSequence++;
                    frameBuilt = false;
                    nextLogicalFrameAt = now + logicalFrameInterval;
                    const CarouselSnapshot carouselAfter = builder.GetCarouselSnapshot();
                    Require(broadcastStartedAt.has_value(), "stable presentation has no broadcast start timestamp");
                    const std::uint64_t broadcastMilliseconds = ElapsedMilliseconds(*broadcastStartedAt);
                    const double elapsedSeconds = static_cast<double>(broadcastMilliseconds) / 1000.0;
                    const double logicalObservationSeconds = firstLogicalFrameAt ?
                        std::chrono::duration<double>(now - *firstLogicalFrameAt).count() : 0;
                    snapshot_.Update([&](EncoderSnapshot& value)
                    {
                        if (value.runGeneration != runGeneration)
                        {
                            return;
                        }
                        value.broadcastRuntimeMilliseconds = broadcastMilliseconds;
                        value.cycleCount = carouselAfter.cycleCount;
                        value.cyclePosition = carouselAfter.cyclePosition;
                        value.cycleFrameCount = carouselAfter.cycleFrameCount;
                        value.currentOuterBlockId = outerBlockId;
                        value.frameSequence = frameSequence;
                        ApplyEncoderPresentationSnapshot(windowSnapshot, value);
                        value.generatedVisualFramesPerSecond = frameSequence > 1 && logicalObservationSeconds > 0 ?
                            std::optional<double>(static_cast<double>(frameSequence - 1) / logicalObservationSeconds) : std::nullopt;
                        value.generatedPayloadBytesPerSecond = elapsedSeconds > 0 ?
                            std::optional<double>(static_cast<double>(generatedPayloadBytes) / elapsedSeconds) : std::nullopt;
                        value.minimumObservedLogicalDwellMilliseconds = minimumObservedLogicalDwellMilliseconds;
                        value.logicalDwellViolationCount = logicalDwellViolationCount;
                        value.pendingFrames = 1;
                        value.pendingHighWater = 1;
                    });
                }
                else if (submit.code != pbrenderd3d::PresentationErrorCode::EpochMismatch &&
                    submit.code != pbrenderd3d::PresentationErrorCode::Paused &&
                    submit.code != pbrenderd3d::PresentationErrorCode::NotRunning)
                {
                    throw RuntimeFailure("DataWindow SubmitFrame failed: " + DescribePresentationStatus(submit));
                }
            }
            else
            {
                const std::uint64_t broadcastMilliseconds = broadcastStartedAt ?
                    ElapsedMilliseconds(*broadcastStartedAt) : 0;
                snapshot_.Update([&](EncoderSnapshot& value)
                {
                    if (value.runGeneration != runGeneration)
                    {
                        return;
                    }
                    value.broadcastRuntimeMilliseconds = broadcastMilliseconds;
                    ApplyEncoderPresentationSnapshot(windowSnapshot, value);
                    value.minimumObservedLogicalDwellMilliseconds = minimumObservedLogicalDwellMilliseconds;
                    value.logicalDwellViolationCount = logicalDwellViolationCount;
                });
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        window->Stop();
        const pbrenderd3d::DataWindowSnapshot stopped = window->GetSnapshot();
        Require(stopped.state == pbrenderd3d::WindowState::Stopped &&
            !stopped.pendingFrame && !stopped.inFlightFrame,
            "DataWindow did not complete a clean bounded shutdown");
        Require(stopRequested_, "DataWindow stopped without an explicit user stop request");
        sourceStable = IsSourceStable(source);
        Require(sourceStable, "源文件在 Session 广播期间发生变化，Session 已中止");
        const std::uint64_t broadcastMilliseconds = broadcastStartedAt ?
            ElapsedMilliseconds(*broadcastStartedAt) : 0;
        snapshot_.Update([&](EncoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration)
            {
                return;
            }
            value.state = EncoderState::Stopped;
            value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
            value.broadcastRuntimeMilliseconds = broadcastMilliseconds;
            value.sourceStable = true;
            ApplyEncoderPresentationSnapshot(stopped, value);
            value.minimumObservedLogicalDwellMilliseconds = minimumObservedLogicalDwellMilliseconds;
            value.logicalDwellViolationCount = logicalDwellViolationCount;
            value.statusMessage = "Broadcast stopped by user; no sender-side receiver completion was inferred";
        });
    }
    catch (const std::exception& exception)
    {
        try
        {
            snapshot_.Update([&](EncoderSnapshot& value)
            {
                if (value.runGeneration != runGeneration)
                {
                    return;
                }
                value.state = EncoderState::Failed;
                value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                value.statusMessage = "Encoder failed";
                value.errorDetail = exception.what();
                value.sourceStable = sourceStable;
                if (config.visualProfile == VisualProfile::RemoteVisualLowFps)
                {
                    value.monitorSafetyStatus = config.singleMonitorFullscreen ?
                        "SingleMonitorFullscreenFailed" : "FAIL";
                }
            });
        }
        catch (...)
        {
        }
    }
    catch (...)
    {
        try
        {
            snapshot_.Update([runGeneration, &config](EncoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = EncoderState::Failed;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.statusMessage = "Encoder failed";
                    value.errorDetail = "unknown non-standard exception";
                    if (config.visualProfile == VisualProfile::RemoteVisualLowFps)
                    {
                        value.monitorSafetyStatus = config.singleMonitorFullscreen ?
                            "SingleMonitorFullscreenFailed" : "FAIL";
                    }
                }
            });
        }
        catch (...)
        {
        }
    }
}

DecoderRuntime::~DecoderRuntime()
{
    Stop();
}

RuntimeStatus DecoderRuntime::Start(const DecoderConfig& config)
{
    const RuntimeStatus validation = ValidateDecoderConfig(config);
    if (!validation)
    {
        return validation;
    }
    std::unique_lock lock(lifecycleMutex_);
    if (workerRunning_)
    {
        return RuntimeStatus::Failure("Decoder 已处于接收/停止流程，拒绝重复 Start");
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
    if (nextRunGeneration_ == 0)
    {
        return RuntimeStatus::Failure("run generation exhausted");
    }
    const std::uint64_t runGeneration = nextRunGeneration_++;
    stopRequested_ = false;
    DecoderSnapshot initial;
    initial.state = DecoderState::WaitingForBootstrap;
    initial.runGeneration = runGeneration;
    initial.runId = config.runId.empty() ? "pending" : config.runId;
    initial.runStartedUnixMilliseconds = GetUnixTimeMilliseconds();
    initial.requestedBackend = config.captureBackend;
    initial.visualProfile = config.visualProfile;
    initial.remoteMetadata = config.remoteMetadata;
    const bool offlineReplay = !config.replayInputPath.empty();
    if (offlineReplay)
    {
        initial.backendReason = "Offline Replay v2 adapter; no live WGC/DXGI capture and no monitor pixels are accessed";
        initial.statusMessage = "Validating sealed Replay v2 before production demodulation";
        initial.monitorSafetyStatus = "NotApplicableOfflineReplay";
    }
    else
    {
        initial.backendReason = "Starting the explicitly requested backend; no fallback policy is enabled";
        initial.roiLeft = config.region.physicalRect.left;
        initial.roiTop = config.region.physicalRect.top;
        initial.roiWidth = static_cast<std::uint32_t>(static_cast<std::int64_t>(config.region.physicalRect.right) -
            config.region.physicalRect.left);
        initial.roiHeight = static_cast<std::uint32_t>(static_cast<std::int64_t>(config.region.physicalRect.bottom) -
            config.region.physicalRect.top);
        initial.monitorLeft = config.region.monitorPhysicalRect.left;
        initial.monitorTop = config.region.monitorPhysicalRect.top;
        initial.monitorWidth = static_cast<std::uint32_t>(static_cast<std::int64_t>(config.region.monitorPhysicalRect.right) -
            config.region.monitorPhysicalRect.left);
        initial.monitorHeight = static_cast<std::uint32_t>(static_cast<std::int64_t>(config.region.monitorPhysicalRect.bottom) -
            config.region.monitorPhysicalRect.top);
        initial.dpiX = config.region.dpiX;
        initial.dpiY = config.region.dpiY;
        initial.rotation = static_cast<std::uint32_t>(config.region.rotation);
        initial.statusMessage = config.diagnosticCaptureOnly ?
            "Recording RemoteVisual ROI in bounded replay capture-only mode; Bootstrap/demod/FEC/Receiver/publish disabled" :
            "Waiting for same-profile Bootstrap pixels";
        initial.monitorSafetyPreflightPassed = false;
        initial.monitorSafetyStatus = config.remoteMetadata.channelType == ChannelType::RemoteVisual ?
            "Pending" : "NotRequired";
    }
    initial.replayEnabled = offlineReplay || !config.replayOutputPath.empty();
    initial.replayDiagnosticOnly = initial.replayEnabled;
    initial.replayCaptureOnly = config.diagnosticCaptureOnly;
    initial.replayOfflineMode = offlineReplay;
    initial.replayMaximumCaptureFramesPerSecond = config.replayMaximumCaptureFramesPerSecond;
    initial.replaySamplingInterval100ns = CalculateReplaySamplingInterval100ns(
        config.replayMaximumCaptureFramesPerSecond);
    const std::wstring& replayPath = offlineReplay ? config.replayInputPath : config.replayOutputPath;
    initial.replayPath = replayPath.empty() ? "" : Utf8FromWide(replayPath);
    try
    {
        const ProfileBinding profile = GetProfileBinding(config.visualProfile);
        initial.visualProfileId = profile.visualProfileId;
        initial.visualLayoutVersion = profile.layoutVersion;
        initial.codedDataBytesPerFrame = profile.dataBytes;
        initial.codewordsPerFrame = profile.codewords;
        snapshot_.Replace(std::move(initial));
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(std::string("无法初始化 Decoder snapshot：") + exception.what());
    }
    catch (...)
    {
        return RuntimeStatus::Failure("无法初始化 Decoder snapshot");
    }
    workerRunning_ = true;
    try
    {
        worker_ = std::thread(&DecoderRuntime::Run, this, config, runGeneration);
    }
    catch (const std::exception& exception)
    {
        workerRunning_ = false;
        try
        {
            snapshot_.Update([runGeneration, &exception](DecoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = DecoderState::Failed;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.errorDetail = exception.what();
                }
            });
        }
        catch (...)
        {
        }
        return RuntimeStatus::Failure("无法创建 Decoder worker thread");
    }
    return {};
}

void DecoderRuntime::RequestStop() noexcept
{
    stopRequested_ = true;
    try
    {
        const std::scoped_lock lock(lifecycleMutex_);
        stopRequested_ = true;
        snapshot_.Update([](DecoderSnapshot& value)
        {
            if (value.state != DecoderState::Idle && value.state != DecoderState::Completed &&
                value.state != DecoderState::Failed && value.state != DecoderState::Stopped)
            {
                value.state = DecoderState::Stopping;
            }
        });
    }
    catch (...)
    {
    }
}

void DecoderRuntime::Stop() noexcept
{
    RequestStop();
    try
    {
        const std::scoped_lock lock(lifecycleMutex_);
        stopRequested_ = true;
        if (worker_.joinable())
        {
            worker_.join();
        }
    }
    catch (...)
    {
        std::terminate();
    }
}

DecoderSnapshot DecoderRuntime::GetSnapshot() const
{
    return snapshot_.Get();
}

void DecoderRuntime::Run(const DecoderConfig& config, const std::uint64_t runGeneration) noexcept
{
    WorkerRunningGuard runningGuard(workerRunning_);
    AuthoritativeCompletion completion;
    try
    {
        const auto started = std::chrono::steady_clock::now();
        ProcessResourceSampler resourceSampler;
        if (config.replayInputPath.empty() && config.monitorSafety)
        {
            const MonitorSafetyStatus monitorSafety = RevalidateMonitorSafetySelection(*config.monitorSafety);
            Require(static_cast<bool>(monitorSafety), std::string("display topology changed before RemoteVisual startup: ") +
                GetMonitorSafetyErrorName(monitorSafety.code));
            snapshot_.Update([runGeneration](DecoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.monitorSafetyPreflightPassed = true;
                    value.monitorSafetyRevalidationCount++;
                    value.monitorSafetyStatus = "PASS";
                }
            });
        }
        std::unique_ptr<pbrealcapturereplay::ReplayV2Reader> replayReader;
        pbrealcapturereplay::ReplayV2FileSnapshot replayFileSnapshot;
        if (!config.replayInputPath.empty())
        {
            pbrealcapturereplay::ReplayV2Limits limits;
            limits.maximumFileBytes = config.replayMaximumFileBytes;
            limits.maximumTotalRasterBytes = config.replayMaximumFileBytes;
            limits.maximumCaptureFrames = config.replayMaximumCaptureFrames;
            const auto openStatus = pbrealcapturereplay::ReplayV2Reader::Open(
                std::filesystem::path(config.replayInputPath), limits, replayReader);
            Require(static_cast<bool>(openStatus), "Replay v2 sealed input validation failed: " +
                DescribeReplayStatus(openStatus));
            replayFileSnapshot = replayReader->GetSnapshot();
            Require(replayFileSnapshot.descriptor.datasetClass ==
                pbrealcapturereplay::ReplayDatasetClass::RemoteVisual,
                "Replay v2 dataset class is not RemoteVisual");
            Require(config.runId.empty() || config.runId == replayFileSnapshot.descriptor.runId,
                "Configured RunId does not match the sealed Replay v2 descriptor");
        }
        const std::string runId = replayReader ? replayFileSnapshot.descriptor.runId :
            config.runId.empty() ? GenerateRunId() : config.runId;
        snapshot_.Update([&](DecoderSnapshot& value)
        {
            if (value.runGeneration == runGeneration)
            {
                value.runId = runId;
                value.remoteMetadata.runId = runId;
                if (replayReader)
                {
                    value.replayFileBytes = replayFileSnapshot.fileBytes;
                    value.replayEvidenceValid = true;
                }
            }
        });
        if (stopRequested_)
        {
            snapshot_.Update([runGeneration](DecoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = DecoderState::Stopped;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.statusMessage = "Stopped before capture startup";
                }
            });
            return;
        }
        const ProfileBinding profile = GetProfileBinding(config.visualProfile);
        if (replayReader)
        {
            Require(replayFileSnapshot.descriptor.visualProfileId == profile.visualProfileId,
                "Replay v2 descriptor VisualProfileId does not match the configured production profile");
        }
        if (config.diagnosticCaptureOnly)
        {
            Require(!replayReader, "diagnostic capture-only cannot consume an offline Replay input");
            RunRemoteVisualDiagnosticCapture(config, profile, runId, snapshot_, runGeneration, started,
                resourceSampler, stopRequested_);
            return;
        }
        const pbprotocol::ReceiverResourcePolicy policy = pbprotocol::GetDefaultReceiverResourcePolicy();
        auto receiverResult = pbreceiver::ReceiverIngress::Create(policy, outerBlockBytes);
        RequireResult(receiverResult, "ReceiverIngress creation failed");
        pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
        ReceiverPipeline pipeline(receiver, config.outputDirectory, policy, snapshot_, completion, runGeneration,
            started, config.visualProfile, replayReader != nullptr);

        const pbdemodd3d11::CaptureDemodulatorConfig demodConfig = MakeCaptureDemodulatorConfig(
            config, profile, replayReader != nullptr);
        std::shared_ptr<pbdemodd3d11::CaptureDemodulator> demodulator;
        const auto demodStatus = pbdemodd3d11::CaptureDemodulator::Create(demodConfig, demodulator);
        Require(static_cast<bool>(demodStatus), "CaptureDemodulator creation failed: " +
            DescribeCaptureStatus(demodStatus));

        if (replayReader)
        {
            RunOfflineReplayDataset(config, *replayReader, profile, receiver, pipeline, demodulator,
                snapshot_, runGeneration, started, resourceSampler, stopRequested_);
            return;
        }

        const auto captureConfig = MakeCaptureConfig(config);
        std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> consumer = demodulator;
        std::shared_ptr<RemoteVisualReplayRecorder> replayRecorder;
        std::shared_ptr<pbcapturenormalize::DiagnosticCpuReadback> replayReadback;
        std::shared_ptr<OptionalDiagnosticFanout> replayFanout;
        if (!config.replayOutputPath.empty())
        {
            const std::uint32_t roiWidth = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(config.region.physicalRect.right) - config.region.physicalRect.left);
            const std::uint32_t roiHeight = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(config.region.physicalRect.bottom) - config.region.physicalRect.top);
            const auto datasetId = pbprotocol::GenerateRandomSessionId();
            RequireResult(datasetId, "Replay dataset CSPRNG failed");
            pbrealcapturereplay::ReplayV2FileDescriptor replayDescriptor;
            replayDescriptor.datasetClass = pbrealcapturereplay::ReplayDatasetClass::RemoteVisual;
            replayDescriptor.datasetId = datasetId.Value().bytes;
            replayDescriptor.runId = runId;
            replayDescriptor.visualProfileId = profile.visualProfileId;
            replayDescriptor.createdUtc100ns = GetUtcFileTime100ns();
            replayDescriptor.remoteMetadataJsonUtf8 = BuildRemoteVisualRunMetadataJson(config.remoteMetadata);
            Require(replayDescriptor.createdUtc100ns > 0, "Replay UTC timestamp acquisition failed");

            RemoteVisualReplayRecorderConfig recorderConfig;
            recorderConfig.outputPath = config.replayOutputPath;
            recorderConfig.descriptor = std::move(replayDescriptor);
            recorderConfig.limits.maximumFileBytes = config.replayMaximumFileBytes;
            recorderConfig.limits.maximumTotalRasterBytes = config.replayMaximumFileBytes;
            recorderConfig.limits.maximumCaptureFrames = config.replayMaximumCaptureFrames;
            recorderConfig.roiWidth = roiWidth;
            recorderConfig.roiHeight = roiHeight;
            recorderConfig.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            recorderConfig.displayIdentityUtf8 = !config.remoteMetadata.experimentMonitorIdentity.empty() ?
                config.remoteMetadata.experimentMonitorIdentity : Utf8FromWide(config.monitorSafety->experimentMonitor.deviceName);
            recorderConfig.dpiX = config.region.dpiX;
            recorderConfig.dpiY = config.region.dpiY;
            recorderConfig.scaleX = config.remoteMetadata.estimatedScaleX.value_or(
                static_cast<double>(roiWidth) / phase1CanvasWidth);
            recorderConfig.scaleY = config.remoteMetadata.estimatedScaleY.value_or(
                static_cast<double>(roiHeight) / phase1CanvasHeight);
            recorderConfig.queueCapacity = remoteVisualReplayRecorderQueueCapacity;
            const auto recorderStatus = RemoteVisualReplayRecorder::Create(recorderConfig, replayRecorder);
            Require(static_cast<bool>(recorderStatus), "Replay recorder creation failed: " +
                DescribeCaptureStatus(recorderStatus));
            Require(replayRecorder->ProcessingReservedBytes() <=
                maximumProductionReplayProcessorResidentBytes,
                "Replay recorder exceeded the preflighted bounded processing reservation");

            const pbcapturenormalize::DiagnosticReadbackConfig readbackConfig =
                MakeProductionReplayReadbackConfig(config, captureConfig,
                    replayRecorder->ProcessingReservedBytes());
            const auto readbackStatus = pbcapturenormalize::DiagnosticCpuReadback::Create(
                readbackConfig, replayRecorder, replayReadback);
            Require(static_cast<bool>(readbackStatus), "Replay diagnostic readback creation failed: " +
                DescribeCaptureStatus(readbackStatus));
            replayFanout = std::make_shared<OptionalDiagnosticFanout>(demodulator, replayReadback);
            consumer = replayFanout;
        }
        NativeCaptureSession capture(config.captureBackend, captureConfig, consumer);
        snapshot_.Update([&](DecoderSnapshot& value)
        {
            if (value.runGeneration == runGeneration)
            {
                value.actualBackend = config.captureBackend;
                value.backendReason = "Explicit backend selected; no fallback policy was used";
            }
        });
        std::uint64_t receiverCaptureEpoch = 1;
        auto nextMonitorSafetyCheck = started;
        std::optional<std::chrono::steady_clock::time_point> replayTailDeadline;
        for (;;)
        {
            const auto now = std::chrono::steady_clock::now();
            if (replayTailDeadline && now >= *replayTailDeadline)
            {
                capture.RequestStop();
                break;
            }
            if (config.monitorSafety && now >= nextMonitorSafetyCheck)
            {
                const MonitorSafetyStatus monitorSafety = RevalidateMonitorSafetySelection(*config.monitorSafety);
                Require(static_cast<bool>(monitorSafety), std::string("display topology changed during RemoteVisual run: ") +
                    GetMonitorSafetyErrorName(monitorSafety.code));
                snapshot_.Update([runGeneration](DecoderSnapshot& value)
                {
                    if (value.runGeneration == runGeneration)
                    {
                        value.monitorSafetyPreflightPassed = true;
                        value.monitorSafetyRevalidationCount++;
                        value.monitorSafetyStatus = "PASS";
                    }
                });
                nextMonitorSafetyCheck = now + std::chrono::seconds(1);
            }
            resourceSampler.Sample(ElapsedMilliseconds(started));
            snapshot_.Update([&](DecoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    ApplyProcessResourceSample(resourceSampler.GetSnapshot(), value);
                }
            });
            if (stopRequested_)
            {
                capture.RequestStop();
                break;
            }
            std::uint32_t drained = 0;
            pbdemodd3d11::CaptureDemodulatorResult result;
            while (!stopRequested_ && drained < pbdemodd3d11::maximumCaptureDemodResultQueue &&
                demodulator->TakeResult(result))
            {
                if (stopRequested_)
                {
                    break;
                }
                const std::uint64_t resultEpoch = result.metadata.domain.captureEpoch;
                Require(resultEpoch >= receiverCaptureEpoch,
                    "stale CaptureEpoch result escaped the bounded CaptureDemodulator queue");
                if (resultEpoch > receiverCaptureEpoch)
                {
                    const auto reset = receiver.ResetCaptureEpoch(outerBlockBytes);
                    RequireResult(reset, "ReceiverIngress CaptureEpoch reset failed");
                    Require(reset.Value(), "ReceiverIngress CaptureEpoch reset made no state transition");
                    receiverCaptureEpoch = resultEpoch;
                    pipeline.CaptureEpochReset(ElapsedMilliseconds(started));
                }
                ReceiverProcessResult processResult;
                if (!pipeline.IsCompleted())
                {
                    processResult = pipeline.Process(result);
                }
                if (replayRecorder && config.replayMaximumCaptureFramesPerSecond == 0 &&
                    !replayTailDeadline)
                {
                    replayRecorder->RecordDemodObservation(
                        MakeReplayDemodObservation(result, profile.visualProfileId, processResult));
                }
                drained++;
                if (pipeline.IsCompleted())
                {
                    if (!replayRecorder)
                    {
                        capture.RequestStop();
                        break;
                    }
                    if (!replayTailDeadline)
                    {
                        replayTailDeadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(remoteVisualReplayPostCompletionTailMilliseconds);
                    }
                    break;
                }
            }
            const pbcapturenormalize::CaptureSnapshot captureSnapshot = capture.GetSnapshot();
            const pbdemodd3d11::CaptureDemodulatorSnapshot demodSnapshot = demodulator->GetSnapshot();
            pipeline.ObserveDroppedFrames(captureSnapshot.droppedFrames,
                demodSnapshot.resultQueueDrops, demodSnapshot.staleResultDrops);
            if (captureSnapshot.state == pbcapturenormalize::CaptureState::Failed ||
                (demodSnapshot.error.code != pbcapturenormalize::CaptureError::None &&
                 captureSnapshot.state != pbcapturenormalize::CaptureState::Recreating))
            {
                throw RuntimeFailure("capture/demod entered terminal failure: capture=" +
                    DescribeCaptureStatus(captureSnapshot.error) + " consumer=" +
                    DescribeCaptureStatus(demodSnapshot.error));
            }
            snapshot_.Update([&](DecoderSnapshot& value)
            {
                if (value.runGeneration != runGeneration)
                {
                    return;
                }
                ApplyCaptureComponentSnapshot(captureSnapshot, value);
                value.bootstrapAcceptedFrames = demodSnapshot.bootstrapAcceptedFrames;
                value.bootstrapRejectedFrames = demodSnapshot.bootstrapRejectedFrames;
                value.bootstrapMismatchFrames = demodSnapshot.bootstrapErasures[
                    static_cast<std::size_t>(pbmodulation::LocalDesktopErasureReason::BootstrapMismatch)];
                value.bootstrapControlFrameFailures = demodSnapshot.controlFrameFailures;
                value.demodPendingHighWater = demodSnapshot.pendingHighWater;
                value.resultQueueHighWater = demodSnapshot.resultQueueHighWater;
                value.staleResultDrops = demodSnapshot.staleResultDrops;
                value.demodGpuTimeTotal100ns = demodSnapshot.demodulator.gpuTimeTotal100ns;
                value.bootstrapCpuTimeTotal100ns = demodSnapshot.bootstrapCpuTimeTotal100ns;
                value.postGpuFecCpuTimeTotal100ns = demodSnapshot.demodulationCpuTimeTotal100ns;
                value.recoveryRuntimeMilliseconds = ElapsedMilliseconds(started);
                if (replayRecorder)
                {
                    ApplyReplaySnapshot(replayRecorder->GetSnapshot(), value);
                    if (replayReadback)
                    {
                        const auto readback = replayReadback->GetSnapshot();
                        value.captureReadbackDropEvents = readback.dropEvents;
                        value.replaySampledOutFrames = readback.sampledOutFrames;
                        value.replayDroppedFrames = pbprotocol::SaturatingAddUnsigned(value.replayDroppedFrames,
                            readback.dropEvents);
                    }
                    if (replayFanout && !replayFanout->GetDiagnosticStatus())
                    {
                        value.replayEvidenceValid = false;
                        value.replayError = "Diagnostic fan-out failure: " +
                            DescribeCaptureStatus(replayFanout->GetDiagnosticStatus());
                    }
                }
            });
            pipeline.ObserveStall(ElapsedMilliseconds(started), captureSnapshot.arrivedFrames);
            if (pipeline.IsCompleted())
            {
                if (!replayRecorder)
                {
                    capture.RequestStop();
                    break;
                }
                if (!replayTailDeadline)
                {
                    replayTailDeadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(remoteVisualReplayPostCompletionTailMilliseconds);
                }
            }
            if (stopRequested_)
            {
                capture.RequestStop();
                break;
            }
            if (drained == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            else
            {
                std::this_thread::yield();
            }
        }
        const auto stopStatus = capture.Stop();
        Require(static_cast<bool>(stopStatus), "capture shutdown failed: " + DescribeCaptureStatus(stopStatus));
        if (replayReadback)
        {
            const auto readbackStopStatus = replayReadback->Stop();
            if (!readbackStopStatus)
            {
                snapshot_.Update([&](DecoderSnapshot& value)
                {
                    if (value.runGeneration == runGeneration)
                    {
                        value.replayEvidenceValid = false;
                        value.replayError = "Replay readback shutdown failed: " + DescribeCaptureStatus(readbackStopStatus);
                    }
                });
            }
        }
        if (replayRecorder)
        {
            const auto recorderStopStatus = replayRecorder->Stop();
            snapshot_.Update([&](DecoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    ApplyReplaySnapshot(replayRecorder->GetSnapshot(), value);
                    if (!recorderStopStatus)
                    {
                        value.replayEvidenceValid = false;
                        if (value.replayError.empty())
                        {
                            value.replayError = "Replay recorder finalize failed: " +
                                DescribeCaptureStatus(recorderStopStatus);
                        }
                    }
                }
            });
        }
        const pbcapturenormalize::CaptureSnapshot stoppedCapture = capture.GetSnapshot();
        const pbdemodd3d11::CaptureDemodulatorSnapshot stoppedDemod = demodulator->GetSnapshot();
        pipeline.ObserveDroppedFrames(stoppedCapture.droppedFrames,
            stoppedDemod.resultQueueDrops, stoppedDemod.staleResultDrops);
        pipeline.EndCaptureTelemetry(ElapsedMilliseconds(started));
        snapshot_.Update([&](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration)
            {
                return;
            }
            ApplyCaptureComponentSnapshot(stoppedCapture, value);
            value.demodPendingHighWater = stoppedDemod.pendingHighWater;
            value.resultQueueHighWater = stoppedDemod.resultQueueHighWater;
            value.staleResultDrops = stoppedDemod.staleResultDrops;
            if (replayReadback)
            {
                const auto finalReplayReadback = replayReadback->GetSnapshot();
                value.captureReadbackDropEvents = finalReplayReadback.dropEvents;
                value.replaySampledOutFrames = finalReplayReadback.sampledOutFrames;
                value.replayDroppedFrames = pbprotocol::SaturatingAddUnsigned(
                    replayRecorder ? replayRecorder->GetSnapshot().droppedFrames : 0,
                    finalReplayReadback.dropEvents);
            }
        });
        Require(stoppedCapture.shutdownComplete && !stoppedCapture.deferredCleanup &&
            stoppedCapture.liveFrameLeases == 0 && stoppedCapture.busyRoiTextures == 0 &&
            stoppedDemod.demodulator.shutdown && stoppedDemod.pendingFrames == 0 &&
            stoppedDemod.queuedResults == 0,
            "capture/demod shutdown did not retire all bounded resources");
        if (!pipeline.IsCompleted())
        {
            snapshot_.Update([&](DecoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = DecoderState::Stopped;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.recoveryRuntimeMilliseconds = ElapsedMilliseconds(started);
                    value.statusMessage = "Receive stopped by user; no final file was published";
                }
            });
        }
    }
    catch (const std::exception& exception)
    {
        try
        {
            snapshot_.Update([&](DecoderSnapshot& value)
            {
                if (value.runGeneration != runGeneration)
                {
                    return;
                }
                value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                if (completion.published)
                {
                    value.state = DecoderState::Completed;
                    value.wholeFileDigestVerified = true;
                    value.finalPublishSucceeded = true;
                    value.wholeFileDigestHex = completion.wholeFileDigestHex;
                    value.outputPath = completion.outputPath;
                    value.recoveryRuntimeMilliseconds = completion.recoveryRuntimeMilliseconds;
                    value.statusMessage = "文件已权威发布；后续 capture/demod 清理或完成快照记录存在警告";
                    value.errorDetail = std::string("Post-publish cleanup warning: ") + exception.what();
                    if ((!config.replayOutputPath.empty() || !config.replayInputPath.empty()) &&
                        !value.replayFinalized)
                    {
                        value.replayEvidenceValid = false;
                        if (value.replayError.empty())
                        {
                            value.replayError = std::string("Post-publish Replay finalization failed: ") +
                                exception.what();
                        }
                    }
                    return;
                }
                value.state = DecoderState::Failed;
                value.statusMessage = "Decoder failed; final file was not accepted";
                value.errorDetail = exception.what();
                if (config.monitorSafety)
                {
                    value.monitorSafetyStatus = "FAIL";
                }
                if (!value.actualBackend)
                {
                    value.backendReason = config.replayInputPath.empty() ?
                        std::string(GetCaptureBackendName(value.requestedBackend)) +
                            " startup failed; no fallback was attempted: " + exception.what() :
                        std::string("Offline Replay v2 failed; no live-capture or adapter fallback was attempted: ") +
                            exception.what();
                }
            });
        }
        catch (...)
        {
        }
    }
    catch (...)
    {
        try
        {
            snapshot_.Update([&](DecoderSnapshot& value)
            {
                if (value.runGeneration != runGeneration)
                {
                    return;
                }
                value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                if (completion.published)
                {
                    value.state = DecoderState::Completed;
                    value.wholeFileDigestVerified = true;
                    value.finalPublishSucceeded = true;
                    value.wholeFileDigestHex = completion.wholeFileDigestHex;
                    value.outputPath = completion.outputPath;
                    value.recoveryRuntimeMilliseconds = completion.recoveryRuntimeMilliseconds;
                    value.statusMessage = "文件已权威发布；后续发生未知清理警告";
                    value.errorDetail = "Post-publish unknown non-standard exception";
                    if ((!config.replayOutputPath.empty() || !config.replayInputPath.empty()) &&
                        !value.replayFinalized)
                    {
                        value.replayEvidenceValid = false;
                        if (value.replayError.empty())
                        {
                            value.replayError = "Post-publish Replay finalization failed: unknown non-standard exception";
                        }
                    }
                }
                else
                {
                    value.state = DecoderState::Failed;
                    value.statusMessage = "Decoder failed; final file was not accepted";
                    value.errorDetail = "unknown non-standard exception";
                    if (config.monitorSafety)
                    {
                        value.monitorSafetyStatus = "FAIL";
                    }
                }
            });
        }
        catch (...)
        {
        }
    }
}

} // namespace pbapp
