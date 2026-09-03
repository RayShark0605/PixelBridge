#include "pbrenderd3d/data_window.h"
#include "presentation_backend.h"
#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace pbrenderd3d
{

PresentationViewportGeometry ResolvePresentationViewport(const DataWindowConfig& config,
    const std::uint32_t clientWidth, const std::uint32_t clientHeight) noexcept
{
    if (config.width == 0 || config.height == 0 || clientWidth == 0 || clientHeight == 0)
    {
        return {};
    }
    const std::uint64_t scaledClientWidth = static_cast<std::uint64_t>(clientWidth) * dataWindowMinimumScaleDenominator;
    const std::uint64_t scaledClientHeight = static_cast<std::uint64_t>(clientHeight) * dataWindowMinimumScaleDenominator;
    const std::uint64_t minimumWidth = static_cast<std::uint64_t>(config.width) * dataWindowMinimumScaleNumerator;
    const std::uint64_t minimumHeight = static_cast<std::uint64_t>(config.height) * dataWindowMinimumScaleNumerator;
    if (scaledClientWidth < minimumWidth || scaledClientHeight < minimumHeight)
    {
        return PresentationViewportGeometry{PresentationViewportDisposition::PausedBelowMinimumScale};
    }
    const double widthScale = static_cast<double>(clientWidth) / config.width;
    const double heightScale = static_cast<double>(clientHeight) / config.height;
    const double availableScale = (std::min)(widthScale, heightScale);
    const double maximumScale = static_cast<double>(dataWindowMaximumScaleNumerator) / dataWindowMaximumScaleDenominator;
    const bool clamped = availableScale > maximumScale;
    const double scale = clamped ? maximumScale : availableScale;
    const double viewportWidth = config.width * scale;
    const double viewportHeight = config.height * scale;
    return {clamped ? PresentationViewportDisposition::ActiveClampedToMaximumScale : PresentationViewportDisposition::Active,
        (static_cast<double>(clientWidth) - viewportWidth) / 2.0,
        (static_cast<double>(clientHeight) - viewportHeight) / 2.0, viewportWidth, viewportHeight, scale};
}

bool CanPresentData(const PresentationViewportDisposition disposition) noexcept
{
    return disposition == PresentationViewportDisposition::Active ||
        disposition == PresentationViewportDisposition::ActiveClampedToMaximumScale;
}

namespace
{

[[nodiscard]] bool CanOwnClientBackBuffer(const DataWindowConfig& config, const WindowEnvironment& environment) noexcept
{
    if (environment.clientWidth == 0 || environment.clientHeight == 0 || environment.clientWidth > 16384 ||
        environment.clientHeight > 16384 || !environment.singleMonitor || !environment.adapterAvailable ||
        environment.minimized || environment.closed)
    {
        return false;
    }
    const auto pixelCount = pbprotocol::CheckedMultiplyUint64(environment.clientWidth, environment.clientHeight);
    const auto frameBytes = pixelCount ? pbprotocol::CheckedMultiplyUint64(pixelCount.Value(), 4) : pixelCount;
    return frameBytes && frameBytes.Value() <= config.maximumFrameBytes;
}

[[nodiscard]] bool CanDisplay(const DataWindowConfig& config, const WindowEnvironment& environment) noexcept
{
    return CanOwnClientBackBuffer(config, environment) && !environment.occluded &&
        CanPresentData(ResolvePresentationViewport(config, environment.clientWidth, environment.clientHeight).disposition);
}

[[nodiscard]] bool CanShowNeutralMatte(const DataWindowConfig& config, const WindowEnvironment& environment) noexcept
{
    return CanOwnClientBackBuffer(config, environment) && !environment.occluded &&
        ResolvePresentationViewport(config, environment.clientWidth, environment.clientHeight).disposition ==
            PresentationViewportDisposition::PausedBelowMinimumScale;
}

[[nodiscard]] bool CheckContract(const DataWindowConfig& config, const PresentationContract& contract) noexcept
{
    return contract.bufferCount == config.bufferCount && contract.maximumFrameLatency == config.maximumFrameLatency &&
           contract.flipEffect == config.flipEffect && contract.bgraUnorm && contract.noMsaa && contract.alphaIgnored && contract.scalingNone &&
           contract.tearingDisabled && contract.latencyWaitable && contract.perMonitorV2 &&
           contract.resizableChrome == !config.topmost && contract.immutableCanonicalSource && contract.pointSampled &&
           contract.centeredLetterbox && contract.neutralMatteBelowMinimum;
}

[[nodiscard]] bool CheckEnvironmentContract(const DataWindowConfig& config, const PresentationContract& contract, const WindowEnvironment& environment) noexcept
{
    return CheckContract(config, contract) && (!CanOwnClientBackBuffer(config, environment) ||
        (contract.bufferWidth == environment.clientWidth && contract.bufferHeight == environment.clientHeight));
}

[[nodiscard]] std::optional<pbpresenttiming::EpochReason> FindEnvironmentChange(const WindowEnvironment& previous, const WindowEnvironment& current) noexcept
{
    using pbpresenttiming::EpochReason;
    if (previous.monitorIdentity != current.monitorIdentity || previous.adapterLuidLow != current.adapterLuidLow ||
        previous.adapterLuidHigh != current.adapterLuidHigh || previous.adapterAvailable != current.adapterAvailable)
    {
        return EpochReason::MonitorChanged;
    }
    if (previous.dpi != current.dpi || previous.dpiChangeSerial != current.dpiChangeSerial)
    {
        return EpochReason::DpiChanged;
    }
    if (previous.modeChangeSerial != current.modeChangeSerial || previous.modeWidth != current.modeWidth || previous.modeHeight != current.modeHeight ||
        previous.modeFrequency != current.modeFrequency || previous.modeOrientation != current.modeOrientation)
    {
        return EpochReason::DisplayModeChanged;
    }
    if (previous.clientWidth != current.clientWidth || previous.clientHeight != current.clientHeight || previous.minimized != current.minimized)
    {
        return EpochReason::Resize;
    }
    if (previous.singleMonitor != current.singleMonitor)
    {
        return EpochReason::MonitorChanged;
    }
    if (previous.occluded != current.occluded)
    {
        return current.occluded ? EpochReason::Occluded : EpochReason::Restored;
    }
    return std::nullopt;
}

}

PresentationStatus ValidateDataWindowConfig(const DataWindowConfig& config) noexcept
{
    if (config.width == 0 || config.height == 0 || config.width > 16384 || config.height > 16384 || config.bufferCount < 2 || config.bufferCount > 16 ||
        config.maximumFrameLatency < 1 || config.maximumFrameLatency > 2 ||
        (config.flipEffect != FlipEffect::Discard && config.flipEffect != FlipEffect::Sequential) || config.maximumFrameBytes == 0 ||
        config.maximumFrameBytes == std::numeric_limits<std::uint64_t>::max() || config.waitTimeoutMilliseconds == 0 || config.waitTimeoutMilliseconds > 60000)
    {
        return PresentationStatus::Failure(PresentationErrorCode::InvalidConfiguration, PresentationStage::Configuration);
    }
    const auto pixelCount = pbprotocol::CheckedMultiplyUint64(config.width, config.height);
    const auto frameBytes = pixelCount ? pbprotocol::CheckedMultiplyUint64(pixelCount.Value(), 4) : pixelCount;
    if (!frameBytes || frameBytes.Value() > config.maximumFrameBytes || !pbprotocol::CheckedUint64ToSize(frameBytes.Value()) ||
        !pbprotocol::CheckedMultiplyUnsigned(static_cast<std::size_t>(frameBytes.Value()), std::size_t{2}))
    {
        return PresentationStatus::Failure(PresentationErrorCode::ResourceLimit, PresentationStage::Configuration);
    }
    // INT32_MIN is Win32 CW_USEDEFAULT, not an explicit physical coordinate.
    if (config.clientOrigin &&
        (config.clientOrigin->x == std::numeric_limits<std::int32_t>::min() || config.clientOrigin->y == std::numeric_limits<std::int32_t>::min() ||
         static_cast<std::int64_t>(config.clientOrigin->x) + config.width > std::numeric_limits<std::int32_t>::max() ||
         static_cast<std::int64_t>(config.clientOrigin->y) + config.height > std::numeric_limits<std::int32_t>::max()))
    {
        return PresentationStatus::Failure(PresentationErrorCode::InvalidConfiguration, PresentationStage::Configuration);
    }
    return PresentationStatus::Success();
}

PresentationStatus ValidateCanonicalBgraFrame(const DataWindowConfig& config, const CanonicalBgraFrameView& frame) noexcept
{
    const auto configurationStatus = ValidateDataWindowConfig(config);
    if (!configurationStatus)
    {
        return configurationStatus;
    }
    if (frame.width != config.width || frame.height != config.height)
    {
        return PresentationStatus::Failure(PresentationErrorCode::InvalidFrame, PresentationStage::FrameValidation);
    }
    const std::size_t rowBytes = static_cast<std::size_t>(config.width) * 4;
    if (frame.rowPitch < rowBytes || frame.rowPitch > config.maximumFrameBytes)
    {
        return PresentationStatus::Failure(PresentationErrorCode::InvalidFrame, PresentationStage::FrameValidation);
    }
    const auto precedingBytes = pbprotocol::CheckedMultiplyUnsigned(frame.rowPitch, static_cast<std::size_t>(frame.height - 1));
    const auto neededBytes = precedingBytes ? pbprotocol::CheckedAddSize(precedingBytes.Value(), rowBytes) : precedingBytes;
    if (!neededBytes || neededBytes.Value() > config.maximumFrameBytes || neededBytes.Value() > frame.pixels.size())
    {
        return PresentationStatus::Failure(PresentationErrorCode::InvalidFrame, PresentationStage::FrameValidation);
    }
    return PresentationStatus::Success();
}

struct DataWindow::Implementation
{
    Implementation(const DataWindowConfig& initialConfig, std::unique_ptr<PresentationBackend> initialBackend)
        : config(initialConfig), backend(std::move(initialBackend)), timing(backend->GetQpcFrequency())
    {
        const std::size_t frameBytes = static_cast<std::size_t>(config.width) * config.height * 4;
        activePixels.resize(frameBytes);
        pendingPixels.resize(frameBytes);
    }

    void DiscardPendingLocked() noexcept
    {
        if (snapshot.pendingFrame)
        {
            pbprotocol::SaturatingIncrementUnsigned(snapshot.discardedEpochFrames);
            snapshot.pendingFrame = false;
        }
    }

    void InvalidateActiveLocked() noexcept
    {
        if (snapshot.activeFrame)
        {
            pbprotocol::SaturatingIncrementUnsigned(snapshot.invalidatedActiveFrames);
            snapshot.activeFrame = false;
            snapshot.activeFrameSequence = 0;
            snapshot.activeFramePresentationEpoch = 0;
        }
    }

    void PublishBackendLocked() noexcept
    {
        snapshot.contract = backend->GetContract();
        snapshot.viewport = ResolvePresentationViewport(config, snapshot.environment.clientWidth, snapshot.environment.clientHeight);
        diagnostics = backend->GetDiagnostics();
        snapshot.softwareRasterizer = diagnostics.warp;
        snapshot.swapChainGeneration = diagnostics.swapChainGeneration;
        snapshot.bufferGeneration = diagnostics.bufferGeneration;
        snapshot.candidateContractSatisfied = snapshot.state == WindowState::Running && CanDisplay(config, snapshot.environment) &&
            CheckContract(config, snapshot.contract) && snapshot.contract.bufferWidth == snapshot.environment.clientWidth &&
            snapshot.contract.bufferHeight == snapshot.environment.clientHeight;
    }

    void FailLocked(const PresentationStatus error) noexcept
    {
        const std::int64_t now = backend->NowQpc();
        if (error.code == PresentationErrorCode::DeviceLost && timing.GetSnapshot(now).epochReason != pbpresenttiming::EpochReason::DeviceLost)
        {
            static_cast<void>(timing.BeginEpoch(pbpresenttiming::EpochReason::DeviceLost, now));
        }
        snapshot.error = error;
        snapshot.state = WindowState::Failed;
        snapshot.candidateContractSatisfied = false;
        timing.SetFailed();
        DiscardPendingLocked();
        InvalidateActiveLocked();
    }

    void Fail(const PresentationStatus error) noexcept
    {
        const std::lock_guard lock(stateMutex);
        FailLocked(error);
    }

    [[nodiscard]] bool BeginEpochLocked(const pbpresenttiming::EpochReason reason) noexcept
    {
        DiscardPendingLocked();
        InvalidateActiveLocked();
        snapshot.neutralMattePending = false;
        if (!timing.BeginEpoch(reason, backend->NowQpc()))
        {
            FailLocked(PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::Statistics));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool RefreshEnvironment(bool& framePermit) noexcept
    {
        const auto current = backend->PollEnvironment();
        if (!current)
        {
            Fail(current.Error());
            return false;
        }
        if (current.Value().closed)
        {
            stopRequested.store(true);
            return false;
        }
        std::optional<pbpresenttiming::EpochReason> reason;
        {
            const std::lock_guard lock(stateMutex);
            reason = FindEnvironmentChange(snapshot.environment, current.Value());
            snapshot.environment = current.Value();
            if (reason)
            {
                snapshot.state = WindowState::Paused;
                snapshot.candidateContractSatisfied = false;
                if (!BeginEpochLocked(*reason))
                {
                    return false;
                }
            }
        }
        if (reason)
        {
            framePermit = false;
            const auto status = backend->Reconfigure(current.Value());
            if (!status)
            {
                if (!stopRequested.load() || status.code != PresentationErrorCode::NotRunning)
                {
                    Fail(status);
                }
                return false;
            }
            const std::lock_guard lock(stateMutex);
            if (!CheckEnvironmentContract(config, backend->GetContract(), current.Value()))
            {
                FailLocked(PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::SwapChain));
                return false;
            }
            snapshot.state = CanDisplay(config, current.Value()) ? WindowState::Running : WindowState::Paused;
            snapshot.neutralMattePending = CanShowNeutralMatte(config, current.Value());
            timing.SetPaused(snapshot.state == WindowState::Paused);
            PublishBackendLocked();
        }
        return true;
    }

    void PollStatistics() noexcept
    {
        {
            const std::lock_guard lock(stateMutex);
            if (snapshot.state != WindowState::Running)
            {
                return;
            }
        }
        const auto statistics = backend->GetStatistics();
        const std::int64_t now = backend->NowQpc();
        const std::lock_guard lock(stateMutex);
        const std::uint64_t previousEpoch = timing.GetSnapshot(now).presentationEpoch;
        if (!statistics.error)
        {
            if (statistics.error.code == PresentationErrorCode::DeviceLost)
            {
                DiscardPendingLocked();
                static_cast<void>(timing.BeginEpoch(pbpresenttiming::EpochReason::DeviceLost, now));
            }
            timing.ObserveStatistics(statistics.status, statistics.statistics, now, statistics.nativeStatus);
            FailLocked(statistics.error);
            return;
        }
        timing.ObserveStatistics(statistics.status, statistics.statistics, now, statistics.nativeStatus);
        const auto updated = timing.GetSnapshot(now);
        if (updated.presentationEpoch != previousEpoch)
        {
            DiscardPendingLocked();
            InvalidateActiveLocked();
        }
        if (updated.state == pbpresenttiming::TimingState::Failed)
        {
            FailLocked(PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::Statistics));
        }
    }

    [[nodiscard]] bool PresentAvailable(bool& framePermit) noexcept
    {
        std::uint64_t sequence = 0;
        bool replacesSource = false;
        {
            const std::lock_guard lock(stateMutex);
            const bool canRepeat = config.repeatActiveFrame && snapshot.activeFrame;
            if (snapshot.state != WindowState::Running || (!snapshot.pendingFrame && !canRepeat) || !framePermit)
            {
                return true;
            }
            if (!snapshot.candidateContractSatisfied)
            {
                FailLocked(PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::Environment));
                return false;
            }
            if (snapshot.pendingFrame)
            {
                activePixels.swap(pendingPixels);
                sequence = pendingSequence;
                snapshot.pendingFrame = false;
                replacesSource = true;
            }
            else
            {
                sequence = snapshot.activeFrameSequence;
            }
            snapshot.inFlightFrame = true;
        }
        if (replacesSource)
        {
            const auto upload = backend->Upload(activePixels);
            if (!upload)
            {
                if (!stopRequested.load())
                {
                    Fail(upload);
                }
                return false;
            }
        }
        // Upload/readback and repeated Present preparation can pump messages or
        // take time. Re-check the live environment before issuing Present with
        // a potentially stale raster.
        const std::uint64_t beforeEpoch = GetSnapshot().timing.presentationEpoch;
        if (!RefreshEnvironment(framePermit))
        {
            return false;
        }
        if (!framePermit || GetSnapshot().timing.presentationEpoch != beforeEpoch || stopRequested.load())
        {
            const std::lock_guard lock(stateMutex);
            if (replacesSource)
            {
                pbprotocol::SaturatingIncrementUnsigned(snapshot.discardedEpochFrames);
            }
            return true;
        }
        if (replacesSource)
        {
            const std::lock_guard lock(stateMutex);
            snapshot.activeFrame = true;
            snapshot.activeFrameSequence = sequence;
            snapshot.activeFramePresentationEpoch = beforeEpoch;
            pbprotocol::SaturatingIncrementUnsigned(snapshot.sourceTextureReplacements);
        }
        const std::int64_t beginQpc = backend->NowQpc();
        const auto result = backend->Present();
        const std::int64_t endQpc = backend->NowQpc();
        framePermit = false;
        {
            const std::lock_guard lock(stateMutex);
            pbprotocol::SaturatingIncrementUnsigned(snapshot.totalPresentCalls);
            if (!replacesSource)
            {
                pbprotocol::SaturatingIncrementUnsigned(snapshot.repeatedPresentCalls);
            }
            snapshot.lastPresentIdNativeStatus = result.presentIdNativeStatus;
            if (result.outcome == pbpresenttiming::PresentOutcome::Success)
            {
                pbprotocol::SaturatingIncrementUnsigned(snapshot.totalSuccessfulPresents);
            }
            const std::uint64_t previousEpoch = timing.GetSnapshot(endQpc).presentationEpoch;
            timing.RecordPresent(sequence, beginQpc, endQpc, result.outcome, result.presentId);
            if (previousEpoch != timing.GetSnapshot(endQpc).presentationEpoch)
            {
                DiscardPendingLocked();
                InvalidateActiveLocked();
            }
            PublishBackendLocked();
            if (!result.error)
            {
                FailLocked(result.error);
                return false;
            }
        }
        PollStatistics();
        return true;
    }

    [[nodiscard]] bool PresentNeutralMatteAvailable(bool& framePermit) noexcept
    {
        {
            const std::lock_guard lock(stateMutex);
            if (!snapshot.neutralMattePending || !framePermit)
            {
                return true;
            }
        }
        const auto result = backend->PresentNeutralMatte();
        framePermit = false;
        const std::lock_guard lock(stateMutex);
        snapshot.neutralMattePending = false;
        pbprotocol::SaturatingIncrementUnsigned(snapshot.neutralMattePresentCalls);
        PublishBackendLocked();
        if (!result.error)
        {
            FailLocked(result.error);
            return false;
        }
        return true;
    }

    [[nodiscard]] DataWindowSnapshot GetSnapshot() const noexcept
    {
        const std::lock_guard lock(stateMutex);
        DataWindowSnapshot result = snapshot;
        result.timing = timing.GetSnapshot(backend->NowQpc());
        return result;
    }

    void Run() noexcept
    {
        bool framePermit = false;
        bool idleNotified = false;
        std::optional<std::int64_t> waitingSince;
        while (!stopRequested.load())
        {
            if (!RefreshEnvironment(framePermit))
            {
                break;
            }
            bool pending = false;
            bool running = false;
            bool neutralMattePending = false;
            {
                const std::lock_guard lock(stateMutex);
                if (snapshot.state == WindowState::Failed)
                {
                    break;
                }
                running = snapshot.state == WindowState::Running;
                pending = snapshot.pendingFrame || (config.repeatActiveFrame && snapshot.activeFrame);
                neutralMattePending = snapshot.neutralMattePending;
                if (!pending && !neutralMattePending && !idleNotified)
                {
                    timing.BreakCadence();
                    idleNotified = true;
                }
            }
            if (neutralMattePending && framePermit)
            {
                idleNotified = false;
                waitingSince.reset();
                if (!PresentNeutralMatteAvailable(framePermit))
                {
                    break;
                }
                continue;
            }
            if (running && pending && framePermit)
            {
                idleNotified = false;
                waitingSince.reset();
                const bool presented = PresentAvailable(framePermit);
                {
                    const std::lock_guard lock(stateMutex);
                    snapshot.inFlightFrame = false;
                }
                if (!presented)
                {
                    break;
                }
                continue;
            }
            const bool requestPermit = (neutralMattePending || (running && pending)) && !framePermit;
            if (!requestPermit)
            {
                waitingSince.reset();
            }
            else if (!waitingSince)
            {
                waitingSince = backend->NowQpc();
            }
            const std::uint32_t waitMilliseconds = requestPermit ? std::min(50u, config.waitTimeoutMilliseconds) : 50u;
            const auto wait = backend->Wait(requestPermit, waitMilliseconds);
            if (stopRequested.load())
            {
                break;
            }
            if (wait.wake == BackendWake::Timeout && !requestPermit)
            {
                PollStatistics();
            }
            if (wait.wake == BackendWake::FramePermit)
            {
                if (!requestPermit)
                {
                    Fail(PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::Wait));
                    break;
                }
                framePermit = true;
                waitingSince.reset();
            }
            else if (wait.wake == BackendWake::Failed)
            {
                Fail(wait.error ? PresentationStatus::Failure(PresentationErrorCode::WaitFailed, PresentationStage::Wait) : wait.error);
                break;
            }
            else if (waitingSince)
            {
                const std::int64_t now = backend->NowQpc();
                // A failed QPC returns -1; two failures must not look like a
                // stationary valid clock or bypass the bounded wait forever.
                const double elapsedMilliseconds = *waitingSince >= 0 && now >= *waitingSince
                                                       ? static_cast<double>(now - *waitingSince) * 1000.0 / static_cast<double>(backend->GetQpcFrequency())
                                                       : -1.0;
                if (elapsedMilliseconds < 0 || elapsedMilliseconds >= config.waitTimeoutMilliseconds)
                {
                    Fail(PresentationStatus::Failure(PresentationErrorCode::Timeout, PresentationStage::Wait));
                    break;
                }
            }
        }
    }

    void ThreadMain() noexcept
    {
        try
        {
            auto status = backend->Initialize(config);
            if (status)
            {
                const auto environment = backend->PollEnvironment();
                if (!environment)
                {
                    status = environment.Error();
                }
                else
                {
                    const std::lock_guard lock(stateMutex);
                    snapshot.environment = environment.Value();
                    if (!CheckEnvironmentContract(config, backend->GetContract(), environment.Value()))
                    {
                        status = PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::SwapChain);
                    }
                    else if (!BeginEpochLocked(pbpresenttiming::EpochReason::Initial))
                    {
                        status = snapshot.error;
                    }
                    else
                    {
                        snapshot.state = CanDisplay(config, environment.Value()) ? WindowState::Running : WindowState::Paused;
                        snapshot.neutralMattePending = CanShowNeutralMatte(config, environment.Value());
                        timing.SetPaused(snapshot.state == WindowState::Paused);
                        PublishBackendLocked();
                    }
                }
            }
            {
                const std::lock_guard lock(stateMutex);
                if (!status)
                {
                    FailLocked(status);
                }
                initialized = true;
            }
            initializedCondition.notify_all();
            if (status)
            {
                Run();
            }
        }
        catch (const std::bad_alloc&)
        {
            Fail(PresentationStatus::Failure(PresentationErrorCode::OutOfMemory, PresentationStage::Thread));
        }
        catch (...)
        {
            Fail(PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::Thread));
        }
        backend->Shutdown();
        {
            const std::lock_guard lock(stateMutex);
            diagnostics = backend->GetDiagnostics();
            if (snapshot.state != WindowState::Failed)
            {
                snapshot.state = WindowState::Stopped;
                timing.SetPaused(true);
            }
            snapshot.candidateContractSatisfied = false;
            snapshot.inFlightFrame = false;
            snapshot.neutralMattePending = false;
            DiscardPendingLocked();
            snapshot.activeFrame = false;
            snapshot.activeFrameSequence = 0;
            snapshot.activeFramePresentationEpoch = 0;
            initialized = true;
        }
        initializedCondition.notify_all();
    }

    const DataWindowConfig config;
    std::unique_ptr<PresentationBackend> backend;
    mutable std::mutex stateMutex;
    std::mutex stopMutex;
    std::condition_variable initializedCondition;
    bool initialized = false;
    std::atomic<bool> stopRequested = false;
    std::thread owner;
    pbpresenttiming::PresentTiming timing;
    DataWindowSnapshot snapshot;
    BackendDiagnostics diagnostics;
    std::vector<std::byte> activePixels;
    std::vector<std::byte> pendingPixels;
    std::uint64_t pendingSequence = 0;
};

