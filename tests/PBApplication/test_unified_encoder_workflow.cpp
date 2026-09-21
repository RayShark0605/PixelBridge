#include "local_desktop_runtime.h"
#include "unified_decoder_test_support.h"
#include "encoder_session_store.h"
#include "sender_carousel_scheduler.h"
#include "pbmodulation/unified_visual.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/control_plane_receiver.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/transport_block_codec.h"
#include "pbreceiver/receiver_ingress.h"
#include "pbstorage/output_file.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

namespace pbapp
{

class EncoderRuntimeClockTestAccess
{
public:
    static void SetClock(EncoderRuntime& runtime, std::function<std::chrono::steady_clock::time_point()> clock)
    {
        runtime.runClock_ = std::move(clock);
    }
};

} // namespace pbapp

namespace
{

class Scratch final
{
public:
    Scratch()
    {
        const std::filesystem::path root = std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal();
        path_ = root / (L"g15-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        REQUIRE(path_.parent_path() == root);
        REQUIRE(std::filesystem::create_directory(path_));
    }
    ~Scratch()
    {
        const std::filesystem::path root = std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal();
        if (path_.parent_path() == root && path_.filename().wstring().starts_with(L"g15-"))
        {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    [[nodiscard]] const std::filesystem::path& Path() const
    {
        return path_;
    }
private:
    std::filesystem::path path_;
};

void WriteBytes(const std::filesystem::path& path, const std::span<const std::byte> bytes)
{
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

[[nodiscard]] bool WaitFor(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

struct PresentedFrame
{
    std::uint64_t sequence = 0;
    std::vector<std::byte> pixels;
};

struct PresentationState
{
    std::mutex mutex;
    bool stopped = false;
    bool failOnStop = false;
    std::function<void()> beforeSnapshot;
    bool paused = false;
    bool rejectFirst = false;
    bool retryIdentical = false;
    std::uint32_t attempts = 0;
    std::uint32_t maximumFrames = 2;
    std::uint64_t epoch = 1;
    std::array<std::byte, 32> rejectedDigest{};
    std::uint64_t rejectedSequence = 0;
    std::vector<PresentedFrame> frames;
};

class MockPresentation final : public pbapp::EncoderPresentation
{
public:
    explicit MockPresentation(std::shared_ptr<PresentationState> state) : state_(std::move(state))
    {
    }
    [[nodiscard]] pbrenderd3d::DataWindowSnapshot GetSnapshot() const override
    {
        if (state_->beforeSnapshot)
        {
            state_->beforeSnapshot();
        }
        const std::scoped_lock lock(state_->mutex);
        pbrenderd3d::DataWindowSnapshot snapshot;
        snapshot.state = state_->stopped ? pbrenderd3d::WindowState::Stopped : pbrenderd3d::WindowState::Running;
        if (state_->stopped && state_->failOnStop)
        {
            snapshot.state = pbrenderd3d::WindowState::Failed;
        }
        snapshot.environment.clientWidth = 1920;
        snapshot.environment.clientHeight = 1080;
        snapshot.contract = {1920, 1080, 2, 1, pbrenderd3d::FlipEffect::Discard,
            true, true, true, true, true, true, true, true, true, true, true, true};
        snapshot.candidateContractSatisfied = true;
        snapshot.viewport.disposition = state_->paused ? pbrenderd3d::PresentationViewportDisposition::PausedBelowMinimumScale :
            pbrenderd3d::PresentationViewportDisposition::Active;
        snapshot.timing.presentationEpoch = state_->epoch;
        snapshot.submittedFrames = state_->frames.size();
        snapshot.pendingFrame = !state_->stopped && state_->frames.size() >= state_->maximumFrames;
        // Deliberately unrelated to logical generation; must not advance IDs.
        snapshot.repeatedPresentCalls = 100;
        return snapshot;
    }
    [[nodiscard]] pbrenderd3d::PresentationStatus SubmitFrame(const pbrenderd3d::CanonicalBgraFrameView& frame) override
    {
        const std::scoped_lock lock(state_->mutex);
        state_->attempts++;
        if (state_->rejectFirst && state_->attempts == 1)
        {
            state_->rejectedDigest = pbprotocol::ComputeBlake3Digest(frame.pixels);
            state_->rejectedSequence = frame.frameSequence;
            state_->paused = true;
            state_->epoch++;
            return pbrenderd3d::PresentationStatus::Failure(pbrenderd3d::PresentationErrorCode::EpochMismatch,
                pbrenderd3d::PresentationStage::FrameValidation);
        }
        if (state_->rejectFirst && state_->attempts == 2)
        {
            state_->retryIdentical = state_->rejectedSequence == frame.frameSequence &&
                state_->rejectedDigest == pbprotocol::ComputeBlake3Digest(frame.pixels) && frame.presentationEpoch == state_->epoch;
        }
        if (state_->frames.size() >= state_->maximumFrames)
        {
            return pbrenderd3d::PresentationStatus::Failure(pbrenderd3d::PresentationErrorCode::Paused,
                pbrenderd3d::PresentationStage::FrameValidation);
        }
        state_->frames.push_back({frame.frameSequence, {frame.pixels.begin(), frame.pixels.end()}});
        return {};
    }
    void RequestStop() noexcept override
    {
        const std::scoped_lock lock(state_->mutex);
        state_->stopped = true;
    }
    void Stop() noexcept override
    {
        RequestStop();
    }
private:
    std::shared_ptr<PresentationState> state_;
};

[[nodiscard]] std::filesystem::path SessionPath(const pbapp::EncoderSnapshot& snapshot)
{
    return std::filesystem::path(std::u8string(snapshot.sessionStateDirectory.begin(), snapshot.sessionStateDirectory.end()));
}

void VerifyPublishedPixels(const std::vector<PresentedFrame>& frames, const std::span<const std::byte> original,
    const std::filesystem::path& outputDirectory)
{
    auto oracleResult = pbmodulation::UnifiedVisualCpuOracle::Create(pbmodulation::UnifiedVisualCpuOracle::RequiredBytes());
    REQUIRE(oracleResult);
    auto oracle = std::move(oracleResult).Value();
    const auto policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    auto receiverResult = pbreceiver::ReceiverIngress::Create(policy, 1314);
    REQUIRE(receiverResult);
    auto receiver = std::move(receiverResult).Value();
    auto controlsResult = pbprotocol::ControlPlaneReceiver::Create(policy);
    REQUIRE(controlsResult);
    auto controls = std::move(controlsResult).Value();
    std::unique_ptr<pbstorage::OutputFile> output;
    pbprotocol::SessionTag sessionTag{};
    bool sawControl = false;
    bool sawTransport = false;
    for (const PresentedFrame& frame : frames)
    {
        const pbmodulation::LumaView view{frame.pixels, 1920, 1080, 1920 * 4,
            pbmodulation::LumaPixelFormat::Bgra8};
        const auto observation = oracle.DecodeMixedFrame(view);
        REQUIRE(observation.IsFrameAvailable());
        REQUIRE(observation.bootstrapRecord.visualProfileId == pbprotocol::kUnifiedVisualProfileId);
        REQUIRE(observation.bootstrapRecord.visualLayoutVersion == pbprotocol::kUnifiedVisualLayoutVersion);
        REQUIRE(observation.bootstrapRecord.frameSequence == frame.sequence);
        sessionTag = observation.bootstrapRecord.sessionTag;
        for (const auto& block : oracle.GetAcceptedBlocks())
        {
            const auto bytes = std::span(block.bytes).first(block.size);
            if (block.kind == pbmodulation::UnifiedSlotKind::Control)
            {
                sawControl = true;
                REQUIRE(controls.ReceiveControlRecord(bytes));
                REQUIRE(receiver.ReceiveControlRecord(bytes));
                if (!output)
                {
                    const auto session = controls.GetSessionDescriptor(sessionTag);
                    if (session)
                    {
                        REQUIRE(session.Value().originalFileSize == original.size());
                        REQUIRE(session.Value().sessionVisualProfileId == pbprotocol::kUnifiedVisualProfileId);
                        pbstorage::OutputFileConfig config;
                        config.outputDirectory = outputDirectory.wstring();
                        config.sessionTag = sessionTag;
                        config.fileBytes = session.Value().originalFileSize;
                        config.maximumFileBytes = policy.maxAcceptedFileBytes;
                        config.originalFileNameUtf8 = session.Value().fileNameUtf8;
                        REQUIRE(pbstorage::OutputFile::Create(config, output));
                    }
                }
                continue;
            }
            sawTransport = true;
            const auto parsed = pbprotocol::ParseTransportBlock(bytes);
            REQUIRE(parsed);
            std::array<std::byte, 1314> padded{};
            std::copy(parsed.Value().payload.begin(), parsed.Value().payload.end(), padded.begin());
            auto admission = receiver.ReceiveDataBlock({parsed.Value().header.sessionTag, parsed.Value().header.segmentOrdinal,
                parsed.Value().header.outerBlockId, parsed.Value().header.payloadBytes, padded});
            REQUIRE(admission);
            if (admission.Value().completedSegment)
            {
                auto verified = receiver.VerifyRecoveredSegment(std::move(*admission.Value().completedSegment));
                REQUIRE(verified);
                REQUIRE(output);
                const auto& descriptor = verified.Value().GetBoundSegmentDescriptor().GetDescriptor();
                REQUIRE(output->WriteVerifiedSegment(descriptor.rawOffset, verified.Value().GetRawBytes()));
                REQUIRE(output->FlushVerifiedSegment());
                REQUIRE(receiver.CommitStoredSegment(std::move(verified).Value()));
            }
        }
    }
    REQUIRE(sawControl);
    REQUIRE(sawTransport == !original.empty());
    const auto finalization = receiver.PrepareFinalization(sessionTag);
    REQUIRE(finalization);
    REQUIRE(output);
    REQUIRE(output->Publish(finalization.Value().wholeFileDigest));
    const auto published = output->GetSnapshot();
    REQUIRE(published.published);
    REQUIRE_FALSE(std::filesystem::exists(published.partPath));
    std::ifstream reopened(published.finalPath, std::ios::binary);
    REQUIRE(reopened);
    std::vector<std::byte> actual(original.size());
    reopened.read(reinterpret_cast<char*>(actual.data()), static_cast<std::streamsize>(actual.size()));
    REQUIRE(std::ranges::equal(actual, original));
    REQUIRE(reopened.peek() == std::char_traits<char>::eof());
    REQUIRE(pbprotocol::ComputeBlake3Digest(actual) == finalization.Value().wholeFileDigest.bytes);
}

} // namespace

TEST_CASE("Unified sender stripes eight active Segments while the matching receiver retains all eight",
    "[application][g21][unified][striping][receiver-resource]")
{
    pbapp::UnifiedTemporalStripingProbeSnapshot probe;
    const pbapp::RuntimeStatus status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedTemporalStriping(probe);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(probe.configuredWindowSize == pbapp::senderUnifiedActiveSegmentWindowSize);
    REQUIRE(probe.unifiedReceiverActiveDecoderLimit == pbapp::senderUnifiedReceiverActiveDecoderLimit);
    REQUIRE(pbapp::senderUnifiedActiveSegmentWindowSize < pbapp::senderUnifiedReceiverActiveDecoderLimit);
    REQUIRE(probe.legacyReceiverActiveDecoderLimit == 4);
    REQUIRE(probe.receiverActiveDecoderCount == pbapp::senderUnifiedActiveSegmentWindowSize);
    REQUIRE(probe.receiverDeferredResourceBusyCount == 0);
    const std::uint64_t windowSize = pbapp::senderUnifiedActiveSegmentWindowSize;
    REQUIRE(probe.initialSegmentOrdinals.size() == windowSize * windowSize);
    REQUIRE(probe.initialCheckpointSegmentOrdinals.size() == probe.initialSegmentOrdinals.size());
    for (std::size_t index = 0; index < probe.initialSegmentOrdinals.size(); index++)
    {
        const std::uint64_t sweepOrdinal = index / windowSize;
        const std::uint64_t phaseEpoch = sweepOrdinal / pbapp::senderUnifiedSweepPhaseHold;
        const std::uint64_t positionInSweep = index % windowSize;
        CHECK(probe.initialSegmentOrdinals[index] ==
            (phaseEpoch * pbapp::senderUnifiedSweepPhaseStep + positionInSweep) % windowSize);
        CHECK(probe.initialCheckpointSegmentOrdinals[index] == 0);
    }
    REQUIRE(probe.passZeroLogicalFrames >= probe.initialSegmentOrdinals.size());
    // Per-Segment graduation slides the window mid-pass, so the durable
    // checkpoint advances once per graduated front instead of only at the
    // whole-window barrier.
    REQUIRE(probe.durablePositionUpdateCount >= 1);
    REQUIRE(probe.durablePositionUpdateCount <= pbapp::senderUnifiedActiveSegmentWindowSize);
    REQUIRE(probe.peakResidentEncodedSegmentCount == pbapp::senderUnifiedActiveSegmentWindowSize);
    REQUIRE(probe.peakResidentEncodedSegmentBytes ==
        static_cast<std::uint64_t>(pbapp::senderUnifiedActiveSegmentWindowSize) * 64ULL * 1024ULL);
    REQUIRE(probe.blockCounts.size() == pbapp::senderUnifiedActiveSegmentWindowSize);
    REQUIRE(probe.passZeroScheduledEquationCounts.size() == probe.blockCounts.size());
    REQUIRE(probe.passZeroUniqueOuterBlockCounts.size() == probe.blockCounts.size());
    REQUIRE(probe.passZeroMaximumOuterBlockIds.size() == probe.blockCounts.size());
    REQUIRE(probe.passOneFirstRepairIds.size() == probe.blockCounts.size());
    REQUIRE(probe.repairIdLeaseEnds.size() == probe.blockCounts.size());
    for (std::size_t segmentIndex = 0; segmentIndex < probe.blockCounts.size(); segmentIndex++)
    {
        const std::uint64_t expectedInitialRepairEquations =
            (std::max)(static_cast<std::uint64_t>(pbapp::senderUnifiedMinimumInitialRepairBlocks),
                (static_cast<std::uint64_t>(probe.blockCounts[segmentIndex]) - 1ULL) /
                    (pbapp::senderUnifiedInitialRepairPercentDenominator /
                        pbapp::senderUnifiedInitialRepairPercentNumerator) + 1ULL) +
            pbapp::senderUnifiedInitialTransitionGuardBlocks;
        CHECK(probe.blockCounts[segmentIndex] > 2);
        // Graduation re-passes (multi-window files) or barrier re-visits
        // (small files) add repair on top of the initial pass while the
        // Segment stays in observation; only the lower bound is contractual.
        CHECK(probe.passZeroScheduledEquationCounts[segmentIndex] >=
            probe.blockCounts[segmentIndex] + expectedInitialRepairEquations);
        CHECK(probe.passZeroScheduledEquationCounts[segmentIndex] ==
            probe.passZeroUniqueOuterBlockCounts[segmentIndex]);
        CHECK(probe.passZeroMaximumOuterBlockIds[segmentIndex] + 1ULL ==
            probe.passZeroScheduledEquationCounts[segmentIndex]);
        CHECK(probe.passOneFirstRepairIds[segmentIndex] ==
            probe.passZeroMaximumOuterBlockIds[segmentIndex] + 1ULL);
        // The striping probe drives a threshold-rate (15 Hz) sender, so Pass 1
        // keeps the historical K+20% FullRepairPass budget.
        const std::uint64_t expectedFullRepairPassEquations =
            probe.blockCounts[segmentIndex] +
            (std::max)(static_cast<std::uint64_t>(pbapp::senderCarouselMinimumRepairBlocks),
                (static_cast<std::uint64_t>(probe.blockCounts[segmentIndex]) - 1ULL) /
                    (pbapp::senderCarouselRepairPercentDenominator / pbapp::senderCarouselRepairPercentNumerator) + 1ULL);
        CHECK(probe.repairIdLeaseEnds[segmentIndex] ==
            probe.passOneFirstRepairIds[segmentIndex] + expectedFullRepairPassEquations);
    }
}

TEST_CASE("GrayFast graduation counts committed equations rather than leased repair IDs",
    "[application][grayfast-graduation][scheduler][resume]")
{
    for (const std::uint64_t initialCarouselPass : {0ULL, 3ULL, 50ULL})
    {
        for (const std::uint32_t logicalVisualFps : {15U, 30U, 60U})
        {
            INFO("Initial Carousel pass: " << initialCarouselPass << ", FPS: " << logicalVisualFps);
            pbapp::UnifiedGraduationProbeSnapshot probe;
            const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduation(13, initialCarouselPass,
                initialCarouselPass == 0 ? 0U : static_cast<std::uint32_t>(pbapp::encoderDurableIdLeaseSize), logicalVisualFps, probe);
            INFO(status.message);
            REQUIRE(status);
            REQUIRE(probe.completedCarouselPasses == 2);
            REQUIRE(probe.blockCounts.size() == 13);
            CHECK(probe.laterSystematicEquations == 0);
            CHECK(probe.repeatedScheduledRepairIds == 0);
            CHECK(probe.repairIdsBelowLeaseStart == 0);
            CHECK(probe.peakResidentEncodedSegmentCount <= pbapp::senderUnifiedActiveSegmentWindowSize);
            for (std::size_t segmentIndex = 0; segmentIndex < probe.blockCounts.size(); segmentIndex++)
            {
                INFO("Segment: " << segmentIndex);
                const std::uint64_t blockCount = probe.blockCounts[segmentIndex];
                REQUIRE(blockCount == 161);
                // Independent tuning vector: ceil(sqrt(161 * 17)) = 53.
                // The lease accounting assertions remain exact at every FPS.
                const std::uint64_t expectedInitialEquations = logicalVisualFps == 15 ? 534ULL : logicalVisualFps == 30 ? 1068ULL : 2136ULL;
                CHECK(probe.firstCarouselEquations[segmentIndex] == expectedInitialEquations);
                CHECK(probe.firstCarouselSystematicEquations[segmentIndex] == (initialCarouselPass == 0 ? blockCount : 0));
                CHECK(probe.secondCarouselEquations[segmentIndex] == blockCount / 4ULL + 1ULL);
            }
        }
    }
}

TEST_CASE("GrayFast two-window initial Carousel visits every Segment before switching to repair-only",
    "[application][grayfast-barrier][scheduler]")
{
    pbapp::UnifiedGraduationProbeSnapshot probe;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduation(7, 0, 0, 30, probe);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(probe.completedCarouselPasses == 2);
    CHECK(probe.laterSystematicEquations == 0);
    CHECK(probe.repeatedScheduledRepairIds == 0);
    for (std::size_t segmentIndex = 0; segmentIndex < probe.blockCounts.size(); segmentIndex++)
    {
        INFO("Segment: " << segmentIndex);
        const std::uint64_t blockCount = probe.blockCounts[segmentIndex];
        REQUIRE(blockCount != 0);
        CHECK(probe.firstCarouselSystematicEquations[segmentIndex] == blockCount);
        CHECK(probe.firstCarouselEquations[segmentIndex] > blockCount);
        CHECK(probe.secondCarouselEquations[segmentIndex] > 0);
        CHECK(probe.secondCarouselEquations[segmentIndex] < blockCount);
    }
}

TEST_CASE("GrayFast more-than-quota Segments recover after frame erasures through safe publication",
    "[application][grayfast-recovery][receiver][publish]")
{
    Scratch scratch;
    for (const auto& [eraseTwoThirds, logicalVisualFps, periodicErasure] : std::array{std::tuple{false, 15U, false},
        std::tuple{true, 15U, false}, std::tuple{true, 30U, false}, std::tuple{true, 30U, true}})
    {
        INFO("Erase two thirds of frames: " << eraseTwoThirds << ", FPS: " << logicalVisualFps << ", periodic: " << periodicErasure);
        const auto outputDirectory = scratch.Path() / ((eraseTwoThirds ? L"lossy-" : L"lossless-") +
            std::to_wstring(logicalVisualFps) + (periodicErasure ? L"-periodic" : L"-permuted"));
        REQUIRE(std::filesystem::create_directory(outputDirectory));
        pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
        const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(outputDirectory.wstring(), eraseTwoThirds, logicalVisualFps, probe, periodicErasure);
        INFO(status.message);
        REQUIRE(status);
        REQUIRE(probe.decoder.state == pbapp::DecoderState::Completed);
        REQUIRE(probe.decoder.verifiedRawBytes == 13ULL * 256ULL * 1024ULL);
        REQUIRE(probe.decoder.wholeFileDigestCheck == true);
        REQUIRE(probe.decoder.finalRenameSucceeded == true);
        REQUIRE(probe.decoder.finalReopenVerified == true);
        const auto policy = pbapp::MakeUnifiedReceiverResourcePolicy();
        REQUIRE(probe.peakActiveDecoders <= policy.maxActiveOuterFecDecoders);
        REQUIRE(probe.peakReservedDecoderBytes <= policy.maxTotalOuterFecDecoderBytes);
        REQUIRE(probe.decoder.outerConflictRejections == 0);
        if (eraseTwoThirds && logicalVisualFps == 15)
        {
            // The finite-window margin now absorbs this milder projection
            // without quota rejection; a separate quarter-rate case below
            // still exercises the actual saturated receiver and recovery.
            REQUIRE(probe.deferredResourceBusyCount == 0);
            REQUIRE(probe.observedLogicalFrames * 2 < probe.senderLogicalFrames);
        }
        else
        {
            REQUIRE(probe.deferredResourceBusyCount == 0);
            REQUIRE(probe.completedCarouselPasses == 0);
            REQUIRE((probe.observedLogicalFrames == probe.senderLogicalFrames) == !eraseTwoThirds);
        }
        REQUIRE(std::filesystem::file_size(probe.publishedPath) == probe.decoder.verifiedRawBytes);
        std::ifstream file(probe.publishedPath, std::ios::binary);
        REQUIRE(file);
        std::vector<std::byte> reopenedBytes(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
        file.read(reinterpret_cast<char*>(reopenedBytes.data()), static_cast<std::streamsize>(reopenedBytes.size()));
        REQUIRE(file.gcount() == static_cast<std::streamsize>(reopenedBytes.size()));
        REQUIRE(pbprotocol::ComputeBlake3Digest(reopenedBytes) == probe.expectedWholeFileDigest);
        std::cout << "{\"probe\":\"GrayFastGraduationRecovery\",\"syntheticNoRaster\":true,\"eraseTwoThirds\":"
            << (eraseTwoThirds ? "true" : "false") << ",\"periodicErasure\":" << (periodicErasure ? "true" : "false")
            << ",\"fps\":" << logicalVisualFps << ",\"senderFrames\":" << probe.senderLogicalFrames
            << ",\"observedFrames\":" << probe.observedLogicalFrames << ",\"carouselPasses\":" << probe.completedCarouselPasses
            << ",\"peakActiveDecoders\":" << probe.peakActiveDecoders << ",\"deferredResourceBusy\":" << probe.deferredResourceBusyCount
            << ",\"publishedReopenedBytes\":" << probe.decoder.verifiedRawBytes << "}\n";
    }
}

TEST_CASE("Unified headless probes use the configured wall clock rather than a fixed 15 FPS clock",
    "[application][grayfast-clock][scheduler]")
{
    for (const std::uint32_t logicalVisualFps : {15U, 30U, 60U})
    {
        INFO("Configured FPS: " << logicalVisualFps);
        const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedHeadlessClock(logicalVisualFps);
        INFO(status.message);
        CHECK(status);
    }
}

TEST_CASE("GrayFast still recovers after actual receiver quota deferrals under stronger erasures",
    "[application][grayfast-recovery][receiver][quota][publish]")
{
    Scratch scratch;
    const pbapp::UnifiedGraduationRecoveryProbeConfig config{13, 256U * 1024U, 15, pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter};
    pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(scratch.Path().wstring(), config, probe);
    INFO(status.message);
    REQUIRE(status);
    const auto policy = pbapp::MakeUnifiedReceiverResourcePolicy();
    REQUIRE(probe.peakActiveDecoders == policy.maxActiveOuterFecDecoders);
    REQUIRE(probe.peakReservedDecoderBytes <= policy.maxTotalOuterFecDecoderBytes);
    REQUIRE(probe.deferredResourceBusyCount > 0);
    REQUIRE(probe.completedCarouselPasses > 0);
    REQUIRE(probe.decoder.state == pbapp::DecoderState::Completed);
    REQUIRE(probe.decoder.wholeFileDigestCheck == true);
    REQUIRE(probe.decoder.finalRenameSucceeded == true);
    REQUIRE(probe.decoder.finalReopenVerified == true);
    REQUIRE(probe.decoder.verifiedRawBytes == 13ULL * 256ULL * 1024ULL);
    std::ifstream file(probe.publishedPath, std::ios::binary);
    REQUIRE(file);
    std::vector<std::byte> reopenedBytes(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
    file.read(reinterpret_cast<char*>(reopenedBytes.data()), static_cast<std::streamsize>(reopenedBytes.size()));
    REQUIRE(file.gcount() == static_cast<std::streamsize>(reopenedBytes.size()));
    REQUIRE(pbprotocol::ComputeBlake3Digest(reopenedBytes) == probe.expectedWholeFileDigest);
    std::cout << "{\"probe\":\"GrayFastQuarterRateQuotaRecovery\",\"syntheticNoRaster\":true,\"senderFrames\":"
        << probe.senderLogicalFrames << ",\"observedFrames\":" << probe.observedLogicalFrames
        << ",\"carouselPasses\":" << probe.completedCarouselPasses << ",\"peakActiveDecoders\":" << probe.peakActiveDecoders
        << ",\"deferredResourceBusy\":" << probe.deferredResourceBusyCount << "}\n";
}

TEST_CASE("GrayFast many small Segments do not wait a whole file rotation under irregular frame loss",
    "[application][grayfast-sparse-recovery][receiver][publish]")
{
    Scratch scratch;
    const pbapp::UnifiedGraduationRecoveryProbeConfig config{64, 256U * 1024U, 30, pbapp::UnifiedRecoveryErasureModel::SparseBursty};
    pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(scratch.Path().wstring(), config, probe);
    INFO(status.message);
    std::cout << "{\"probe\":\"GrayFastSparseBurstyRecovery\",\"syntheticNoRaster\":true,\"senderFrames\":"
        << probe.senderLogicalFrames << ",\"observedFrames\":" << probe.observedLogicalFrames
        << ",\"carouselPasses\":" << probe.completedCarouselPasses << ",\"peakActiveDecoders\":" << probe.peakActiveDecoders
        << ",\"deferredResourceBusy\":" << probe.deferredResourceBusyCount
        << ",\"longestNoUsefulEquationFrames\":" << probe.longestNoUsefulEquationFrames << "}\n";
    REQUIRE(status);
    REQUIRE(probe.decoder.state == pbapp::DecoderState::Completed);
    REQUIRE(probe.decoder.verifiedRawBytes == static_cast<std::uint64_t>(config.segmentCount) * config.segmentBytes);
    REQUIRE(probe.decoder.wholeFileDigestCheck == true);
    REQUIRE(probe.decoder.finalRenameSucceeded == true);
    REQUIRE(probe.decoder.finalReopenVerified == true);
    const auto policy = pbapp::MakeUnifiedReceiverResourcePolicy();
    REQUIRE(probe.peakActiveDecoders <= policy.maxActiveOuterFecDecoders);
    REQUIRE(probe.peakReservedDecoderBytes <= policy.maxTotalOuterFecDecoderBytes);
    REQUIRE(probe.decoder.outerConflictRejections == 0);
    // At most a 30-second useful-equation drought in this bounded synthetic
    // projection. This is not a promise about arbitrary remote outages.
    REQUIRE(probe.longestNoUsefulEquationFrames <= 30ULL * config.logicalVisualFps);
    REQUIRE(std::filesystem::file_size(probe.publishedPath) == probe.decoder.verifiedRawBytes);
    std::ifstream file(probe.publishedPath, std::ios::binary);
    REQUIRE(file);
    std::vector<std::byte> reopenedBytes(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
    file.read(reinterpret_cast<char*>(reopenedBytes.data()), static_cast<std::streamsize>(reopenedBytes.size()));
    REQUIRE(file.gcount() == static_cast<std::streamsize>(reopenedBytes.size()));
    REQUIRE(pbprotocol::ComputeBlake3Digest(reopenedBytes) == probe.expectedWholeFileDigest);
}

TEST_CASE("Unified fountain mid-join recovers every Segment from pure incremental repair passes",
    "[application][g21][unified][fountain][mid-join]")
{
    pbapp::UnifiedFountainMidJoinProbeSnapshot probe;
    const pbapp::RuntimeStatus status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedFountainMidJoin(probe);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(probe.segmentCount == pbapp::senderUnifiedActiveSegmentWindowSize);
    REQUIRE(probe.completedSegments == probe.segmentCount);
    REQUIRE(probe.everySegmentDigestVerified);
    REQUIRE(probe.skippedPassZeroLogicalFrames > 0);
    REQUIRE(probe.observedRepairLogicalFrames > 0);
    // The receiver never saw a Pass-0 frame, so every admitted symbol must be
    // a repair equation far above the systematic range.
    REQUIRE(probe.everyAdmittedSymbolWasRepair);
    REQUIRE(probe.admittedUniqueOuterSymbols >= probe.segmentCount);
    REQUIRE(probe.alreadyCompletedSymbols == 0);
    // Accumulating K repair equations per Segment from the incremental
    // budget must span several Carousel passes, never just one.
    REQUIRE(probe.completedCarouselPasses >= 2);
    // The fountain waste bound: re-sweeping an already-recovered Segment only
    // schedules the small incremental budget, never a full K-sized pass.
    for (std::size_t segmentIndex = 0; segmentIndex < probe.perSegmentFountainRepairBudget.size(); segmentIndex++)
    {
        REQUIRE(probe.perSegmentFountainRepairBudget[segmentIndex] >= pbapp::senderCarouselMinimumRepairBlocks);
    }
    REQUIRE(probe.receiverDeferredResourceBusyCount == 0);
    REQUIRE(probe.receiverOuterFecQuotaExceededCount == 0);
    REQUIRE(probe.receiverPeakActiveDecoderCount <= pbapp::senderUnifiedActiveSegmentWindowSize);
}

TEST_CASE("Unified eight-Segment window recovers 64 MiB through sparse observations and bounded burst erasures",
    "[.g21-large-window][application][g21][unified][large-window][receiver]")
{
    pbapp::UnifiedLargeWindowRecoveryProbeSnapshot probe;
    const pbapp::RuntimeStatus status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedLargeWindowRecovery(probe);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(probe.sourceBytes ==
        static_cast<std::uint64_t>(pbapp::senderUnifiedActiveSegmentWindowSize) *
        pbprotocol::kDefaultSourceSegmentTargetBytes);
    REQUIRE(probe.completedSegments == pbapp::senderUnifiedActiveSegmentWindowSize);
    REQUIRE(probe.everySegmentDigestVerified);
    REQUIRE(probe.senderLogicalFrames > probe.uniqueLogicalFrames);
    REQUIRE(probe.senderLogicalFrames <= 27000);
    REQUIRE(probe.uniqueLogicalFrames > 0);
    REQUIRE(probe.unobservedSenderLogicalFrames > 0);
    REQUIRE(probe.intentionallyErasedLogicalFrames > 0);
    REQUIRE(probe.completedCarouselPasses > 0);
    REQUIRE(probe.uniqueOuterSymbols > 0);
    REQUIRE(probe.receiverPeakActiveDecoderCount == pbapp::senderUnifiedActiveSegmentWindowSize);
    REQUIRE(probe.receiverPeakReservedDecoderBytes > 0);
    REQUIRE(probe.receiverDeferredResourceBusyCount == 0);
    REQUIRE(probe.receiverOuterFecQuotaExceededCount == 0);
    REQUIRE(probe.verifiedEncodedBytesPerUniqueFrame >= 16.0 * 1024.0);
    const std::uint64_t windowSize = pbapp::senderUnifiedActiveSegmentWindowSize;
    REQUIRE(probe.phaseVisitCounts.size() == windowSize * windowSize);
    for (std::uint64_t segmentOrdinal = 0; segmentOrdinal < windowSize; segmentOrdinal++)
    {
        std::uint64_t visitedPhaseCount = 0;
        for (std::uint64_t framePhase = 0; framePhase < windowSize; framePhase++)
        {
            visitedPhaseCount += static_cast<std::uint64_t>(
                probe.phaseVisitCounts[segmentOrdinal * windowSize + framePhase] != 0);
        }
        INFO("segmentOrdinal=" << segmentOrdinal);
        CHECK(visitedPhaseCount >= 4);
    }
}

TEST_CASE("Unified current-Segment descriptor precedes Transport after a bounded capture gap",
    "[application][g21][unified][descriptor-prelude][receiver-resource]")
{
    pbapp::UnifiedDescriptorPreludeProbeSnapshot probe;
    const pbapp::RuntimeStatus status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedDescriptorPrelude(probe);
    INFO(status.message);
    INFO("resourceLimitExceededCount=" << probe.resourceLimitExceededCount);
    INFO("resourceLimitExceededWhileSixActive=" << probe.resourceLimitExceededWhileSixActive);
    INFO("resourceLimitExceededWhileSevenActive=" << probe.resourceLimitExceededWhileSevenActive);
    INFO("resourceLimitExceededAfterEightActive=" << probe.resourceLimitExceededAfterEightActive);
    INFO("orphanAdmittedBlockCount=" << probe.orphanAdmittedBlockCount);
    INFO("orphanDroppedByQuotaCount=" << probe.orphanDroppedByQuotaCount);
    INFO("orphanCachedBlockCount=" << probe.orphanCachedBlockCount);
    INFO("finalActiveDecoderCount=" << probe.finalActiveDecoderCount);
    REQUIRE(status);
    REQUIRE(probe.observedLogicalFrames > 0);
    REQUIRE(probe.activeDecoderTransitions.size() >= 3);
    REQUIRE(probe.activeDecoderTransitions[probe.activeDecoderTransitions.size() - 3] ==
        pbapp::senderUnifiedActiveSegmentWindowSize - 2);
    REQUIRE(probe.activeDecoderTransitions[probe.activeDecoderTransitions.size() - 2] ==
        pbapp::senderUnifiedActiveSegmentWindowSize - 1);
    REQUIRE(probe.activeDecoderTransitions.back() == pbapp::senderUnifiedActiveSegmentWindowSize);
    REQUIRE(probe.finalActiveDecoderCount == pbapp::senderUnifiedActiveSegmentWindowSize);
    REQUIRE(probe.resourceLimitExceededCount == 0);
    REQUIRE(probe.resourceExhaustedCount == 0);
    REQUIRE(probe.resourceLimitExceededWhileSixActive == 0);
    REQUIRE(probe.resourceLimitExceededWhileSevenActive == 0);
    REQUIRE(probe.resourceLimitExceededAfterEightActive == 0);
    REQUIRE(probe.orphanAdmittedBlockCount == 0);
    REQUIRE(probe.orphanDroppedByQuotaCount == 0);
    REQUIRE(probe.orphanResourceExhaustedCount == 0);
    REQUIRE(probe.orphanCachedBlockCount == 0);
    REQUIRE(probe.orphanCachedBytes == 0);
    REQUIRE(probe.totalResourcePolicyRejectedCount == 0);
    REQUIRE(probe.outerFecQuotaExceededCount == 0);
    REQUIRE(probe.deferredResourceBusyCount == 0);
}

TEST_CASE("Unified Encoder product policy is shared and rejects legacy tuning", "[application][g15][model]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"source.bin";
    WriteBytes(source, {});
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring());
    REQUIRE(config.visualProfile == pbapp::VisualProfile::UnifiedLc4);
    REQUIRE(config.logicalVisualFps == 15);
    REQUIRE(config.compressionEnabled);
    REQUIRE(config.compressionLevel == 3);
    REQUIRE_FALSE(config.monitorClientOrigin);
    REQUIRE(pbapp::ValidateEncoderConfig(config));
    REQUIRE(pbapp::ParseVisualProfileToken("unified") == config.visualProfile);
    REQUIRE(pbapp::ParseVisualProfileToken(L"unified") == config.visualProfile);
    REQUIRE(std::string_view(pbapp::GetVisualProfileName(config.visualProfile)) == "PB-Unified-SC6-V3");
    REQUIRE_FALSE(pbapp::IsRemoteVisualProfile(config.visualProfile));
    for (const std::uint32_t fps : {0U, 61U, 240U})
    {
        config.logicalVisualFps = fps;
        REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    }
    for (const std::uint32_t fps : {1U, 15U, 60U})
    {
        config.logicalVisualFps = fps;
        REQUIRE(pbapp::ValidateEncoderConfig(config));
    }
    config.compressionEnabled = false;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.compressionEnabled = true;
    config.compressionLevel = 4;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.compressionLevel = 3;
    config.controlRepetitions = 12;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
}

TEST_CASE("GrayFast demand presentation preserves pending retries and sequential visual identities",
    "[application][g15][runtime][pixels][grayfast-demand-present]")
{
    for (const auto profile : {pbapp::VisualProfile::UnifiedGrayFast, pbapp::VisualProfile::UnifiedGray, pbapp::VisualProfile::UnifiedLc4})
    {
        CAPTURE(static_cast<unsigned int>(profile));
        Scratch scratch;
        const auto source = scratch.Path() / L"presentation-source.bin";
        const std::vector<std::byte> bytes(4096, std::byte{0x59});
        WriteBytes(source, bytes);
        auto state = std::make_shared<PresentationState>();
        state->rejectFirst = true;
        std::atomic<int> observedRepeatPolicy{-1};
        pbapp::EncoderRuntime runtime([&](const pbrenderd3d::DataWindowConfig& window)
        {
            observedRepeatPolicy = window.repeatActiveFrame ? 1 : 0;
            return std::make_unique<MockPresentation>(state);
        });
        auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 30);
        config.visualProfile = profile;
        config.sessionStateRoot = scratch.Path() / L"sessions";
        REQUIRE(runtime.Start(config));
        REQUIRE(WaitFor([&]()
        {
            const std::scoped_lock lock(state->mutex);
            return state->attempts == 1;
        }));
        REQUIRE(observedRepeatPolicy.load() == (profile == pbapp::VisualProfile::UnifiedGrayFast ? 0 : 1));
        const auto pending = runtime.GetSnapshot();
        REQUIRE(pending.frameSequence == 0);
        REQUIRE(pending.cycleCount == 0);
        REQUIRE(pending.durableFrameSequenceLeaseEnd > 0);
        {
            const std::scoped_lock lock(state->mutex);
            state->paused = false;
        }
        REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().frameSequence == 2; }));
        runtime.Stop();
        const auto stopped = runtime.GetSnapshot();
        REQUIRE(stopped.state == pbapp::EncoderState::Stopped);
        REQUIRE(stopped.frameSequence == 2);
        REQUIRE(stopped.sourceStable);
        REQUIRE(state->retryIdentical);
        REQUIRE(state->frames.size() == 2);
        for (std::size_t index = 0; index < state->frames.size(); index++)
        {
            const auto& frame = state->frames[index];
            REQUIRE(frame.sequence == index);
            const pbmodulation::LumaView view{frame.pixels, 1920, 1080, 1920U * 4U, pbmodulation::LumaPixelFormat::Bgra8};
            const auto bootstrap = pbmodulation::DecodeLocalDesktopFixedCanvasBootstrap(view,
                {stopped.visualProfileId, stopped.visualLayoutVersion});
            REQUIRE(bootstrap.IsAccepted());
            const auto record = pbprotocol::ParseBootstrapRecord(bootstrap.canonical44);
            REQUIRE(record);
            REQUIRE(record.Value().frameSequence == index);
        }
    }
}

TEST_CASE("Unified runtime emits real mixed pixels after prescan and retries a pending frame without advancing IDs",
    "[application][g15][runtime][pixels]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"source.bin";
    std::vector<std::byte> bytes(20000);
    std::uint32_t randomState = 15092026;
    for (auto& byte : bytes)
    {
        randomState ^= randomState << 13U;
        randomState ^= randomState >> 17U;
        randomState ^= randomState << 5U;
        byte = static_cast<std::byte>(randomState & 255U);
    }
    WriteBytes(source, bytes);
    auto state = std::make_shared<PresentationState>();
    state->rejectFirst = true;
    std::atomic<bool> preparedBeforePresentation = false;
    pbapp::EncoderRuntime* runtimePointer = nullptr;
    pbapp::EncoderRuntime runtime([&](const pbrenderd3d::DataWindowConfig& window)
    {
        const auto prepared = runtimePointer->GetSnapshot();
        preparedBeforePresentation = prepared.preparationComplete && prepared.sourceStabilityVerified &&
            prepared.preparedSourceBytes == bytes.size() && prepared.preparedSegmentCount == 1 &&
            !prepared.sessionIdHex.empty() && window.repeatActiveFrame && !window.topmost;
        return std::make_unique<MockPresentation>(state);
    });
    runtimePointer = &runtime;
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 15);
    config.sessionStateRoot = scratch.Path() / L"sessions";
    REQUIRE(runtime.Start(config));
    REQUIRE(WaitFor([&]()
    {
        const std::scoped_lock lock(state->mutex);
        return state->attempts == 1;
    }));
    REQUIRE(preparedBeforePresentation);
    const auto pending = runtime.GetSnapshot();
    REQUIRE(pending.frameSequence == 0);
    REQUIRE(pending.cycleCount == 0);
    REQUIRE(pending.rawSegmentCount == 1);
    REQUIRE(pending.zstdSegmentCount == 0);
    REQUIRE(pending.outerFecMode == pbprotocol::OuterFecMode::WirehairV2);
    REQUIRE(pending.durableFrameSequenceLeaseEnd > 0);
    REQUIRE(pending.durableRepairIdLeaseEnd > 0);
    REQUIRE(pending.preparationBytesPerSecond.has_value());
    REQUIRE_FALSE(runtime.EndAndDeleteSession(pending.runGeneration));
    const HANDLE writer = CreateFileW(source.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(writer == INVALID_HANDLE_VALUE);
    REQUIRE(GetLastError() == ERROR_SHARING_VIOLATION);
    REQUIRE(runtime.SetLogicalVisualFps(60));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(runtime.GetSnapshot().frameSequence == 0);
    REQUIRE(runtime.GetSnapshot().configuredLogicalVisualFps == 15);
    {
        const std::scoped_lock lock(state->mutex);
        state->paused = false;
    }
    REQUIRE(WaitFor([&]()
    {
        return runtime.GetSnapshot().frameSequence == 2;
    }));
    runtime.Stop();
    const auto stopped = runtime.GetSnapshot();
    REQUIRE(stopped.state == pbapp::EncoderState::Stopped);
    REQUIRE(stopped.frameSequence == 2);
    REQUIRE(stopped.configuredLogicalVisualFps == 60);
    REQUIRE(stopped.repeatedPresentCalls == 100);
    REQUIRE(stopped.sourceStable);
    REQUIRE(std::filesystem::exists(SessionPath(stopped)));
    REQUIRE(state->retryIdentical);
    REQUIRE(state->frames.size() == 2);
    REQUIRE(state->frames[0].sequence == 0);
    REQUIRE(state->frames[1].sequence == 1);
    const auto output = scratch.Path() / L"output";
    REQUIRE(std::filesystem::create_directory(output));
    VerifyPublishedPixels(state->frames, bytes, output);
    REQUIRE_FALSE(runtime.EndAndDeleteSession(stopped.runGeneration + 1));
    REQUIRE(runtime.EndAndDeleteSession(stopped.runGeneration));
    REQUIRE(runtime.EndAndDeleteSession(stopped.runGeneration));
    REQUIRE_FALSE(std::filesystem::exists(SessionPath(stopped)));
    REQUIRE(std::filesystem::file_size(source) == bytes.size());
}

TEST_CASE("Unified runtime retains and resumes Session leases then explicit deletion forces a new Session",
    "[application][g15][runtime][resume]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"source.bin";
    WriteBytes(source, {});
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 60);
    config.sessionStateRoot = scratch.Path() / L"sessions";
    pbapp::EncoderSnapshot previous;
    for (std::uint32_t run = 0; run < 3; run++)
    {
        auto state = std::make_shared<PresentationState>();
        state->maximumFrames = 1;
        pbapp::EncoderRuntime runtime([state](const pbrenderd3d::DataWindowConfig&)
        {
            return std::make_unique<MockPresentation>(state);
        });
        REQUIRE(runtime.Start(config));
        REQUIRE(WaitFor([&]()
        {
            return runtime.GetSnapshot().submittedFrames == 1;
        }));
        runtime.Stop();
        const auto stopped = runtime.GetSnapshot();
        REQUIRE(stopped.state == pbapp::EncoderState::Stopped);
        REQUIRE(stopped.preparationComplete);
        REQUIRE(stopped.segmentCount == 0);
        REQUIRE(stopped.preparedSourceBytes == 0);
        if (run == 0)
        {
            const auto output = scratch.Path() / L"empty-output";
            REQUIRE(std::filesystem::create_directory(output));
            VerifyPublishedPixels(state->frames, {}, output);
        }
        if (run == 1)
        {
            REQUIRE(stopped.resumedSession);
            REQUIRE(stopped.sessionIdHex == previous.sessionIdHex);
            REQUIRE(state->frames.front().sequence >= previous.durableFrameSequenceLeaseEnd);
            REQUIRE(runtime.EndAndDeleteSession(stopped.runGeneration));
        }
        else if (run == 2)
        {
            REQUIRE_FALSE(stopped.resumedSession);
            REQUIRE(stopped.sessionIdHex != previous.sessionIdHex);
        }
        previous = stopped;
    }
}

TEST_CASE("Encoder runtime stops at the configured maximum duration and records the timeout reason",
    "[application][encoder][runtime][timeout]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"timeout-source.bin";
    WriteBytes(source, std::vector<std::byte>(1024, std::byte{0x42}));
    const auto state = std::make_shared<PresentationState>();
    state->maximumFrames = 1;
    pbapp::EncoderRuntime runtime([state](const pbrenderd3d::DataWindowConfig&)
    {
        return std::make_unique<MockPresentation>(state);
    });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 60);
    config.sessionStateRoot = scratch.Path() / L"sessions";
    config.maximumRunDurationSeconds = 1;
    REQUIRE(runtime.Start(config));
    REQUIRE(WaitFor([&]()
    {
        return runtime.GetSnapshot().state == pbapp::EncoderState::Stopped;
    }));
    runtime.Stop();
    const auto stopped = runtime.GetSnapshot();
    REQUIRE(stopped.state == pbapp::EncoderState::Stopped);
    REQUIRE(stopped.configuredMaximumRunDurationSeconds == 1);
    REQUIRE(stopped.stoppedByTimeout);
    REQUIRE(stopped.errorDetail.empty());
}

TEST_CASE("Encoder runtime zero maximum duration remains manual until explicitly stopped",
    "[application][encoder][runtime][timeout]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"manual-source.bin";
    WriteBytes(source, {});
    const auto state = std::make_shared<PresentationState>();
    state->maximumFrames = 1;
    pbapp::EncoderRuntime runtime([state](const pbrenderd3d::DataWindowConfig&)
    {
        return std::make_unique<MockPresentation>(state);
    });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 60);
    config.sessionStateRoot = scratch.Path() / L"sessions";
    config.maximumRunDurationSeconds = 0;
    REQUIRE(runtime.Start(config));
    REQUIRE(WaitFor([&]()
    {
        return runtime.GetSnapshot().submittedFrames == 1;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(pbapp::IsEncoderStateActive(runtime.GetSnapshot().state));
    runtime.RequestStop();
    REQUIRE(WaitFor([&]()
    {
        return runtime.GetSnapshot().state == pbapp::EncoderState::Stopped;
    }));
    runtime.Stop();
    const auto stopped = runtime.GetSnapshot();
    REQUIRE(stopped.state == pbapp::EncoderState::Stopped);
    REQUIRE(stopped.configuredMaximumRunDurationSeconds == 0);
    REQUIRE_FALSE(stopped.stoppedByTimeout);
}

TEST_CASE("Encoder timeout never suppresses a presentation shutdown failure", "[application][encoder][timeout]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"shutdown-failure.bin";
    WriteBytes(source, {});
    const auto state = std::make_shared<PresentationState>();
    state->failOnStop = true;
    state->maximumFrames = 1;
    pbapp::EncoderRuntime runtime([state](const pbrenderd3d::DataWindowConfig&)
    {
        return std::make_unique<MockPresentation>(state);
    });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring());
    config.sessionStateRoot = scratch.Path() / L"sessions";
    config.maximumRunDurationSeconds = 1;
    REQUIRE(runtime.Start(config));
    REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().state == pbapp::EncoderState::Failed; }));
    runtime.Stop();
    const auto failed = runtime.GetSnapshot();
    REQUIRE(failed.state == pbapp::EncoderState::Failed);
    REQUIRE(failed.errorDetail.find("DataWindow failed") != std::string::npos);
    REQUIRE_FALSE(failed.stoppedByTimeout);
}

