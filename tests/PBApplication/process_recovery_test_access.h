#pragma once

#include <filesystem>

namespace pbapp::test
{

enum class ProcessRecoveryMode
{
    Combined,
    SenderOnly,
    QuotaSeed
};

// Linked only from the opt-in, instrumented clone of PBApplication. The
// ordinary application public headers, targets and ABI do not expose this.
[[nodiscard]] int RunProcessRecoveryWorker(const std::filesystem::path& sourcePath,
    const std::filesystem::path& sessionRoot, const std::filesystem::path& outputDirectory,
    ProcessRecoveryMode mode) noexcept;

} // namespace pbapp::test
