#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace pbcore
{

enum class DiagnosticStage : std::uint8_t
{
    OuterGenerate,
    InnerPackEncode,
    Raster,
    FullscreenCompose,
    SubmitCall,
    SenderBackpressure,
    PresentationUpload,
    PresentCall,
    MediaDecode,
    ReplayUpload,
    ReplayDemod,
    ReceiverProcess,
    SegmentRecover,
    SegmentWrite,
    FinalPublish,
    BaseFec,
    FineFec,
    ChromaFec,
    SlotProtocol,
    OuterReceive,
    StorageFlushCheckpoint,
    Count
};

struct StageTiming
{
    std::uint64_t calls = 0;
    std::uint64_t samples = 0;
    std::uint64_t totalNanoseconds = 0;
    std::uint64_t maximumNanoseconds = 0;
};

struct StageDiagnosticsSnapshot
{
    std::array<StageTiming, static_cast<std::size_t>(DiagnosticStage::Count)> stages{};
    std::uint32_t sampleStride = 1;
    bool valid = true;
};

// Observation only: never receives pixels, protocol bytes, callbacks or a
// filesystem path. Fixed storage and sticky evidence failure; no admission use.
class StageDiagnostics
{
public:
    explicit StageDiagnostics(std::uint32_t sampleStride = 1) noexcept;
    [[nodiscard]] bool Begin(DiagnosticStage stage) noexcept;
    void End(DiagnosticStage stage, std::uint64_t elapsedNanoseconds) noexcept;
    void Invalidate() noexcept;
    [[nodiscard]] StageDiagnosticsSnapshot GetSnapshot() const noexcept;

private:
    mutable std::mutex mutex_;
    StageDiagnosticsSnapshot snapshot_;
};

[[nodiscard]] std::uint64_t DiagnosticNowNanoseconds() noexcept;
[[nodiscard]] const char* GetDiagnosticStageName(DiagnosticStage stage) noexcept;
[[nodiscard]] std::string BuildStageDiagnosticsJson(const StageDiagnosticsSnapshot& snapshot);

class DiagnosticScope
{
public:
    DiagnosticScope(StageDiagnostics* diagnostics, DiagnosticStage stage) noexcept;
    ~DiagnosticScope();
    DiagnosticScope(const DiagnosticScope&) = delete;
    DiagnosticScope& operator=(const DiagnosticScope&) = delete;

private:
    StageDiagnostics* diagnostics_;
    DiagnosticStage stage_;
    std::uint64_t started_ = 0;
};

} // namespace pbcore