TEST_CASE("Encoder timeout cancels the real prescan before creating a persistent Session", "[application][encoder][timeout]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"prescan.bin";
    WriteBytes(source, std::vector<std::byte>(16U * 1024U * 1024U, std::byte{0x5A}));
    std::atomic<bool> presentationCreated = false;
    pbapp::EncoderRuntime runtime([&](const pbrenderd3d::DataWindowConfig&)
    {
        presentationCreated = true;
        return std::make_unique<MockPresentation>(std::make_shared<PresentationState>());
    });
    const auto clockOrigin = std::chrono::steady_clock::now();
    pbapp::EncoderRuntimeClockTestAccess::SetClock(runtime, [&]()
    {
        return clockOrigin + std::chrono::seconds(runtime.GetSnapshot().preparedSourceBytes >= 8U * 1024U * 1024U ? 1 : 0);
    });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring());
    config.sessionStateRoot = scratch.Path() / L"sessions";
    config.maximumRunDurationSeconds = 1;
    REQUIRE(runtime.Start(config));
    REQUIRE(WaitFor([&]() { return !pbapp::IsEncoderStateActive(runtime.GetSnapshot().state); }));
    runtime.Stop();
    const auto stopped = runtime.GetSnapshot();
    REQUIRE(stopped.state == pbapp::EncoderState::Stopped);
    REQUIRE(stopped.stoppedByTimeout);
    REQUIRE(stopped.errorDetail.empty());
    REQUIRE(stopped.preparedSourceBytes > 0);
    REQUIRE_FALSE(stopped.preparationComplete);
    REQUIRE(stopped.sessionIdHex.empty());
    REQUIRE(stopped.submittedFrames == 0);
    REQUIRE_FALSE(presentationCreated);
    REQUIRE_FALSE(std::filesystem::exists(config.sessionStateRoot));
}

