#include "pbcapturenormalize/diagnostic_readback.h"

#include <string_view>

int main()
{
    std::int64_t timestamp = -1;
    if (!pbcapturenormalize::ConvertQpcTo100ns(1, 3, timestamp) || timestamp != 3333333)
    {
        return 1;
    }
    if (pbcapturenormalize::ValidateCaptureConfig({}).code != pbcapturenormalize::CaptureError::InvalidConfiguration)
    {
        return 2;
    }
    std::shared_ptr<pbcapturenormalize::DiagnosticCpuReadback> readback;
    if (pbcapturenormalize::DiagnosticCpuReadback::Create({}, nullptr, readback).code != pbcapturenormalize::CaptureError::InvalidConfiguration || readback)
    {
        return 3;
    }
    return std::string_view(pbcapturenormalize::GetCaptureErasureName(pbcapturenormalize::CaptureErasureReason::InactiveDomain)) == "InactiveDomain" ? 0 : 4;
}
