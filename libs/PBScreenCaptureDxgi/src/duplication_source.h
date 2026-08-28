#pragma once

#include "capture_internal.h"

#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <span>
#include <vector>

namespace pbscreencapturedxgi::detail
{

inline constexpr std::uint32_t maximumPointerShapeBytes = 256 * 1024;
inline constexpr std::uint32_t maximumPointerDimension = 2048;
inline constexpr std::uint32_t maximumEnvironmentAttempts = 3;

struct DuplicationFormatPlan
{
    std::array<DXGI_FORMAT, 3> formats{};
    std::uint32_t formatCount = 0;
    bool allowLegacy = false;
    bool operator==(const DuplicationFormatPlan&) const = default;
};

[[nodiscard]] CaptureStatus BuildDuplicationFormatPlan(DXGI_FORMAT requestedFormat, bool hdr, DuplicationFormatPlan& output) noexcept;
[[nodiscard]] CaptureStatus FilterDuplicationFormatPlan(const DuplicationFormatPlan& plan, const std::array<UINT, 3>& formatSupport,
                                                        DuplicationFormatPlan& output) noexcept;
[[nodiscard]] CaptureStatus ValidateDuplicationPreflight(const CaptureConfig& config, const pbscreenregion::ScreenCaptureRegion& currentRegion) noexcept;
[[nodiscard]] bool MayUseLegacyDuplication(const DuplicationFormatPlan& plan, HRESULT modernResult) noexcept;
[[nodiscard]] bool IsEnvironmentUnavailable(HRESULT result) noexcept;
enum class OutputQueryDisposition
{
    Unchanged,
    Rebuild,
    Failure
};
[[nodiscard]] OutputQueryDisposition ClassifyOutputQueryResult(HRESULT result, bool alreadyConfirmedChange, bool waiting) noexcept;
[[nodiscard]] CaptureStatus ResolveDuplicationEnvironment(const CaptureConfig& config, const DXGI_OUTDUPL_DESC& description,
                                                          bool modernDuplication, CaptureEnvironment& environment) noexcept;
[[nodiscard]] CaptureStatus QpcTo100ns(std::int64_t counter, std::int64_t frequency, std::int64_t& output) noexcept;
[[nodiscard]] CaptureStatus ValidatePointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape, std::uint32_t bytes) noexcept;

class EnvironmentRetryBudget
{
public:
    using Clock = std::chrono::steady_clock;
    void Reset() noexcept;
    void RecordAttempt(Clock::time_point now) noexcept;
    [[nodiscard]] bool CanRetry(Clock::time_point now) const noexcept;
    [[nodiscard]] std::uint32_t Attempts() const noexcept;
private:
    std::uint32_t attempts_ = 0;
    Clock::time_point nextAttempt_{};
};

// Only these OS methods are replaced by deterministic tests. The production
// lease ownership, pointer parsing, error mapping and inbox path remain shared.
class DuplicationApi
{
public:
    virtual ~DuplicationApi() = default;
    [[nodiscard]] virtual HRESULT AcquireNextFrame(std::uint32_t timeoutMilliseconds, DXGI_OUTDUPL_FRAME_INFO& info,
                                                  Microsoft::WRL::ComPtr<IDXGIResource>& resource) noexcept = 0;
    [[nodiscard]] virtual HRESULT GetFramePointerShape(std::span<std::byte> buffer, std::uint32_t& requiredBytes,
                                                       DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape) noexcept = 0;
    [[nodiscard]] virtual HRESULT ReleaseFrame() noexcept = 0;
};

class DuplicationFrameSource
{
public:
    DuplicationFrameSource() = default;
    DuplicationFrameSource(const DuplicationFrameSource&) = delete;
    DuplicationFrameSource& operator=(const DuplicationFrameSource&) = delete;
    DuplicationFrameSource(DuplicationFrameSource&&) = delete;
    DuplicationFrameSource& operator=(DuplicationFrameSource&&) = delete;
    [[nodiscard]] CaptureStatus Initialize(std::unique_ptr<DuplicationApi> duplication, std::shared_ptr<FrameInbox> inbox,
                                           const CaptureEnvironment& environment, std::int64_t qpcFrequency) noexcept;
    void Start(std::uint64_t epoch) noexcept;
    [[nodiscard]] CaptureStatus Acquire() noexcept;
    // Reset is legal only after shared runtime has retired all source leases.
    [[nodiscard]] CaptureStatus Reset() noexcept;
    [[nodiscard]] bool Outstanding() const noexcept;
    [[nodiscard]] bool AccessLost() const noexcept;
    [[nodiscard]] bool ImageAcquired() const noexcept;
    [[nodiscard]] CursorState GetCursorState() const noexcept;
    void UpdateSnapshot(CaptureSnapshot& snapshot) const noexcept;

private:
    [[nodiscard]] static HRESULT CloseLease(void* source) noexcept;
    [[nodiscard]] static HRESULT GetLeaseTexture(void* source, ID3D11Texture2D** texture) noexcept;
    [[nodiscard]] CaptureStatus UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO& info) noexcept;
    [[nodiscard]] CaptureStatus HandleResult(HRESULT result, CaptureStage stage) noexcept;
    void ReportAccessLost() noexcept;
    std::unique_ptr<DuplicationApi> duplication_;
    std::shared_ptr<FrameInbox> inbox_;
    Microsoft::WRL::ComPtr<IDXGIResource> resource_;
    std::vector<std::byte> pointerShape_;
    CaptureEnvironment environment_;
    std::int64_t qpcFrequency_ = 0;
    std::int64_t lastMouseUpdate_ = 0;
    std::int64_t lastPresent_ = 0;
    std::uint64_t epoch_ = 0;
    CursorState cursorState_ = CursorState::Unknown;
    CapturePointerMetadata pointer_;
    bool outstanding_ = false;
    bool accessLost_ = false;
    std::uint64_t acquireTimeouts_ = 0;
    std::uint64_t pointerOnlyFrames_ = 0;
    std::uint64_t accumulatedFrames_ = 0;
    std::uint64_t accessLostEvents_ = 0;
};

} // namespace pbscreencapturedxgi::detail