TEST_CASE("Encoder deadline resets on a resumed run and accepts the full uint64 range", "[application][encoder][timeout]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"restart.bin";
    WriteBytes(source, {});
    std::atomic<std::int64_t> elapsedMilliseconds = 0;
    const auto clockOrigin = std::chrono::steady_clock::now();
    pbapp::EncoderRuntime runtime([](const pbrenderd3d::DataWindowConfig&)
    {
        return std::make_unique<MockPresentation>(std::make_shared<PresentationState>());
    });
    pbapp::EncoderRuntimeClockTestAccess::SetClock(runtime, [&]()
    {
        return clockOrigin + std::chrono::milliseconds(elapsedMilliseconds.load());
    });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 60);
    config.sessionStateRoot = scratch.Path() / L"sessions";
    pbapp::EncoderSnapshot previous;
    for (const std::uint64_t seconds : {1ULL, 1ULL, 0ULL, UINT64_MAX})
    {
        config.maximumRunDurationSeconds = seconds;
        const std::int64_t startedAt = elapsedMilliseconds;
        REQUIRE(runtime.Start(config));
        REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().submittedFrames == 2; }));
        REQUIRE_FALSE(runtime.GetSnapshot().stoppedByTimeout);
        REQUIRE(runtime.GetSnapshot().configuredMaximumRunDurationSeconds == seconds);
        elapsedMilliseconds = startedAt + 999;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        REQUIRE(runtime.GetSnapshot().state == pbapp::EncoderState::Broadcasting);
        elapsedMilliseconds = startedAt + (seconds == 1 ? 1000 : 8000000);
        if (seconds != 1)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            REQUIRE(runtime.GetSnapshot().state == pbapp::EncoderState::Broadcasting);
            runtime.RequestStop();
        }
        REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().state == pbapp::EncoderState::Stopped; }));
        runtime.Stop();
        const auto stopped = runtime.GetSnapshot();
        REQUIRE(stopped.stoppedByTimeout == (seconds == 1));
        REQUIRE(stopped.errorDetail.empty());
        REQUIRE(std::filesystem::exists(SessionPath(stopped)));
        if (previous.runGeneration != 0)
        {
            REQUIRE(stopped.runGeneration > previous.runGeneration);
            REQUIRE(stopped.resumedSession);
            REQUIRE(stopped.sessionIdHex == previous.sessionIdHex);
            REQUIRE(stopped.frameSequence >= previous.durableFrameSequenceLeaseEnd);
        }
        previous = stopped;
    }
}

