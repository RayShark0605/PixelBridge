#include "held_presentation.h"

#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <limits>
#include <ostream>
#include <stdexcept>

namespace pbexperiment
{
namespace
{
using pbrenderd3d::PresentationErrorCode;
using pbrenderd3d::PresentationStage;
using pbrenderd3d::PresentationStatus;

[[nodiscard]] PresentationStatus Failure(const PresentationErrorCode code) noexcept
{
    return PresentationStatus::Failure(code, PresentationStage::FrameValidation);
}

[[nodiscard]] bool ValidTreatment(const Treatment treatment) noexcept
{
    return treatment == Treatment::ColorControl || treatment == Treatment::NeutralChroma;
}

template <typename ValueType> void WriteOptional(std::ostream& output, const std::optional<ValueType>& value)
{
    if (value)
    {
        output << *value;
    }
    else
    {
        output << "null";
    }
}
} // namespace

PresentationStatus TransformFrame(const pbrenderd3d::DataWindowConfig& config,
    const pbrenderd3d::CanonicalBgraFrameView& source, const Treatment treatment, const std::span<std::byte> destination) noexcept
{
    const auto validation = pbrenderd3d::ValidateCanonicalBgraFrame(config, source);
    if (!validation)
    {
        return validation;
    }
    const auto rowBytes = pbprotocol::CheckedMultiplyUnsigned<std::size_t>(source.width, 4U);
    const auto frameBytes = rowBytes ? pbprotocol::CheckedMultiplyUnsigned<std::size_t>(rowBytes.Value(), source.height) : rowBytes;
    if (!ValidTreatment(treatment) || !frameBytes || frameBytes.Value() > maximumTreatmentFrameBytes || destination.size() != frameBytes.Value())
    {
        return Failure(PresentationErrorCode::InvalidFrame);
    }
    // Integer addresses avoid ordering unrelated pointers. Validate ranges
    // before adding lengths so an overlapping output never mutates the input.
    const auto sourceAddress = reinterpret_cast<std::uintptr_t>(source.pixels.data());
    const auto destinationAddress = reinterpret_cast<std::uintptr_t>(destination.data());
    const auto sourceEnd = pbprotocol::CheckedAddUnsigned(sourceAddress, source.pixels.size());
    const auto destinationEnd = pbprotocol::CheckedAddUnsigned(destinationAddress, destination.size());
    if (!sourceEnd || !destinationEnd || (sourceAddress < destinationEnd.Value() && destinationAddress < sourceEnd.Value()))
    {
        return Failure(PresentationErrorCode::InvalidFrame);
    }
    for (std::uint32_t row = 0; row < source.height; row++)
    {
        const auto inputRow = source.pixels.subspan(static_cast<std::size_t>(row) * source.rowPitch, rowBytes.Value());
        auto outputRow = destination.subspan(static_cast<std::size_t>(row) * rowBytes.Value(), rowBytes.Value());
        if (treatment == Treatment::ColorControl)
        {
            std::copy(inputRow.begin(), inputRow.end(), outputRow.begin());
            continue;
        }
        for (std::size_t pixel = 0; pixel < rowBytes.Value(); pixel += 4)
        {
            const auto blue = std::to_integer<std::uint32_t>(inputRow[pixel]);
            const auto green = std::to_integer<std::uint32_t>(inputRow[pixel + 1]);
            const auto red = std::to_integer<std::uint32_t>(inputRow[pixel + 2]);
            const auto gray = static_cast<std::byte>((722U * blue + 7152U * green + 2126U * red + 5000U) / 10000U);
            outputRow[pixel] = gray;
            outputRow[pixel + 1] = gray;
            outputRow[pixel + 2] = gray;
            outputRow[pixel + 3] = inputRow[pixel + 3];
        }
    }
    return {};
}

struct HeldPresentation::Implementation
{
    pbrenderd3d::DataWindowConfig config;
    Treatment treatment;
    std::unique_ptr<pbapp::EncoderPresentation> native;
    std::shared_ptr<PresentationEvidence> evidence;
    Clock clock;
    std::vector<std::byte> pixels;
    std::optional<std::size_t> queuedRecord;
    std::optional<std::size_t> awaitingRecord;
    std::optional<std::uint64_t> epoch;
    std::optional<pbrenderd3d::DataWindowSnapshot> initialNativeSnapshot;
    std::optional<std::uint64_t> previousNow;
    std::uint64_t nextRelease = 0;
    bool stopped = false;

