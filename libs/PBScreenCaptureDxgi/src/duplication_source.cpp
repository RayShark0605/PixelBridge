#include "duplication_source.h"
#include "pbprotocol/checked_integer.h"

#include <limits>
#include <new>

namespace pbscreencapturedxgi::detail
{

CaptureStatus BuildDuplicationFormatPlan(const DXGI_FORMAT requestedFormat, const bool hdr, DuplicationFormatPlan& output) noexcept
{
    if (requestedFormat != DXGI_FORMAT_B8G8R8A8_UNORM && requestedFormat != DXGI_FORMAT_R16G16B16A16_FLOAT &&
        requestedFormat != DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::CaptureItem);
    }
    DuplicationFormatPlan plan;
    plan.formats[0] = requestedFormat;
    plan.formatCount = 1;
    plan.allowLegacy = requestedFormat == DXGI_FORMAT_B8G8R8A8_UNORM && !hdr;
    for (const auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT})
    {
        if (format != requestedFormat)
        {
            plan.formats[plan.formatCount++] = format;
        }
    }
    output = plan;
    return {};
}

CaptureStatus FilterDuplicationFormatPlan(const DuplicationFormatPlan& plan, const std::array<UINT, 3>& formatSupport,
                                          DuplicationFormatPlan& output) noexcept
{
    if (plan.formatCount == 0 || plan.formatCount > plan.formats.size())
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::CaptureItem);
    }
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_DISPLAY;
    DuplicationFormatPlan filtered;
    filtered.allowLegacy = plan.allowLegacy;
    bool includesBgra = false;
    for (std::size_t index = 0; index < plan.formatCount; index++)
    {
        if ((formatSupport[index] & required) == required)
        {
            filtered.formats[filtered.formatCount++] = plan.formats[index];
            includesBgra = includesBgra || plan.formats[index] == DXGI_FORMAT_B8G8R8A8_UNORM;
        }
    }
    if (!includesBgra)
    {
        return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::CaptureItem);
    }
    output = filtered;
    return {};
}

CaptureStatus ValidateDuplicationPreflight(const CaptureConfig& config, const pbscreenregion::ScreenCaptureRegion& currentRegion) noexcept
{
    auto admissionConfig = config;
    admissionConfig.region = currentRegion;
    const auto status = ValidateCaptureConfig(admissionConfig, CaptureBackendKind::Dxgi);
    if (!status)
    {
        return status;
    }
    // DuplicateOutput1 may choose any advertised format, not just the preferred
    // one. Reserve its largest candidate before the OS creates a duplication
    // surface; a budget must never be satisfied by dropping high-depth formats.
    admissionConfig.pixelFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    CaptureEnvironment environment;
    environment.region = currentRegion;
    environment.backendKind = CaptureBackendKind::Dxgi;
    environment.pixelFormat = admissionConfig.pixelFormat;
    environment.contentSize = {static_cast<std::int32_t>(static_cast<std::int64_t>(currentRegion.monitorPhysicalRect.right) - currentRegion.monitorPhysicalRect.left),
                               static_cast<std::int32_t>(static_cast<std::int64_t>(currentRegion.monitorPhysicalRect.bottom) - currentRegion.monitorPhysicalRect.top)};
    environment.sourceRotation = currentRegion.rotation;
    const bool quarterTurn = currentRegion.rotation == DXGI_MODE_ROTATION_ROTATE90 || currentRegion.rotation == DXGI_MODE_ROTATION_ROTATE270;
    environment.sourceSize = quarterTurn ? CaptureSize{environment.contentSize.height, environment.contentSize.width} : environment.contentSize;
    CaptureLayout layout;
    const auto layoutStatus = ValidateLayout(admissionConfig, environment, layout);
    if (!layoutStatus)
    {
        return layoutStatus;
    }
    const auto totalBytes = pbprotocol::CheckedAddUint64(layout.totalBytes, maximumPointerShapeBytes);
    return totalBytes && totalBytes.Value() <= config.maximumCaptureBytes ? CaptureStatus{} :
        CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration);
}

