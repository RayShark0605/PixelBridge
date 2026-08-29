#pragma once

#include "pbdemodd3d11/demodulator.h"
#include "pbmodulation/local_desktop_decode.h"
#include "pbmodulation/reference_visual_profile.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace pbdemodd3d11
{

inline constexpr std::uint32_t maximumCaptureDemodResultQueue = 256;
inline constexpr std::size_t localDesktopErasureCount =
    static_cast<std::size_t>(pbmodulation::LocalDesktopErasureReason::ExcessResidual) + 1;

enum class CaptureDemodulatorResultKind : std::uint8_t
{
    Transport, ControlRecord, ControlFragment
};

struct CaptureDemodulatorResult
{
    CaptureDemodulatorResultKind kind = CaptureDemodulatorResultKind::Transport;
    pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bootstrapRecord{};
    std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> controlBytes{};
    std::uint32_t controlByteCount = 0;
    pbmodulation::LocalDesktopObservation bootstrap;
    DemodFrameResult demodulation;
};

struct CaptureDemodulatorConfig
{
    std::uint64_t visualProfileId = 0;
    std::uint32_t slotCount = 3;
    std::uint32_t maximumFrameAgeMilliseconds = 250;
    std::uint32_t resultQueueCapacity = 64;
    std::uint64_t maximumResidentBytes = 128ULL * 1024 * 1024;
    pbdesktoplevels::EvaluationMode evaluationMode = pbdesktoplevels::EvaluationMode::Transport;
};

struct CaptureDemodulatorBudget
{
    std::uint64_t demodulatorBytes = 0;
    std::uint64_t bootstrapStagingBytes = 0;
    std::uint64_t referenceScratchBytes = 0;
    std::uint64_t resultQueueBytes = 0;
    std::uint64_t fixedOverheadBytes = 0;
    std::uint64_t totalBytes = 0;
    bool operator==(const CaptureDemodulatorBudget&) const = default;
};

struct CaptureDemodulatorSnapshot
{
    bool active = false;
    pbcapturenormalize::ScreenCaptureDomain domain;
    pbcapturenormalize::CaptureStatus error;
    DemodStatus lastDemodStatus;
    CaptureDemodulatorBudget reservation;
    DemodSnapshot demodulator;
    std::uint64_t domainStarts = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t submittedFrames = 0;
    std::uint64_t completedFrames = 0;
    std::uint64_t cancelledFrames = 0;
    std::uint64_t expiredFrames = 0;
    std::uint64_t captureErasures = 0;
    std::uint64_t bootstrapAcceptedFrames = 0;
    std::uint64_t bootstrapRejectedFrames = 0;
    std::uint64_t controlFrames = 0;
    std::uint64_t controlFrameFailures = 0;
    std::array<std::uint64_t, localDesktopErasureCount> bootstrapErasures{};
    std::uint64_t bootstrapMapCalls = 0;
    std::uint64_t bootstrapReadbackBytes = 0;
    std::uint64_t bootstrapCpuTimeTotal100ns = 0;
    std::uint64_t bootstrapCpuTimeHighWater100ns = 0;
    std::uint64_t bootstrapCpuTimingSamples = 0;
    std::uint64_t demodulationCpuTimeTotal100ns = 0;
    std::uint64_t demodulationCpuTimeHighWater100ns = 0;
    std::uint64_t demodulationCpuTimingSamples = 0;
    std::uint64_t cpuTimingUnavailable = 0;
    std::uint64_t demodulationRejectedFrames = 0;
    std::uint64_t verifiedFrames = 0;
    std::uint64_t postFecFailedFrames = 0;
    std::uint64_t acceptedTransportBlocks = 0;
    std::uint64_t resultQueueDrops = 0;
    std::uint64_t staleResultDrops = 0;
    std::uint64_t resultsTaken = 0;
    std::uint32_t pendingFrames = 0;
    std::uint32_t pendingHighWater = 0;
    std::uint32_t queuedResults = 0;
    std::uint32_t resultQueueHighWater = 0;
};

// Calculates every fixed allocation before capture pool/ring creation. Failure
// leaves output unchanged.
[[nodiscard]] pbcapturenormalize::CaptureStatus CalculateCaptureDemodulatorBudget(
    const CaptureDemodulatorConfig& config, CaptureDemodulatorBudget& output) noexcept;

// Same-frame LocalDesktop Bootstrap plus D3D11 metric/FEC consumer. Submit
// queues a bounded staging copy and GPU demodulation on the capture owner;
// Completed maps only after the capture runtime's later retirement marker.
// Results enter a fixed ring and carry only Transport blocks that passed
// QC-LDPC, canonical framing/CRC, and Bootstrap SessionTag validation.
class CaptureDemodulator final : public pbcapturenormalize::ScreenCaptureConsumer
{
public:
    [[nodiscard]] static pbcapturenormalize::CaptureStatus Create(const CaptureDemodulatorConfig& config,
        std::shared_ptr<CaptureDemodulator>& output) noexcept;
    ~CaptureDemodulator() override;
    [[nodiscard]] std::uint64_t ReservedBytes() const noexcept override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus ValidateConfiguration(
        const pbcapturenormalize::CaptureConfig& config) const noexcept override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus DomainStarted(
        const pbcapturenormalize::ScreenCaptureDomain& domain,
        const pbcapturenormalize::CaptureEnvironment& environment, ID3D11Device* device) override;
    void DomainInvalidated(const pbcapturenormalize::ScreenCaptureDomain& domain) noexcept override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus Submit(
        const pbcapturenormalize::ScreenCaptureFrame& frame, ID3D11DeviceContext* context) override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus Completed(
        const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata,
        ID3D11DeviceContext* context, bool cancelled) override;
    void Erased(const pbcapturenormalize::CaptureErasure& erasure) noexcept override;
    [[nodiscard]] bool TakeResult(CaptureDemodulatorResult& output) noexcept;
    [[nodiscard]] CaptureDemodulatorSnapshot GetSnapshot() const noexcept;

    CaptureDemodulator(const CaptureDemodulator&) = delete;
    CaptureDemodulator& operator=(const CaptureDemodulator&) = delete;

private:
    struct Implementation;
    explicit CaptureDemodulator(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace pbdemodd3d11