    [[nodiscard]] bool CheckTime(const std::uint64_t now)
    {
        if (previousNow && now < *previousNow)
        {
            evidence->failure = Failure(PresentationErrorCode::ContractViolation);
            return false;
        }
        previousNow = now;
        return true;
    }

    [[nodiscard]] pbrenderd3d::DataWindowSnapshot Snapshot()
    {
        auto snapshot = native->GetSnapshot();
        if (!stopped && evidence->failure)
        {
            const auto now = clock();
            if (CheckTime(now))
            {
                Pump(now, snapshot);
            }
            snapshot = native->GetSnapshot();
        }
        if (!evidence->failure && !stopped)
        {
            snapshot.state = pbrenderd3d::WindowState::Failed;
            snapshot.error = evidence->failure;
        }
        snapshot.pendingFrame = snapshot.pendingFrame || queuedRecord.has_value();
        return snapshot;
    }

    void Pump(const std::uint64_t now, const pbrenderd3d::DataWindowSnapshot& snapshot)
    {
        if (epoch && snapshot.timing.presentationEpoch != *epoch)
        {
            const auto reason = snapshot.timing.epochReason;
            const bool statisticsOnly = reason == pbpresenttiming::EpochReason::StatisticsDisjoint || reason == pbpresenttiming::EpochReason::StatisticsRecovered;
            const bool drained = snapshot.state == pbrenderd3d::WindowState::Running && snapshot.error && snapshot.candidateContractSatisfied &&
                !snapshot.pendingFrame && !snapshot.inFlightFrame && !snapshot.activeFrame;
            const bool sameSurface = initialNativeSnapshot && snapshot.environment == initialNativeSnapshot->environment &&
                snapshot.viewport == initialNativeSnapshot->viewport && snapshot.swapChainGeneration == initialNativeSnapshot->swapChainGeneration &&
                snapshot.bufferGeneration == initialNativeSnapshot->bufferGeneration && snapshot.softwareRasterizer == initialNativeSnapshot->softwareRasterizer;
            if (statisticsOnly && drained && sameSurface && snapshot.timing.presentationEpoch > *epoch && evidence->drainedStatisticsEpochCount < evidence->drainedStatisticsEpochs.size())
            {
                // Native PollStatistics has already invalidated its old source.
                // Drop our old-epoch queue, never retag/replay it into the new
                // epoch. A forwarded-but-unobserved record stays unknown, not
                // retroactively accepted. The original runtime builds the next
                // canonical FrameSequence; no equation is generated here.
                if (queuedRecord)
                {
                    evidence->records[*queuedRecord].invalidatedAtStatisticsEpoch = true;
                }
                if (awaitingRecord)
                {
                    evidence->records[*awaitingRecord].invalidatedAtStatisticsEpoch = true;
                }
                evidence->drainedStatisticsEpochs[evidence->drainedStatisticsEpochCount] = snapshot;
                evidence->drainedStatisticsEpochCount++;
                queuedRecord.reset();
                awaitingRecord.reset();
                epoch = snapshot.timing.presentationEpoch;
                nextRelease = 0;
                return;
            }
            evidence->nativeAtEpochMismatch = snapshot;
            evidence->failure = Failure(PresentationErrorCode::EpochMismatch);
            return;
        }
        if (snapshot.state != pbrenderd3d::WindowState::Running)
        {
            if (epoch)
            {
                evidence->failure = Failure(PresentationErrorCode::NotRunning);
            }
            return;
        }
        if (awaitingRecord)
        {
            auto& record = evidence->records[*awaitingRecord];
            const auto& present = snapshot.timing.lastPresent;
            if (present && present->frameSequence == record.frameSequence && present->outcome == pbpresenttiming::PresentOutcome::Success)
            {
                const auto release = pbprotocol::CheckedAddUint64(now, minimumObservedPresentHoldNanoseconds);
                if (!release)
                {
                    evidence->failure = Failure(PresentationErrorCode::ResourceLimit);
                    return;
                }
                record.successfulPresentObservedNanoseconds = now;
                record.observedPresentEndQpc = present->endQpc;
                record.nextReleaseNanoseconds = release.Value();
                nextRelease = release.Value();
                evidence->observedFrames++;
                awaitingRecord.reset();
            }
            else if (now - *record.forwardedNanoseconds >= nativePresentTimeoutNanoseconds)
            {
                evidence->failure = Failure(PresentationErrorCode::Timeout);
                return;
            }
        }
        // Native SubmitFrame copies into its separate pendingPixels while the
        // GPU may still use activePixels. Match the original runtime's pending
        // gate; waiting for an idle GPU here starves continuous repeat Present.
        if (queuedRecord && !awaitingRecord && now >= nextRelease && !snapshot.pendingFrame)
        {
            auto& record = evidence->records[*queuedRecord];
            const auto status = native->SubmitFrame({pixels, config.width, config.height,
                static_cast<std::size_t>(config.width) * 4U, record.frameSequence, record.presentationEpoch});
            if (!status)
            {
                evidence->failure = status;
                return;
            }
            record.forwardedNanoseconds = now;
            evidence->forwardedFrames++;
            awaitingRecord = queuedRecord;
            queuedRecord.reset();
        }
    }
};

HeldPresentation::HeldPresentation(const pbrenderd3d::DataWindowConfig& config, const Treatment treatment,
    std::unique_ptr<pbapp::EncoderPresentation> nativePresentation, std::shared_ptr<PresentationEvidence> evidence, Clock clock)
{
    const auto rowBytes = pbprotocol::CheckedMultiplyUnsigned<std::size_t>(config.width, 4U);
    const auto frameBytes = rowBytes ? pbprotocol::CheckedMultiplyUnsigned<std::size_t>(rowBytes.Value(), config.height) : rowBytes;
    if (!pbrenderd3d::ValidateDataWindowConfig(config) || !ValidTreatment(treatment) || !frameBytes ||
        frameBytes.Value() > maximumTreatmentFrameBytes || !nativePresentation || !evidence || !clock || evidence->recordCount != 0)
    {
        throw std::invalid_argument("Invalid experimental presentation configuration");
    }
    implementation_ = std::make_unique<Implementation>();
    auto& state = *implementation_;
    state.config = config;
    state.treatment = treatment;
    state.native = std::move(nativePresentation);
    state.evidence = std::move(evidence);
    state.clock = std::move(clock);
    state.pixels.resize(frameBytes.Value());
    state.evidence->treatment = treatment;
    state.evidence->allocatedTreatmentBytes = frameBytes.Value();
}

HeldPresentation::~HeldPresentation()
{
    Stop();
}

pbrenderd3d::DataWindowSnapshot HeldPresentation::GetSnapshot() const
{
    return implementation_->Snapshot();
}

PresentationStatus HeldPresentation::SubmitFrame(const pbrenderd3d::CanonicalBgraFrameView& frame)
{
    auto& state = *implementation_;
    if (state.stopped || !state.evidence->failure)
    {
        return state.stopped ? Failure(PresentationErrorCode::NotRunning) : state.evidence->failure;
    }
    const auto snapshot = state.native->GetSnapshot();
    if (snapshot.state != pbrenderd3d::WindowState::Running || state.queuedRecord || snapshot.pendingFrame)
    {
        return Failure(PresentationErrorCode::NotRunning);
    }
    if (frame.presentationEpoch != snapshot.timing.presentationEpoch || (state.epoch && *state.epoch != frame.presentationEpoch))
    {
        return Failure(PresentationErrorCode::EpochMismatch);
    }
    if (state.evidence->recordCount >= maximumPresentationRecords)
    {
        return Failure(PresentationErrorCode::ResourceLimit);
    }
    if (state.evidence->recordCount != 0 && frame.frameSequence <= state.evidence->records[state.evidence->recordCount - 1].frameSequence)
    {
        return Failure(PresentationErrorCode::ContractViolation);
    }
    const auto now = state.clock();
    if (!state.CheckTime(now))
    {
        return state.evidence->failure;
    }
    const auto transformed = TransformFrame(state.config, frame, state.treatment, state.pixels);
    if (!transformed)
    {
        return transformed;
    }
    const auto queuedAt = state.clock();
    if (!state.CheckTime(queuedAt))
    {
        return state.evidence->failure;
    }
    const auto index = state.evidence->recordCount;
    auto& record = state.evidence->records[index];
    record.frameSequence = frame.frameSequence;
    record.presentationEpoch = frame.presentationEpoch;
    record.queuedNanoseconds = queuedAt;
    state.evidence->recordCount++;
    state.queuedRecord = index;
    state.epoch = frame.presentationEpoch;
    if (!state.initialNativeSnapshot)
    {
        state.initialNativeSnapshot = snapshot;
    }
    return {};
}

void HeldPresentation::RequestStop() noexcept
{
    auto& state = *implementation_;
    state.stopped = true;
    if (state.queuedRecord)
    {
        state.evidence->records[*state.queuedRecord].discardedOnStop = true;
        state.evidence->discardedOnStop++;
        state.queuedRecord.reset();
    }
    state.awaitingRecord.reset();
    state.native->RequestStop();
}

void HeldPresentation::Stop() noexcept
{
    if (implementation_)
    {
        RequestStop();
        implementation_->native->Stop();
    }
}

void WritePresentationEvidenceJson(std::ostream& output, const PresentationEvidence& evidence)
{
    if (evidence.recordCount > maximumPresentationRecords || evidence.drainedStatisticsEpochCount > evidence.drainedStatisticsEpochs.size())
    {
        throw std::invalid_argument("Experimental presentation record limit");
    }
    output << "{\"schema\":\"PixelBridge.ExperimentalHeldPresentation.1\",\"treatment\":\""
        << (evidence.treatment == Treatment::NeutralChroma ? "NeutralChroma" : "ColorControl")
        << "\",\"certifiedProfile\":false,\"nativePresentIsScanoutOrReceiverAck\":false,\"offlineH2TimingEquivalent\":false,"
        << "\"timingSemantics\":\"queued=post-transform admission;forwarded=successful native Submit call begin;observed=poll sees matching successful Present return\","
        << "\"minimumHoldAfterObservedSuccessfulPresentNanoseconds\":" << minimumObservedPresentHoldNanoseconds
        << ",\"allocatedTreatmentBytes\":" << evidence.allocatedTreatmentBytes << ",\"queuedFrames\":" << evidence.recordCount
        << ",\"forwardedFrames\":" << evidence.forwardedFrames << ",\"observedSuccessfulPresentFrames\":" << evidence.observedFrames
        << ",\"discardedOnStop\":" << evidence.discardedOnStop << ",\"failure\":\""
        << pbrenderd3d::GetPresentationErrorName(evidence.failure.code) << "\",\"records\":[";
    for (std::size_t index = 0; index < evidence.recordCount; index++)
    {
        const auto& record = evidence.records[index];
        output << (index == 0 ? "" : ",") << "{\"frameSequence\":" << record.frameSequence
            << ",\"presentationEpoch\":" << record.presentationEpoch << ",\"queuedNanoseconds\":" << record.queuedNanoseconds
            << ",\"forwardedNanoseconds\":";
        WriteOptional(output, record.forwardedNanoseconds);
        output << ",\"successfulPresentObservedNanoseconds\":";
        WriteOptional(output, record.successfulPresentObservedNanoseconds);
        output << ",\"observedPresentEndQpc\":";
        WriteOptional(output, record.observedPresentEndQpc);
        output << ",\"nextReleaseNanoseconds\":";
        WriteOptional(output, record.nextReleaseNanoseconds);
        output << ",\"discardedOnStop\":" << (record.discardedOnStop ? "true" : "false")
            << ",\"invalidatedAtStatisticsEpoch\":" << (record.invalidatedAtStatisticsEpoch ? "true" : "false") << '}';
    }
    output << "],\"nativeAtEpochMismatch\":";
    if (evidence.nativeAtEpochMismatch)
    {
        pbrenderd3d::WriteDataWindowSnapshotJson(output, *evidence.nativeAtEpochMismatch);
    }
    else
    {
        output << "null";
    }
    output << ",\"drainedStatisticsEpochs\":[";
    for (std::size_t index = 0; index < evidence.drainedStatisticsEpochCount; index++)
    {
        if (index != 0)
        {
            output << ',';
        }
        pbrenderd3d::WriteDataWindowSnapshotJson(output, evidence.drainedStatisticsEpochs[index]);
    }
    output << "]}\n";
}
} // namespace pbexperiment