bool MayUseLegacyDuplication(const DuplicationFormatPlan& plan, const HRESULT modernResult) noexcept
{
    return plan.allowLegacy && (modernResult == E_NOINTERFACE || modernResult == E_NOTIMPL);
}

bool IsEnvironmentUnavailable(const HRESULT result) noexcept
{
    return result == DXGI_ERROR_ACCESS_LOST || result == DXGI_ERROR_UNSUPPORTED || result == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ||
           result == DXGI_ERROR_SESSION_DISCONNECTED || result == E_ACCESSDENIED;
}

OutputQueryDisposition ClassifyOutputQueryResult(const HRESULT result, const bool alreadyConfirmedChange, const bool waiting) noexcept
{
    // An observed display/desktop change makes the old output advisory. Without
    // that evidence, an unrelated query failure must retain its fatal meaning.
    if (alreadyConfirmedChange)
    {
        return OutputQueryDisposition::Rebuild;
    }
    if (SUCCEEDED(result) || waiting)
    {
        return OutputQueryDisposition::Unchanged;
    }
    return IsEnvironmentUnavailable(result) ? OutputQueryDisposition::Rebuild : OutputQueryDisposition::Failure;
}

CaptureStatus ResolveDuplicationEnvironment(const CaptureConfig& config, const DXGI_OUTDUPL_DESC& description,
                                             const bool modernDuplication, CaptureEnvironment& environment) noexcept
{
    DuplicationFormatPlan plan;
    const auto formatStatus = BuildDuplicationFormatPlan(config.pixelFormat, environment.hdr, plan);
    if (!formatStatus)
    {
        return formatStatus;
    }
    const auto actualFormat = description.ModeDesc.Format;
    if ((actualFormat != DXGI_FORMAT_B8G8R8A8_UNORM && actualFormat != DXGI_FORMAT_R10G10B10A2_UNORM && actualFormat != DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        (environment.hdr && actualFormat == DXGI_FORMAT_B8G8R8A8_UNORM) ||
        (!modernDuplication && (!plan.allowLegacy || environment.bitsPerColor == 0 || environment.bitsPerColor > 8 || actualFormat != DXGI_FORMAT_B8G8R8A8_UNORM)))
    {
        return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::CaptureItem);
    }
    const auto width = static_cast<std::int64_t>(environment.region.monitorPhysicalRect.right) - environment.region.monitorPhysicalRect.left;
    const auto height = static_cast<std::int64_t>(environment.region.monitorPhysicalRect.bottom) - environment.region.monitorPhysicalRect.top;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || description.Rotation != environment.region.rotation ||
        description.Rotation < DXGI_MODE_ROTATION_IDENTITY || description.Rotation > DXGI_MODE_ROTATION_ROTATE270)
    {
        return CaptureStatus::Failure(CaptureError::RegionChanged, CaptureStage::CaptureItem);
    }
    const bool quarterTurn = description.Rotation == DXGI_MODE_ROTATION_ROTATE90 || description.Rotation == DXGI_MODE_ROTATION_ROTATE270;
    const CaptureSize visualSize{static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
    const CaptureSize sourceSize = quarterTurn ? CaptureSize{visualSize.height, visualSize.width} : visualSize;
    // ModeDesc describes a display mode; the actual unrotated surface is checked
    // independently by D3dRoiRing against sourceSize before any source GPU read.
    const bool rawDimensions = description.ModeDesc.Width == static_cast<UINT>(sourceSize.width) &&
                               description.ModeDesc.Height == static_cast<UINT>(sourceSize.height);
    const bool visualDimensions = description.ModeDesc.Width == static_cast<UINT>(visualSize.width) &&
                                  description.ModeDesc.Height == static_cast<UINT>(visualSize.height);
    if (!rawDimensions && !visualDimensions)
    {
        return CaptureStatus::Failure(CaptureError::RegionChanged, CaptureStage::CaptureItem);
    }
    auto resolved = environment;
    resolved.backendKind = CaptureBackendKind::Dxgi;
    resolved.pixelFormat = description.ModeDesc.Format;
    resolved.contentSize = visualSize;
    resolved.sourceSize = sourceSize;
    resolved.sourceRotation = description.Rotation;
    CaptureLayout layout;
    auto actualConfig = config;
    actualConfig.pixelFormat = actualFormat;
    const auto layoutStatus = ValidateLayout(actualConfig, resolved, layout);
    if (!layoutStatus)
    {
        return layoutStatus;
    }
    const auto totalBytes = pbprotocol::CheckedAddUint64(layout.totalBytes, maximumPointerShapeBytes);
    if (!totalBytes || totalBytes.Value() > config.maximumCaptureBytes)
    {
        return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration);
    }
    environment = resolved;
    return {};
}