TEST_CASE("Encoder checks its deadline before opening a prepared presentation", "[application][encoder][timeout]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"pre-window.bin";
    WriteBytes(source, {});
    std::atomic<bool> presentationCreated = false;
    pbapp::EncoderRuntime runtime([&](const pbrenderd3d::DataWindowConfig&)
    {
        presentationCreated = true;
        return std::make_unique<MockPresentation>(std::make_shared<PresentationState>());
    });
    const auto clockOrigin = std::chrono::steady_clock::now();
    pbapp::EncoderRuntimeClockTestAccess::SetClock(runtime, [&]()
    {
        return clockOrigin + std::chrono::seconds(runtime.GetSnapshot().statusMessage == "Creating the production D3D11 Data Window" ? 1 : 0);
    });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring());
    config.sessionStateRoot = scratch.Path() / L"sessions";
    config.maximumRunDurationSeconds = 1;
    REQUIRE(runtime.Start(config));
    REQUIRE(WaitFor([&]() { return !pbapp::IsEncoderStateActive(runtime.GetSnapshot().state); }));
    runtime.Stop();
    const auto stopped = runtime.GetSnapshot();
    REQUIRE(stopped.state == pbapp::EncoderState::Stopped);
    REQUIRE(stopped.stoppedByTimeout);
    REQUIRE(stopped.errorDetail.empty());
    REQUIRE(std::filesystem::exists(SessionPath(stopped)));
    REQUIRE_FALSE(presentationCreated);
}

TEST_CASE("Encoder never submits a frame after a slow presentation observation crosses the deadline", "[application][encoder][timeout]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"submit-deadline.bin";
    WriteBytes(source, {});
    std::atomic<bool> deadlineReached = false;
    const auto state = std::make_shared<PresentationState>();
    state->beforeSnapshot = [&]() { deadlineReached = true; };
    pbapp::EncoderRuntime runtime([state](const pbrenderd3d::DataWindowConfig&)
    {
        return std::make_unique<MockPresentation>(state);
    });
    const auto clockOrigin = std::chrono::steady_clock::now();
    pbapp::EncoderRuntimeClockTestAccess::SetClock(runtime, [&]()
    {
        return clockOrigin + std::chrono::seconds(deadlineReached ? 1 : 0);
    });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring());
    config.sessionStateRoot = scratch.Path() / L"sessions";
    config.maximumRunDurationSeconds = 1;
    REQUIRE(runtime.Start(config));
    REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().state == pbapp::EncoderState::Stopped; }));
    runtime.Stop();
    REQUIRE(runtime.GetSnapshot().stoppedByTimeout);
    REQUIRE(runtime.GetSnapshot().submittedFrames == 0);
    REQUIRE(state->attempts == 0);
}

TEST_CASE("Explicit Encoder state roots isolate fresh Sessions without deleting either resume history",
    "[application][g15][runtime][resume][session-root-isolation]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"source.bin";
    WriteBytes(source, {});
    const std::array roots{scratch.Path() / L"first-state", scratch.Path() / L"second-state"};
    std::array<pbapp::EncoderSnapshot, 2> previous;
    for (std::size_t run = 0; run < 4; run++)
    {
        const std::size_t rootIndex = run % roots.size();
        auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 60);
        config.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
        config.sessionStateRoot = roots[rootIndex];
        auto state = std::make_shared<PresentationState>();
        state->maximumFrames = 1;
        pbapp::EncoderRuntime runtime([state](const pbrenderd3d::DataWindowConfig&)
        {
            return std::make_unique<MockPresentation>(state);
        });
        REQUIRE(runtime.Start(config));
        REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().submittedFrames == 1; }));
        runtime.Stop();
        const auto stopped = runtime.GetSnapshot();
        REQUIRE(stopped.state == pbapp::EncoderState::Stopped);
        REQUIRE(stopped.preparationComplete);
        REQUIRE(stopped.resumedSession == (run >= roots.size()));
        REQUIRE(std::filesystem::is_directory(roots[rootIndex] / stopped.sessionIdHex));
        if (run >= roots.size())
        {
            REQUIRE(stopped.sessionIdHex == previous[rootIndex].sessionIdHex);
            REQUIRE(state->frames.front().sequence >= previous[rootIndex].durableFrameSequenceLeaseEnd);
        }
        else if (run == 1)
        {
            REQUIRE(stopped.sessionIdHex != previous[0].sessionIdHex);
            REQUIRE(std::filesystem::is_directory(roots[0] / previous[0].sessionIdHex));
        }
        previous[rootIndex] = stopped;
    }
    REQUIRE(std::filesystem::file_size(source) == 0);
    REQUIRE(std::filesystem::is_directory(roots[0] / previous[0].sessionIdHex));
    REQUIRE(std::filesystem::is_directory(roots[1] / previous[1].sessionIdHex));
}

TEST_CASE("Unified prescan visits two fixed size Segments and reports automatic compression", "[application][g15][runtime][prescan]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"source.bin";
    const std::vector<std::byte> bytes(8 * 1024 * 1024 + 1, std::byte{0x35});
    WriteBytes(source, bytes);
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 60);
    config.sessionStateRoot = scratch.Path() / L"sessions";
    constexpr std::uint32_t expectedSegmentCount = 2;
    const std::uint32_t maximumControlSlots = pbmodulation::GetUnifiedMaximumControlSlots();
    const std::uint32_t controlRecordsPerSegment = 3 * config.controlRepetitions;
    const std::uint32_t controlFramesPerSegment =
        (controlRecordsPerSegment + maximumControlSlots - 1) / maximumControlSlots;
    auto state = std::make_shared<PresentationState>();
    state->maximumFrames = expectedSegmentCount * controlFramesPerSegment;
    pbapp::EncoderRuntime runtime([state](const pbrenderd3d::DataWindowConfig&)
    {
        return std::make_unique<MockPresentation>(state);
    });
    REQUIRE(runtime.Start(config));
    REQUIRE(WaitFor([&]()
    {
        return runtime.GetSnapshot().submittedFrames == state->maximumFrames;
    }));
    runtime.Stop();
    const auto stopped = runtime.GetSnapshot();
    REQUIRE(stopped.state == pbapp::EncoderState::Stopped);
    REQUIRE(stopped.preparedSegmentCount == expectedSegmentCount);
    REQUIRE(stopped.preparedSourceBytes == bytes.size());
    REQUIRE(stopped.zstdSegmentCount == 1);
    REQUIRE(stopped.rawSegmentCount == 1);
    // Two Segments fall under the barrier gate (graduation engages only
    // beyond two windows), so the short frame budget still wraps once.
    REQUIRE(stopped.cycleCount == 1);
    const auto output = scratch.Path() / L"output";
    REQUIRE(std::filesystem::create_directory(output));
    VerifyPublishedPixels(state->frames, bytes, output);
}

TEST_CASE("Gray segment target overrides are bounded and cannot change the certified product policy",
    "[application][grayfast-segmentation][model]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"validation-only.bin";
    WriteBytes(source, {});
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 30);
    for (const auto profile : {pbapp::VisualProfile::UnifiedGray, pbapp::VisualProfile::UnifiedGrayFast})
    {
        config.visualProfile = profile;
        for (const std::uint32_t target : {0U, 1024U * 1024U, 15U * 1024U * 1024U})
        {
            config.segmentTargetBytes = target;
            const auto status = pbapp::ValidateEncoderConfig(config);
            INFO(status.message);
            CHECK(status);
        }
        for (const std::uint32_t target : {1U, 1024U * 1024U - 1U, 15U * 1024U * 1024U + 1U})
        {
            config.segmentTargetBytes = target;
            CHECK_FALSE(pbapp::ValidateEncoderConfig(config));
        }
    }
    config.visualProfile = pbapp::VisualProfile::UnifiedLc4;
    config.segmentTargetBytes = 1024U * 1024U;
    CHECK_FALSE(pbapp::ValidateEncoderConfig(config));
    config.segmentTargetBytes = 0;
    CHECK(pbapp::ValidateEncoderConfig(config));
}

TEST_CASE("GrayFast segment override agrees with prepared descriptors and durable Session identity",
    "[application][grayfast-segmentation][runtime][prescan][resume]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"source.bin";
    const std::vector<std::byte> bytes(2U * 1024U * 1024U + 17U, std::byte{0x35});
    WriteBytes(source, bytes);
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 30);
    config.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
    config.sessionStateRoot = scratch.Path() / L"sessions";
    std::string previousSessionId;
    for (std::uint32_t run = 0; run < 3; run++)
    {
        config.segmentTargetBytes = (run == 2 ? 2U : 1U) * 1024U * 1024U;
        const std::uint64_t expectedSegments = run == 2 ? 2U : 3U;
        auto state = std::make_shared<PresentationState>();
        state->maximumFrames = 1;
        pbapp::EncoderRuntime runtime([state](const pbrenderd3d::DataWindowConfig&)
        {
            return std::make_unique<MockPresentation>(state);
        });
        REQUIRE(runtime.Start(config));
        REQUIRE(WaitFor([&]()
        {
            const auto snapshot = runtime.GetSnapshot();
            return snapshot.submittedFrames == 1 || snapshot.state == pbapp::EncoderState::Failed;
        }));
        runtime.Stop();
        const auto stopped = runtime.GetSnapshot();
        INFO(stopped.statusMessage);
        REQUIRE(stopped.state == pbapp::EncoderState::Stopped);
        REQUIRE(stopped.preparationComplete);
        REQUIRE(stopped.segmentCount == expectedSegments);
        REQUIRE(stopped.preparedSegmentCount == expectedSegments);
        REQUIRE(stopped.preparedSourceBytes == bytes.size());
        REQUIRE(stopped.resumedSession == (run == 1));
        if (run != 0)
        {
            REQUIRE((stopped.sessionIdHex == previousSessionId) == (run == 1));
        }
        previousSessionId = stopped.sessionIdHex;
        REQUIRE(state->frames.size() == 1);
        auto oracleResult = pbmodulation::UnifiedVisualCpuOracle::Create(pbmodulation::UnifiedVisualCpuOracle::RequiredBytes());
        REQUIRE(oracleResult);
        auto oracle = std::move(oracleResult).Value();
        pbmodulation::UnifiedExpectedFrameIdentity expected;
        expected.visualProfileId = pbprotocol::kGrayFastExperimentalProfile.visualProfileId;
        expected.visualLayoutVersion = pbprotocol::kGrayFastExperimentalProfile.visualLayoutVersion;
        const pbmodulation::LumaView view{state->frames.front().pixels, 1920, 1080, 1920 * 4, pbmodulation::LumaPixelFormat::Bgra8};
        const auto observation = oracle.DecodeMixedFrame(view, expected);
        REQUIRE(observation.IsFrameAvailable());
        auto controlsResult = pbprotocol::ControlPlaneReceiver::Create(pbprotocol::GetDefaultReceiverResourcePolicy());
        REQUIRE(controlsResult);
        auto controls = std::move(controlsResult).Value();
        for (const auto& block : oracle.GetAcceptedBlocks())
        {
            if (block.kind == pbmodulation::UnifiedSlotKind::Control)
            {
                REQUIRE(controls.ReceiveControlRecord(std::span(block.bytes).first(block.size)));
            }
        }
        const auto session = controls.GetSessionDescriptor(observation.bootstrapRecord.sessionTag);
        REQUIRE(session);
        REQUIRE(session.Value().sourceSegmentTargetBytes == config.segmentTargetBytes);
        REQUIRE(session.Value().segmentCount == expectedSegments);
    }
}

TEST_CASE("Unified source open failure creates neither Session nor presentation", "[application][g15][runtime][failure]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"source.bin";
    WriteBytes(source, {});
    const HANDLE exclusive = CreateFileW(source.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(exclusive != INVALID_HANDLE_VALUE);
    std::atomic<std::uint32_t> presentations = 0;
    pbapp::EncoderRuntime runtime([&](const pbrenderd3d::DataWindowConfig&) -> std::unique_ptr<pbapp::EncoderPresentation>
    {
        presentations++;
        return nullptr;
    });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring());
    config.sessionStateRoot = scratch.Path() / L"sessions";
    const auto start = runtime.Start(config);
    // Validation may reject the locked file synchronously; either way the
    // production presentation factory must remain unreachable.
    if (start)
    {
        REQUIRE(WaitFor([&]()
        {
            return runtime.GetSnapshot().state == pbapp::EncoderState::Failed;
        }));
    }
    runtime.Stop();
    REQUIRE(CloseHandle(exclusive));
    REQUIRE(presentations == 0);
    REQUIRE(runtime.GetSnapshot().sessionIdHex.empty());
    REQUIRE_FALSE(runtime.GetSnapshot().preparationComplete);
    REQUIRE_FALSE(std::filesystem::exists(config.sessionStateRoot));
}


TEST_CASE("Unified Control cadence follows monotonic time across rate changes and freezes pending plans",
    "[application][g15][scheduler]")
{
    constexpr std::uint64_t second = pbapp::senderLogicalFrameNanosecondsPerSecond;
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({64000, 4, 15, true}, scheduler));
    pbapp::SenderLogicalFrameClock clock;
    REQUIRE(pbapp::SenderLogicalFrameClock::Create(15, 0, clock));
    pbapp::SenderLogicalFrameTick tick;
    pbapp::SenderUnifiedScheduledFrame frame;
    REQUIRE(clock.Acquire(0, tick));
    REQUIRE(scheduler.PrepareFrameAt(tick.logicalTickOrdinal, 0, frame));
    REQUIRE(frame.controlSlotCount == pbmodulation::GetUnifiedMaximumControlSlots());
    REQUIRE(frame.firstEquationIndex == 0);
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(clock.Commit(0));
    REQUIRE(clock.RequestFramesPerSecond(1, second));
    REQUIRE(clock.Acquire(9 * second, tick));
    REQUIRE(tick.disposition == pbapp::SenderLogicalFrameTickDisposition::Ready);
    REQUIRE(scheduler.PrepareFrameAt(tick.logicalTickOrdinal, 9 * second, frame));
    REQUIRE(frame.controlSlotCount == 4);
    REQUIRE(frame.firstEquationIndex ==
        pbapp::senderUnifiedCodewordSlotCount - pbmodulation::GetUnifiedMaximumControlSlots());
    REQUIRE(clock.RequestFramesPerSecond(60, 9 * second));
    pbapp::SenderUnifiedScheduledFrame repeated;
    REQUIRE(scheduler.PrepareFrameAt(tick.logicalTickOrdinal, 25 * second, repeated));
    REQUIRE(repeated == frame);
    REQUIRE_FALSE(scheduler.PrepareFrame(tick.logicalTickOrdinal, repeated));
    REQUIRE_FALSE(scheduler.PrepareFrameAt(tick.logicalTickOrdinal + 1, 25 * second, repeated));
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(clock.Commit(9 * second));
    REQUIRE(clock.Acquire(10 * second, tick));
    REQUIRE(scheduler.PrepareFrameAt(tick.logicalTickOrdinal, 10 * second, frame));
    // Since bf52b24 ("balance pass-zero window budget") a periodic control
    // refresh carries one complete Session/Manifest/current-Segment triplet in
    // a single mixed frame (3 record kinds x senderUnifiedPeriodicControlRepetitions)
    // instead of re-filling the maximum slot count; every other non-empty frame
    // still carries its per-frame SegmentDescriptor prelude.
    REQUIRE(frame.controlSlotCount ==
        3 * pbapp::senderUnifiedPeriodicControlRepetitions);
    REQUIRE(frame.firstEquationIndex == 18);
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(clock.Commit(10 * second));
    REQUIRE_FALSE(scheduler.PrepareFrameAt(tick.logicalTickOrdinal + 1, 9 * second, frame));
    REQUIRE(scheduler.GetSnapshot().committedFrameCount == 3);
    // 15 codewords minus 8 (startup burst head), 4 (startup burst tail) and
    // 3 (single-triplet periodic refresh) control slots: 7 + 11 + 12.
    REQUIRE(scheduler.GetSnapshot().committedEquationCount == 30);
    REQUIRE(scheduler.GetSnapshot().controlBurstCount == 2);
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({64000, 4, 15, true}, scheduler));
    REQUIRE(scheduler.PrepareFrameAt(0, (std::numeric_limits<std::uint64_t>::max)() - 1, frame));
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(scheduler.PrepareFrameAt(1, (std::numeric_limits<std::uint64_t>::max)(), frame));
    const auto before = scheduler.GetSnapshot();
    REQUIRE_FALSE(scheduler.CommitPreparedFrame());
    REQUIRE(scheduler.GetSnapshot() == before);
}

