#pragma once

#include "application_model.h"

#include <string>
#include <iosfwd>

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
[[nodiscard]] std::string BuildRemoteVisualRunMetadataJson(const RemoteRunMetadata& metadata);
[[nodiscard]] std::string BuildEncoderDiagnostics(const EncoderSnapshot& snapshot);
// Appends comma-separated fields inside an already-open presentation object.
void WriteEncoderPresentationPolicyJson(std::ostream& stream, const EncoderSnapshot& snapshot);
void WriteEncoderRatePriorJson(std::ostream& stream, const SenderRatePriorSnapshot& prior);
void WriteFeedbackTelemetryJson(std::ostream& stream, const FeedbackTelemetry& feedback);
void WriteEncoderStopReasonJson(std::ostream& stream, const EncoderSnapshot& snapshot);
[[nodiscard]] std::string BuildDecoderDiagnostics(const DecoderSnapshot& snapshot);

} // namespace pbapp
