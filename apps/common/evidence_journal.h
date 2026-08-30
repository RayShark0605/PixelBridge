#pragma once

#include "application_model.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace pbapp
{

struct RunJournalLimits
{
    std::uint64_t samplingIntervalMilliseconds = 1000;
    std::uint64_t maximumDurationMilliseconds = 6ULL * 60ULL * 60ULL * 1000ULL;
    std::uint64_t maximumBytes = 32ULL * 1024ULL * 1024ULL;
    std::uint32_t maximumRecordBytes = 64U * 1024U;
};

struct RunJournalSnapshot
{
    bool enabled = false;
    bool valid = true;
    bool truncated = false;
    bool finished = false;
    std::uint64_t samples = 0;
    std::uint64_t bytesWritten = 0;
    std::string invalidReason;
};

// Single-owner, direct-write NDJSON evidence journal. It has no queue and
// never participates in receive acceptance. A write/flush failure invalidates
// evidence for the run but is reported to the caller instead of changing the
// visual transport state.
class RunEvidenceJournal
{
public:
    RunEvidenceJournal() noexcept;
    RunEvidenceJournal(RunEvidenceJournal&&) noexcept;
    RunEvidenceJournal& operator=(RunEvidenceJournal&&) noexcept;
    ~RunEvidenceJournal();
    RunEvidenceJournal(const RunEvidenceJournal&) = delete;
    RunEvidenceJournal& operator=(const RunEvidenceJournal&) = delete;

    [[nodiscard]] static RunJournalSnapshot Create(const std::filesystem::path& path,
        const RunJournalLimits& limits, std::unique_ptr<RunEvidenceJournal>& output) noexcept;
    [[nodiscard]] RunJournalSnapshot AppendSample(std::uint64_t elapsedMilliseconds,
        std::string_view recordJson) noexcept;
    [[nodiscard]] RunJournalSnapshot AppendTerminal(std::uint64_t elapsedMilliseconds,
        std::string_view recordJson) noexcept;
    [[nodiscard]] RunJournalSnapshot Finish() noexcept;
    [[nodiscard]] RunJournalSnapshot GetSnapshot() const noexcept;

private:
    struct Implementation;
    explicit RunEvidenceJournal(std::unique_ptr<Implementation> implementation) noexcept;
    [[nodiscard]] RunJournalSnapshot Append(std::uint64_t elapsedMilliseconds,
        std::string_view recordJson, bool terminal) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

[[nodiscard]] std::string BuildEncoderJournalRecord(std::uint64_t unixMilliseconds,
    const EncoderSnapshot& snapshot);
[[nodiscard]] std::string BuildDecoderJournalRecord(std::uint64_t unixMilliseconds,
    const DecoderSnapshot& snapshot);
void ApplyJournalSnapshot(const RunJournalSnapshot& journal, EncoderSnapshot& snapshot);
void ApplyJournalSnapshot(const RunJournalSnapshot& journal, DecoderSnapshot& snapshot);

} // namespace pbapp
