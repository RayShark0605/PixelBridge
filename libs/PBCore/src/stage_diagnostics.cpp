#include "pbcore/stage_diagnostics.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>

namespace pbcore
{

namespace
{
constexpr std::array<const char*, static_cast<std::size_t>(DiagnosticStage::Count)> names{"outerGenerate", "innerPackEncode", "raster", "fullscreenCompose", "submitCall",
    "senderBackpressure", "presentationUpload", "presentCall", "mediaDecode", "replayUpload", "replayDemod", "receiverProcess", "segmentVerifyDecompress", "segmentWrite",
    "finalPublish", "baseFec", "fineFec", "chromaFec", "slotProtocol", "outerReceive", "storageFlushCheckpoint"};
constexpr std::uint64_t maximumStageCalls = 2000000;
constexpr std::uint64_t maximumStageNanoseconds = 1800ULL * 1000000000ULL;
} // namespace

StageDiagnostics::StageDiagnostics(const std::uint32_t sampleStride) noexcept
{
    snapshot_.sampleStride = sampleStride;
    snapshot_.valid = sampleStride >= 1 && sampleStride <= 4096;
}

bool StageDiagnostics::Begin(const DiagnosticStage stage) noexcept
{
    const std::lock_guard lock(mutex_);
    const auto index = static_cast<std::size_t>(stage);
    if (!snapshot_.valid || index >= snapshot_.stages.size())
    {
        snapshot_.valid = false;
        return false;
    }
    auto& timing = snapshot_.stages[index];
    if (timing.calls == maximumStageCalls)
    {
        snapshot_.valid = false;
        return false;
    }
    timing.calls++;
    return (timing.calls - 1) % snapshot_.sampleStride == 0;
}

void StageDiagnostics::End(const DiagnosticStage stage, const std::uint64_t elapsedNanoseconds) noexcept
{
    const std::lock_guard lock(mutex_);
    const auto index = static_cast<std::size_t>(stage);
    if (!snapshot_.valid || index >= snapshot_.stages.size() || elapsedNanoseconds > maximumStageNanoseconds)
    {
        snapshot_.valid = false;
        return;
    }
    auto& timing = snapshot_.stages[index];
    if (timing.samples >= timing.calls || elapsedNanoseconds > std::numeric_limits<std::uint64_t>::max() - timing.totalNanoseconds)
    {
        snapshot_.valid = false;
        return;
    }
    timing.samples++;
    timing.totalNanoseconds += elapsedNanoseconds;
    timing.maximumNanoseconds = std::max(timing.maximumNanoseconds, elapsedNanoseconds);
}

void StageDiagnostics::Invalidate() noexcept
{
    const std::lock_guard lock(mutex_);
    snapshot_.valid = false;
}

StageDiagnosticsSnapshot StageDiagnostics::GetSnapshot() const noexcept
{
    const std::lock_guard lock(mutex_);
    return snapshot_;
}

std::uint64_t DiagnosticNowNanoseconds() noexcept
{
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

const char* GetDiagnosticStageName(const DiagnosticStage stage) noexcept
{
    const auto index = static_cast<std::size_t>(stage);
    return index < names.size() ? names[index] : "invalid";
}

std::string BuildStageDiagnosticsJson(const StageDiagnosticsSnapshot& snapshot)
{
    std::ostringstream stream;
    stream << "{\"schema\":\"PixelBridge.StageDiagnostics.1\",\"clock\":\"LocalSteadyNanoseconds\",\"valid\":" << (snapshot.valid ? "true" : "false")
           << ",\"sampleStride\":" << snapshot.sampleStride << ",\"aggregation\":\"SampledInclusiveDurationsNotAdditive\",\"stages\":{";
    for (std::size_t index = 0; index < snapshot.stages.size(); index++)
    {
        const auto& timing = snapshot.stages[index];
        stream << (index == 0 ? "" : ",") << '"' << names[index] << "\":{\"calls\":" << timing.calls << ",\"samples\":" << timing.samples << ",\"totalNanoseconds\":";
        if (snapshot.valid && timing.samples != 0)
        {
            stream << timing.totalNanoseconds << ",\"maximumNanoseconds\":" << timing.maximumNanoseconds;
        }
        else
        {
            stream << "null,\"maximumNanoseconds\":null";
        }
        stream << ",\"unavailableReason\":" << (!snapshot.valid ? "\"EvidenceInvalid\"" : timing.samples == 0 ? "\"StageNotObserved\"" : "null") << '}';
    }
    stream << "}}";
    return stream.str();
}

DiagnosticScope::DiagnosticScope(StageDiagnostics* const diagnostics, const DiagnosticStage stage) noexcept
    : diagnostics_(diagnostics != nullptr && diagnostics->Begin(stage) ? diagnostics : nullptr), stage_(stage)
{
    if (diagnostics_ != nullptr)
    {
        started_ = DiagnosticNowNanoseconds();
    }
}

DiagnosticScope::~DiagnosticScope()
{
    if (diagnostics_ != nullptr)
    {
        const auto ended = DiagnosticNowNanoseconds();
        if (started_ == 0 || ended < started_)
        {
            diagnostics_->Invalidate();
        }
        else
        {
            diagnostics_->End(stage_, ended - started_);
        }
    }
}

} // namespace pbcore
