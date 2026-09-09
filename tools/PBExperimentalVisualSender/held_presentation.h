#pragma once

#include "local_desktop_runtime.h"

#include <array>
#include <functional>

namespace pbexperiment
{

inline constexpr std::uint64_t minimumObservedPresentHoldNanoseconds = 133333334;
inline constexpr std::uint64_t nativePresentTimeoutNanoseconds = 2000000000;
inline constexpr std::size_t maximumPresentationRecords = 8192;
inline constexpr std::uint64_t maximumTreatmentFrameBytes = 16ULL * 1024 * 1024;

enum class Treatment : std::uint8_t
{
    ColorControl,
    NeutralChroma
};

struct PresentationRecord
{
    std::uint64_t frameSequence = 0;
    std::uint64_t presentationEpoch = 0;
    std::uint64_t queuedNanoseconds = 0;
    std::optional<std::uint64_t> forwardedNanoseconds;
    std::optional<std::uint64_t> successfulPresentObservedNanoseconds;
    std::optional<std::int64_t> observedPresentEndQpc;
    std::optional<std::uint64_t> nextReleaseNanoseconds;
    bool discardedOnStop = false;
    bool invalidatedAtStatisticsEpoch = false;
};

// The runtime worker is the sole writer. Read only after EncoderRuntime::Stop
// joins it. These records contain timing/identity, never payload or feedback.
struct PresentationEvidence
{
    Treatment treatment = Treatment::ColorControl;
    std::array<PresentationRecord, maximumPresentationRecords> records;
    std::size_t recordCount = 0;
    std::uint64_t forwardedFrames = 0;
    std::uint64_t observedFrames = 0;
    std::uint64_t discardedOnStop = 0;
    std::uint64_t allocatedTreatmentBytes = 0;
    pbrenderd3d::PresentationStatus failure;
    std::optional<pbrenderd3d::DataWindowSnapshot> nativeAtEpochMismatch;
    std::array<pbrenderd3d::DataWindowSnapshot, 2> drainedStatisticsEpochs;
    std::size_t drainedStatisticsEpochCount = 0;
};

// Exact integer form of the frozen offline luma rule. Input/output cannot
// alias; output is tightly packed. No allocations and no protocol knowledge.
[[nodiscard]] pbrenderd3d::PresentationStatus TransformFrame(const pbrenderd3d::DataWindowConfig& config,
    const pbrenderd3d::CanonicalBgraFrameView& source, Treatment treatment, std::span<std::byte> destination) noexcept;

// Tool-only post-raster conditioner. One actual queued image feeds the native
// DataWindow; GetSnapshot pumps it on the existing runtime owner thread. Native
// Present counters are untouched. pendingFrame also reports this real queue,
// never an invented timer-only pending flag. A successful local Present return
// is NOT a scanout, unique capture, or receiver acknowledgement.
class HeldPresentation final : public pbapp::EncoderPresentation
{
public:
    using Clock = std::function<std::uint64_t()>;
    HeldPresentation(const pbrenderd3d::DataWindowConfig& config, Treatment treatment,
        std::unique_ptr<pbapp::EncoderPresentation> nativePresentation,
        std::shared_ptr<PresentationEvidence> evidence, Clock clock);
    ~HeldPresentation() override;
    [[nodiscard]] pbrenderd3d::DataWindowSnapshot GetSnapshot() const override;
    [[nodiscard]] pbrenderd3d::PresentationStatus SubmitFrame(const pbrenderd3d::CanonicalBgraFrameView& frame) override;
    void RequestStop() noexcept override;
    void Stop() noexcept override;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

void WritePresentationEvidenceJson(std::ostream& output, const PresentationEvidence& evidence);

} // namespace pbexperiment