DataWindow::DataWindow(std::unique_ptr<Implementation> implementation) noexcept : implementation_(std::move(implementation))
{
}

DataWindow::~DataWindow()
{
    Stop();
}

PresentationResult<std::unique_ptr<DataWindow>> DataWindow::Create(const DataWindowConfig& config) noexcept
{
    try
    {
        return DataWindowTestAccess::Create(config, MakeNativeBackend());
    }
    catch (const std::bad_alloc&)
    {
        return PresentationResult<std::unique_ptr<DataWindow>>::Failure(
            PresentationStatus::Failure(PresentationErrorCode::OutOfMemory, PresentationStage::Configuration));
    }
    catch (...)
    {
        return PresentationResult<std::unique_ptr<DataWindow>>::Failure(
            PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::Configuration));
    }
}

PresentationResult<std::unique_ptr<DataWindow>> DataWindowTestAccess::Create(const DataWindowConfig& config,
                                                                             std::unique_ptr<PresentationBackend> backend) noexcept
{
    const auto validation = ValidateDataWindowConfig(config);
    if (!validation)
    {
        return PresentationResult<std::unique_ptr<DataWindow>>::Failure(validation);
    }
    if (!backend)
    {
        return PresentationResult<std::unique_ptr<DataWindow>>::Failure(
            PresentationStatus::Failure(PresentationErrorCode::InvalidConfiguration, PresentationStage::Configuration));
    }
    try
    {
        auto implementation = std::make_unique<DataWindow::Implementation>(config, std::move(backend));
        auto window = std::unique_ptr<DataWindow>(new DataWindow(std::move(implementation)));
        auto* const state = window->implementation_.get();
        state->owner = std::thread(
            [state]
            {
                state->ThreadMain();
            });
        {
            std::unique_lock lock(state->stateMutex);
            state->initializedCondition.wait(lock,
                                             [state]
                                             {
                                                 return state->initialized;
                                             });
            if (!state->snapshot.error)
            {
                return PresentationResult<std::unique_ptr<DataWindow>>::Failure(state->snapshot.error);
            }
        }
        return PresentationResult<std::unique_ptr<DataWindow>>::Success(std::move(window));
    }
    catch (const std::bad_alloc&)
    {
        return PresentationResult<std::unique_ptr<DataWindow>>::Failure(
            PresentationStatus::Failure(PresentationErrorCode::OutOfMemory, PresentationStage::Configuration));
    }
    catch (...)
    {
        return PresentationResult<std::unique_ptr<DataWindow>>::Failure(
            PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Thread));
    }
}