namespace
{

[[nodiscard]] pbapp::EncoderSessionStoreCreateConfig StoreConfig(const std::filesystem::path& root)
{
    pbapp::EncoderSessionStoreCreateConfig config;
    config.rootDirectory = root;
    config.sourceIdentity.volumeSerialNumber = 7;
    config.sourceIdentity.fileId.fill(std::byte{0x15});
    config.sourceIdentity.fileBytes = 1;
    config.sourceIdentity.lastWriteTime = 100;
    config.sourcePathUtf8 = "fixture.bin";
    config.sessionId.bytes.fill(std::byte{0xab});
    config.buildIdentity = "g15-build";
    config.compressionIdentity = "g15-compression";
    config.outerFecIdentity = "g15-fec";
    config.segmentCount = 1;
    config.descriptorBundle = {std::byte{1}, std::byte{2}};
    return config;
}

constexpr const char* firstSessionId = "abababababababababababababababab";
constexpr const char* secondSessionId = "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";

} // namespace

TEST_CASE("Explicit Encoder deletion refuses live owners and foreign files and preserves a newer source index",
    "[application][g15][session-store]")
{
    Scratch scratch;
    auto config = StoreConfig(scratch.Path() / L"sessions");
    std::unique_ptr<pbapp::EncoderSessionStore> store;
    REQUIRE(pbapp::EncoderSessionStore::Create(config, store));
    const auto firstDirectory = store->GetSessionDirectory();
    REQUIRE_FALSE(pbapp::EncoderSessionStore::EndAndDelete(config.rootDirectory, firstSessionId));
    bool found = false;
    std::unique_ptr<pbapp::EncoderSessionStore> secondOwner;
    REQUIRE_FALSE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        config.buildIdentity, config.compressionIdentity, config.outerFecIdentity, secondOwner, found));
    REQUIRE_FALSE(found);
    store.reset();
    const auto sentinel = firstDirectory / L"user-owned.txt";
    WriteBytes(sentinel, {});
    REQUIRE_FALSE(pbapp::EncoderSessionStore::EndAndDelete(config.rootDirectory, firstSessionId));
    REQUIRE(std::filesystem::exists(sentinel));
    REQUIRE(std::filesystem::exists(firstDirectory / L"descriptors.bin"));
    REQUIRE_FALSE(std::filesystem::exists(firstDirectory / L"ended.descriptors"));
    REQUIRE(std::filesystem::remove(sentinel));
    REQUIRE_FALSE(pbapp::EncoderSessionStore::EndAndDelete(config.rootDirectory, "../outside"));
    config.sessionId.bytes.fill(std::byte{0xcd});
    REQUIRE(pbapp::EncoderSessionStore::Create(config, store));
    store.reset();
    REQUIRE(pbapp::EncoderSessionStore::EndAndDelete(config.rootDirectory, firstSessionId));
    REQUIRE_FALSE(std::filesystem::exists(firstDirectory));
    REQUIRE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        config.buildIdentity, config.compressionIdentity, config.outerFecIdentity, store, found));
    REQUIRE(found);
    REQUIRE(store->GetSessionId() == config.sessionId);
    store.reset();
    REQUIRE(pbapp::EncoderSessionStore::EndAndDelete(config.rootDirectory, secondSessionId));
    REQUIRE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        config.buildIdentity, config.compressionIdentity, config.outerFecIdentity, store, found));
    REQUIRE_FALSE(found);
}

TEST_CASE("An interrupted explicit Encoder deletion cannot resume and can finish from its durable marker",
    "[application][g15][session-store]")
{
    Scratch scratch;
    const auto config = StoreConfig(scratch.Path() / L"sessions");
    std::unique_ptr<pbapp::EncoderSessionStore> store;
    REQUIRE(pbapp::EncoderSessionStore::Create(config, store));
    const auto directory = store->GetSessionDirectory();
    store.reset();
    REQUIRE(std::filesystem::copy_file(directory / L"descriptors.bin", directory / L"ended.descriptors"));
    REQUIRE(std::filesystem::remove(directory / L"runtime.state"));
    REQUIRE(std::filesystem::remove(directory / L"descriptors.bin"));
    bool found = true;
    REQUIRE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        config.buildIdentity, config.compressionIdentity, config.outerFecIdentity, store, found));
    REQUIRE_FALSE(found);
    REQUIRE(pbapp::EncoderSessionStore::EndAndDelete(config.rootDirectory, firstSessionId));
    REQUIRE_FALSE(std::filesystem::exists(directory));
}

TEST_CASE("Encoder deletion fails closed on a malformed source index without touching source or state",
    "[application][g15][session-store]")
{
    Scratch scratch;
    const auto config = StoreConfig(scratch.Path() / L"sessions");
    std::unique_ptr<pbapp::EncoderSessionStore> store;
    REQUIRE(pbapp::EncoderSessionStore::Create(config, store));
    const auto directory = store->GetSessionDirectory();
    store.reset();
    for (const auto& entry : std::filesystem::directory_iterator(config.rootDirectory / L"SourceIndex"))
    {
        if (entry.path().extension() == L".txt")
        {
            WriteBytes(entry.path(), {});
        }
    }
    REQUIRE_FALSE(pbapp::EncoderSessionStore::EndAndDelete(config.rootDirectory, firstSessionId));
    REQUIRE(std::filesystem::exists(directory / L"descriptors.bin"));
    REQUIRE(std::filesystem::exists(directory / L"runtime.state"));
    REQUIRE_FALSE(std::filesystem::exists(directory / L"ended.descriptors"));
}


TEST_CASE("A failed Encoder source index publication cleans the new Session and preserves the old Session",
    "[application][g15][session-store]")
{
    Scratch scratch;
    auto config = StoreConfig(scratch.Path() / L"sessions");
    std::unique_ptr<pbapp::EncoderSessionStore> store;
    REQUIRE(pbapp::EncoderSessionStore::Create(config, store));
    store.reset();
    std::filesystem::path indexPath;
    for (const auto& entry : std::filesystem::directory_iterator(config.rootDirectory / L"SourceIndex"))
    {
        if (entry.path().extension() == L".txt")
        {
            indexPath = entry.path();
        }
    }
    REQUIRE_FALSE(indexPath.empty());
    const HANDLE readLease = CreateFileW(indexPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(readLease != INVALID_HANDLE_VALUE);
    config.sessionId.bytes.fill(std::byte{0xcd});
    const auto created = pbapp::EncoderSessionStore::Create(config, store);
    REQUIRE(CloseHandle(readLease));
    REQUIRE_FALSE(created);
    REQUIRE_FALSE(store);
    REQUIRE_FALSE(std::filesystem::exists(config.rootDirectory / secondSessionId));
    REQUIRE(std::filesystem::exists(config.rootDirectory / firstSessionId / L"descriptors.bin"));
    bool found = false;
    REQUIRE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        config.buildIdentity, config.compressionIdentity, config.outerFecIdentity, store, found));
    REQUIRE(found);
    REQUIRE(store->GetSessionDirectory().filename() == firstSessionId);
}

TEST_CASE("A corrupt Encoder end marker is rejected instead of authorizing a new Session",
    "[application][g15][session-store]")
{
    Scratch scratch;
    const auto config = StoreConfig(scratch.Path() / L"sessions");
    std::unique_ptr<pbapp::EncoderSessionStore> store;
    REQUIRE(pbapp::EncoderSessionStore::Create(config, store));
    const auto directory = store->GetSessionDirectory();
    store.reset();
    WriteBytes(directory / L"ended.descriptors", {});
    bool found = true;
    REQUIRE_FALSE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        config.buildIdentity, config.compressionIdentity, config.outerFecIdentity, store, found));
    REQUIRE_FALSE(found);
    REQUIRE_FALSE(pbapp::EncoderSessionStore::EndAndDelete(config.rootDirectory, firstSessionId));
    REQUIRE(std::filesystem::exists(directory / L"descriptors.bin"));
}

TEST_CASE("GrayFast spatial interleaving is opt-in and refuses other profiles", "[application][grayfast-spatial][model]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"validation.bin";
    WriteBytes(source, {});
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 30);
    REQUIRE_FALSE(config.grayFastSpatialInterleave);
    config.grayFastSpatialInterleave = true;
    for (const auto profile : {pbapp::VisualProfile::UnifiedLc4, pbapp::VisualProfile::UnifiedGray, pbapp::VisualProfile::RemoteVisualLowFps})
    {
        config.visualProfile = profile;
        REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    }
    config.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
    const auto status = pbapp::ValidateEncoderConfig(config);
    INFO(status.message);
    REQUIRE(status);
    config.measurement = std::make_shared<pbapp::RunMeasurementRecorder>();
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
}

TEST_CASE("GrayFast spatial rows recover short windows and many-window erasures without widening receiver quotas",
    "[application][grayfast-spatial][receiver][publish]")
{
    Scratch scratch;
    for (const auto& [segmentCount, erasureModel] : std::array{
        std::pair{1U, pbapp::UnifiedRecoveryErasureModel::None},
        std::pair{5U, pbapp::UnifiedRecoveryErasureModel::None},
        std::pair{7U, pbapp::UnifiedRecoveryErasureModel::PeriodicThird},
        std::pair{13U, pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter},
        std::pair{64U, pbapp::UnifiedRecoveryErasureModel::SparseBursty}})
    {
        CAPTURE(segmentCount, static_cast<unsigned int>(erasureModel));
        const auto output = scratch.Path() / std::to_wstring(segmentCount);
        REQUIRE(std::filesystem::create_directory(output));
        pbapp::UnifiedGraduationRecoveryProbeConfig config;
        config.segmentCount = segmentCount;
        config.erasureModel = erasureModel;
        config.grayFastSpatialInterleave = true;
        pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
        const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(output.wstring(), config, probe);
        INFO(status.message);
        REQUIRE(status);
        REQUIRE(probe.decoder.state == pbapp::DecoderState::Completed);
        REQUIRE(probe.decoder.verifiedRawBytes == static_cast<std::uint64_t>(segmentCount) * config.segmentBytes);
        REQUIRE(probe.decoder.wholeFileDigestCheck == true);
        REQUIRE(probe.decoder.finalRenameSucceeded == true);
        REQUIRE(probe.decoder.finalReopenVerified == true);
        REQUIRE(probe.decoder.outerConflictRejections == 0);
        REQUIRE(probe.deferredResourceBusyCount == 0);
        REQUIRE(probe.longestNoUsefulEquationFrames < 900);
        REQUIRE(probe.peakActiveDecoders <= pbapp::senderUnifiedReceiverActiveDecoderLimit);
        REQUIRE(probe.peakReservedDecoderBytes <= pbapp::MakeUnifiedReceiverResourcePolicy().maxTotalOuterFecDecoderBytes);
        REQUIRE(probe.deferredResourceBusyCount == 0);
        REQUIRE(probe.longestNoUsefulEquationFrames < 900);
        REQUIRE(probe.spatialCommitAndLeaseVerified);
        REQUIRE(probe.spatialBankStorageBytes > 0);
        REQUIRE(probe.spatialBankStorageBytes <= 256U * 1024U);
        std::ifstream file(probe.publishedPath, std::ios::binary);
        REQUIRE(file);
        std::vector<std::byte> reopened(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
        file.read(reinterpret_cast<char*>(reopened.data()), static_cast<std::streamsize>(reopened.size()));
        REQUIRE(file.gcount() == static_cast<std::streamsize>(reopened.size()));
        REQUIRE(file.peek() == std::char_traits<char>::eof());
        REQUIRE(pbprotocol::ComputeBlake3Digest(reopened) == probe.expectedWholeFileDigest);
        std::cout << "{\"probe\":\"GrayFastSpatialRecovery\",\"syntheticNoRaster\":true,\"segments\":" << segmentCount
            << ",\"frames\":" << probe.senderLogicalFrames << ",\"observed\":" << probe.observedLogicalFrames
            << ",\"wraps\":" << probe.completedCarouselPasses << ",\"peakActive\":" << probe.peakActiveDecoders
            << ",\"quotaRejections\":" << probe.deferredResourceBusyCount << ",\"longestDrought\":" << probe.longestNoUsefulEquationFrames
            << ",\"bankBytes\":" << probe.spatialBankStorageBytes << "}\n";
    }
}

TEST_CASE("GrayFast spatial pixels preserve partial-bank restart and recover compressed raw and empty files",
    "[application][grayfast-spatial][runtime][pixels][resume][publish]")
{
    for (const bool empty : {false, true})
    {
        CAPTURE(empty);
        Scratch scratch;
        const auto source = scratch.Path() / L"spatial-source.bin";
        std::vector<std::byte> original(empty ? 0 : 6U * 1024U * 1024U + 2048U, std::byte{0x59});
        std::uint32_t randomState = 0x713B58D1U;
        if (!empty)
        {
            for (std::size_t index = 6U * 1024U * 1024U; index < original.size(); index++)
            {
                randomState ^= randomState << 13U;
                randomState ^= randomState >> 17U;
                randomState ^= randomState << 5U;
                original[index] = static_cast<std::byte>(randomState & 0xFFU);
            }
        }
        WriteBytes(source, original);
        auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 30);
        config.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
        config.grayFastSpatialInterleave = true;
        config.segmentTargetBytes = 1024U * 1024U;
        config.sessionStateRoot = scratch.Path() / L"sessions";
        pbapp::EncoderSnapshot previous;
        std::shared_ptr<PresentationState> completedFrames;
        for (std::uint32_t run = 0; run < 2; run++)
        {
            auto state = std::make_shared<PresentationState>();
            state->rejectFirst = true;
            state->maximumFrames = run == 0 ? 2 : empty ? 2 : 24;
            pbapp::EncoderRuntime runtime([state](const pbrenderd3d::DataWindowConfig& window)
            {
                if (window.repeatActiveFrame)
                {
                    throw std::runtime_error("Spatial GrayFast unexpectedly enabled repeat Present");
                }
                return std::make_unique<MockPresentation>(state);
            });
            REQUIRE(runtime.Start(config));
            REQUIRE(WaitFor([&]()
            {
                const std::scoped_lock lock(state->mutex);
                return state->attempts == 1 || runtime.GetSnapshot().state == pbapp::EncoderState::Failed;
            }));
            REQUIRE(runtime.GetSnapshot().state != pbapp::EncoderState::Failed);
            REQUIRE(runtime.GetSnapshot().submittedFrames == 0);
            {
                const std::scoped_lock lock(state->mutex);
                state->paused = false;
            }
            REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().submittedFrames == state->maximumFrames; }));
            runtime.Stop();
            const auto stopped = runtime.GetSnapshot();
            INFO(stopped.errorDetail);
            REQUIRE(stopped.state == pbapp::EncoderState::Stopped);
            REQUIRE(stopped.sourceStable);
            REQUIRE(state->retryIdentical);
            REQUIRE(stopped.segmentCount == (empty ? 0 : 7));
            REQUIRE(stopped.rawSegmentCount == (empty ? 0 : 1));
            if (run != 0)
            {
                REQUIRE(stopped.resumedSession);
                REQUIRE(stopped.sessionIdHex == previous.sessionIdHex);
                REQUIRE(state->frames.front().sequence >= previous.durableFrameSequenceLeaseEnd);
            }
            for (std::size_t index = 1; index < state->frames.size(); index++)
            {
                REQUIRE(state->frames[index].sequence == state->frames[index - 1].sequence + 1);
            }
            previous = stopped;
            completedFrames = state;
        }
        const auto outputDirectory = scratch.Path() / L"decoded";
        REQUIRE(std::filesystem::create_directory(outputDirectory));
        auto receiverState = std::make_shared<g16test::ReceiveState>();
        pbapp::DecoderRuntime decoder(g16test::Services(receiverState));
        auto decoderConfig = pbapp::MakeUnifiedDecoderConfig(outputDirectory.wstring(), g16test::Region());
        decoderConfig.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
        REQUIRE(decoder.Start(decoderConfig));
        auto oracleResult = pbmodulation::UnifiedVisualCpuOracle::Create(pbmodulation::UnifiedVisualCpuOracle::RequiredBytes());
        REQUIRE(oracleResult);
        auto oracle = std::move(oracleResult).Value();
        pbmodulation::UnifiedExpectedFrameIdentity expected;
        expected.visualProfileId = pbprotocol::kGrayFastExperimentalProfile.visualProfileId;
        expected.visualLayoutVersion = pbprotocol::kGrayFastExperimentalProfile.visualLayoutVersion;
        std::uint32_t mixedSegmentFrames = 0;
        for (const auto& frame : completedFrames->frames)
        {
            const pbmodulation::LumaView view{frame.pixels, 1920U, 1080U, 1920U * 4U, pbmodulation::LumaPixelFormat::Bgra8};
            const auto observation = oracle.DecodeMixedFrame(view, expected);
            REQUIRE(observation.IsFrameAvailable());
            REQUIRE(observation.bootstrapRecord.frameSequence == frame.sequence);
            std::set<std::uint64_t> segmentsInFrame;
            for (const auto& block : oracle.GetAcceptedBlocks())
            {
                if (block.kind == pbmodulation::UnifiedSlotKind::Transport)
                {
                    const auto parsed = pbprotocol::ParseTransportBlock(std::span(block.bytes).first(block.size));
                    REQUIRE(parsed);
                    segmentsInFrame.insert(parsed.Value().header.segmentOrdinal);
                }
            }
            mixedSegmentFrames += segmentsInFrame.size() > 1 ? 1U : 0U;
            pbdemodd3d11::CaptureDemodulatorResult result;
            result.kind = pbdemodd3d11::CaptureDemodulatorResultKind::UnifiedFrame;
            result.geometryStatus = pbdemodd3d11::CaptureDemodulatorGeometryStatus::ExactCanvas;
            result.bootstrap = observation.bootstrap;
            REQUIRE(pbprotocol::SerializeBootstrapRecord(observation.bootstrapRecord, result.bootstrapRecord));
            result.demodulation.visualProfileId = observation.bootstrapRecord.visualProfileId;
            result.demodulation.unifiedObservation = observation;
            const auto blocks = oracle.GetAcceptedBlocks();
            result.demodulation.acceptedUnifiedBlockCount = static_cast<std::uint32_t>(blocks.size());
            std::copy(blocks.begin(), blocks.end(), result.demodulation.acceptedUnifiedBlocks.begin());
            receiverState->Push(result);
            REQUIRE(WaitFor([&]()
            {
                const std::scoped_lock lock(receiverState->mutex);
                return receiverState->frames.empty() || decoder.GetSnapshot().state == pbapp::DecoderState::Completed || decoder.GetSnapshot().state == pbapp::DecoderState::Failed;
            }));
            if (decoder.GetSnapshot().state == pbapp::DecoderState::Completed || decoder.GetSnapshot().state == pbapp::DecoderState::Failed)
            {
                break;
            }
        }
        REQUIRE(WaitFor([&]() { return decoder.GetSnapshot().state == pbapp::DecoderState::Completed || decoder.GetSnapshot().state == pbapp::DecoderState::Failed; }));
        decoder.Stop();
        const auto decoded = decoder.GetSnapshot();
        INFO(decoded.errorDetail);
        REQUIRE(decoded.state == pbapp::DecoderState::Completed);
        REQUIRE(decoded.wholeFileDigestCheck == true);
        REQUIRE(decoded.finalRenameSucceeded == true);
        REQUIRE(decoded.finalReopenVerified == true);
        REQUIRE(decoded.outerConflictRejections == 0);
        REQUIRE(g16test::VerifyOutput(decoded, original));
        REQUIRE((mixedSegmentFrames != 0) == !empty);
        std::cout << "{\"probe\":\"GrayFastSpatialActualPixels\",\"empty\":" << empty
            << ",\"mixedSegmentFrames\":" << mixedSegmentFrames << ",\"rawBytes\":" << decoded.verifiedRawBytes
            << ",\"reopenVerified\":true}\n";
    }
}

