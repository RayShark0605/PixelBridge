#include "pbscreencapturewgc/wgc_capture.h"

#include <string_view>

int main()
{
    const pbscreencapturewgc::WgcCaptureConfig config;
    const auto status = pbscreencapturewgc::ValidateWgcCaptureConfig(config);
    std::unique_ptr<pbscreencapturewgc::WgcCapture> capture;
    const auto strict = pbscreencapturewgc::WgcCapture::CreateNormalized({}, nullptr, capture);
    return !status && std::string_view(pbscreencapturewgc::GetCaptureErrorName(status.code)) == "InvalidConfiguration" &&
           strict.code == pbscreencapturewgc::CaptureError::InvalidConfiguration && !capture ? 0 : 1;
}