PresentationStatus DataWindow::SubmitFrame(const CanonicalBgraFrameView& frame) noexcept
{
    const auto validation = ValidateCanonicalBgraFrame(implementation_->config, frame);
    if (!validation)
    {
        return validation;
    }
    const std::lock_guard lock(implementation_->stateMutex);
    if (implementation_->stopRequested.load() || implementation_->snapshot.state == WindowState::Failed ||
        implementation_->snapshot.state == WindowState::Stopped)
    {
        return PresentationStatus::Failure(PresentationErrorCode::NotRunning, PresentationStage::FrameValidation);
    }
    if (frame.presentationEpoch != implementation_->timing.GetSnapshot(implementation_->backend->NowQpc()).presentationEpoch)
    {
        return PresentationStatus::Failure(PresentationErrorCode::EpochMismatch, PresentationStage::FrameValidation);
    }
    if (implementation_->snapshot.state != WindowState::Running)
    {
        return PresentationStatus::Failure(PresentationErrorCode::Paused, PresentationStage::FrameValidation);
    }
    const std::size_t rowBytes = static_cast<std::size_t>(frame.width) * 4;
    for (std::size_t row = 0; row < frame.height; row++)
    {
        std::copy_n(frame.pixels.data() + row * frame.rowPitch, rowBytes, implementation_->pendingPixels.data() + row * rowBytes);
    }
    if (implementation_->snapshot.pendingFrame)
    {
        pbprotocol::SaturatingIncrementUnsigned(implementation_->snapshot.replacedPendingFrames);
    }
    implementation_->pendingSequence = frame.frameSequence;
    implementation_->snapshot.pendingFrame = true;
    pbprotocol::SaturatingIncrementUnsigned(implementation_->snapshot.submittedFrames);
    implementation_->backend->Wake();
    return PresentationStatus::Success();
}

