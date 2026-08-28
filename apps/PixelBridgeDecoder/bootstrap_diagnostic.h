#pragma once

#include "pbcapturenormalize/diagnostic_readback.h"
#include "pbmodulation/local_desktop_decode.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbdesktoplevels/reference_channel.h"

#include <array>
#include <mutex>
#include <optional>

namespace pbdecoder
{

enum class BootstrapDisposition : std::uint8_t
{
    None, Accepted, DuplicatePixels, DuplicateObservation, DuplicateGeometryChanged, DuplicateCalibrationChanged,
    VisualErasure, UnsupportedSignal, InvalidMetadata, IdentityConflict, StaleSequence, SessionLimit, GenerationExhausted,
    PostFecFailure, StatisticsFailure
};

struct BootstrapDiagnosticEvent
{
    pbcapturenormalize::ScreenCaptureFrameMetadata capture;
    pbmodulation::LocalDesktopObservation visual;
    pbprotocol::BootstrapRecord bootstrap;
    BootstrapDisposition disposition = BootstrapDisposition::None;
    std::uint64_t geometryGeneration = 0;
    std::uint64_t calibrationGeneration = 0;
    std::array<std::byte, 32> pixelDigest{};
    bool desktopLevels = false;
    pbdesktoplevels::ReferenceObservation levels;
};

struct DesktopLevelsCandidateSnapshot
{
    std::uint64_t geometryErasures = 0;
    std::uint64_t pilotErasures = 0;
    std::uint64_t otherErasures = 0;
    std::uint64_t duplicates = 0;
    pbdesktoplevels::StatisticsSummary statistics;
};

struct BootstrapDiagnosticSnapshot
{
    std::optional<pbcapturenormalize::ScreenCaptureDomain> domain;
    std::uint64_t resets = 0;
    std::uint64_t observations = 0;
    std::uint64_t accepted = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t erasures = 0;
    std::uint64_t identityConflicts = 0;
    std::uint64_t discardedCandidates = 0;
    std::uint64_t staleCommits = 0;
    std::uint64_t diagnosticQueueDrops = 0;
    std::uint64_t geometryGeneration = 0;
    std::uint64_t calibrationGeneration = 0;
    std::uint32_t trackedSessions = 0;
    std::uint32_t retainedSequences = 0;
    std::uint32_t queuedEvents = 0;
    BootstrapDisposition lastDisposition = BootstrapDisposition::None;
    bool desktopLevels = false;
    std::uint64_t unrecognizedBootstrap = 0;
    std::uint64_t statisticsFailures = 0;
    std::array<DesktopLevelsCandidateSnapshot, 2> candidates;
};

// Explicit Bootstrap-only or DesktopLevels diagnostic mode, not a file receiver.
// All admission/measurement mutation lives in Reset/Commit on the same worker;
// Analyze creates a candidate. No cross-frame soft-combine entry point exists.
class BootstrapDiagnosticProcessor final : public pbcapturenormalize::CpuFrameProcessor
{
public:
    static constexpr std::size_t historyCapacity = 64;
    static constexpr std::size_t sessionCapacity = 8;
    static constexpr std::size_t eventCapacity = 16;

    explicit BootstrapDiagnosticProcessor(const pbmodulation::LocalDesktopDecodePolicy& policy = {}) noexcept;
    ~BootstrapDiagnosticProcessor() override;
    [[nodiscard]] static pbcapturenormalize::CaptureStatus CreateDesktopLevels(std::shared_ptr<BootstrapDiagnosticProcessor>& output) noexcept;
    [[nodiscard]] std::uint64_t ProcessingReservedBytes() const noexcept override;
    void Reset(std::optional<pbcapturenormalize::ScreenCaptureDomain> domain) noexcept override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus Analyze(const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata,
                                                          std::span<const std::byte> pixels, std::size_t rowPitch) override;
    void Commit(const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata) noexcept override;
    void Discard() noexcept override;
    [[nodiscard]] BootstrapDiagnosticSnapshot GetSnapshot() const noexcept;
    [[nodiscard]] bool TakeEvent(BootstrapDiagnosticEvent& output) noexcept;

private:
    struct HistoryEntry
    {
        bool active = false;
        bool conflict = false;
        std::uint64_t sessionTag = 0;
        std::uint64_t sequence = 0;
        std::array<std::byte, 44> canonical44{};
        std::array<std::byte, 32> pixelDigest{};
        std::uint64_t geometryGeneration = 0;
        std::uint64_t calibrationGeneration = 0;
    };
    struct SessionEntry
    {
        bool active = false;
        std::uint64_t tag = 0;
        std::uint64_t highestSequence = 0;
        std::uint64_t profileId = 0;
    };
    void PublishLocked(const BootstrapDiagnosticEvent& event) noexcept;
    [[nodiscard]] BootstrapDisposition AdmitLocked(BootstrapDiagnosticEvent& event) noexcept;

    const pbmodulation::LocalDesktopDecodePolicy policy_;
    // Only published state is protected here, never a whole-image scan/hash.
    mutable std::mutex mutex_;
    BootstrapDiagnosticSnapshot snapshot_;
    std::array<HistoryEntry, historyCapacity> history_{};
    std::array<SessionEntry, sessionCapacity> sessions_{};
    std::size_t nextHistory_ = 0;
    std::array<BootstrapDiagnosticEvent, eventCapacity> events_{};
    std::size_t eventHead_ = 0;
    std::size_t eventCount_ = 0;
    std::optional<pbmodulation::LocalDesktopGeometry> geometry_;
    double blackLevel_ = 0;
    double whiteLevel_ = 0;
    bool calibrated_ = false;
    std::uint64_t lastObservation_ = 0;
    // Worker-only candidate, not visible to telemetry readers or admission.
    BootstrapDiagnosticEvent pending_;
    bool pendingReady_ = false;
    struct DesktopLevelsState;
    std::unique_ptr<DesktopLevelsState> levels_;
};

[[nodiscard]] const char* GetBootstrapDispositionName(BootstrapDisposition disposition) noexcept;
// Native runtime may publish a device error while it is still draining and
// exercising its finite recovery allowance. Such a snapshot is not terminal.
[[nodiscard]] bool IsTerminalDiagnosticFailure(const pbcapturenormalize::CaptureSnapshot& capture,
    const pbcapturenormalize::DiagnosticReadbackSnapshot& readback) noexcept;
// Called only after successful capture/worker shutdown. Event delivery is a
// bounded, lossy diagnostic stream; only committed admission counters decide
// whether this invocation actually recovered a Bootstrap from pixels.
[[nodiscard]] int GetBootstrapDiagnosticSuccessExitCode(const BootstrapDiagnosticSnapshot& snapshot) noexcept;

} // namespace pbdecoder
