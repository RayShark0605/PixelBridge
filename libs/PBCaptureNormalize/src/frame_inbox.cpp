#include "capture_runtime.h"
#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <limits>

namespace pbcapturenormalize::detail
{

FrameLease::FrameLease(void* const frame, const CloseFunction close, const TextureFunction texture, std::shared_ptr<LeaseCounters> counters,
                       const CaptureSize size, const std::int64_t timestamp, const std::uint64_t epoch) noexcept
    : contentSize(size), timestamp100ns(timestamp), captureEpoch(epoch), rawTimestamp(timestamp), frame_(frame), close_(close), texture_(texture), counters_(std::move(counters))
{
    if (frame_ != nullptr && counters_)
    {
        const auto live = counters_->live.fetch_add(1) + 1;
        auto previous = counters_->highWater.load();
        while (previous < live && !counters_->highWater.compare_exchange_weak(previous, live))
        {
        }
    }
}

FrameLease::~FrameLease()
{
    Reset();
}

FrameLease::FrameLease(FrameLease&& other) noexcept
{
    *this = std::move(other);
}

FrameLease& FrameLease::operator=(FrameLease&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        frame_ = std::exchange(other.frame_, nullptr);
        close_ = other.close_;
        texture_ = other.texture_;
        counters_ = std::move(other.counters_);
        contentSize = other.contentSize;
        timestamp100ns = other.timestamp100ns;
        captureEpoch = other.captureEpoch;
        arrivalOrdinal = other.arrivalOrdinal;
        cursorState = other.cursorState;
        pointer = other.pointer;
        timestampDomain = other.timestampDomain;
        rawTimestamp = other.rawTimestamp;
        rawFrequency = other.rawFrequency;
        arrivalQpc100ns = other.arrivalQpc100ns;
    }
    return *this;
}

void FrameLease::Reset() noexcept
{
    if (frame_ != nullptr)
    {
        const HRESULT result = close_(std::exchange(frame_, nullptr));
        if (counters_)
        {
            if (FAILED(result))
            {
                HRESULT expected = S_OK;
                if (!counters_->closeError.compare_exchange_strong(expected, result))
                {
                    const bool previousDeviceLoss = expected == DXGI_ERROR_DEVICE_REMOVED || expected == DXGI_ERROR_DEVICE_RESET || expected == DXGI_ERROR_DEVICE_HUNG;
                    const bool currentDeviceLoss = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG;
                    if (previousDeviceLoss && !currentDeviceLoss)
                    {
                        // At most one promotion follows the initial S_OK -> error.
                        // A strong-CAS failure here means another non-device error
                        // already won; keep that first error without a retry loop.
                        static_cast<void>(counters_->closeError.compare_exchange_strong(expected, result));
                    }
                }
            }
            counters_->live.fetch_sub(1);
        }
    }
    counters_.reset();
}

FrameLease::operator bool() const noexcept
{
    return frame_ != nullptr;
}

HRESULT FrameLease::GetTexture(ID3D11Texture2D** const texture) const noexcept
{
    if (texture == nullptr)
    {
        return E_POINTER;
    }
    *texture = nullptr;
    return frame_ != nullptr && texture_ != nullptr ? texture_(frame_, texture) : E_UNEXPECTED;
}

FrameInbox::FrameInbox(const CaptureConfig& config) : counters(std::make_shared<LeaseCounters>()), limit_(config.queuedFrameLimit)
{
}

void FrameInbox::SetClockFrequency(const std::int64_t frequency) noexcept
{
    clockFrequency.store(frequency, std::memory_order_release);
}

void FrameInbox::Push(FrameLease frame) noexcept
{
    // Sample the arrival on the producer thread before any queue/epoch check:
    // the capture instant is never later than this sample, so it bounds the
    // frame age independently of the backend's own (possibly ahead-of-time)
    // stamp. A failed sample leaves the lease invalid-marked and the age gate
    // falls back to the backend claim, exactly as before this fix.
    const auto frequency = clockFrequency.load(std::memory_order_acquire);
    LARGE_INTEGER counter{};
    std::int64_t arrival100ns = -1;
    if (frequency > 0 && QueryPerformanceCounter(&counter) && ConvertQpcTo100ns(counter.QuadPart, frequency, arrival100ns))
    {
        frame.arrivalQpc100ns = arrival100ns;
    }
    FrameLease stale;
    {
        const std::lock_guard lock(mutex_);
        pbprotocol::SaturatingIncrementUnsigned(snapshot_.arrivedFrames);
        frame.arrivalOrdinal = snapshot_.arrivedFrames;
        if (!frame || frame.captureEpoch != epoch_ || !accepting_ || snapshot_.stopRequested)
        {
            pbprotocol::SaturatingIncrementUnsigned(snapshot_.droppedFrames);
            return;
        }
        if (frame.contentSize.width <= 0 || frame.contentSize.height <= 0 || frame.contentSize.width > 16384 || frame.contentSize.height > 16384 ||
            frame.timestamp100ns < 0)
        {
            snapshot_.error = CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Callback);
            accepting_ = false;
            pbprotocol::SaturatingIncrementUnsigned(snapshot_.droppedFrames);
        }
        else if (frame.contentSize != size_)
        {
            accepting_ = false;
            snapshot_.recreateRequested = true;
            snapshot_.requestedSize = frame.contentSize;
            pbprotocol::SaturatingIncrementUnsigned(snapshot_.droppedFrames);
        }
        else
        {
            if (count_ == limit_ - retiringFrames_)
            {
                stale = std::move(frames_[0]);
                for (std::size_t index = 1; index < count_; index++)
                {
                    frames_[index - 1] = std::move(frames_[index]);
                }
                count_--;
                pbprotocol::SaturatingIncrementUnsigned(snapshot_.droppedFrames);
            }
            frames_[count_++] = std::move(frame);
        }
    }
    wake_.notify_one();
}