DataWindowSnapshot DataWindow::GetSnapshot() const noexcept
{
    return implementation_->GetSnapshot();
}

void DataWindow::RequestStop() noexcept
{
    implementation_->stopRequested.store(true);
    implementation_->backend->Cancel();
}

void DataWindow::Stop() noexcept
{
    const std::lock_guard lock(implementation_->stopMutex);
    RequestStop();
    if (implementation_->owner.joinable())
    {
        implementation_->owner.join();
    }
}

std::uintptr_t DataWindowTestAccess::GetWindowToken(const DataWindow& window) noexcept
{
    return window.implementation_->backend->GetWindowToken();
}

BackendDiagnostics DataWindowTestAccess::GetDiagnostics(const DataWindow& window) noexcept
{
    const std::lock_guard lock(window.implementation_->stateMutex);
    return window.implementation_->diagnostics;
}

const char* GetPresentationErrorName(const PresentationErrorCode code) noexcept
{
    switch (code)
    {
    case PresentationErrorCode::None:
        return "none";
    case PresentationErrorCode::InvalidConfiguration:
        return "invalid-configuration";
    case PresentationErrorCode::InvalidFrame:
        return "invalid-frame";
    case PresentationErrorCode::ResourceLimit:
        return "resource-limit";
    case PresentationErrorCode::EpochMismatch:
        return "epoch-mismatch";
    case PresentationErrorCode::NotRunning:
        return "not-running";
    case PresentationErrorCode::Paused:
        return "paused";
    case PresentationErrorCode::DpiAwarenessRequired:
        return "per-monitor-v2-required";
    case PresentationErrorCode::NativeFailure:
        return "native-failure";
    case PresentationErrorCode::WaitFailed:
        return "wait-failed";
    case PresentationErrorCode::Timeout:
        return "timeout";
    case PresentationErrorCode::DeviceLost:
        return "device-lost";
    case PresentationErrorCode::ContractViolation:
        return "contract-violation";
    case PresentationErrorCode::OutOfMemory:
        return "out-of-memory";
    case PresentationErrorCode::InternalError:
        return "internal-error";
    }
    return "invalid";
}

