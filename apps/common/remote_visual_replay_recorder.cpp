#include "remote_visual_replay_recorder.h"

#include "pbprotocol/checked_integer.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace pbapp
{
namespace
{

enum class SlotState : std::uint8_t
{
    Free,
    Candidate,
    Queued,
    Writing
};

struct RecorderSlot
{
    SlotState state = SlotState::Free;
    pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
    std::vector<std::byte> pixels;
    std::size_t rowPitch = 0;
    std::size_t pixelBytes = 0;
};

std::optional<std::uint32_t> BytesPerPixel(const DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    default: return std::nullopt;
    }
}

pbcapturenormalize::CaptureStatus Failure(const pbcapturenormalize::CaptureError code,
    const HRESULT nativeError = S_OK,
    const pbcapturenormalize::CaptureStage stage = pbcapturenormalize::CaptureStage::Consumer) noexcept
{
    return pbcapturenormalize::CaptureStatus::Failure(code, stage, nativeError);
}

} // namespace

struct RemoteVisualReplayRecorder::Implementation
{
    struct State
    {
        RemoteVisualReplayRecorderConfig config;
        std::unique_ptr<pbrealcapturereplay::ReplayV2Writer> writer;
        std::vector<RecorderSlot> captureSlots;
        std::vector<std::uint32_t> queue;
        std::vector<pbrealcapturereplay::ReplayV2DemodObservationView> demodObservations;
        std::optional<std::uint32_t> candidateIndex;
        std::uint32_t queueHead = 0;
        std::uint32_t queueTail = 0;
        std::uint32_t queueCount = 0;
        std::uint64_t processingReservedBytes = 0;
        std::uint64_t analyzedFrames = 0;
        std::uint64_t enqueuedFrames = 0;
        std::uint64_t writtenFrames = 0;
        std::uint64_t droppedFrames = 0;
        std::uint64_t writtenDemodObservations = 0;
        std::uint64_t droppedDemodObservations = 0;
        std::uint32_t queueHighWater = 0;
        pbrealcapturereplay::ReplayStatus lastError;
        bool stopRequested = false;
        bool workerStopped = false;
        bool finalized = false;
        bool evidenceValid = true;
        mutable std::mutex mutex;
        std::condition_variable condition;
    };

    std::shared_ptr<State> state;
    std::thread worker;
    bool detached = false;
};

namespace
{

bool CaptureWasWritten(const RemoteVisualReplayRecorder::Implementation::State& state,
    const pbrealcapturereplay::ReplayV2DemodObservationView& observation) noexcept
{
    const auto writerSnapshot = state.writer->GetSnapshot();
    if (writerSnapshot.captureFrames == 0)
    {
        return false;
    }
    // ReplayV2Writer performs the authoritative key check. This fast bound
    // avoids calling it when no capture could possibly have been written.
    return observation.captureObservation != 0 && observation.captureEpoch != 0;
}

void RunWriter(const std::shared_ptr<RemoteVisualReplayRecorder::Implementation::State>& state) noexcept
{
    for (;;)
    {
        std::uint32_t slotIndex = 0;
        {
            std::unique_lock lock(state->mutex);
            state->condition.wait(lock, [&] { return state->stopRequested || state->queueCount != 0; });
            if (state->queueCount == 0)
            {
                break;
            }
            slotIndex = state->queue[state->queueHead];
            state->queueHead = (state->queueHead + 1) % static_cast<std::uint32_t>(state->queue.size());
            state->queueCount--;
            state->captureSlots[slotIndex].state = SlotState::Writing;
        }
        RecorderSlot& slot = state->captureSlots[slotIndex];
        const pbrealcapturereplay::ReplayV2CaptureView capture{slot.metadata, state->config.dpiX,
            state->config.dpiY, state->config.scaleX, state->config.scaleY,
            state->config.displayIdentityUtf8,
            {state->config.roiWidth, state->config.roiHeight, static_cast<std::uint32_t>(slot.rowPitch),
                state->config.pixelFormat, std::span<const std::byte>(slot.pixels).first(slot.pixelBytes)},
            std::nullopt, {}};
        const auto status = state->writer->AppendCapture(capture);
        {
            std::lock_guard lock(state->mutex);
            if (status)
            {
                state->writtenFrames++;
            }
            else
            {
                state->evidenceValid = false;
                if (state->lastError)
                {
                    state->lastError = status;
                }
            }
            slot.state = SlotState::Free;
            slot.pixelBytes = 0;
            slot.rowPitch = 0;
            state->condition.notify_all();
        }
    }

    // StopRequested is set under the same mutex before this point. Subsequent
    // RecordDemodObservation calls only count a drop and cannot mutate the
    // pre-reserved vector, so iteration performs no allocation or I/O callback.
    for (const auto& observation : state->demodObservations)
    {
        if (!CaptureWasWritten(*state, observation))
        {
            std::lock_guard lock(state->mutex);
            state->droppedDemodObservations++;
            continue;
        }
        const auto status = state->writer->AppendDemodObservation(observation);
        std::lock_guard lock(state->mutex);
        if (status)
        {
            state->writtenDemodObservations++;
        }
        else if (status.code == pbrealcapturereplay::ReplayError::InvalidArgument)
        {
            state->droppedDemodObservations++;
        }
        else
        {
            state->evidenceValid = false;
            if (state->lastError)
            {
                state->lastError = status;
            }
        }
    }
    const auto finalStatus = state->writer->Finalize();
    {
        std::lock_guard lock(state->mutex);
        if (finalStatus)
        {
            state->finalized = true;
        }
        else
        {
            state->evidenceValid = false;
            if (state->lastError)
            {
                state->lastError = finalStatus;
            }
        }
        state->workerStopped = true;
        state->condition.notify_all();
    }
}

} // namespace

