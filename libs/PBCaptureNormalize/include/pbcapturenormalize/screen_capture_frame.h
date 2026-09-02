#pragma once

#include "pbcapturenormalize/capture_types.h"

#include <array>
#include <cstddef>
#include <optional>

namespace pbcapturenormalize
{

struct ScreenCaptureDomain
{
    // Local capture identity, not a sender SessionId or a payload channel.
    // A new capture/backend instance gets an independent OS-random identity.
    std::array<std::byte, 16> sourceId{};
    std::uint64_t captureEpoch = 0;
    bool operator==(const ScreenCaptureDomain&) const = default;
};

struct ScreenCaptureTimestamp
{
    CaptureTimestampDomain domain = CaptureTimestampDomain::WgcSystemRelative100ns;
    std::int64_t rawValue = 0;
    std::int64_t rawFrequency = 10000000;
    std::int64_t monotonic100ns = 0;
    // QPC arrival sampled at inbox push; -1 = not measured. The age gate uses
    // the earliest valid time source, so a backend claim stamped ahead of the
    // arrival (WGC SystemRelativeTime) cannot masquerade as a fresh frame.
    std::int64_t arrivalQpc100ns = -1;
    bool operator==(const ScreenCaptureTimestamp&) const = default;
};

enum class CaptureSignalEncoding : std::uint8_t
{
    Unknown, SdrRgb, LinearScRgb
};

// Pure value metadata: safe to copy to a CPU job without retaining GPU objects.
struct ScreenCaptureFrameMetadata
{
    ScreenCaptureDomain domain;
    CaptureBackendKind backend = CaptureBackendKind::Wgc;
    std::uint64_t captureObservation = 0;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t slotGeneration = 0;
    std::uint32_t slotIndex = 0;
    RECT physicalRoi{};
    CaptureSize sourceContentSize;
    CaptureSize sourceExtent;
    CaptureSize roiSize;
    DXGI_MODE_ROTATION displayRotation = DXGI_MODE_ROTATION_IDENTITY;
    DXGI_MODE_ROTATION sourceTransform = DXGI_MODE_ROTATION_IDENTITY;
    DXGI_FORMAT sourcePixelFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT pixelFormat = DXGI_FORMAT_UNKNOWN;
    LUID adapterLuid{};
    std::uint32_t bitsPerColor = 0;
    std::uint32_t outputColorSpace = 0;
    CaptureSignalEncoding signalEncoding = CaptureSignalEncoding::Unknown;
    bool hdr = false;
    ScreenCaptureTimestamp timestamp;
    std::optional<std::uint64_t> roiCopyTime100ns;
    bool isCursorExcluded = false;
    CursorState sourceCursorState = CursorState::Unknown;
    CapturePointerMetadata pointer;
};

struct ScreenCaptureFrame
{
    ScreenCaptureFrameMetadata metadata;
    // Borrowed PB-owned upright ROI, never an OS acquisition/pool surface.
    // Submit and a non-cancelled CompleteStage may use this pointer. A consumer
    // may retain only its pointer value while that staged job is pending;
    // ComPtr/AddRef does NOT extend the capture slot lease outside callbacks.
    ID3D11Texture2D* texture = nullptr;
};

enum class CaptureErasureReason : std::uint8_t
{
    None, InactiveDomain, StaleObservation, Expired, InvalidTimestamp, CursorUnknown,
    CursorPossiblyComposited, InvalidMetadata, InvalidOwnedTexture, Count
};

struct CaptureErasure
{
    ScreenCaptureDomain domain;
    std::uint64_t captureObservation = 0;
    CaptureErasureReason reason = CaptureErasureReason::None;
};

class ScreenCaptureConsumer
{
public:
    virtual ~ScreenCaptureConsumer() = default;
    // Exact additional resident budget (for example diagnostic staging + CPU
    // buffers), reserved BEFORE allocating the OS pool and owned texture ring.
    [[nodiscard]] virtual std::uint64_t ReservedBytes() const noexcept { return 0; }
    [[nodiscard]] virtual CaptureStatus ValidateConfiguration(const CaptureConfig&) const noexcept { return {}; }
    [[nodiscard]] virtual CaptureStatus DomainStarted(const ScreenCaptureDomain& domain, const CaptureEnvironment& environment, ID3D11Device* device) = 0;
    // Owner-side admission barrier, before drain/rebuild/stop, even if no new
    // device or frame ever arrives. Must be bounded; never scan an image here.
    virtual void DomainInvalidated(const ScreenCaptureDomain& domain) noexcept = 0;
    [[nodiscard]] virtual CaptureStatus Submit(const ScreenCaptureFrame& frame, ID3D11DeviceContext* context) = 0;
    // Optional staged completion. The first non-cancelled callback may submit
    // exactly one additional bounded GPU stage on context and return true. The
    // capture runtime records another retirement marker while retaining the
    // same PB-owned ROI slot; the following callback must return false. Once a
    // callback might have submitted work it must return true even when its
    // status is failure. cancelled always has null GPU objects and must never
    // request a continuation.
    [[nodiscard]] virtual CaptureConsumerCompletion CompleteStage(const ScreenCaptureFrameMetadata& metadata, ID3D11Texture2D*,
                                                                   ID3D11DeviceContext* context, bool cancelled)
    {
        return {Completed(metadata, context, cancelled), false};
    }
    // Legacy completion hook. It runs after the consumer-work marker but cannot
    // extend the ROI lease or submit new GPU work. cancelled can run during
    // deferred cleanup: do NOT Map or touch the context.
    [[nodiscard]] virtual CaptureStatus Completed(const ScreenCaptureFrameMetadata&, ID3D11DeviceContext*, bool) { return {}; }
    virtual void Erased(const CaptureErasure&) noexcept {}
};

struct CaptureNormalizeConfig
{
    CaptureConfig capture = []
    {
        CaptureConfig config;
        config.maximumFrameAgeMilliseconds = 250;
        return config;
    }();
};

struct CaptureNormalizeSnapshot
{
    bool enabled = false;
    bool active = false;
    ScreenCaptureDomain domain;
    std::uint64_t epochStarts = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t acceptedFrames = 0;
    std::uint64_t erasedFrames = 0;
    CaptureErasureReason lastErasure = CaptureErasureReason::None;
    std::array<std::uint64_t, static_cast<std::size_t>(CaptureErasureReason::Count)> erasures{};
    std::uint64_t reservedConsumerBytes = 0;
};

[[nodiscard]] const char* GetCaptureErasureName(CaptureErasureReason reason) noexcept;

} // namespace pbcapturenormalize