const char* GetPresentationStageName(const PresentationStage stage) noexcept
{
    switch (stage)
    {
    case PresentationStage::None:
        return "none";
    case PresentationStage::Configuration:
        return "configuration";
    case PresentationStage::FrameValidation:
        return "frame-validation";
    case PresentationStage::Thread:
        return "thread";
    case PresentationStage::WakeEvent:
        return "wake-event";
    case PresentationStage::DpiAwareness:
        return "dpi-awareness";
    case PresentationStage::WindowClass:
        return "window-class";
    case PresentationStage::Window:
        return "window";
    case PresentationStage::Adapter:
        return "adapter";
    case PresentationStage::Device:
        return "device";
    case PresentationStage::SwapChain:
        return "swap-chain";
    case PresentationStage::FrameLatency:
        return "frame-latency";
    case PresentationStage::BackBuffer:
        return "back-buffer";
    case PresentationStage::Environment:
        return "environment";
    case PresentationStage::GpuDrain:
        return "gpu-drain";
    case PresentationStage::Resize:
        return "resize";
    case PresentationStage::Wait:
        return "wait";
    case PresentationStage::Upload:
        return "upload";
    case PresentationStage::Readback:
        return "readback";
    case PresentationStage::Present:
        return "present";
    case PresentationStage::Statistics:
        return "statistics";
    case PresentationStage::DebugLayer:
        return "debug-layer";
    }
    return "invalid";
}

}