TEST_CASE("GrayFast spatial large-K files recover periodic bursty loss and repair-only late join",
    "[application][grayfast-spatial-large][receiver][publish][late-join]")
{
    Scratch scratch;
    for (const auto& [erasureModel, firstObservedFrame] : std::array{
        std::pair{pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter, 0ULL},
        std::pair{pbapp::UnifiedRecoveryErasureModel::SparseBursty, 0ULL},
        std::pair{pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter, 15000ULL}})
    {
        DYNAMIC_SECTION("Loss model " << static_cast<unsigned int>(erasureModel) << " join frame " << firstObservedFrame)
        {
            CAPTURE(static_cast<unsigned int>(erasureModel), firstObservedFrame);
            const auto output = scratch.Path() / (std::to_wstring(static_cast<unsigned int>(erasureModel)) + L"-" + std::to_wstring(firstObservedFrame));
            REQUIRE(std::filesystem::create_directory(output));
            pbapp::UnifiedGraduationRecoveryProbeConfig config;
            config.segmentCount = 13;
            config.segmentBytes = 6U * 1024U * 1024U;
            config.grayFastSpatialInterleave = true;
            config.erasureModel = erasureModel;
            config.firstObservedLogicalFrame = firstObservedFrame;
            config.maximumSenderLogicalFrames = firstObservedFrame == 0 ? 20000 : 60000;
            config.collectSegmentTrace = true;
            pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
            const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(output.wstring(), config, probe);
            std::cout << "{\"probe\":\"GrayFastSpatialLargeK\",\"syntheticNoRaster\":true,\"lossModel\":" << static_cast<unsigned int>(erasureModel)
                << ",\"joinFrame\":" << firstObservedFrame << ",\"firstObservedPass\":" << probe.firstObservedCarouselPass
                << ",\"success\":" << static_cast<bool>(status) << ",\"frames\":" << probe.senderLogicalFrames
                << ",\"observedFrames\":" << probe.observedLogicalFrames << ",\"wraps\":" << probe.completedCarouselPasses
                << ",\"verifiedRawBytes\":" << probe.decoder.verifiedRawBytes << ",\"uniqueSymbols\":" << probe.decoder.outerUniqueSymbols
                << ",\"peakActive\":" << probe.peakActiveDecoders << ",\"quotaRejections\":" << probe.deferredResourceBusyCount
                << ",\"resourceRejections\":" << probe.decoder.outerResourceRejections
                << ",\"orphanQuotaDrops\":" << probe.decoder.outerOrphanDroppedByQuotaCount
                << ",\"afterJoinDrought\":" << probe.longestAfterJoinNoUsefulEquationFrames
                << ",\"activeAtDrought\":" << probe.activeDecodersAtLongestDrought
                << ",\"verifiedAtDrought\":" << probe.verifiedRawBytesAtLongestDrought << "}\n";
            for (std::size_t ordinal = 0; ordinal < probe.segmentTrace.size(); ordinal++)
            {
                const auto& trace = probe.segmentTrace[ordinal];
                const auto PrintFrame = [](const std::optional<std::uint64_t>& frame)
                {
                    if (frame)
                    {
                        std::cout << *frame;
                    }
                    else
                    {
                        std::cout << "null";
                    }
                };
                std::cout << "{\"probe\":\"GrayFastSegmentTrace\",\"lossModel\":" << static_cast<unsigned int>(erasureModel)
                    << ",\"joinFrame\":" << firstObservedFrame << ",\"segment\":" << ordinal << ",\"blockCount\":" << trace.blockCount
                    << ",\"firstObservedFrame\":";
                PrintFrame(trace.firstObservedFrame);
                std::cout << ",\"firstUniqueFrame\":";
                PrintFrame(trace.firstUniqueFrame);
                std::cout << ",\"firstBoundUniqueFrame\":";
                PrintFrame(trace.firstBoundUniqueFrame);
                std::cout << ",\"completedFrame\":";
                PrintFrame(trace.completedFrame);
                std::cout << ",\"observedDataFrames\":" << trace.observedDataFrames << ",\"maximumObservedRevisitGap\":" << trace.longestObservedFrameGap
                    << ",\"directUniqueEvents\":" << trace.uniqueAdmissionEvents << ",\"deferredBlocks\":" << trace.deferredBlocks
                    << ",\"orphanUniqueEvents\":" << trace.orphanUniqueAdmissionEvents
                    << ",\"resourceRejections\":" << trace.resourceRejections << ",\"orphanQuotaDrops\":" << trace.orphanQuotaDrops
                    << ",\"alreadyCompletedBlocks\":" << trace.alreadyCompletedBlocks << "}\n";
            }
            INFO(status.message);
            REQUIRE(status);
            REQUIRE(probe.decoder.state == pbapp::DecoderState::Completed);
            REQUIRE(probe.decoder.verifiedRawBytes == 13ULL * 6ULL * 1024ULL * 1024ULL);
            REQUIRE(probe.decoder.wholeFileDigestCheck == true);
            REQUIRE(probe.decoder.finalRenameSucceeded == true);
            REQUIRE(probe.decoder.finalReopenVerified == true);
            REQUIRE(probe.decoder.outerConflictRejections == 0);
            REQUIRE(probe.spatialCommitAndLeaseVerified);
            REQUIRE(probe.peakActiveDecoders <= pbapp::senderUnifiedReceiverActiveDecoderLimit);
            REQUIRE(probe.peakReservedDecoderBytes <= pbapp::MakeUnifiedReceiverResourcePolicy().maxTotalOuterFecDecoderBytes);
            REQUIRE(probe.segmentTrace.size() == config.segmentCount);
            for (const auto& trace : probe.segmentTrace)
            {
                REQUIRE(trace.blockCount > 0);
                REQUIRE(trace.firstObservedFrame);
                REQUIRE(trace.firstUniqueFrame);
                REQUIRE(trace.completedFrame);
                REQUIRE(*trace.firstObservedFrame <= *trace.firstUniqueFrame);
                REQUIRE(*trace.firstUniqueFrame <= *trace.completedFrame);
                REQUIRE(trace.uniqueAdmissionEvents > 0);
            }
            if (firstObservedFrame == 0)
            {
                REQUIRE(probe.deferredResourceBusyCount == 0);
                REQUIRE(probe.decoder.outerResourceRejections == 0);
                REQUIRE(probe.decoder.outerOrphanDroppedByQuotaCount == 0);
                REQUIRE(probe.completedCarouselPasses == 0);
                REQUIRE(probe.longestAfterJoinNoUsefulEquationFrames < 900);
            }
            else
            {
                REQUIRE(probe.firstObservedCarouselPass > 0);
                REQUIRE(probe.longestAfterJoinNoUsefulEquationFrames < 3000);
                // Bound the post-join wait as well as short progress droughts.
                // The prior MicroRepair-only path needed 23481 frames here.
                REQUIRE(probe.senderLogicalFrames - firstObservedFrame < 12000);
                REQUIRE(probe.completedCarouselPasses <= 8);
            }
            std::ifstream file(probe.publishedPath, std::ios::binary);
            REQUIRE(file);
            std::vector<std::byte> reopened(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
            file.read(reinterpret_cast<char*>(reopened.data()), static_cast<std::streamsize>(reopened.size()));
            REQUIRE(file.gcount() == static_cast<std::streamsize>(reopened.size()));
            REQUIRE(file.peek() == std::char_traits<char>::eof());
            REQUIRE(pbprotocol::ComputeBlake3Digest(reopened) == probe.expectedWholeFileDigest);
        }
    }
}

TEST_CASE("Experimental budget-bound decoder policy changes only the finite count ceiling",
    "[application][budgeted-decoder-policy][quota]")
{
    const auto normal = pbapp::MakeUnifiedReceiverResourcePolicy();
    auto experimental = pbapp::MakeBudgetBoundUnifiedReceiverResourcePolicy();
    REQUIRE(normal.maxActiveOuterFecDecoders == 8);
    REQUIRE(experimental.maxActiveOuterFecDecoders == normal.maxSegmentCount);
    REQUIRE(experimental.maxActiveOuterFecDecoders > normal.maxActiveOuterFecDecoders);
    REQUIRE(pbprotocol::ValidateReceiverResourcePolicy(experimental));
    experimental.maxActiveOuterFecDecoders = normal.maxActiveOuterFecDecoders;
    // Compare every policy field rather than a subset of the safety budgets.
    REQUIRE(experimental.maxAcceptedFileBytes == normal.maxAcceptedFileBytes);
    REQUIRE(experimental.maxSegmentCount == normal.maxSegmentCount);
    REQUIRE(experimental.maxRawSegmentBytes == normal.maxRawSegmentBytes);
    REQUIRE(experimental.maxEncodedSegmentBytes == normal.maxEncodedSegmentBytes);
    REQUIRE(experimental.maxOuterBlockBytes == normal.maxOuterBlockBytes);
    REQUIRE(experimental.maxDescriptorStateBytes == normal.maxDescriptorStateBytes);
    REQUIRE(experimental.maxConcurrentSessions == normal.maxConcurrentSessions);
    REQUIRE(experimental.maxTotalDescriptorStateBytes == normal.maxTotalDescriptorStateBytes);
    REQUIRE(experimental.maxDirectRepeatBlockCount == normal.maxDirectRepeatBlockCount);
    REQUIRE(experimental.maxOuterFecDecoderBytes == normal.maxOuterFecDecoderBytes);
    REQUIRE(experimental.maxTotalOuterFecDecoderBytes == normal.maxTotalOuterFecDecoderBytes);
    REQUIRE(experimental.maxControlRecordBytes == normal.maxControlRecordBytes);
    REQUIRE(experimental.maxConcurrentControlReassemblies == normal.maxConcurrentControlReassemblies);
    REQUIRE(experimental.maxControlReassemblyBytes == normal.maxControlReassemblyBytes);
    REQUIRE(experimental.maxControlFragmentsPerRecord == normal.maxControlFragmentsPerRecord);
    REQUIRE(experimental.maxControlReassemblyInactivityObservations == normal.maxControlReassemblyInactivityObservations);
    REQUIRE(experimental.maxOrphanTransportBytes == normal.maxOrphanTransportBytes);
    REQUIRE(experimental.maxOrphanTransportBlocks == normal.maxOrphanTransportBlocks);
    REQUIRE(experimental.maxZstdWindowBytes == normal.maxZstdWindowBytes);
    REQUIRE(experimental.maxResumeBytes == normal.maxResumeBytes);
    REQUIRE(experimental.maxOutputPreallocationBytesWithoutPrompt == normal.maxOutputPreallocationBytesWithoutPrompt);
}

TEST_CASE("Short initial airtime is explicit spatial GrayFast tuning and not formal measurement",
    "[application][grayfast-initial-airtime-bounds][model]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"validation.bin";
    WriteBytes(source, {});
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 30);
    REQUIRE_FALSE(config.grayFastShortInitialAirtime);
    config.grayFastShortInitialAirtime = true;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.grayFastSpatialInterleave = true;
    for (const auto profile : {pbapp::VisualProfile::UnifiedLc4, pbapp::VisualProfile::UnifiedGray, pbapp::VisualProfile::RemoteVisualLowFps})
    {
        config.visualProfile = profile;
        REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    }
    config.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
    REQUIRE(pbapp::ValidateEncoderConfig(config));
    config.measurement = std::make_shared<pbapp::RunMeasurementRecorder>();
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
}

TEST_CASE("Initial airtime experiment rejects invalid bounds before creating recovery state",
    "[application][grayfast-initial-airtime-bounds]")
{
    Scratch scratch;
    for (const std::uint32_t percent : {0U, 49U, 101U, 0xffffffffU})
    {
        pbapp::UnifiedGraduationRecoveryProbeConfig config;
        config.grayFastSpatialInterleave = true;
        config.initialAirtimePercent = percent;
        pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
        REQUIRE_FALSE(pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(scratch.Path().wstring(), config, probe));
        REQUIRE(std::filesystem::is_empty(scratch.Path()));
    }
    pbapp::UnifiedGraduationRecoveryProbeConfig config;
    REQUIRE(config.initialAirtimePercent == 100);
    config.initialAirtimePercent = 65;
    pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
    REQUIRE_FALSE(pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(scratch.Path().wstring(), config, probe));
    REQUIRE(std::filesystem::is_empty(scratch.Path()));
}

TEST_CASE("Extended visits are explicit spatial GrayFast tuning and exclude confounded or formal modes",
    "[application][grayfast-extended-visits][model]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"validation.bin";
    WriteBytes(source, {});
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 30);
    REQUIRE_FALSE(config.grayFastExtendedVisitBudget);
    config.grayFastExtendedVisitBudget = true;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.grayFastSpatialInterleave = true;
    for (const auto profile : {pbapp::VisualProfile::UnifiedLc4, pbapp::VisualProfile::UnifiedGray, pbapp::VisualProfile::RemoteVisualLowFps})
    {
        config.visualProfile = profile;
        REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    }
    config.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
    REQUIRE(pbapp::ValidateEncoderConfig(config));
    config.grayFastShortInitialAirtime = true;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.grayFastShortInitialAirtime = false;
    config.measurement = std::make_shared<pbapp::RunMeasurementRecorder>();
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
}

TEST_CASE("Extended visit configuration reaches runtime without changing the initial-percent identity",
    "[application][grayfast-extended-visits][runtime]")
{
    const bool extended = GENERATE(false, true);
    Scratch scratch;
    const auto source = scratch.Path() / L"startup.bin";
    WriteBytes(source, {});
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 30);
    config.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
    config.grayFastSpatialInterleave = true;
    config.grayFastExtendedVisitBudget = extended;
    config.sessionStateRoot = scratch.Path() / L"sessions";
    auto state = std::make_shared<PresentationState>();
    state->maximumFrames = 1;
    pbapp::EncoderRuntime runtime([state](const pbrenderd3d::DataWindowConfig&)
    {
        return std::make_unique<MockPresentation>(state);
    });
    REQUIRE(runtime.Start(config));
    REQUIRE(WaitFor([&]()
    {
        const auto snapshot = runtime.GetSnapshot();
        return snapshot.submittedFrames == 1 || snapshot.state == pbapp::EncoderState::Failed;
    }));
    const auto started = runtime.GetSnapshot();
    runtime.Stop();
    REQUIRE(started.submittedFrames == 1);
    REQUIRE(started.grayFastSpatialInterleave);
    REQUIRE(started.configuredInitialAirtimePercent == 100);
    REQUIRE(started.configuredVisitBudgetPercent == (extended ? 150U : 100U));
    REQUIRE(runtime.GetSnapshot().state == pbapp::EncoderState::Stopped);
}