CaptureStatus QpcTo100ns(const std::int64_t counter, const std::int64_t frequency, std::int64_t& output) noexcept
{
    return ConvertQpcTo100ns(counter, frequency, output) ? CaptureStatus{} : CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Callback);
}

CaptureStatus ValidatePointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape, const std::uint32_t bytes) noexcept
{
    if (bytes > maximumPointerShapeBytes || shape.Width > maximumPointerDimension || shape.Height > maximumPointerDimension * 2)
    {
        return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Callback);
    }
    if (bytes == 0 || shape.Width == 0 || shape.Height == 0 || shape.Pitch == 0 ||
        (shape.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME && shape.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR &&
         shape.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR))
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Callback);
    }
    const bool monochrome = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const auto visibleHeight = monochrome ? shape.Height / 2 : shape.Height;
    const std::uint64_t minimumPitch = monochrome ? (static_cast<std::uint64_t>(shape.Width) + 7) / 8 : static_cast<std::uint64_t>(shape.Width) * 4;
    const auto requiredBytes = pbprotocol::CheckedMultiplyUint64(shape.Pitch, shape.Height);
    if ((monochrome && shape.Height % 2 != 0) || visibleHeight > maximumPointerDimension || minimumPitch > shape.Pitch ||
        !requiredBytes || requiredBytes.Value() > bytes || shape.HotSpot.x < 0 || shape.HotSpot.y < 0 ||
        static_cast<UINT>(shape.HotSpot.x) >= shape.Width || static_cast<UINT>(shape.HotSpot.y) >= visibleHeight)
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Callback);
    }
    return {};
}

void EnvironmentRetryBudget::Reset() noexcept
{
    attempts_ = 0;
    nextAttempt_ = {};
}

void EnvironmentRetryBudget::RecordAttempt(const Clock::time_point now) noexcept
{
    if (attempts_ < maximumEnvironmentAttempts)
    {
        attempts_++;
    }
    nextAttempt_ = now + std::chrono::milliseconds(attempts_ <= 1 ? 250 : 1000);
}

bool EnvironmentRetryBudget::CanRetry(const Clock::time_point now) const noexcept
{
    return attempts_ < maximumEnvironmentAttempts && now >= nextAttempt_;
}

std::uint32_t EnvironmentRetryBudget::Attempts() const noexcept
{
    return attempts_;
}

CaptureStatus DuplicationFrameSource::Initialize(std::unique_ptr<DuplicationApi> duplication, std::shared_ptr<FrameInbox> inbox,
                                                const CaptureEnvironment& environment, const std::int64_t qpcFrequency) noexcept
{
    if (outstanding_ || !duplication || !inbox || qpcFrequency <= 0)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    try
    {
        pointerShape_.resize(maximumPointerShapeBytes);
    }
    catch (const std::bad_alloc&)
    {
        return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Configuration);
    }
    catch (...)
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Configuration);
    }
    duplication_ = std::move(duplication);
    inbox_ = std::move(inbox);
    environment_ = environment;
    qpcFrequency_ = qpcFrequency;
    lastMouseUpdate_ = 0;
    lastPresent_ = 0;
    cursorState_ = CursorState::Unknown;
    pointer_ = {};
    accessLost_ = false;
    return {};
}

void DuplicationFrameSource::Start(const std::uint64_t epoch) noexcept
{
    epoch_ = epoch;
}

