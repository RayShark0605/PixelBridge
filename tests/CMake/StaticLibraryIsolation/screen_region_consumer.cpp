#include "pbscreenregion/screen_region.h"

#include <sstream>

int main()
{
    pbscreenregion::ScreenCaptureRegion region;
    const auto status = pbscreenregion::ResolveScreenCaptureRegion(RECT{}, region);
    if (status ||
        (status.code != pbscreenregion::ScreenRegionErrorCode::DpiAwarenessRequired && status.code != pbscreenregion::ScreenRegionErrorCode::InvalidRectangle))
    {
        return 1;
    }
    std::ostringstream stream;
    pbscreenregion::WriteScreenCaptureRegionJson(stream, region);
    return stream.str().find("physical-desktop") != std::string::npos ? 0 : 2;
}