void FrameInbox::ReportError(const CaptureStatus status, const std::uint64_t epoch) noexcept
{
    {
        const std::lock_guard lock(mutex_);
        if (epoch == epoch_ && accepting_ && !snapshot_.stopRequested && snapshot_.error)
        {
            snapshot_.error = status ? CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Callback) : status;
            accepting_ = false;
        }
    }
    wake_.notify_one();
}

void FrameInbox::Resume(const CaptureSize size, const std::uint64_t epoch) noexcept
{
    const std::lock_guard lock(mutex_);
    size_ = size;
    epoch_ = epoch;
    snapshot_.error = {};
    snapshot_.recreateRequested = false;
    snapshot_.requestedSize = size;
    accepting_ = !snapshot_.stopRequested;
}

void FrameInbox::Pause() noexcept
{
    std::array<FrameLease, maximumQueuedFrames> stale;
    {
        const std::lock_guard lock(mutex_);
        accepting_ = false;
        stale.swap(frames_);
        snapshot_.droppedFrames = pbprotocol::SaturatingAddUnsigned(snapshot_.droppedFrames, static_cast<std::uint64_t>(count_));
        count_ = 0;
    }
    for (auto& frame : stale)
    {
        frame.Reset();
    }
}

void FrameInbox::RequestStop() noexcept
{
    {
        const std::lock_guard lock(mutex_);
        snapshot_.stopRequested = true;
        accepting_ = false;
    }
    wake_.notify_one();
}

void FrameInbox::RequestRecreate() noexcept
{
    {
        const std::lock_guard lock(mutex_);
        if (accepting_)
        {
            snapshot_.recreateRequested = true;
            snapshot_.requestedSize = size_;
            accepting_ = false;
        }
    }
    wake_.notify_one();
}

bool FrameInbox::TakeNewest(FrameLease& frame) noexcept
{
    std::array<FrameLease, maximumQueuedFrames> stale;
    std::uint32_t newest = 0;
    {
        const std::lock_guard lock(mutex_);
        if (!accepting_ || count_ == 0 || snapshot_.stopRequested)
        {
            return false;
        }
        stale.swap(frames_);
        snapshot_.droppedFrames = pbprotocol::SaturatingAddUnsigned(snapshot_.droppedFrames, static_cast<std::uint64_t>(count_ - 1));
        newest = count_ - 1;
        retiringFrames_ = newest;
        count_ = 0;
    }
    frame = std::move(stale[newest]);
    for (auto& retired : stale)
    {
        retired.Reset();
    }
    {
        const std::lock_guard lock(mutex_);
        retiringFrames_ = 0;
    }
    return true;
}

InboxSnapshot FrameInbox::GetSnapshot() const noexcept
{
    const std::lock_guard lock(mutex_);
    auto result = snapshot_;
    result.queuedFrames = count_;
    return result;
}

void FrameInbox::Wait() noexcept
{
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, std::chrono::milliseconds(2), [this]
    {
        // The producer can publish just before the owner enters wait. Recheck
        // every durable wake reason under the same mutex so that notification
        // order cannot add a fixed 2 ms delay to an already queued WGC frame.
        // An empty healthy queue still times out to poll GPU completions.
        return count_ != 0 || snapshot_.stopRequested || snapshot_.recreateRequested || !snapshot_.error;
    });
}

CaptureStatus FromHresult(const HRESULT result, const CaptureStage stage) noexcept
{
    if (SUCCEEDED(result))
    {
        return {};
    }
    const auto error = result == E_OUTOFMEMORY ? CaptureError::OutOfMemory :
                       result == E_ACCESSDENIED ? CaptureError::AccessLost : CaptureError::NativeFailure;
    return CaptureStatus::Failure(error, stage, static_cast<std::int32_t>(result));
}

CaptureStatus ResolveRecreateContentSize(const CaptureSize requestedSize, const CaptureEnvironment& previous,
                                        const pbscreenregion::ScreenCaptureRegion& current, CaptureSize& output) noexcept
{
    const auto width = static_cast<std::int64_t>(current.monitorPhysicalRect.right) - current.monitorPhysicalRect.left;
    const auto height = static_cast<std::int64_t>(current.monitorPhysicalRect.bottom) - current.monitorPhysicalRect.top;
    if (requestedSize.width <= 0 || requestedSize.height <= 0 || requestedSize.width > 16384 || requestedSize.height > 16384 ||
        width <= 0 || height <= 0 || width > 16384 || height > 16384 || current.monitor != previous.region.monitor)
    {
        return CaptureStatus::Failure(CaptureError::RegionChanged, CaptureStage::Recreate);
    }
    const CaptureSize liveSize{static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
    // A changed ContentSize is authoritative only when it agrees with the live
    // monitor. An environment-only invalidation carries the previous size.
    // Never use GraphicsCaptureItem's potentially stale initial Size here.
    if (requestedSize != previous.contentSize && requestedSize != liveSize)
    {
        return CaptureStatus::Failure(CaptureError::RegionChanged, CaptureStage::Recreate);
    }
    output = liveSize;
    return {};
}

} // namespace pbcapturenormalize::detail
