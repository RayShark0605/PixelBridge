#include "pbscreencapturedxgi/dxgi_capture.h"

int main()
{
    const auto status = pbscreencapturedxgi::ValidateDxgiCaptureConfig({});
    std::unique_ptr<pbscreencapturedxgi::DxgiCapture> capture;
    const auto strict = pbscreencapturedxgi::DxgiCapture::Create({}, nullptr, capture);
    return status.code == pbscreencapturedxgi::CaptureError::InvalidConfiguration &&
           strict.code == pbscreencapturedxgi::CaptureError::InvalidConfiguration && !capture ? 0 : 1;
}
