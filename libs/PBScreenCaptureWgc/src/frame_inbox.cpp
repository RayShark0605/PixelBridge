#include "capture_internal.h"
#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <limits>

namespace pbscreencapturewgc::detail
{

FrameLease::FrameLease(void* const frame, const CloseFunction close, const TextureFunction texture, std::shared_ptr<LeaseCounters> counters,
                       const CaptureSize size, const std::int64_t timestamp, const std::uint64_t epoch) noexcept
    : contentSize(size), timestamp100ns(timestamp), captureEpoch(epoch), frame_(frame), close_(close), texture_(texture), counters_(std::move(counters))
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
                counters_->closeError.compare_exchange_strong(expected, result);
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

FrameInbox::FrameInbox(const WgcCaptureConfig& config) : counters(std::make_shared<LeaseCounters>()), limit_(config.queuedFrameLimit)
{
}

void FrameInbox::Push(FrameLease frame) noexcept
{
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
    wake_.wait_for(lock, std::chrono::milliseconds(2));
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

CaptureStatus ValidateLayout(const WgcCaptureConfig& config, const CaptureEnvironment& environment, CaptureLayout& layout) noexcept
{
    if (config.queuedFrameLimit == 0 || config.queuedFrameLimit > maximumQueuedFrames || config.roiTextureCount < 2 ||
        config.roiTextureCount > maximumRoiTextures ||
        (config.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM && config.pixelFormat != DXGI_FORMAT_R16G16B16A16_FLOAT))
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    const auto& region = environment.region;
    const auto& rectangle = region.physicalRect;
    const auto& monitor = region.monitorPhysicalRect;
    const auto width = static_cast<std::int64_t>(rectangle.right) - rectangle.left;
    const auto height = static_cast<std::int64_t>(rectangle.bottom) - rectangle.top;
    const auto monitorWidth = static_cast<std::int64_t>(monitor.right) - monitor.left;
    const auto monitorHeight = static_cast<std::int64_t>(monitor.bottom) - monitor.top;
    if (region.monitor == nullptr || width <= 0 || height <= 0 || monitorWidth <= 0 || monitorHeight <= 0 || monitorWidth > 16384 || monitorHeight > 16384 ||
        rectangle.left < monitor.left || rectangle.top < monitor.top || rectangle.right > monitor.right || rectangle.bottom > monitor.bottom ||
        environment.contentSize.width != monitorWidth || environment.contentSize.height != monitorHeight || region.dpiX == 0 || region.dpiY == 0 ||
        region.rotation < DXGI_MODE_ROTATION_IDENTITY || region.rotation > DXGI_MODE_ROTATION_ROTATE270)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Region);
    }
    const std::uint64_t pixelBytes = config.pixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM ? 4 : 8;
    const auto roiPixels = pbprotocol::CheckedMultiplyUint64(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height));
    const auto roiBytes = roiPixels ? pbprotocol::CheckedMultiplyUint64(roiPixels.Value(), pixelBytes) : roiPixels;
    const auto ringBytes = roiBytes ? pbprotocol::CheckedMultiplyUint64(roiBytes.Value(), config.roiTextureCount) : roiBytes;
    const auto poolBuffers = config.queuedFrameLimit + config.roiTextureCount + 1;
    const auto surfacePixels = pbprotocol::CheckedMultiplyUint64(static_cast<std::uint64_t>(monitorWidth), static_cast<std::uint64_t>(monitorHeight));
    const auto surfaceBytes = surfacePixels ? pbprotocol::CheckedMultiplyUint64(surfacePixels.Value(), pixelBytes) : surfacePixels;
    const auto poolBytes = surfaceBytes ? pbprotocol::CheckedMultiplyUint64(surfaceBytes.Value(), poolBuffers) : surfaceBytes;
    const auto totalBytes = poolBytes && ringBytes ? pbprotocol::CheckedAddUint64(poolBytes.Value(), ringBytes.Value()) : poolBytes;
    if (!roiBytes || !ringBytes || !poolBytes || !totalBytes || ringBytes.Value() > config.maximumRoiBytes || totalBytes.Value() > config.maximumCaptureBytes ||
        !pbprotocol::CheckedUint64ToSize(totalBytes.Value()))
    {
        return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration);
    }
    CaptureLayout candidate;
    candidate.sourceBox = {static_cast<UINT>(static_cast<std::int64_t>(rectangle.left) - monitor.left),
                           static_cast<UINT>(static_cast<std::int64_t>(rectangle.top) - monitor.top), 0,
                           static_cast<UINT>(static_cast<std::int64_t>(rectangle.right) - monitor.left),
                           static_cast<UINT>(static_cast<std::int64_t>(rectangle.bottom) - monitor.top), 1};
    candidate.roiWidth = static_cast<std::uint32_t>(width);
    candidate.roiHeight = static_cast<std::uint32_t>(height);
    candidate.poolBufferCount = poolBuffers;
    candidate.totalBytes = totalBytes.Value();
    layout = candidate;
    return {};
}

} // namespace pbscreencapturewgc::detail