void DuplicationFrameSource::ReportAccessLost() noexcept
{
    accessLost_ = true;
    pbprotocol::SaturatingIncrementUnsigned(accessLostEvents_);
    inbox_->RequestRecreate();
}

CaptureStatus DuplicationFrameSource::HandleResult(const HRESULT result, const CaptureStage stage) noexcept
{
    if (result == DXGI_ERROR_ACCESS_LOST)
    {
        ReportAccessLost();
        return {};
    }
    return FromHresult(result, stage);
}

CaptureStatus DuplicationFrameSource::UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO& info) noexcept
{
    if (info.LastMouseUpdateTime.QuadPart < 0 || (info.LastMouseUpdateTime.QuadPart == 0 && info.PointerShapeBufferSize != 0) ||
        (info.LastMouseUpdateTime.QuadPart != 0 && info.LastMouseUpdateTime.QuadPart < lastMouseUpdate_))
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Callback);
    }
    if (info.LastMouseUpdateTime.QuadPart == 0)
    {
        // PointerPosition is unspecified without an update timestamp. In
        // particular, its zero-initialized Visible field is not exclusion proof.
        return {};
    }
    if (info.PointerShapeBufferSize > pointerShape_.size())
    {
        return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Callback);
    }
    auto pointer = pointer_;
    if (info.PointerShapeBufferSize != 0)
    {
        std::uint32_t requiredBytes = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
        const HRESULT result = duplication_->GetFramePointerShape(pointerShape_, requiredBytes, shape);
        if (result == DXGI_ERROR_MORE_DATA)
        {
            return CaptureStatus::Failure(requiredBytes > pointerShape_.size() ? CaptureError::ResourceLimit : CaptureError::InvalidFrame,
                                          CaptureStage::Callback, result);
        }
        const auto status = HandleResult(result, CaptureStage::Callback);
        if (!status || accessLost_)
        {
            return status;
        }
        const auto shapeStatus = ValidatePointerShape(shape, requiredBytes);
        if (!shapeStatus)
        {
            return shapeStatus;
        }
        pointer.shapeKnown = true;
        pointer.shapeType = shape.Type;
        pointer.shapeWidth = shape.Width;
        pointer.shapeRawHeight = shape.Height;
        pointer.shapeVisibleHeight = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? shape.Height / 2 : shape.Height;
        pointer.shapePitch = shape.Pitch;
        pointer.shapeBytes = requiredBytes;
        pointer.hotspotX = shape.HotSpot.x;
        pointer.hotspotY = shape.HotSpot.y;
    }
    pointer.separateVisible = info.PointerPosition.Visible != FALSE;
    pointer.positionKnown = pointer.separateVisible;
    // DXGI does not define Position when a separate pointer is invisible. It
    // can contain stale/uninitialized values and cannot preserve a known box.
    pointer.physicalLeft = pointer.positionKnown ? static_cast<std::int64_t>(info.PointerPosition.Position.x) + environment_.region.monitorPhysicalRect.left : 0;
    pointer.physicalTop = pointer.positionKnown ? static_cast<std::int64_t>(info.PointerPosition.Position.y) + environment_.region.monitorPhysicalRect.top : 0;
    pointer.rawUpdateTimestamp = info.LastMouseUpdateTime.QuadPart;
    pointer_ = pointer;
    lastMouseUpdate_ = info.LastMouseUpdateTime.QuadPart;
    cursorState_ = info.PointerPosition.Visible != FALSE ? CursorState::SeparatePointer : CursorState::PossiblyComposited;
    return {};
}

