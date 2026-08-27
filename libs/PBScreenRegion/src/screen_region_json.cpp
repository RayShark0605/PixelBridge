#include "pbscreenregion/screen_region.h"

#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>

namespace pbscreenregion
{

const char* GetScreenRegionErrorName(const ScreenRegionErrorCode code) noexcept
{
    switch (code)
    {
    case ScreenRegionErrorCode::None:
        return "none";
    case ScreenRegionErrorCode::Cancelled:
        return "cancelled";
    case ScreenRegionErrorCode::DpiAwarenessRequired:
        return "dpi-awareness-required";
    case ScreenRegionErrorCode::InvalidRectangle:
        return "invalid-rectangle";
    case ScreenRegionErrorCode::NotSingleMonitor:
        return "not-single-monitor";
    case ScreenRegionErrorCode::AmbiguousMonitor:
        return "ambiguous-monitor";
    case ScreenRegionErrorCode::MetadataUnavailable:
        return "metadata-unavailable";
    case ScreenRegionErrorCode::DisplayChanged:
        return "display-changed";
    case ScreenRegionErrorCode::ResourceLimit:
        return "resource-limit";
    case ScreenRegionErrorCode::NativeFailure:
        return "native-failure";
    case ScreenRegionErrorCode::OutOfMemory:
        return "out-of-memory";
    case ScreenRegionErrorCode::InternalError:
        return "internal-error";
    }
    return "unknown-error";
}

void WriteScreenCaptureRegionJson(std::ostream& stream, const ScreenCaptureRegion& region)
{
    std::ostringstream text;
    text.exceptions(std::ios::badbit | std::ios::failbit);
    text.imbue(std::locale::classic());
    const auto writeRect = [&text](const RECT& rect)
    {
        text << "{\"left\":" << rect.left << ",\"top\":" << rect.top << ",\"right\":" << rect.right << ",\"bottom\":" << rect.bottom << '}';
    };
    text << "{\"coordinateSpace\":\"physical-desktop\",\"monitor\":\"0x" << std::hex << reinterpret_cast<std::uintptr_t>(region.monitor) << std::dec
         << "\",\"physicalRect\":";
    writeRect(region.physicalRect);
    text << ",\"monitorPhysicalRect\":";
    writeRect(region.monitorPhysicalRect);
    text << ",\"dpiX\":" << region.dpiX << ",\"dpiY\":" << region.dpiY << ",\"rotation\":" << static_cast<unsigned int>(region.rotation) << '}';
    const auto value = text.str();
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

} // namespace pbscreenregion
