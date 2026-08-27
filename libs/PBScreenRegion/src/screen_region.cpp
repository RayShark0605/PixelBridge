#include "screen_region_internal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace pbscreenregion
{

ScreenRegionStatus ScreenRegionStatus::Failure(const ScreenRegionErrorCode code, const ScreenRegionStage stage, const std::int32_t nativeError) noexcept
{
    return {code == ScreenRegionErrorCode::None ? ScreenRegionErrorCode::InternalError : code, stage, nativeError};
}

namespace detail
{
namespace
{

ScreenRegionStatus GeometryError() noexcept
{
    return ScreenRegionStatus::Failure(ScreenRegionErrorCode::InvalidRectangle, ScreenRegionStage::Geometry);
}

class BackendCleanup
{
public:
    explicit BackendCleanup(ScreenRegionBackend& backend) noexcept : backend_(backend)
    {
    }
    ~BackendCleanup()
    {
        if (!closed_)
        {
            (void)backend_.Close();
        }
    }
    BackendCleanup(const BackendCleanup&) = delete;
    BackendCleanup& operator=(const BackendCleanup&) = delete;
    ScreenRegionStatus Finish(const ScreenRegionStatus status) noexcept
    {
        closed_ = true;
        const auto cleanup = backend_.Close();
        return status ? cleanup : status;
    }

private:
    ScreenRegionBackend& backend_;
    bool closed_ = false;
};

ScreenRegionStatus Revalidate(ScreenRegionBackend& backend, const MonitorSnapshot& original, const RECT& rect, ScreenCaptureRegion& candidate) noexcept
{
    MonitorSnapshot current;
    const auto status = backend.ReadTopology(current);
    if (!status)
    {
        return status;
    }
    if (!EqualTopology(original, current))
    {
        return ScreenRegionStatus::Failure(ScreenRegionErrorCode::DisplayChanged, ScreenRegionStage::Revalidation);
    }
    return ResolveFromTopology(current, rect, candidate);
}

} // namespace

bool EqualRect(const RECT& first, const RECT& second) noexcept
{
    return first.left == second.left && first.top == second.top && first.right == second.right && first.bottom == second.bottom;
}

bool DeclaresPerMonitorV2(std::wstring_view setting) noexcept
{
    // dpiAwareness is a case-insensitive, comma-separated priority list. The
    // first Windows-recognized mode wins; a later V2 cannot override "system".
    while (!setting.empty())
    {
        const auto comma = setting.find(L',');
        auto token = setting.substr(0, comma);
        const auto first = token.find_first_not_of(L" \t\r\n");
        if (first != std::wstring_view::npos)
        {
            token = token.substr(first, token.find_last_not_of(L" \t\r\n") - first + 1);
            const auto equals = [token](const std::wstring_view expected)
            {
                if (token.size() != expected.size())
                {
                    return false;
                }
                for (std::size_t index = 0; index < token.size(); index++)
                {
                    const auto character = token[index] >= L'A' && token[index] <= L'Z' ? token[index] + (L'a' - L'A') : token[index];
                    if (character != expected[index])
                    {
                        return false;
                    }
                }
                return true;
            };
            if (equals(L"permonitorv2"))
            {
                return true;
            }
            if (equals(L"permonitor") || equals(L"system") || equals(L"unaware"))
            {
                return false;
            }
        }
        if (comma == std::wstring_view::npos)
        {
            break;
        }
        setting.remove_prefix(comma + 1);
    }
    return false;
}

ScreenRegionStatus ValidatePhysicalRect(const RECT& rect) noexcept
{
    const auto width = static_cast<std::int64_t>(rect.right) - rect.left;
    const auto height = static_cast<std::int64_t>(rect.bottom) - rect.top;
    if (width <= 0 || height <= 0 || width > (std::numeric_limits<int>::max)() || height > (std::numeric_limits<int>::max)())
    {
        return GeometryError();
    }
    return ScreenRegionStatus::Success();
}

ScreenRegionStatus ValidateTopology(const MonitorSnapshot& snapshot) noexcept
{
    if (snapshot.count > maximumMonitors)
    {
        return ScreenRegionStatus::Failure(ScreenRegionErrorCode::ResourceLimit, ScreenRegionStage::MonitorEnumeration);
    }
    if (snapshot.count == 0)
    {
        return ScreenRegionStatus::Failure(ScreenRegionErrorCode::MetadataUnavailable, ScreenRegionStage::MonitorEnumeration);
    }
    for (std::size_t index = 0; index < snapshot.count; index++)
    {
        const auto& monitor = snapshot.monitors[index];
        if (monitor.monitor == nullptr || !ValidatePhysicalRect(monitor.physicalRect))
        {
            return ScreenRegionStatus::Failure(ScreenRegionErrorCode::MetadataUnavailable, ScreenRegionStage::Geometry);
        }
        if (monitor.dpiX == 0 || monitor.dpiY == 0)
        {
            return ScreenRegionStatus::Failure(ScreenRegionErrorCode::MetadataUnavailable, ScreenRegionStage::DpiQuery);
        }
        if (monitor.rotation != DXGI_MODE_ROTATION_IDENTITY && monitor.rotation != DXGI_MODE_ROTATION_ROTATE90 &&
            monitor.rotation != DXGI_MODE_ROTATION_ROTATE180 && monitor.rotation != DXGI_MODE_ROTATION_ROTATE270)
        {
            return ScreenRegionStatus::Failure(ScreenRegionErrorCode::MetadataUnavailable, ScreenRegionStage::OutputQuery);
        }
        for (std::size_t previous = 0; previous < index; previous++)
        {
            if (monitor.monitor == snapshot.monitors[previous].monitor)
            {
                return ScreenRegionStatus::Failure(ScreenRegionErrorCode::AmbiguousMonitor, ScreenRegionStage::MonitorEnumeration);
            }
        }
    }
    return ScreenRegionStatus::Success();
}

bool EqualTopology(const MonitorSnapshot& first, const MonitorSnapshot& second) noexcept
{
    if (!ValidateTopology(first) || !ValidateTopology(second) || first.count != second.count)
    {
        return false;
    }
    // Enumeration order is not an identity; a fresh DXGI factory can reorder outputs.
    for (std::size_t index = 0; index < first.count; index++)
    {
        const auto& original = first.monitors[index];
        bool found = false;
        for (std::size_t candidate = 0; candidate < second.count; candidate++)
        {
            const auto& current = second.monitors[candidate];
            if (original.monitor == current.monitor)
            {
                found = EqualRect(original.physicalRect, current.physicalRect) && original.dpiX == current.dpiX && original.dpiY == current.dpiY &&
                        original.rotation == current.rotation && original.deviceName == current.deviceName;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}

ScreenRegionStatus BuildDragRect(const POINT first, const POINT last, RECT& rect) noexcept
{
    const auto right = static_cast<std::int64_t>((std::max)(first.x, last.x)) + 1;
    const auto bottom = static_cast<std::int64_t>((std::max)(first.y, last.y)) + 1;
    if (right > (std::numeric_limits<LONG>::max)() || bottom > (std::numeric_limits<LONG>::max)())
    {
        return GeometryError();
    }
    const RECT candidate{(std::min)(first.x, last.x), (std::min)(first.y, last.y), static_cast<LONG>(right), static_cast<LONG>(bottom)};
    const auto status = ValidatePhysicalRect(candidate);
    if (status)
    {
        rect = candidate;
    }
    return status;
}

ScreenRegionStatus ResolveFromTopology(const MonitorSnapshot& snapshot, const RECT& rect, ScreenCaptureRegion& region) noexcept
{
    auto status = ValidatePhysicalRect(rect);
    if (!status)
    {
        return status;
    }
    status = ValidateTopology(snapshot);
    if (!status)
    {
        return status;
    }
    const MonitorMetadata* selected = nullptr;
    for (std::size_t index = 0; index < snapshot.count; index++)
    {
        const auto& monitor = snapshot.monitors[index];
        if (rect.left >= monitor.physicalRect.left && rect.top >= monitor.physicalRect.top && rect.right <= monitor.physicalRect.right &&
            rect.bottom <= monitor.physicalRect.bottom)
        {
            if (selected != nullptr)
            {
                return ScreenRegionStatus::Failure(ScreenRegionErrorCode::AmbiguousMonitor, ScreenRegionStage::Geometry);
            }
            selected = &monitor;
        }
    }
    if (selected == nullptr)
    {
        return ScreenRegionStatus::Failure(ScreenRegionErrorCode::NotSingleMonitor, ScreenRegionStage::Geometry);
    }
    // The validated monitor dimensions bound all later monitor-local subtractions
    // by INT_MAX, even when its desktop origin is negative.
    region = {selected->monitor, rect, selected->physicalRect, selected->dpiX, selected->dpiY, selected->rotation};
    return ScreenRegionStatus::Success();
}

ScreenRegionStatus RunResolve(ScreenRegionBackend& backend, const RECT& rect, ScreenCaptureRegion& region) noexcept
{
    BackendCleanup cleanup(backend);
    auto status = backend.CheckAwareness();
    if (status)
    {
        status = ValidatePhysicalRect(rect);
    }
    MonitorSnapshot snapshot;
    if (status)
    {
        status = backend.ReadTopology(snapshot);
    }
    ScreenCaptureRegion candidate;
    if (status)
    {
        status = ResolveFromTopology(snapshot, rect, candidate);
    }
    if (status)
    {
        status = Revalidate(backend, snapshot, rect, candidate);
    }
    status = cleanup.Finish(status);
    if (status)
    {
        region = candidate;
    }
    return status;
}

ScreenRegionStatus RunSelection(ScreenRegionBackend& backend, ScreenCaptureRegion& region) noexcept
{
    BackendCleanup cleanup(backend);
    auto status = backend.CheckAwareness();
    MonitorSnapshot snapshot;
    if (status)
    {
        status = backend.ReadTopology(snapshot);
    }
    if (status)
    {
        status = ValidateTopology(snapshot);
    }
    if (status)
    {
        status = backend.OpenOverlay(snapshot);
    }
    SelectionPreview preview;
    if (status)
    {
        status = backend.Draw(preview);
    }
    POINT anchor{};
    bool moved = false;
    while (status)
    {
        SelectionEvent event;
        status = backend.GetEvent(event);
        if (!status)
        {
            break;
        }
        if (event.type == SelectionEventType::Cancel)
        {
            status = ScreenRegionStatus::Failure(ScreenRegionErrorCode::Cancelled, ScreenRegionStage::Input);
            break;
        }
        if (event.type == SelectionEventType::DisplayChanged)
        {
            status = ScreenRegionStatus::Failure(ScreenRegionErrorCode::DisplayChanged, ScreenRegionStage::Revalidation);
            break;
        }
        if (event.type == SelectionEventType::CheckEnvironment)
        {
            MonitorSnapshot current;
            status = backend.ReadTopology(current);
            if (status && !EqualTopology(snapshot, current))
            {
                status = ScreenRegionStatus::Failure(ScreenRegionErrorCode::DisplayChanged, ScreenRegionStage::Revalidation);
            }
            continue;
        }
        if (event.type == SelectionEventType::PointerDown)
        {
            if (preview.dragging)
            {
                continue;
            }
            status = backend.AcquirePointer(event.physicalPoint);
            if (!status)
            {
                break;
            }
            anchor = event.physicalPoint;
            moved = false;
            preview.dragging = true;
        }
        else if (event.type != SelectionEventType::PointerMove && event.type != SelectionEventType::PointerUp)
        {
            status = ScreenRegionStatus::Failure(ScreenRegionErrorCode::InternalError, ScreenRegionStage::Input);
            break;
        }
        if (!preview.dragging)
        {
            continue;
        }
        moved = moved || anchor.x != event.physicalPoint.x || anchor.y != event.physicalPoint.y;
        preview.validation = BuildDragRect(anchor, event.physicalPoint, preview.physicalRect);
        preview.hasRectangle = static_cast<bool>(preview.validation);
        ScreenCaptureRegion candidate;
        if (preview.validation)
        {
            preview.validation = ResolveFromTopology(snapshot, preview.physicalRect, candidate);
        }
        if (event.type == SelectionEventType::PointerUp)
        {
            preview.dragging = false;
            status = backend.ReleasePointer();
            if (!moved)
            {
                preview.hasRectangle = false;
                preview.validation = GeometryError();
            }
        }
        if (status)
        {
            status = backend.Draw(preview);
        }
        if (status && event.type == SelectionEventType::PointerUp && preview.validation)
        {
            status = Revalidate(backend, snapshot, preview.physicalRect, candidate);
            status = cleanup.Finish(status);
            if (status)
            {
                region = candidate;
            }
            return status;
        }
    }
    return cleanup.Finish(status);
}

} // namespace detail

ScreenRegionStatus SelectScreenCaptureRegion(ScreenCaptureRegion& region) noexcept
{
    try
    {
        const auto backend = detail::MakeNativeBackend();
        return detail::RunSelection(*backend, region);
    }
    catch (const std::bad_alloc&)
    {
        return ScreenRegionStatus::Failure(ScreenRegionErrorCode::OutOfMemory, ScreenRegionStage::Window);
    }
    catch (...)
    {
        return ScreenRegionStatus::Failure(ScreenRegionErrorCode::InternalError, ScreenRegionStage::None);
    }
}

ScreenRegionStatus ResolveScreenCaptureRegion(const RECT& physicalRect, ScreenCaptureRegion& region) noexcept
{
    try
    {
        const auto backend = detail::MakeNativeBackend();
        return detail::RunResolve(*backend, physicalRect, region);
    }
    catch (const std::bad_alloc&)
    {
        return ScreenRegionStatus::Failure(ScreenRegionErrorCode::OutOfMemory, ScreenRegionStage::MonitorEnumeration);
    }
    catch (...)
    {
        return ScreenRegionStatus::Failure(ScreenRegionErrorCode::InternalError, ScreenRegionStage::None);
    }
}

} // namespace pbscreenregion
