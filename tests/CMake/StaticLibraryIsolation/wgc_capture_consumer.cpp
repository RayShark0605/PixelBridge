#include "pbscreencapturewgc/wgc_capture.h"

#include <string_view>

int main()
{
    const pbscreencapturewgc::WgcCaptureConfig config;
    const auto status = pbscreencapturewgc::ValidateWgcCaptureConfig(config);
    return !status && std::string_view(pbscreencapturewgc::GetCaptureErrorName(status.code)) == "InvalidConfiguration" ? 0 : 1;
}
