#pragma once

#include "bootstrap_diagnostic.h"

#include <string>

namespace pbdecoder
{

// JSONL is observability, not an alternate Bootstrap input. Every identity in an
// observation is recovered from pixels; all 64-bit identities are decimal strings.
[[nodiscard]] std::string SerializeBootstrapDiagnosticEvent(const BootstrapDiagnosticEvent& event);
// These snapshots are sampled independently. Each asynchronous stage retains
// its own original domain; visual.temporalStateCurrent reports compatibility
// of the supplied samples, not an atomic observation or a worker-state reset.
[[nodiscard]] std::string SerializeCaptureBootstrapSnapshot(const char* eventType,
    const pbcapturenormalize::CaptureSnapshot& capture, const pbcapturenormalize::CaptureNormalizeSnapshot& normalized,
    const pbcapturenormalize::DiagnosticReadbackSnapshot& readback, const BootstrapDiagnosticSnapshot& visual,
    std::uint64_t staleDiagnosticEvents, std::uint64_t elapsedMilliseconds);

} // namespace pbdecoder
