#pragma once

#include "application_model.h"

#include <string>

namespace pbapp
{

struct RunReportContext
{
    std::string applicationName;
    std::string applicationVersion;
    std::string gitCommit;
    std::string exportedAtUtc;
};

[[nodiscard]] std::string BuildEncoderRunReportJson(const RunReportContext& context,
    const EncoderSnapshot& snapshot);
[[nodiscard]] std::string BuildDecoderRunReportJson(const RunReportContext& context,
    const DecoderSnapshot& snapshot);
[[nodiscard]] std::string BuildEncoderDiagnostics(const EncoderSnapshot& snapshot);
[[nodiscard]] std::string BuildDecoderDiagnostics(const DecoderSnapshot& snapshot);

} // namespace pbapp