RemoteVisualReplayRecorder::RemoteVisualReplayRecorder(std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

RemoteVisualReplayRecorder::~RemoteVisualReplayRecorder()
{
    RequestStop();
    static_cast<void>(Stop());
}

pbcapturenormalize::CaptureStatus RemoteVisualReplayRecorder::Create(
    const RemoteVisualReplayRecorderConfig& config,
    std::shared_ptr<RemoteVisualReplayRecorder>& output) noexcept
{
    const auto bytesPerPixel = BytesPerPixel(config.pixelFormat);
    if (config.outputPath.empty() || config.roiWidth == 0 || config.roiHeight == 0 || !bytesPerPixel ||
        config.roiWidth > config.limits.maximumDimension || config.roiHeight > config.limits.maximumDimension ||
        config.displayIdentityUtf8.empty() || config.dpiX == 0 || config.dpiY == 0 ||
        config.queueCapacity == 0 || config.queueCapacity > 16)
    {
        return Failure(pbcapturenormalize::CaptureError::InvalidConfiguration, E_INVALIDARG,
            pbcapturenormalize::CaptureStage::Configuration);
    }
    const auto rowBytes = pbprotocol::CheckedMultiplyUint64(config.roiWidth, *bytesPerPixel);
    if (!rowBytes)
    {
        return Failure(pbcapturenormalize::CaptureError::ResourceLimit, E_OUTOFMEMORY,
            pbcapturenormalize::CaptureStage::Configuration);
    }
    const auto frameBytes = pbprotocol::CheckedMultiplyUint64(rowBytes.Value(), config.roiHeight);
    const auto slotCount = pbprotocol::CheckedAddUint64(config.queueCapacity, 1);
    if (!frameBytes || !slotCount)
    {
        return Failure(pbcapturenormalize::CaptureError::ResourceLimit, E_OUTOFMEMORY,
            pbcapturenormalize::CaptureStage::Configuration);
    }
    const auto pixelReservation = pbprotocol::CheckedMultiplyUint64(frameBytes.Value(), slotCount.Value());
    if (!pixelReservation ||
        frameBytes.Value() > config.limits.maximumRasterBytesPerFrame ||
        pixelReservation.Value() > config.limits.maximumFileBytes ||
        rowBytes.Value() > (std::numeric_limits<std::uint32_t>::max)())
    {
        return Failure(pbcapturenormalize::CaptureError::ResourceLimit, E_OUTOFMEMORY,
            pbcapturenormalize::CaptureStage::Configuration);
    }
    try
    {
        auto implementation = std::make_unique<Implementation>();
        implementation->state = std::make_shared<Implementation::State>();
        auto& state = *implementation->state;
        state.config = config;
        const auto writerStatus = pbrealcapturereplay::ReplayV2Writer::Create(config.outputPath,
            config.descriptor, config.limits, state.writer);
        if (!writerStatus)
        {
            return Failure(writerStatus.code == pbrealcapturereplay::ReplayError::AlreadyExists ?
                pbcapturenormalize::CaptureError::InvalidConfiguration :
                pbcapturenormalize::CaptureError::NativeFailure,
                writerStatus.nativeError == 0 ? E_FAIL : static_cast<HRESULT>(writerStatus.nativeError),
                pbcapturenormalize::CaptureStage::Configuration);
        }
        state.captureSlots.resize(static_cast<std::size_t>(slotCount.Value()));
        for (auto& slot : state.captureSlots)
        {
            slot.pixels.resize(static_cast<std::size_t>(frameBytes.Value()));
        }
        state.queue.resize(config.queueCapacity);
        state.demodObservations.reserve(config.limits.maximumCaptureFrames);
        const std::uint64_t fixedReservation = sizeof(Implementation::State) +
            state.captureSlots.size() * sizeof(RecorderSlot) + state.queue.size() * sizeof(std::uint32_t) +
            static_cast<std::uint64_t>(config.limits.maximumCaptureFrames) *
                sizeof(pbrealcapturereplay::ReplayV2DemodObservationView);
        const auto totalReservation = pbprotocol::CheckedAddUint64(pixelReservation.Value(), fixedReservation);
        if (!totalReservation)
        {
            return Failure(pbcapturenormalize::CaptureError::ResourceLimit, E_OUTOFMEMORY,
                pbcapturenormalize::CaptureStage::Configuration);
        }
        state.processingReservedBytes = totalReservation.Value();
        implementation->worker = std::thread(RunWriter, implementation->state);
        output = std::shared_ptr<RemoteVisualReplayRecorder>(
            new RemoteVisualReplayRecorder(std::move(implementation)));
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return Failure(pbcapturenormalize::CaptureError::OutOfMemory, E_OUTOFMEMORY,
            pbcapturenormalize::CaptureStage::Configuration);
    }
    catch (const std::system_error& error)
    {
        return Failure(pbcapturenormalize::CaptureError::NativeFailure,
            HRESULT_FROM_WIN32(static_cast<unsigned long>(error.code().value())),
            pbcapturenormalize::CaptureStage::Configuration);
    }
}

std::uint64_t RemoteVisualReplayRecorder::ProcessingReservedBytes() const noexcept
{
    return implementation_ ? implementation_->state->processingReservedBytes : 0;
}

void RemoteVisualReplayRecorder::Reset(const std::optional<pbcapturenormalize::ScreenCaptureDomain>) noexcept
{
    Discard();
}

pbcapturenormalize::CaptureStatus RemoteVisualReplayRecorder::Analyze(
    const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata,
    const std::span<const std::byte> pixels, const std::size_t rowPitch)
{
    if (!implementation_)
    {
        return Failure(pbcapturenormalize::CaptureError::InternalError, E_UNEXPECTED);
    }
    auto& state = *implementation_->state;
    const auto expectedBytes = pbprotocol::CheckedMultiplyUint64(rowPitch, state.config.roiHeight);
    if (!expectedBytes || expectedBytes.Value() != pixels.size() ||
        rowPitch != static_cast<std::size_t>(state.config.roiWidth) * *BytesPerPixel(state.config.pixelFormat) ||
        metadata.roiSize.width != static_cast<std::int32_t>(state.config.roiWidth) ||
        metadata.roiSize.height != static_cast<std::int32_t>(state.config.roiHeight) ||
        metadata.pixelFormat != state.config.pixelFormat)
    {
        return Failure(pbcapturenormalize::CaptureError::InvalidFrame, E_INVALIDARG);
    }
    std::uint32_t slotIndex = 0;
    {
        std::lock_guard lock(state.mutex);
        state.analyzedFrames++;
        if (state.stopRequested || state.enqueuedFrames >= state.config.limits.maximumCaptureFrames)
        {
            state.droppedFrames++;
            return {};
        }
        const auto iterator = std::ranges::find_if(state.captureSlots, [](const RecorderSlot& slot)
        {
            return slot.state == SlotState::Free;
        });
        if (iterator == state.captureSlots.end())
        {
            state.droppedFrames++;
            return {};
        }
        slotIndex = static_cast<std::uint32_t>(std::distance(state.captureSlots.begin(), iterator));
        iterator->state = SlotState::Candidate;
        state.candidateIndex = slotIndex;
    }
    RecorderSlot& slot = state.captureSlots[slotIndex];
    std::memcpy(slot.pixels.data(), pixels.data(), pixels.size());
    slot.pixelBytes = pixels.size();
    slot.rowPitch = rowPitch;
    slot.metadata = metadata;
    return {};
}

void RemoteVisualReplayRecorder::Commit(const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata) noexcept
{
    if (!implementation_)
    {
        return;
    }
    auto& state = *implementation_->state;
    std::lock_guard lock(state.mutex);
    if (!state.candidateIndex)
    {
        return;
    }
    RecorderSlot& slot = state.captureSlots[*state.candidateIndex];
    const bool identityMatches = slot.metadata.domain == metadata.domain &&
        slot.metadata.captureObservation == metadata.captureObservation;
    if (!identityMatches || state.stopRequested || state.queueCount >= state.queue.size())
    {
        slot.state = SlotState::Free;
        slot.pixelBytes = 0;
        slot.rowPitch = 0;
        state.droppedFrames++;
        state.candidateIndex.reset();
        return;
    }
    slot.state = SlotState::Queued;
    state.queue[state.queueTail] = *state.candidateIndex;
    state.queueTail = (state.queueTail + 1) % static_cast<std::uint32_t>(state.queue.size());
    state.queueCount++;
    state.queueHighWater = (std::max)(state.queueHighWater, state.queueCount);
    state.enqueuedFrames++;
    state.candidateIndex.reset();
    state.condition.notify_one();
}

void RemoteVisualReplayRecorder::Discard() noexcept
{
    if (!implementation_)
    {
        return;
    }
    auto& state = *implementation_->state;
    std::lock_guard lock(state.mutex);
    if (state.candidateIndex)
    {
        RecorderSlot& slot = state.captureSlots[*state.candidateIndex];
        slot.state = SlotState::Free;
        slot.pixelBytes = 0;
        slot.rowPitch = 0;
        state.candidateIndex.reset();
    }
}

void RemoteVisualReplayRecorder::RecordDemodObservation(
    const pbrealcapturereplay::ReplayV2DemodObservationView& observation) noexcept
{
    if (!implementation_)
    {
        return;
    }
    auto& state = *implementation_->state;
    std::lock_guard lock(state.mutex);
    if (state.stopRequested || state.demodObservations.size() >= state.config.limits.maximumCaptureFrames)
    {
        state.droppedDemodObservations++;
        return;
    }
    const bool duplicate = std::ranges::any_of(state.demodObservations, [&](const auto& existing)
    {
        return existing.captureEpoch == observation.captureEpoch &&
            existing.captureObservation == observation.captureObservation;
    });
    if (duplicate)
    {
        state.droppedDemodObservations++;
        return;
    }
    try
    {
        state.demodObservations.push_back(observation);
    }
    catch (...)
    {
        state.evidenceValid = false;
        state.droppedDemodObservations++;
    }
}

void RemoteVisualReplayRecorder::RequestStop() noexcept
{
    if (!implementation_)
    {
        return;
    }
    auto& state = *implementation_->state;
    std::lock_guard lock(state.mutex);
    state.stopRequested = true;
    state.condition.notify_all();
}

pbcapturenormalize::CaptureStatus RemoteVisualReplayRecorder::Stop(const std::uint32_t maximumWaitMilliseconds) noexcept
{
    if (!implementation_)
    {
        return {};
    }
    RequestStop();
    auto& implementation = *implementation_;
    auto& state = *implementation.state;
    {
        std::unique_lock lock(state.mutex);
        if (!state.condition.wait_for(lock, std::chrono::milliseconds(maximumWaitMilliseconds),
            [&] { return state.workerStopped; }))
        {
            state.evidenceValid = false;
            if (state.lastError)
            {
                state.lastError = pbrealcapturereplay::ReplayStatus::Failure(
                    pbrealcapturereplay::ReplayError::IoFailure,
                    pbrealcapturereplay::ReplayStage::Flush, WAIT_TIMEOUT);
            }
            if (implementation.worker.joinable())
            {
                implementation.worker.detach();
                implementation.detached = true;
            }
            return Failure(pbcapturenormalize::CaptureError::Timeout, HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        }
    }
    if (implementation.worker.joinable())
    {
        implementation.worker.join();
    }
    return state.finalized && state.evidenceValid ? pbcapturenormalize::CaptureStatus{} :
        Failure(pbcapturenormalize::CaptureError::ConsumerFailure, E_FAIL);
}

RemoteVisualReplayRecorderSnapshot RemoteVisualReplayRecorder::GetSnapshot() const noexcept
{
    if (!implementation_)
    {
        return {};
    }
    const auto& state = *implementation_->state;
    std::lock_guard lock(state.mutex);
    RemoteVisualReplayRecorderSnapshot snapshot;
    snapshot.enabled = true;
    snapshot.stopRequested = state.stopRequested;
    snapshot.workerStopped = state.workerStopped;
    snapshot.finalized = state.finalized;
    snapshot.evidenceValid = state.evidenceValid;
    snapshot.processingReservedBytes = state.processingReservedBytes;
    snapshot.analyzedFrames = state.analyzedFrames;
    snapshot.enqueuedFrames = state.enqueuedFrames;
    snapshot.writtenFrames = state.writtenFrames;
    snapshot.droppedFrames = state.droppedFrames;
    snapshot.queuedDemodObservations = state.demodObservations.size();
    snapshot.writtenDemodObservations = state.writtenDemodObservations;
    snapshot.droppedDemodObservations = state.droppedDemodObservations;
    snapshot.queueDepth = state.queueCount;
    snapshot.queueHighWater = state.queueHighWater;
    snapshot.lastError = state.lastError;
    if (state.writer)
    {
        snapshot.fileBytes = state.writer->GetSnapshot().fileBytes;
    }
    return snapshot;
}

} // namespace pbapp
