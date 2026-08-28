#pragma once

#include "pbcapturenormalize/capture_types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace pbcapturenormalize::detail
{

inline constexpr std::size_t maximumQueuedFrames = 4;
inline constexpr std::size_t maximumRoiTextures = 8;

struct LeaseCounters
{
    std::atomic<std::uint32_t> live{0};
    std::atomic<std::uint32_t> highWater{0};
    std::atomic<HRESULT> closeError{S_OK};
};

// An allocation-free, move-only OS lease. The native object is the FRAME, not
// a detached Surface. Texture acquisition is private to the submission owner.
class FrameLease
{
public:
    using CloseFunction = HRESULT (*)(void*) noexcept;
    using TextureFunction = HRESULT (*)(void*, ID3D11Texture2D**) noexcept;
    FrameLease() noexcept = default;
    FrameLease(void* frame, CloseFunction close, TextureFunction texture, std::shared_ptr<LeaseCounters> counters,
               CaptureSize size, std::int64_t timestamp, std::uint64_t epoch) noexcept;
    ~FrameLease();
    FrameLease(FrameLease&& other) noexcept;
    FrameLease& operator=(FrameLease&& other) noexcept;
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;
    void Reset() noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] HRESULT GetTexture(ID3D11Texture2D** texture) const noexcept;
    CaptureSize contentSize;
    std::int64_t timestamp100ns = 0;
    std::uint64_t captureEpoch = 0;
    std::uint64_t arrivalOrdinal = 0;
    CursorState cursorState = CursorState::Unknown;
    CapturePointerMetadata pointer;
    CaptureTimestampDomain timestampDomain = CaptureTimestampDomain::WgcSystemRelative100ns;
    std::int64_t rawTimestamp = 0;
    std::int64_t rawFrequency = 10000000;

private:
    void* frame_ = nullptr;
    CloseFunction close_ = nullptr;
    TextureFunction texture_ = nullptr;
    std::shared_ptr<LeaseCounters> counters_;
};

struct InboxSnapshot
{
    bool stopRequested = false;
    bool recreateRequested = false;
    CaptureSize requestedSize;
    CaptureStatus error;
    std::uint64_t arrivedFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint32_t queuedFrames = 0;
};

class FrameInbox
{
public:
    explicit FrameInbox(const CaptureConfig& config);
    // Push/ReportError are the only producer entrypoints. No COM Close, user
    // call, texture operation or OS teardown is performed under this mutex.
    void Push(FrameLease frame) noexcept;
    void ReportError(CaptureStatus status, std::uint64_t epoch) noexcept;
    void Resume(CaptureSize size, std::uint64_t epoch) noexcept;
    void Pause() noexcept;
    void RequestStop() noexcept;
    void RequestRecreate() noexcept;
    [[nodiscard]] bool TakeNewest(FrameLease& frame) noexcept;
    [[nodiscard]] InboxSnapshot GetSnapshot() const noexcept;
    void Wait() noexcept;
    std::shared_ptr<LeaseCounters> counters;

private:
    const std::uint32_t limit_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::array<FrameLease, maximumQueuedFrames> frames_;
    std::uint32_t count_ = 0;
    // Stale frames being closed outside the mutex still consume queue capacity.
    std::uint32_t retiringFrames_ = 0;
    std::uint64_t epoch_ = 0;
    CaptureSize size_;
    bool accepting_ = false;
    InboxSnapshot snapshot_;
};

struct CaptureLayout
{
    D3D11_BOX sourceBox{};
    std::uint32_t roiWidth = 0;
    std::uint32_t roiHeight = 0;
    std::uint32_t poolBufferCount = 0;
    std::uint64_t totalBytes = 0;
};

[[nodiscard]] CaptureStatus ValidateLayout(const CaptureConfig& config, const CaptureEnvironment& environment, CaptureLayout& layout) noexcept;
[[nodiscard]] CaptureStatus ResolveRecreateContentSize(CaptureSize requestedSize, const CaptureEnvironment& previous,
                                                      const pbscreenregion::ScreenCaptureRegion& current, CaptureSize& output) noexcept;