TEST_CASE("Short initial airtime reaches runtime startup with explicit snapshot identity",
    "[application][grayfast-initial-airtime-bounds][runtime]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"startup.bin";
    WriteBytes(source, {});
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 30);
    config.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
    config.grayFastSpatialInterleave = true;
    config.grayFastShortInitialAirtime = true;
    config.sessionStateRoot = scratch.Path() / L"sessions";
    auto state = std::make_shared<PresentationState>();
    state->maximumFrames = 1;
    pbapp::EncoderRuntime runtime([state](const pbrenderd3d::DataWindowConfig&)
    {
        return std::make_unique<MockPresentation>(state);
    });
    REQUIRE(runtime.Start(config));
    REQUIRE(WaitFor([&]()
    {
        const auto snapshot = runtime.GetSnapshot();
        return snapshot.submittedFrames == 1 || snapshot.state == pbapp::EncoderState::Failed;
    }));
    const auto started = runtime.GetSnapshot();
    runtime.Stop();
    REQUIRE(started.submittedFrames == 1);
    REQUIRE(started.grayFastSpatialInterleave);
    REQUIRE(started.configuredInitialAirtimePercent == 65);
    REQUIRE(runtime.GetSnapshot().state == pbapp::EncoderState::Stopped);
}

TEST_CASE("Short initial visits still recover after a stricter eight-decoder resource ceiling",
    "[application][grayfast-short-budget-pressure][receiver][publish]")
{
    Scratch scratch;
    for (const auto erasureModel : {pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter, pbapp::UnifiedRecoveryErasureModel::PeriodicFifth})
    {
        const auto output = scratch.Path() / std::to_wstring(static_cast<unsigned int>(erasureModel));
        REQUIRE(std::filesystem::create_directory(output));
        pbapp::UnifiedGraduationRecoveryProbeConfig config;
        config.segmentCount = 13;
        config.segmentBytes = 6U * 1024U * 1024U;
        config.grayFastSpatialInterleave = true;
        config.erasureModel = erasureModel;
        config.maximumSenderLogicalFrames = 60000;
        config.initialAirtimePercent = 65;
        pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
        const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(output.wstring(), config, probe);
        std::cout << "{\"probe\":\"ShortInitialBudgetPressure\",\"syntheticNoRaster\":true,\"lossModel\":" << static_cast<unsigned int>(erasureModel)
            << ",\"success\":" << static_cast<bool>(status) << ",\"frames\":" << probe.senderLogicalFrames
            << ",\"wraps\":" << probe.completedCarouselPasses << ",\"peakActive\":" << probe.peakActiveDecoders
            << ",\"peakReservedBytes\":" << probe.peakReservedDecoderBytes << ",\"fecDeferred\":" << probe.deferredResourceBusyCount
            << ",\"resourceRejections\":" << probe.decoder.outerResourceRejections << ",\"drought\":" << probe.longestAfterJoinNoUsefulEquationFrames << "}\n";
        INFO(status.message);
        REQUIRE(status);
        REQUIRE(probe.decoder.state == pbapp::DecoderState::Completed);
        REQUIRE(probe.decoder.verifiedRawBytes == 13ULL * 6ULL * 1024ULL * 1024ULL);
        REQUIRE(probe.decoder.wholeFileDigestCheck == true);
        REQUIRE(probe.decoder.finalRenameSucceeded == true);
        REQUIRE(probe.decoder.finalReopenVerified == true);
        REQUIRE(probe.decoder.outerConflictRejections == 0);
        REQUIRE(probe.decoder.outerActiveDecoderLimit == 8);
        REQUIRE(probe.peakActiveDecoders == 8);
        REQUIRE(probe.decoder.outerResourceRejections > 0);
        REQUIRE(probe.peakReservedDecoderBytes <= 1024ULL * 1024ULL * 1024ULL);
        std::ifstream file(probe.publishedPath, std::ios::binary);
        REQUIRE(file);
        std::vector<std::byte> reopened(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
        file.read(reinterpret_cast<char*>(reopened.data()), static_cast<std::streamsize>(reopened.size()));
        REQUIRE(file.gcount() == static_cast<std::streamsize>(reopened.size()));
        REQUIRE(file.peek() == std::char_traits<char>::eof());
        REQUIRE(pbprotocol::ComputeBlake3Digest(reopened) == probe.expectedWholeFileDigest);
    }
}

TEST_CASE("Short initial airtime retains full repair recovery without receiver feedback",
    "[application][grayfast-initial-airtime-recovery][receiver][publish]")
{
    Scratch scratch;
    for (const auto& [erasureModel, firstObservedFrame] : std::array{
        std::pair{pbapp::UnifiedRecoveryErasureModel::PeriodicTwoFifths, 0ULL},
        std::pair{pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter, 0ULL},
        std::pair{pbapp::UnifiedRecoveryErasureModel::PeriodicFifth, 0ULL},
        std::pair{pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter, 15000ULL}})
    {
        DYNAMIC_SECTION("Loss " << static_cast<unsigned int>(erasureModel) << " join " << firstObservedFrame)
        {
            std::array<pbapp::UnifiedGraduationRecoveryProbeSnapshot, 2> probes;
            for (std::size_t mode = 0; mode < probes.size(); mode++)
            {
                const auto output = scratch.Path() / (std::to_wstring(static_cast<unsigned int>(erasureModel)) + L"-" + std::to_wstring(firstObservedFrame) + L"-" + std::to_wstring(mode));
                REQUIRE(std::filesystem::create_directory(output));
                pbapp::UnifiedGraduationRecoveryProbeConfig config;
                config.segmentCount = 13;
                config.segmentBytes = 6U * 1024U * 1024U;
                config.grayFastSpatialInterleave = true;
                config.erasureModel = erasureModel;
                config.firstObservedLogicalFrame = firstObservedFrame;
                config.maximumSenderLogicalFrames = 60000;
                config.collectSegmentTrace = true;
                config.budgetBoundDecoders = true;
                config.initialAirtimePercent = mode == 0 ? 100U : 65U;
                auto& probe = probes[mode];
                const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(output.wstring(), config, probe);
                std::cout << "{\"probe\":\"InitialAirtimeRecovery\",\"syntheticNoRaster\":true,\"initialPercent\":" << config.initialAirtimePercent
                    << ",\"lossModel\":" << static_cast<unsigned int>(erasureModel) << ",\"joinFrame\":" << firstObservedFrame
                    << ",\"success\":" << static_cast<bool>(status) << ",\"frames\":" << probe.senderLogicalFrames
                    << ",\"wraps\":" << probe.completedCarouselPasses << ",\"verifiedRawBytes\":" << probe.decoder.verifiedRawBytes
                    << ",\"peakActive\":" << probe.peakActiveDecoders << ",\"peakReservedBytes\":" << probe.peakReservedDecoderBytes
                    << ",\"fecDeferred\":" << probe.deferredResourceBusyCount << ",\"resourceRejections\":" << probe.decoder.outerResourceRejections
                    << ",\"orphanQuotaDrops\":" << probe.decoder.outerOrphanDroppedByQuotaCount
                    << ",\"drought\":" << probe.longestAfterJoinNoUsefulEquationFrames << "}\n";
                INFO(status.message);
                REQUIRE(status);
                REQUIRE(probe.decoder.state == pbapp::DecoderState::Completed);
                REQUIRE(probe.decoder.verifiedRawBytes == 13ULL * 6ULL * 1024ULL * 1024ULL);
                REQUIRE(probe.decoder.wholeFileDigestCheck == true);
                REQUIRE(probe.decoder.finalRenameSucceeded == true);
                REQUIRE(probe.decoder.finalReopenVerified == true);
                REQUIRE(probe.decoder.outerConflictRejections == 0);
                REQUIRE(probe.spatialCommitAndLeaseVerified);
                REQUIRE(probe.peakActiveDecoders <= probe.decoder.outerActiveDecoderLimit);
                REQUIRE(probe.peakReservedDecoderBytes <= 1024ULL * 1024ULL * 1024ULL);
                std::ifstream file(probe.publishedPath, std::ios::binary);
                REQUIRE(file);
                std::vector<std::byte> reopened(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
                file.read(reinterpret_cast<char*>(reopened.data()), static_cast<std::streamsize>(reopened.size()));
                REQUIRE(file.gcount() == static_cast<std::streamsize>(reopened.size()));
                REQUIRE(file.peek() == std::char_traits<char>::eof());
                REQUIRE(pbprotocol::ComputeBlake3Digest(reopened) == probe.expectedWholeFileDigest);
            }
            REQUIRE(probes[0].expectedWholeFileDigest == probes[1].expectedWholeFileDigest);
            if (erasureModel == pbapp::UnifiedRecoveryErasureModel::PeriodicTwoFifths)
            {
                REQUIRE(probes[0].completedCarouselPasses == 0);
                REQUIRE(probes[1].completedCarouselPasses == 0);
                REQUIRE(probes[1].senderLogicalFrames * 100 < probes[0].senderLogicalFrames * 80);
                REQUIRE(probes[1].decoder.outerResourceRejections == 0);
                REQUIRE(probes[1].deferredResourceBusyCount == 0);
            }
        }
    }
}

TEST_CASE("Useful-cadence probe rejects unsupported projection and rate before recovery state",
    "[application][grayfast-fixed-useful-cadence-bounds]")
{
    Scratch scratch;
    pbapp::UnifiedGraduationRecoveryProbeConfig config;
    pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
    config.erasureModel = static_cast<pbapp::UnifiedRecoveryErasureModel>(255);
    REQUIRE_FALSE(pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(scratch.Path().wstring(), config, probe));
    REQUIRE(std::filesystem::is_empty(scratch.Path()));
    config.erasureModel = pbapp::UnifiedRecoveryErasureModel::PeriodicHalf;
    config.logicalVisualFps = 27;
    REQUIRE_FALSE(pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(scratch.Path().wstring(), config, probe));
    REQUIRE(std::filesystem::is_empty(scratch.Path()));
}

TEST_CASE("Large-file airtime is compared at equal synthetic useful-frame cadence",
    "[application][grayfast-fixed-useful-cadence][receiver][publish]")
{
    Scratch scratch;
    std::optional<std::array<std::byte, pbprotocol::kDigestBytes>> sharedDigest;
    for (const std::uint32_t logicalFps : {30U, 15U})
    {
        for (const std::uint32_t initialPercent : {65U, 100U})
        {
            CAPTURE(logicalFps, initialPercent);
            const auto output = scratch.Path() / (std::to_wstring(logicalFps) + L"-" + std::to_wstring(initialPercent));
            REQUIRE(std::filesystem::create_directory(output));
            pbapp::UnifiedGraduationRecoveryProbeConfig config;
            config.segmentCount = 13;
            config.segmentBytes = 6U * 1024U * 1024U;
            config.logicalVisualFps = logicalFps;
            config.erasureModel = logicalFps == 30 ? pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter : pbapp::UnifiedRecoveryErasureModel::PeriodicHalf;
            config.grayFastSpatialInterleave = true;
            config.maximumSenderLogicalFrames = 60000;
            config.collectSegmentTrace = true;
            config.budgetBoundDecoders = true;
            config.initialAirtimePercent = initialPercent;
            pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
            const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(output.wstring(), config, probe);
            std::cout << "{\"probe\":\"FixedUsefulCadenceRecovery\",\"syntheticNoRaster\":true,\"logicalFps\":" << logicalFps
                << ",\"initialPercent\":" << initialPercent << ",\"hypotheticalUsefulFps\":7.5,\"success\":" << static_cast<bool>(status)
                << ",\"frames\":" << probe.senderLogicalFrames << ",\"observedFrames\":" << probe.observedLogicalFrames
                << ",\"modeledSenderMilliseconds\":" << probe.senderLogicalFrames * 1000ULL / logicalFps
                << ",\"wraps\":" << probe.completedCarouselPasses << ",\"verifiedRawBytes\":" << probe.decoder.verifiedRawBytes
                << ",\"alreadyCompletedSymbols\":" << probe.decoder.outerAlreadyCompletedSymbols
                << ",\"peakActive\":" << probe.peakActiveDecoders << ",\"peakReservedBytes\":" << probe.peakReservedDecoderBytes
                << ",\"fecDeferred\":" << probe.deferredResourceBusyCount << ",\"resourceRejections\":" << probe.decoder.outerResourceRejections
                << ",\"orphanQuotaDrops\":" << probe.decoder.outerOrphanDroppedByQuotaCount
                << ",\"droughtFrames\":" << probe.longestAfterJoinNoUsefulEquationFrames << "}\n";
            INFO(status.message);
            REQUIRE(status);
            REQUIRE(probe.decoder.state == pbapp::DecoderState::Completed);
            REQUIRE(probe.decoder.verifiedRawBytes == 13ULL * 6ULL * 1024ULL * 1024ULL);
            REQUIRE(probe.decoder.wholeFileDigestCheck == true);
            REQUIRE(probe.decoder.finalRenameSucceeded == true);
            REQUIRE(probe.decoder.finalReopenVerified == true);
            REQUIRE(probe.decoder.outerConflictRejections == 0);
            REQUIRE(probe.spatialCommitAndLeaseVerified);
            REQUIRE(probe.peakActiveDecoders <= probe.decoder.outerActiveDecoderLimit);
            REQUIRE(probe.peakReservedDecoderBytes <= 1024ULL * 1024ULL * 1024ULL);
            REQUIRE(probe.segmentTrace.size() == config.segmentCount);
            for (std::size_t ordinal = 0; ordinal < probe.segmentTrace.size(); ordinal++)
            {
                const auto& trace = probe.segmentTrace[ordinal];
                REQUIRE(trace.completedFrame);
                std::cout << "{\"probe\":\"FixedUsefulCadenceSegment\",\"logicalFps\":" << logicalFps << ",\"initialPercent\":" << initialPercent
                    << ",\"ordinal\":" << ordinal << ",\"completedFrame\":" << *trace.completedFrame
                    << ",\"alreadyCompletedBlocks\":" << trace.alreadyCompletedBlocks << "}\n";
            }
            std::ifstream file(probe.publishedPath, std::ios::binary);
            REQUIRE(file);
            std::vector<std::byte> reopened(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
            file.read(reinterpret_cast<char*>(reopened.data()), static_cast<std::streamsize>(reopened.size()));
            REQUIRE(file.gcount() == static_cast<std::streamsize>(reopened.size()));
            REQUIRE(file.peek() == std::char_traits<char>::eof());
            REQUIRE(pbprotocol::ComputeBlake3Digest(reopened) == probe.expectedWholeFileDigest);
            if (sharedDigest)
            {
                REQUIRE(*sharedDigest == probe.expectedWholeFileDigest);
            }
            sharedDigest = probe.expectedWholeFileDigest;
        }
    }
}

TEST_CASE("Budget-bound decoder admission is compared with fixed eight on the identical spatial sender",
    "[application][budgeted-decoder-recovery][receiver][publish]")
{
    Scratch scratch;
    for (const auto& [erasureModel, firstObservedFrame] : std::array{
        std::pair{pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter, 15000ULL},
        std::pair{pbapp::UnifiedRecoveryErasureModel::PeriodicFifth, 0ULL}})
    {
        DYNAMIC_SECTION("Loss " << static_cast<unsigned int>(erasureModel) << " join " << firstObservedFrame)
        {
            std::array<pbapp::UnifiedGraduationRecoveryProbeSnapshot, 2> probes;
            for (std::size_t mode = 0; mode < probes.size(); mode++)
            {
                const auto output = scratch.Path() / (std::to_wstring(static_cast<unsigned int>(erasureModel)) + L"-" + std::to_wstring(mode));
                REQUIRE(std::filesystem::create_directory(output));
                pbapp::UnifiedGraduationRecoveryProbeConfig config;
                config.segmentCount = 13;
                config.segmentBytes = 6U * 1024U * 1024U;
                config.grayFastSpatialInterleave = true;
                config.erasureModel = erasureModel;
                config.firstObservedLogicalFrame = firstObservedFrame;
                config.maximumSenderLogicalFrames = 60000;
                config.collectSegmentTrace = true;
                config.budgetBoundDecoders = mode != 0;
                auto& probe = probes[mode];
                const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(output.wstring(), config, probe);
                std::cout << "{\"probe\":\"BudgetBoundDecoderRecovery\",\"syntheticNoRaster\":true,\"budgetBound\":" << config.budgetBoundDecoders
                    << ",\"lossModel\":" << static_cast<unsigned int>(erasureModel) << ",\"joinFrame\":" << firstObservedFrame
                    << ",\"success\":" << static_cast<bool>(status) << ",\"frames\":" << probe.senderLogicalFrames
                    << ",\"wraps\":" << probe.completedCarouselPasses << ",\"verifiedRawBytes\":" << probe.decoder.verifiedRawBytes
                    << ",\"uniqueSymbols\":" << probe.decoder.outerUniqueSymbols << ",\"peakActive\":" << probe.peakActiveDecoders
                    << ",\"peakReservedBytes\":" << probe.peakReservedDecoderBytes << ",\"countLimit\":" << probe.decoder.outerActiveDecoderLimit
                    << ",\"byteLimit\":" << probe.decoder.outerTotalDecoderByteLimit << ",\"fecDeferred\":" << probe.deferredResourceBusyCount
                    << ",\"resourceRejections\":" << probe.decoder.outerResourceRejections
                    << ",\"orphanQuotaDrops\":" << probe.decoder.outerOrphanDroppedByQuotaCount
                    << ",\"drought\":" << probe.longestAfterJoinNoUsefulEquationFrames << "}\n";
                INFO(status.message);
                REQUIRE(status);
                REQUIRE(probe.decoder.state == pbapp::DecoderState::Completed);
                REQUIRE(probe.decoder.verifiedRawBytes == 13ULL * 6ULL * 1024ULL * 1024ULL);
                REQUIRE(probe.decoder.wholeFileDigestCheck == true);
                REQUIRE(probe.decoder.finalRenameSucceeded == true);
                REQUIRE(probe.decoder.finalReopenVerified == true);
                REQUIRE(probe.decoder.outerConflictRejections == 0);
                REQUIRE(probe.spatialCommitAndLeaseVerified);
                REQUIRE(probe.decoder.budgetBoundDecoderAdmission == config.budgetBoundDecoders);
                REQUIRE(probe.peakActiveDecoders <= probe.decoder.outerActiveDecoderLimit);
                REQUIRE(probe.peakReservedDecoderBytes <= 1024ULL * 1024ULL * 1024ULL);
                std::ifstream file(probe.publishedPath, std::ios::binary);
                REQUIRE(file);
                std::vector<std::byte> reopened(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
                file.read(reinterpret_cast<char*>(reopened.data()), static_cast<std::streamsize>(reopened.size()));
                REQUIRE(file.gcount() == static_cast<std::streamsize>(reopened.size()));
                REQUIRE(file.peek() == std::char_traits<char>::eof());
                REQUIRE(pbprotocol::ComputeBlake3Digest(reopened) == probe.expectedWholeFileDigest);
            }
            REQUIRE(probes[1].expectedWholeFileDigest == probes[0].expectedWholeFileDigest);
            REQUIRE(probes[1].senderLogicalFrames <= probes[0].senderLogicalFrames);
            if (erasureModel == pbapp::UnifiedRecoveryErasureModel::PeriodicFifth)
            {
                REQUIRE(probes[1].peakActiveDecoders > 8);
                REQUIRE(probes[1].deferredResourceBusyCount == 0);
                REQUIRE(probes[1].senderLogicalFrames < probes[0].senderLogicalFrames);
            }
        }
    }
}

TEST_CASE("Residual Transport erasure rejects invalid percentages before recovery state",
    "[application][grayfast-residual-loss-bounds]")
{
    Scratch scratch;
    pbapp::UnifiedGraduationRecoveryProbeConfig config;
    pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
    for (const std::uint32_t percent : {76U, 100U, (std::numeric_limits<std::uint32_t>::max)()})
    {
        config.transportSlotErasurePercent = percent;
        REQUIRE_FALSE(pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(scratch.Path().wstring(), config, probe));
        REQUIRE(std::filesystem::is_empty(scratch.Path()));
    }
}

TEST_CASE("Modeled visit budget rejects invalid or confounded tuning before recovery state",
    "[application][grayfast-visit-budget-bounds]")
{
    Scratch scratch;
    pbapp::UnifiedGraduationRecoveryProbeConfig config;
    config.grayFastSpatialInterleave = true;
    pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
    REQUIRE(config.modeledVisitBudgetPercent == 100);
    for (const std::uint32_t percent : {0U, 99U, 201U, (std::numeric_limits<std::uint32_t>::max)()})
    {
        config.modeledVisitBudgetPercent = percent;
        REQUIRE_FALSE(pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(scratch.Path().wstring(), config, probe));
        REQUIRE(std::filesystem::is_empty(scratch.Path()));
    }
    config.modeledVisitBudgetPercent = 150;
    config.grayFastSpatialInterleave = false;
    REQUIRE_FALSE(pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(scratch.Path().wstring(), config, probe));
    REQUIRE(std::filesystem::is_empty(scratch.Path()));
    config.grayFastSpatialInterleave = true;
    config.initialAirtimePercent = 65;
    REQUIRE_FALSE(pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(scratch.Path().wstring(), config, probe));
    REQUIRE(std::filesystem::is_empty(scratch.Path()));
}

TEST_CASE("Modeled visit budget preserves bounded recovery and lease semantics at both limits",
    "[application][grayfast-visit-budget][receiver][publish]")
{
    const std::uint32_t budgetPercent = GENERATE(100U, 200U);
    Scratch scratch;
    pbapp::UnifiedGraduationRecoveryProbeConfig config;
    config.segmentCount = 13;
    config.segmentBytes = 128U * 1024U;
    config.grayFastSpatialInterleave = true;
    config.erasureModel = pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter;
    config.transportSlotErasurePercent = 25;
    config.modeledVisitBudgetPercent = budgetPercent;
    config.firstObservedLogicalFrame = 400;
    pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(scratch.Path().wstring(), config, probe);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(probe.decoder.verifiedRawBytes == 13ULL * 128ULL * 1024ULL);
    REQUIRE(probe.decoder.wholeFileDigestCheck == true);
    REQUIRE(probe.decoder.finalRenameSucceeded == true);
    REQUIRE(probe.decoder.finalReopenVerified == true);
    REQUIRE(probe.decoder.outerConflictRejections == 0);
    REQUIRE(probe.spatialCommitAndLeaseVerified);
    REQUIRE(probe.peakActiveDecoders <= probe.decoder.outerActiveDecoderLimit);
    std::ifstream file(probe.publishedPath, std::ios::binary);
    REQUIRE(file);
    std::vector<std::byte> reopened(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
    file.read(reinterpret_cast<char*>(reopened.data()), static_cast<std::streamsize>(reopened.size()));
    REQUIRE(file.gcount() == static_cast<std::streamsize>(reopened.size()));
    REQUIRE(file.peek() == std::char_traits<char>::eof());
    REQUIRE(pbprotocol::ComputeBlake3Digest(reopened) == probe.expectedWholeFileDigest);
}

TEST_CASE("Residual Transport erasure is reproducible and preserves independent final verification",
    "[application][grayfast-residual-loss][receiver][publish]")
{
    Scratch scratch;
    std::array<pbapp::UnifiedGraduationRecoveryProbeSnapshot, 3> probes;
    for (std::size_t index = 0; index < probes.size(); index++)
    {
        const auto output = scratch.Path() / std::to_wstring(index);
        REQUIRE(std::filesystem::create_directory(output));
        pbapp::UnifiedGraduationRecoveryProbeConfig config;
        config.segmentCount = 2;
        config.segmentBytes = 128U * 1024U;
        config.erasureModel = pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter;
        config.transportSlotErasurePercent = index == 0 ? 0U : 25U;
        auto& probe = probes[index];
        const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(output.wstring(), config, probe);
        INFO(status.message);
        REQUIRE(status);
        REQUIRE(probe.decoder.verifiedRawBytes == 2ULL * 128ULL * 1024ULL);
        REQUIRE(probe.decoder.wholeFileDigestCheck == true);
        REQUIRE(probe.decoder.finalRenameSucceeded == true);
        REQUIRE(probe.decoder.finalReopenVerified == true);
        REQUIRE(probe.decoder.outerConflictRejections == 0);
        REQUIRE(probe.modeledTransportSlotCandidates > 0);
        REQUIRE(probe.modeledTransportSlotCandidates <= probe.observedLogicalFrames * 18U);
        REQUIRE(probe.modeledTransportSlotsErased < probe.modeledTransportSlotCandidates);
        REQUIRE((probe.modeledTransportSlotsErased == 0) == (config.transportSlotErasurePercent == 0));
        std::ifstream file(probe.publishedPath, std::ios::binary);
        REQUIRE(file);
        std::vector<std::byte> reopened(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
        file.read(reinterpret_cast<char*>(reopened.data()), static_cast<std::streamsize>(reopened.size()));
        REQUIRE(file.gcount() == static_cast<std::streamsize>(reopened.size()));
        REQUIRE(file.peek() == std::char_traits<char>::eof());
        REQUIRE(pbprotocol::ComputeBlake3Digest(reopened) == probe.expectedWholeFileDigest);
    }
    REQUIRE(probes[0].expectedWholeFileDigest == probes[1].expectedWholeFileDigest);
    REQUIRE(probes[1].expectedWholeFileDigest == probes[2].expectedWholeFileDigest);
    REQUIRE(probes[1].senderLogicalFrames == probes[2].senderLogicalFrames);
    REQUIRE(probes[1].modeledTransportSlotCandidates == probes[2].modeledTransportSlotCandidates);
    REQUIRE(probes[1].modeledTransportSlotsErased == probes[2].modeledTransportSlotsErased);
}

// Explicit opt-in: four 78 MiB clean recoveries. This compares a fixed sender
// budget, not adaptation to the receiver or an estimate of the ToDesk channel.
TEST_CASE("Longer spatial visits are compared against identical two-stage loss models",
    "[application][.nonlocal-visit-budget-stress][receiver][publish]")
{
    const std::uint32_t transportErasurePercent = GENERATE(0U, 25U);
    Scratch scratch;
    for (const std::uint32_t budgetPercent : {100U, 150U})
    {
        const auto output = scratch.Path() / std::to_wstring(budgetPercent);
        REQUIRE(std::filesystem::create_directory(output));
        pbapp::UnifiedGraduationRecoveryProbeConfig config;
        config.segmentCount = 13;
        config.segmentBytes = 6U * 1024U * 1024U;
        config.erasureModel = pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter;
        config.grayFastSpatialInterleave = true;
        config.transportSlotErasurePercent = transportErasurePercent;
        config.modeledVisitBudgetPercent = budgetPercent;
        config.maximumSenderLogicalFrames = 60000;
        config.collectSegmentTrace = true;
        pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
        const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(output.wstring(), config, probe);
        std::cout << "{\"probe\":\"VisitBudgetRecovery\",\"syntheticNoRaster\":true,\"transportErasurePercent\":" << transportErasurePercent
            << ",\"visitBudgetPercent\":" << budgetPercent << ",\"success\":" << static_cast<bool>(status)
            << ",\"senderFrames\":" << probe.senderLogicalFrames << ",\"observedFrames\":" << probe.observedLogicalFrames
            << ",\"candidateTransportSlots\":" << probe.modeledTransportSlotCandidates << ",\"erasedTransportSlots\":" << probe.modeledTransportSlotsErased
            << ",\"wraps\":" << probe.completedCarouselPasses << ",\"verifiedRawBytes\":" << probe.decoder.verifiedRawBytes
            << ",\"alreadyCompletedSymbols\":" << probe.decoder.outerAlreadyCompletedSymbols << ",\"peakActive\":" << probe.peakActiveDecoders
            << ",\"peakReservedBytes\":" << probe.peakReservedDecoderBytes << ",\"fecDeferred\":" << probe.deferredResourceBusyCount
            << ",\"resourceRejections\":" << probe.decoder.outerResourceRejections << ",\"orphanQuotaDrops\":" << probe.decoder.outerOrphanDroppedByQuotaCount
            << ",\"droughtFrames\":" << probe.longestAfterJoinNoUsefulEquationFrames << "}\n";
        INFO(status.message);
        REQUIRE(status);
        REQUIRE(probe.decoder.verifiedRawBytes == 13ULL * 6ULL * 1024ULL * 1024ULL);
        REQUIRE(probe.decoder.wholeFileDigestCheck == true);
        REQUIRE(probe.decoder.finalRenameSucceeded == true);
        REQUIRE(probe.decoder.finalReopenVerified == true);
        REQUIRE(probe.decoder.outerConflictRejections == 0);
        REQUIRE(probe.spatialCommitAndLeaseVerified);
        REQUIRE(probe.peakActiveDecoders <= probe.decoder.outerActiveDecoderLimit);
        REQUIRE(probe.peakReservedDecoderBytes <= 1024ULL * 1024ULL * 1024ULL);
        REQUIRE(probe.segmentTrace.size() == config.segmentCount);
        for (std::size_t ordinal = 0; ordinal < probe.segmentTrace.size(); ordinal++)
        {
            const auto& trace = probe.segmentTrace[ordinal];
            REQUIRE(trace.completedFrame);
            std::cout << "{\"probe\":\"VisitBudgetSegment\",\"transportErasurePercent\":" << transportErasurePercent
                << ",\"visitBudgetPercent\":" << budgetPercent << ",\"ordinal\":" << ordinal << ",\"completedFrame\":" << *trace.completedFrame
                << ",\"uniqueAdmissionEvents\":" << trace.uniqueAdmissionEvents << ",\"deferredBlocks\":" << trace.deferredBlocks
                << ",\"alreadyCompletedBlocks\":" << trace.alreadyCompletedBlocks << "}\n";
        }
        std::ifstream file(probe.publishedPath, std::ios::binary);
        REQUIRE(file);
        std::vector<std::byte> reopened(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
        file.read(reinterpret_cast<char*>(reopened.data()), static_cast<std::streamsize>(reopened.size()));
        REQUIRE(file.gcount() == static_cast<std::streamsize>(reopened.size()));
        REQUIRE(file.peek() == std::char_traits<char>::eof());
        REQUIRE(pbprotocol::ComputeBlake3Digest(reopened) == probe.expectedWholeFileDigest);
    }
}

// Explicit opt-in: four 78 MiB clean recoveries, not part of ordinary CTest.
TEST_CASE("Whole-frame and residual Transport losses are separated in a large-file stress projection",
    "[application][.nonlocal-two-stage-loss-stress][receiver][publish]")
{
    const bool spatial = GENERATE(false, true);
    Scratch scratch;
    for (const std::uint32_t percent : {0U, 25U})
    {
        const auto output = scratch.Path() / std::to_wstring(percent);
        REQUIRE(std::filesystem::create_directory(output));
        pbapp::UnifiedGraduationRecoveryProbeConfig config;
        config.segmentCount = 13;
        config.segmentBytes = 6U * 1024U * 1024U;
        config.erasureModel = pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter;
        config.grayFastSpatialInterleave = spatial;
        config.transportSlotErasurePercent = percent;
        config.maximumSenderLogicalFrames = 60000;
        config.collectSegmentTrace = true;
        pbapp::UnifiedGraduationRecoveryProbeSnapshot probe;
        const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(output.wstring(), config, probe);
        std::cout << "{\"probe\":\"TwoStageLossRecovery\",\"syntheticNoRaster\":true,\"transportErasurePercent\":" << percent
            << ",\"spatial\":" << (spatial ? "true" : "false")
            << ",\"success\":" << static_cast<bool>(status) << ",\"senderFrames\":" << probe.senderLogicalFrames
            << ",\"observedFrames\":" << probe.observedLogicalFrames << ",\"candidateTransportSlots\":" << probe.modeledTransportSlotCandidates
            << ",\"erasedTransportSlots\":" << probe.modeledTransportSlotsErased << ",\"wraps\":" << probe.completedCarouselPasses
            << ",\"verifiedRawBytes\":" << probe.decoder.verifiedRawBytes << ",\"alreadyCompletedSymbols\":" << probe.decoder.outerAlreadyCompletedSymbols
            << ",\"peakActive\":" << probe.peakActiveDecoders << ",\"peakReservedBytes\":" << probe.peakReservedDecoderBytes
            << ",\"fecDeferred\":" << probe.deferredResourceBusyCount << ",\"resourceRejections\":" << probe.decoder.outerResourceRejections
            << ",\"orphanQuotaDrops\":" << probe.decoder.outerOrphanDroppedByQuotaCount
            << ",\"droughtFrames\":" << probe.longestAfterJoinNoUsefulEquationFrames << "}\n";
        INFO(status.message);
        REQUIRE(status);
        REQUIRE(probe.decoder.verifiedRawBytes == 13ULL * 6ULL * 1024ULL * 1024ULL);
        REQUIRE(probe.decoder.wholeFileDigestCheck == true);
        REQUIRE(probe.decoder.finalRenameSucceeded == true);
        REQUIRE(probe.decoder.finalReopenVerified == true);
        REQUIRE(probe.decoder.outerConflictRejections == 0);
        REQUIRE(probe.spatialCommitAndLeaseVerified == spatial);
        REQUIRE(probe.peakActiveDecoders <= probe.decoder.outerActiveDecoderLimit);
        REQUIRE(probe.peakReservedDecoderBytes <= 1024ULL * 1024ULL * 1024ULL);
        REQUIRE(probe.segmentTrace.size() == config.segmentCount);
        for (std::size_t ordinal = 0; ordinal < probe.segmentTrace.size(); ordinal++)
        {
            const auto& trace = probe.segmentTrace[ordinal];
            REQUIRE(trace.completedFrame);
            std::cout << "{\"probe\":\"TwoStageLossSegment\",\"transportErasurePercent\":" << percent
                << ",\"spatial\":" << (spatial ? "true" : "false")
                << ",\"ordinal\":" << ordinal << ",\"completedFrame\":" << *trace.completedFrame
                << ",\"uniqueAdmissionEvents\":" << trace.uniqueAdmissionEvents << ",\"deferredBlocks\":" << trace.deferredBlocks
                << ",\"alreadyCompletedBlocks\":" << trace.alreadyCompletedBlocks << "}\n";
        }
        std::ifstream file(probe.publishedPath, std::ios::binary);
        REQUIRE(file);
        std::vector<std::byte> reopened(static_cast<std::size_t>(probe.decoder.verifiedRawBytes));
        file.read(reinterpret_cast<char*>(reopened.data()), static_cast<std::streamsize>(reopened.size()));
        REQUIRE(file.gcount() == static_cast<std::streamsize>(reopened.size()));
        REQUIRE(file.peek() == std::char_traits<char>::eof());
        REQUIRE(pbprotocol::ComputeBlake3Digest(reopened) == probe.expectedWholeFileDigest);
    }
}