CaptureStatus DuplicationFrameSource::Acquire() noexcept
{
    if (outstanding_ || accessLost_)
    {
        return {};
    }
    if (!duplication_ || !inbox_ || epoch_ == 0)
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Callback);
    }
    DXGI_OUTDUPL_FRAME_INFO info{};
    const HRESULT result = duplication_->AcquireNextFrame(0, info, resource_);
    if (result == DXGI_ERROR_WAIT_TIMEOUT)
    {
        pbprotocol::SaturatingIncrementUnsigned(acquireTimeouts_);
        resource_.Reset();
        return {};
    }
    if (FAILED(result))
    {
        resource_.Reset();
        return HandleResult(result, CaptureStage::Callback);
    }
    outstanding_ = true;
    FrameLease frame(this, CloseLease, GetLeaseTexture, inbox_->counters, environment_.contentSize, 0, epoch_);
    accumulatedFrames_ = pbprotocol::SaturatingAddUnsigned(accumulatedFrames_, static_cast<std::uint64_t>(info.AccumulatedFrames));
    const auto pointerStatus = UpdatePointer(info);
    if (!pointerStatus || accessLost_)
    {
        return pointerStatus;
    }
    if (info.LastPresentTime.QuadPart == 0)
    {
        if (info.AccumulatedFrames != 0 || info.TotalMetadataBufferSize != 0)
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Callback);
        }
        pbprotocol::SaturatingIncrementUnsigned(pointerOnlyFrames_);
        return {};
    }
    if (info.LastPresentTime.QuadPart < lastPresent_ || !resource_ || info.AccumulatedFrames == 0)
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Callback);
    }
    const auto timestampStatus = QpcTo100ns(info.LastPresentTime.QuadPart, qpcFrequency_, frame.timestamp100ns);
    if (!timestampStatus)
    {
        return timestampStatus;
    }
    frame.cursorState = cursorState_;
    frame.pointer = pointer_;
    frame.rawTimestamp = info.LastPresentTime.QuadPart;
    frame.rawFrequency = qpcFrequency_;
    frame.timestampDomain = CaptureTimestampDomain::DxgiQpcTicks;
    lastPresent_ = info.LastPresentTime.QuadPart;
    // Every admitted frame refers to the entire current surface. Dirty/move
    // metadata is deliberately not used as an image assembler or an allocator.
    inbox_->Push(std::move(frame));
    return {};
}

HRESULT DuplicationFrameSource::CloseLease(void* const pointer) noexcept
{
    auto& source = *static_cast<DuplicationFrameSource*>(pointer);
    if (!source.outstanding_ || !source.duplication_)
    {
        return DXGI_ERROR_INVALID_CALL;
    }
    const HRESULT result = source.duplication_->ReleaseFrame();
    source.resource_.Reset();
    source.outstanding_ = false;
    if (result == DXGI_ERROR_ACCESS_LOST)
    {
        // Recreate is recoverable, unlike a terminal generic FrameLease Close
        // error. Signal the inbox while preserving the exact once-only pairing.
        source.ReportAccessLost();
        return S_OK;
    }
    return result;
}

HRESULT DuplicationFrameSource::GetLeaseTexture(void* const pointer, ID3D11Texture2D** const texture) noexcept
{
    auto& source = *static_cast<DuplicationFrameSource*>(pointer);
    if (!source.outstanding_ || !source.resource_ || texture == nullptr)
    {
        return E_UNEXPECTED;
    }
    return source.resource_->QueryInterface(IID_PPV_ARGS(texture));
}

CaptureStatus DuplicationFrameSource::Reset() noexcept
{
    if (outstanding_)
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Shutdown);
    }
    resource_.Reset();
    duplication_.reset();
    inbox_.reset();
    cursorState_ = CursorState::Unknown;
    pointer_ = {};
    accessLost_ = false;
    epoch_ = 0;
    return {};
}

bool DuplicationFrameSource::Outstanding() const noexcept
{
    return outstanding_;
}

bool DuplicationFrameSource::AccessLost() const noexcept
{
    return accessLost_;
}

bool DuplicationFrameSource::ImageAcquired() const noexcept
{
    return lastPresent_ != 0;
}

CursorState DuplicationFrameSource::GetCursorState() const noexcept
{
    return cursorState_;
}

void DuplicationFrameSource::UpdateSnapshot(CaptureSnapshot& snapshot) const noexcept
{
    snapshot.acquireTimeouts = acquireTimeouts_;
    snapshot.pointerOnlyFrames = pointerOnlyFrames_;
    snapshot.accumulatedFrames = accumulatedFrames_;
    snapshot.accessLostEvents = accessLostEvents_;
}

} // namespace pbscreencapturedxgi::detail