[[nodiscard]] CaptureStatus FromHresult(HRESULT result, CaptureStage stage) noexcept;

struct CompletionResult
{
    CaptureStatus status;
    bool complete = false;
};

class DeferredCleanup
{
public:
    virtual ~DeferredCleanup() = default;
    virtual void CompleteDeferredShutdown() noexcept = 0;
};

// Only OS effects are replaceable. Native, deterministic and GPU tests all use
// the same production inbox/owner state machine and lease retirement decisions.
class CaptureBackend
{
public:
    virtual ~CaptureBackend() = default;
    [[nodiscard]] virtual CaptureBackendKind Kind() const noexcept { return CaptureBackendKind::Wgc; }
    [[nodiscard]] virtual CaptureStatus Acquire() noexcept { return {}; }
    [[nodiscard]] virtual bool WaitingForEnvironment() const noexcept { return false; }
    virtual void UpdateSnapshot(CaptureSnapshot&) const noexcept {}
    [[nodiscard]] virtual CaptureStatus Complete(RawRoiConsumer&, const RawRoiFrameMetadata&, std::size_t, bool) noexcept { return {}; }
    [[nodiscard]] virtual CaptureStatus Initialize(const CaptureConfig& config, std::shared_ptr<FrameInbox> inbox) noexcept = 0;
    [[nodiscard]] virtual CaptureStatus Start(std::uint64_t epoch) noexcept = 0;
    [[nodiscard]] virtual CaptureStatus Pause() noexcept = 0;
    [[nodiscard]] virtual bool CallbacksIdle() const noexcept = 0;
    [[nodiscard]] virtual CaptureStatus CheckEnvironment(bool& changed) noexcept = 0;
    [[nodiscard]] virtual CaptureStatus Recreate(CaptureSize requestedSize) noexcept = 0;
    [[nodiscard]] virtual CaptureEnvironment GetEnvironment() const noexcept = 0;
    [[nodiscard]] virtual CaptureCapabilities GetCapabilities() const noexcept = 0;
    [[nodiscard]] virtual CaptureStatus NotifyEpoch(RawRoiConsumer& consumer, std::uint64_t epoch) noexcept = 0;
    // submitted is set BEFORE the first GPU read. Failure after that point still
    // requires retirement; callers must never infer 'no copy' from a failed HRESULT.
    [[nodiscard]] virtual CaptureStatus Copy(const FrameLease& frame, std::size_t slot, bool& submitted) noexcept = 0;
    [[nodiscard]] virtual CaptureStatus Consume(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, std::size_t slot) noexcept = 0;
    [[nodiscard]] virtual CompletionResult Poll(std::size_t slot) noexcept = 0;
    [[nodiscard]] virtual bool DeviceRemoved() const noexcept = 0;
    [[nodiscard]] virtual std::int32_t DeviceRemovalReason() const noexcept = 0;
    // Called only with a quiescent producer and no remaining GPU work, except
    // after a completion/confirmed-device-removal notification in deferred cleanup.
    [[nodiscard]] virtual CaptureStatus Shutdown() noexcept = 0;
    // Preallocated at Initialize: cannot fail/allocate after GPU submission.
    virtual void DeferShutdown(std::shared_ptr<DeferredCleanup> owner) noexcept = 0;
};

class CaptureRuntime
{
public:
    [[nodiscard]] static CaptureStatus Create(const CaptureConfig& config, std::shared_ptr<RawRoiConsumer> consumer,
                                              std::unique_ptr<CaptureBackend> backend, std::unique_ptr<CaptureRuntime>& output) noexcept;
    ~CaptureRuntime();
    [[nodiscard]] CaptureSnapshot GetSnapshot() const noexcept;
    void RequestStop() noexcept;
    void RequestRecreate() noexcept;
    [[nodiscard]] CaptureStatus Stop() noexcept;
    CaptureRuntime(const CaptureRuntime&) = delete;
    CaptureRuntime& operator=(const CaptureRuntime&) = delete;
private:
    struct Implementation;
    explicit CaptureRuntime(std::shared_ptr<Implementation> implementation) noexcept;
    std::shared_ptr<Implementation> implementation_;
};

} // namespace pbcapturenormalize::detail
