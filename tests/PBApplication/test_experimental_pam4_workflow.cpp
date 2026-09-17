#include "local_desktop_runtime.h"
#include "sender_carousel_scheduler.h"
#include "run_report.h"
#include "pbmodulation/experimental_pam4.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <set>

namespace
{

class Scratch final
{
public:
    Scratch()
    {
        static std::atomic<std::uint64_t> nextOrdinal = 0;
        const auto root = std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal();
        path_ = root / (L"pam4-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(nextOrdinal.fetch_add(1)));
        REQUIRE(path_.parent_path() == root);
        REQUIRE(std::filesystem::create_directory(path_));
        REQUIRE(std::filesystem::create_directory(path_ / L"output"));
        REQUIRE(std::filesystem::create_directory(path_ / L"state"));
    }
    ~Scratch()
    {
        const auto root = std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal();
        if (path_.parent_path() == root && path_.filename().wstring().starts_with(L"pam4-"))
        {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return path_;
    }
private:
    std::filesystem::path path_;
};

void WriteFixture(const std::filesystem::path& path, const std::size_t size, const bool compressible)
{
    std::vector<std::byte> bytes(size);
    std::uint32_t state = 0x7CA34919;
    for (auto& value : bytes)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        value = compressible ? std::byte{0x42} : static_cast<std::byte>(state & 0xFFU);
    }
    std::ofstream stream(path, std::ios::binary);
    REQUIRE(stream);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
}

pbapp::MonitorInfo TestMonitor(const bool protectedMonitor)
{
    pbapp::MonitorInfo monitor;
    monitor.monitor = reinterpret_cast<HMONITOR>(protectedMonitor ? std::uintptr_t{1} : std::uintptr_t{2});
    monitor.deviceName = protectedMonitor ? L"\\\\.\\DISPLAY1" : L"\\\\.\\DISPLAY2";
    monitor.physicalRect = protectedMonitor ? RECT{-2560, 0, 0, 1440} : RECT{0, 0, 3840, 2160};
    monitor.workRect = monitor.physicalRect;
    monitor.dpiX = 96;
    monitor.dpiY = 96;
    monitor.refreshRate = 60;
    monitor.rotation = DXGI_MODE_ROTATION_IDENTITY;
    monitor.primary = protectedMonitor;
    monitor.adapterLuid = {1, 0};
    return monitor;
}

pbapp::DecoderConfig DecoderConfiguration(const Scratch& scratch)
{
    const auto protectedMonitor = TestMonitor(true);
    const auto experimentMonitor = TestMonitor(false);
    pbapp::DecoderConfig config;
    config.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
    config.captureBackend = pbapp::CaptureBackend::Auto;
    config.outputDirectory = (scratch.Path() / L"output").wstring();
    config.region = {experimentMonitor.monitor, {0, 0, 2560, 1440}, experimentMonitor.physicalRect, 96, 96, DXGI_MODE_ROTATION_IDENTITY};
    config.monitorSafety = pbapp::MonitorSafetySelection{protectedMonitor, experimentMonitor};
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "OfflineValidationOnly";
    config.remoteMetadata.protectedMonitorIdentity = R"(\\.\DISPLAY1)";
    config.remoteMetadata.experimentMonitorIdentity = R"(\\.\DISPLAY2)";
    return config;
}

}

TEST_CASE("PAM4 scheduler requires an explicit independent layout and rejects foreign slot counts", "[pam4][scheduler]")
{
    REQUIRE_FALSE(pbapp::IsUnifiedVisualFamily(pbapp::VisualProfile::ExperimentalPam4));
    REQUIRE(pbapp::UsesMixedSlotCarousel(pbapp::VisualProfile::ExperimentalPam4));
    REQUIRE(pbapp::ParseVisualProfileToken(std::string_view("experimental-pam4")) == pbapp::VisualProfile::ExperimentalPam4);
    REQUIRE(pbapp::ParseVisualProfileToken(std::wstring_view(L"experimental-pam4")) == pbapp::VisualProfile::ExperimentalPam4);
    REQUIRE(pbapp::FindVisualProfileOption(pbapp::VisualProfile::ExperimentalPam4));
    REQUIRE_FALSE(pbapp::ParseVisualProfileToken(std::string_view("pam4")));
    REQUIRE_FALSE(pbapp::ParseVisualProfileToken(std::string_view("Experimental-Pam4")));
    for (const auto& option : pbapp::GetVisualProfileOptions())
    {
        REQUIRE(option.profile != pbapp::VisualProfile::ExperimentalPam4);
    }
    REQUIRE(std::string_view(pbapp::GetVisualProfileName(pbapp::VisualProfile::ExperimentalPam4)) == "PB-Experimental-Pam4-1");
    pbapp::SenderUnifiedCarouselSchedulerConfig config;
    config.frameCodewordSlots = 11;
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create(config, scheduler));
    config.slotLayout = pbapp::SenderMixedSlotLayout::ExperimentalPam4;
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create(config, scheduler));
    for (const std::uint32_t slots : {0U, 1U, 10U, 12U, 15U, 18U, (std::numeric_limits<std::uint32_t>::max)()})
    {
        config.frameCodewordSlots = slots;
        REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create(config, scheduler));
    }
    config.frameCodewordSlots = 11;
    config.slotLayout = static_cast<pbapp::SenderMixedSlotLayout>(255);
    REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create(config, scheduler));
}

TEST_CASE("PAM4 one-control scheduler keeps every data equation behind its same-frame Segment descriptor", "[pam4][scheduler]")
{
    const std::uint32_t blockCount = GENERATE(0U, 1U, 21U, 645U);
    const std::uint64_t pass = GENERATE(0ULL, 1ULL);
    pbapp::SenderUnifiedCarouselSchedulerConfig config;
    config.slotLayout = pbapp::SenderMixedSlotLayout::ExperimentalPam4;
    config.frameCodewordSlots = 11;
    config.systematicBlockCount = blockCount;
    config.wirehair = blockCount >= 2;
    config.carouselPass = pass;
    config.logicalFramesPerSecond = 30;
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create(config, scheduler));
    std::set<std::uint64_t> equations;
    std::set<pbmodulation::UnifiedControlPriority> controls;
    std::uint64_t inactiveSlots = 0;
    std::uint64_t frames = 0;
    for (; frames < 2000 && !scheduler.IsComplete(); frames++)
    {
        pbapp::SenderUnifiedScheduledFrame frame;
        REQUIRE(scheduler.PrepareFrameAt(frames, frames * 33333333ULL, frame));
        const auto snapshot = scheduler.GetSnapshot();
        pbapp::SenderUnifiedScheduledFrame repeated;
        REQUIRE(scheduler.PrepareFrameAt(frames, frames * 33333333ULL, repeated));
        REQUIRE(repeated == frame);
        REQUIRE(scheduler.GetSnapshot() == snapshot);
        REQUIRE(frame.slotCount == 11);
        REQUIRE(frame.controlSlotCount == 1);
        REQUIRE(frame.transportSlotCount == 10);
        REQUIRE(frame.slots[0].assignment.kind == pbmodulation::UnifiedSlotKind::Control);
        const auto priority = frame.slots[0].assignment.controlPriority;
        controls.insert(priority);
        std::array<pbmodulation::UnifiedSlotAssignment, 11> assignments{};
        for (std::uint32_t slotIndex = 0; slotIndex < 11; slotIndex++)
        {
            assignments[slotIndex] = frame.slots[slotIndex].assignment;
            const auto& slot = frame.slots[slotIndex];
            if (slot.transportDisposition == pbapp::SenderUnifiedTransportSlotDisposition::ScheduledEquation)
            {
                REQUIRE(priority == pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor);
                REQUIRE(equations.insert(slot.equationIndex).second);
                REQUIRE(slot.repairEquation == (slot.equationIndex >= snapshot.systematicEquationCount));
            }
            if (slot.transportDisposition == pbapp::SenderUnifiedTransportSlotDisposition::InactiveControlPrelude ||
                slot.transportDisposition == pbapp::SenderUnifiedTransportSlotDisposition::InactiveZeroByteSession)
            {
                inactiveSlots++;
            }
        }
        REQUIRE(pbmodulation::ValidateExperimentalPam4SlotPlan(assignments));
        if (blockCount == 0 || priority != pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor)
        {
            REQUIRE(frame.scheduledEquationCount == 0);
            REQUIRE(frame.inactiveTransportSlotCount == 10);
        }
        REQUIRE(scheduler.CommitPreparedFrame());
    }
    REQUIRE(scheduler.IsComplete());
    REQUIRE(controls.contains(pbmodulation::UnifiedControlPriority::SessionDescriptor));
    REQUIRE(controls.contains(pbmodulation::UnifiedControlPriority::FinalManifest));
    REQUIRE(controls.contains(pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor) == (blockCount != 0));
    const auto snapshot = scheduler.GetSnapshot();
    REQUIRE(snapshot.scheduledEquationCount == equations.size());
    REQUIRE(snapshot.committedEquationCount == equations.size());
    REQUIRE(snapshot.inactiveTransportSlotCount == inactiveSlots);
    REQUIRE(snapshot.controlSlotCount == frames);
}

TEST_CASE("PAM4 report keeps eleven-slot observation counts outside Unified lane metrics", "[pam4][report]")
{
    pbapp::DecoderSnapshot snapshot;
    const auto legacy = pbapp::BuildDecoderRunReportJson({}, snapshot);
    REQUIRE(legacy.find("\"experimentalPam4\"") == std::string::npos);
    snapshot.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
    snapshot.pam4FrameObservations = 5;
    snapshot.pam4AvailableObservations = 4;
    snapshot.pam4ErasedObservations = 1;
    snapshot.pam4EvaluatedSlots = 44;
    snapshot.pam4AcceptedControlSlots = 4;
    snapshot.pam4AcceptedTransportSlots = 27;
    const auto report = pbapp::BuildDecoderRunReportJson({}, snapshot);
    REQUIRE(report.find("\"experimentalPam4\":{\"slotsPerAvailableObservation\":11") != std::string::npos);
    REQUIRE(report.find("\"evaluatedSlots\":44") != std::string::npos);
    REQUIRE(report.find("\"acceptedTransportSlots\":27") != std::string::npos);
    REQUIRE(report.find("\"verifiedEncodedBytesPerUniqueFrame\":null") != std::string::npos);
    REQUIRE(report.find("\"unifiedTelemetry\":null") != std::string::npos);
    REQUIRE(report.find("\"denominatorObservedUniqueFrames\":null") != std::string::npos);
}

TEST_CASE("PAM4 production source and carousel recover files only through decoded pixels", "[pam4][file][pixels]")
{
    const std::uint32_t fixtureIndex = GENERATE(0U, 1U, 2U, 3U, 4U);
    static constexpr std::array<std::pair<std::size_t, bool>, 5> fixtures{{{0, false}, {1024, false}, {150000, false},
        {2U * 1024U * 1024U + 1024U, false}, {2U * 1024U * 1024U + 1024U, true}}};
    const auto fixture = fixtures[fixtureIndex];
    Scratch scratch;
    const auto source = scratch.Path() / L"fixture.bin";
    WriteFixture(source, fixture.first, fixture.second);
    pbapp::ExperimentalPam4FileProbeOptions options;
    pbapp::ExperimentalPam4FileProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(),
        scratch.Path() / L"state", (scratch.Path() / L"output").wstring(), options, result);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(result.sourceStable);
    REQUIRE(result.independentlyReopenedEqual);
    REQUIRE(result.decoder.finalReopenVerified == true);
    REQUIRE(result.decoder.finalPublishSucceeded);
    REQUIRE(result.decoder.verifiedRawBytes == fixture.first);
    REQUIRE(result.decoder.visualProfileId == pbprotocol::kPam4ExperimentalProfile.visualProfileId);
    REQUIRE(result.decoder.codewordsPerFrame == 11);
    REQUIRE(result.decoder.pam4EvaluatedSlots == result.decoder.pam4AvailableObservations * 11);
    REQUIRE(result.decoder.pam4AcceptedControlSlots == result.decoder.pam4AvailableObservations);
    REQUIRE(result.decoder.unifiedTelemetry.uniqueFrames == 0);
    REQUIRE_FALSE(result.decoder.uniqueVisualFps);
    REQUIRE(result.peakSenderSegments <= 6);
    REQUIRE(result.peakReceiverDecoders <= 8);
    REQUIRE(result.peakReceiverOrphanBytes == 0);
    if (fixture.first > 2U * 1024U * 1024U)
    {
        REQUIRE(result.segmentCount == 3);
        REQUIRE(result.wirehairSegments == (fixture.second ? 0 : 2));
        REQUIRE(result.directRepeatSegments == (fixture.second ? 3 : 1));
    }
    std::cout << "PAM4 offline bytes=" << fixture.first << " compressible=" << fixture.second
        << " generated=" << result.generatedFrames << " decoded=" << result.decodedFrames << " reopen=" << result.independentlyReopenedEqual << '\n';
}

TEST_CASE("PAM4 clean-baseline late join and erased whole frames recover with fresh Fountain repair", "[pam4][file][pixels]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"repair.bin";
    WriteFixture(source, 2U * 1024U * 1024U + 4096U, false);
    pbapp::ExperimentalPam4FileProbeOptions options;
    options.joinAfterFrames = 40;
    options.dropEveryFrames = 5;
    options.duplicateEveryFrames = 7;
    pbapp::ExperimentalPam4FileProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(),
        scratch.Path() / L"state", (scratch.Path() / L"output").wstring(), options, result);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(result.independentlyReopenedEqual);
    REQUIRE(result.skippedFrames >= 40);
    REQUIRE(result.duplicateObservations != 0);
    REQUIRE(result.decoder.outerConflictRejections == 0);
    REQUIRE(result.decoder.outerResourceRejections == 0);
    REQUIRE(result.peakReceiverOrphanBytes == 0);
    REQUIRE(result.peakSenderSegments <= 6);
    REQUIRE(result.peakReceiverDecoders <= 8);
    REQUIRE(result.decoder.pam4FrameObservations == result.decodedFrames + result.duplicateObservations);
    std::cout << "PAM4 offline loss generated=" << result.generatedFrames << " decoded=" << result.decodedFrames
        << " dropped=" << result.skippedFrames << " duplicates=" << result.duplicateObservations << '\n';
}

TEST_CASE("PAM4 receiver rejects foreign and malformed compact handoffs before output allocation", "[pam4][negative]")
{
    const auto fault = GENERATE(pbapp::ExperimentalPam4ProbeFault::ForeignObservationIdentity,
        pbapp::ExperimentalPam4ProbeFault::DuplicateControlSlot, pbapp::ExperimentalPam4ProbeFault::OversizedBlockCount,
        pbapp::ExperimentalPam4ProbeFault::ForeignSlotCount);
    Scratch scratch;
    const auto source = scratch.Path() / L"rejected.bin";
    WriteFixture(source, 1024, false);
    pbapp::ExperimentalPam4FileProbeOptions options;
    options.handoffFaultForTest = fault;
    pbapp::ExperimentalPam4FileProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(),
        scratch.Path() / L"state", (scratch.Path() / L"output").wstring(), options, result);
    REQUIRE_FALSE(status);
    REQUIRE_FALSE(status.message.empty());
    REQUIRE_FALSE(result.independentlyReopenedEqual);
    REQUIRE(std::filesystem::is_empty(scratch.Path() / L"output"));
}

TEST_CASE("PAM4 restarts with fresh receive owners and revalidates persisted partial file state", "[pam4][file][resume]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"restart.bin";
    WriteFixture(source, 2U * 1024U * 1024U + 4096U, false);
    pbapp::ExperimentalPam4FileProbeOptions options;
    options.restartReceiverAfterFrames = 100;
    const unsigned int memoryMode = GENERATE(0U, 1U, 2U);
    options.budgetBoundDecoders = memoryMode != 0;
    if (memoryMode == 2)
    {
        options.memoryBudget = pbapp::DecoderMemoryBudget{2048 * pbapp::decoderMemoryMebibyte, 1024 * pbapp::decoderMemoryMebibyte};
    }
    pbapp::ExperimentalPam4FileProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(),
        scratch.Path() / L"state", (scratch.Path() / L"output").wstring(), options, result);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(result.receiverRestarts == 1);
    REQUIRE(result.verifiedRawBytesBeforeRestart > 0);
    REQUIRE(result.verifiedRawBytesBeforeRestart < result.decoder.verifiedRawBytes);
    REQUIRE(result.decoder.resumeStateLoaded);
    REQUIRE(result.decoder.resumeVerificationSucceeded == true);
    REQUIRE(result.decoder.budgetBoundDecoderAdmission == options.budgetBoundDecoders);
    REQUIRE(result.decoder.outerActiveDecoderLimit == (options.budgetBoundDecoders ? pbapp::MakeBudgetBoundUnifiedReceiverResourcePolicy().maxActiveOuterFecDecoders : 8));
    REQUIRE(result.decoder.outerTotalDecoderByteLimit == (options.memoryBudget ? options.memoryBudget->totalDecoderBytes : pbapp::MakeUnifiedReceiverResourcePolicy().maxTotalOuterFecDecoderBytes));
    REQUIRE(result.decoder.outerPerDecoderByteLimit == (options.memoryBudget ? options.memoryBudget->perDecoderBytes : pbapp::MakeUnifiedReceiverResourcePolicy().maxOuterFecDecoderBytes));
    REQUIRE(result.decoder.receiverResumeByteLimit == (options.memoryBudget ? pbapp::CalculateDecoderResumeBudgetBytes(*options.memoryBudget) : pbapp::MakeUnifiedReceiverResourcePolicy().maxResumeBytes));
    REQUIRE(result.decoder.customDecoderMemoryBudget == options.memoryBudget.has_value());
    REQUIRE(result.decoder.finalReopenVerified == true);
    REQUIRE(result.independentlyReopenedEqual);
    REQUIRE(result.peakReceiverDecoders <= 8);
    REQUIRE(result.peakReceiverOrphanBytes == 0);
    REQUIRE(result.decoder.outerConflictRejections == 0);
    REQUIRE(result.decoder.outerResourceRejections == 0);
    std::cout << "PAM4 offline restart generated=" << result.generatedFrames << " priorVerified=" << result.verifiedRawBytesBeforeRestart
        << " resumeLoaded=" << result.decoder.resumeStateLoaded << " reopen=" << result.independentlyReopenedEqual << '\n';
}

TEST_CASE("PAM4 temporal sweep avoids persistent segment parity under regular frame decimation", "[pam4][scheduler][phase]")
{
    const auto profile = GENERATE(pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::UnifiedGrayFast, pbapp::VisualProfile::UnifiedLc4);
    pbapp::MixedSlotTemporalOrderProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeMixedSlotTemporalOrder(profile, result);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(result.peakSenderSegments <= 6);
    const std::size_t phaseHold = profile == pbapp::VisualProfile::UnifiedLc4 ? 32 : 1;
    for (std::size_t frameIndex = 0; frameIndex < result.segmentOrdinals.size(); frameIndex++)
    {
        CAPTURE(frameIndex, result.segmentOrdinals[frameIndex], phaseHold);
        REQUIRE(result.segmentOrdinals[frameIndex] == (frameIndex / 6 / phaseHold + frameIndex % 6) % 6);
    }
    if (profile == pbapp::VisualProfile::ExperimentalPam4)
    {
        // Skip the twelve startup visits per Segment. In the next eighteen
        // sweeps, keeping either parity or any third phase must serve all six
        // Segments equally. This is a deterministic loss model, not field FPS.
        for (const std::size_t decimation : {2U, 3U})
        {
            for (std::size_t phase = 0; phase < decimation; phase++)
            {
                std::array<std::uint32_t, 6> receivedEquations{};
                for (std::size_t frameIndex = 72; frameIndex < 180; frameIndex++)
                {
                    REQUIRE(result.scheduledEquations[frameIndex] == 10);
                    if (frameIndex % decimation == phase)
                    {
                        receivedEquations[result.segmentOrdinals[frameIndex]] += result.scheduledEquations[frameIndex];
                    }
                }
                for (const std::uint32_t equations : receivedEquations)
                {
                    REQUIRE(equations == 180 / decimation);
                }
            }
        }
    }
    pbapp::MixedSlotTemporalOrderProbeSnapshot rejected;
    REQUIRE_FALSE(pbapp::ApplicationRuntimeTestAccess::ProbeMixedSlotTemporalOrder(pbapp::VisualProfile::RemoteVisualLowFps, rejected));
}

TEST_CASE("PAM4 capture admission reserves only bounded CPU workspace staging and compact results", "[pam4][capture][budget]")
{
    pbdemodd3d11::CaptureDemodulatorConfig config;
    config.visualProfileId = pbprotocol::kPam4ExperimentalProfile.visualProfileId;
    config.slotCount = 2;
    config.resultQueueCapacity = 2;
    config.maximumRoiWidth = 1920;
    config.maximumRoiHeight = 1080;
    pbdemodd3d11::CaptureDemodulatorBudget budget;
    REQUIRE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(config, budget));
    REQUIRE(budget.demodulatorBytes == pbmodulation::ExperimentalPam4CpuDecoder::RequiredBytes());
    REQUIRE(budget.bootstrapStagingBytes == 1920ULL * 1080 * 4 * 2);
    REQUIRE(budget.referenceScratchBytes == sizeof(pbdemodd3d11::CaptureDemodulatorResult));
    REQUIRE(budget.resultQueueBytes == 2 * sizeof(pbdemodd3d11::CaptureDemodulatorResult));
    const auto original = budget;
    config.maximumResidentBytes = budget.totalBytes - 1;
    REQUIRE_FALSE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(config, budget));
    REQUIRE(budget == original);
    config.maximumResidentBytes = original.totalBytes;
    REQUIRE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(config, budget));
    config.evaluationMode = pbdesktoplevels::EvaluationMode::DiagnosticTruth;
    REQUIRE_FALSE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(config, budget));
    REQUIRE(budget == original);
}

TEST_CASE("Capture retirement distinguishes CPU-only PAM4 from GPU shutdown and rejects outstanding work", "[pam4][capture-shutdown]")
{
    pbdemodd3d11::CaptureDemodulatorSnapshot snapshot;
    REQUIRE_FALSE(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(snapshot));
    snapshot.pam4CpuReference = true;
    REQUIRE_FALSE(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(snapshot));
    snapshot.domainStarts = 1;
    snapshot.active = true;
    REQUIRE_FALSE(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(snapshot));
    snapshot.active = false;
    CHECK(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(snapshot));
    snapshot.pendingFrames = 1;
    REQUIRE_FALSE(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(snapshot));
    snapshot.pendingFrames = 0;
    snapshot.queuedResults = 1;
    REQUIRE_FALSE(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(snapshot));
    snapshot.queuedResults = 0;
    CHECK(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(snapshot));

    snapshot.pam4CpuReference = false;
    REQUIRE_FALSE(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(snapshot));
    snapshot.demodulator.shutdown = true;
    REQUIRE(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(snapshot));
    snapshot.active = true;
    CHECK_FALSE(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(snapshot));
    snapshot.active = false;
    snapshot.pendingFrames = 1;
    REQUIRE_FALSE(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(snapshot));
    snapshot.pendingFrames = 0;
    snapshot.queuedResults = 1;
    REQUIRE_FALSE(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(snapshot));
}

TEST_CASE("PAM4 native offline WARP staging matches CPU bytes and survives capture-domain recreation", "[pam4][capture][file][offline-warp]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"native.bin";
    WriteFixture(source, 150000, false);
    pbapp::ExperimentalPam4FileProbeOptions options;
    options.nativeReadback = true;
    options.recreateDomainAfterFrames = 8;
    pbapp::ExperimentalPam4FileProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(),
        scratch.Path() / L"state", (scratch.Path() / L"output").wstring(), options, result);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(result.independentlyReopenedEqual);
    REQUIRE(result.nativeReadbackMatchedReference);
    REQUIRE(result.nativeReadback.pam4CpuReference);
    REQUIRE(result.nativeReadback.domainStarts == 2);
    REQUIRE(result.nativeReadback.invalidations == 2);
    REQUIRE(result.nativeReadback.pam4FrameErasures == 0);
    REQUIRE(result.nativeReadback.pam4DecodedObservations == result.decodedFrames);
    REQUIRE(result.nativeReadback.bootstrapReadbackBytes == result.decodedFrames * 1920ULL * 1080 * 4);
    REQUIRE(result.nativeReadback.stagedGpuSubmissions == 0);
    REQUIRE(result.nativeReadback.stagedGpuCompletions == 0);
    REQUIRE(result.nativeReadback.pendingFrames == 0);
    REQUIRE(result.nativeReadback.queuedResults == 0);
    REQUIRE_FALSE(result.nativeReadback.active);
    REQUIRE_FALSE(result.nativeReadback.demodulator.shutdown);
    REQUIRE(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(result.nativeReadback));
    REQUIRE(result.decoder.captureEpochResets == 1);
    std::cout << "PAM4 offline WARP decoded=" << result.decodedFrames << " domains=" << result.nativeReadback.domainStarts
        << " reservation=" << result.nativeReadback.reservation.totalBytes << " reopen=" << result.independentlyReopenedEqual << '\n';
}

TEST_CASE("PAM4 explicit live configuration preserves safety and rejects unrelated experimental flags", "[pam4][configuration]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"source.bin";
    WriteFixture(source, 1024, false);
    pbapp::EncoderConfig encoder;
    encoder.sourcePath = source.wstring();
    encoder.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
    encoder.compressionEnabled = true;
    encoder.compressionLevel = 3;
    encoder.controlRepetitions = 4;
    encoder.logicalVisualFps = 30;
    encoder.singleMonitorFullscreen = TestMonitor(false);
    encoder.monitorClientOrigin = pbrenderd3d::PhysicalPoint{0, 0};
    encoder.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    encoder.remoteMetadata.remoteProvider = "OfflineValidationOnly";
    encoder.remoteMetadata.experimentMonitorIdentity = R"(\\.\DISPLAY2)";
    REQUIRE(pbapp::ValidateEncoderConfig(encoder));
    for (const std::uint32_t segmentBytes : {1U * 1024 * 1024, 8U * 1024 * 1024})
    {
        auto candidate = encoder;
        candidate.segmentTargetBytes = segmentBytes;
        REQUIRE(pbapp::ValidateEncoderConfig(candidate));
    }
    for (const std::uint32_t segmentBytes : {1U, 1024U * 1024 - 1, 8U * 1024 * 1024 + 1, 15U * 1024 * 1024})
    {
        auto candidate = encoder;
        candidate.segmentTargetBytes = segmentBytes;
        REQUIRE_FALSE(pbapp::ValidateEncoderConfig(candidate));
    }
    for (std::uint32_t invalidCase = 0; invalidCase < 12; invalidCase++)
    {
        auto candidate = encoder;
        switch (invalidCase)
        {
        case 0: candidate.singleMonitorFullscreen.reset(); break;
        case 1: candidate.monitorClientOrigin.reset(); break;
        case 2: candidate.remoteMetadata.channelType = pbapp::ChannelType::LocalDesktop; break;
        case 3: candidate.fullscreenNativeSize = true; break;
        case 4: candidate.fullscreenRasterWidth = 1920; break;
        case 5: candidate.fullscreenSampling = pbapp::FullscreenSamplingMode::Linear; break;
        case 6: candidate.logicalVisualFps = 0; break;
        case 7: candidate.logicalVisualFps = 61; break;
        case 8: candidate.grayFastSpatialInterleave = true; break;
        case 9: candidate.compressionEnabled = false; break;
        case 10: candidate.measurement = std::make_shared<pbapp::RunMeasurementRecorder>(); break;
        case 11: candidate.remoteMetadata.remoteProvider.clear(); break;
        }
        INFO(invalidCase);
        REQUIRE_FALSE(pbapp::ValidateEncoderConfig(candidate));
    }

    const auto decoder = DecoderConfiguration(scratch);
    REQUIRE_FALSE(decoder.budgetBoundDecoders);
    REQUIRE(pbapp::ValidateDecoderConfig(decoder));
    auto budgetedDecoder = decoder;
    budgetedDecoder.budgetBoundDecoders = true;
    REQUIRE(pbapp::ValidateDecoderConfig(budgetedDecoder));
    for (const RECT rect : {RECT{0, 0, 1440, 810}, RECT{0, 0, 3840, 2160}})
    {
        auto candidate = decoder;
        candidate.region.physicalRect = rect;
        REQUIRE(pbapp::ValidateDecoderConfig(candidate));
    }
    for (std::uint32_t invalidCase = 0; invalidCase < 12; invalidCase++)
    {
        auto candidate = decoder;
        switch (invalidCase)
        {
        case 0: candidate.monitorSafety.reset(); break;
        case 1: candidate.captureBackend = pbapp::CaptureBackend::Wgc; break;
        case 2: candidate.region.physicalRect.right = 1439; break;
        case 3: candidate.region.physicalRect.bottom = 809; break;
        case 4: candidate.region.physicalRect.right = 3841; break;
        case 5: candidate.remoteMetadata.channelType = pbapp::ChannelType::LocalDesktop; break;
        case 6: candidate.remoteMetadata.protectedMonitorIdentity = "wrong"; break;
        case 7: candidate.budgetBoundDecoders = true; candidate.measurement = std::make_shared<pbapp::RunMeasurementRecorder>(); break;
        case 8: candidate.replayInputPath = (scratch.Path() / L"missing.replay").wstring(); break;
        case 9: candidate.replayOutputPath = (scratch.Path() / L"new.replay").wstring(); break;
        case 10: candidate.diagnosticCaptureOnly = true; break;
        case 11: candidate.measurement = std::make_shared<pbapp::RunMeasurementRecorder>(); break;
        }
        INFO(invalidCase);
        REQUIRE_FALSE(pbapp::ValidateDecoderConfig(candidate));
    }
    REQUIRE(std::filesystem::is_empty(scratch.Path() / L"output"));
}

TEST_CASE("PAM4 configuration is bounded and public runtime rejects stale topology before opening capture", "[pam4][runtime][configuration]")
{
    Scratch scratch;
    std::promise<pbdemodd3d11::CaptureDemodulatorConfig> capturedConfig;
    auto future = capturedConfig.get_future();
    std::atomic<bool> captureOpened = false;
    pbapp::DecoderRuntimeServices services;
    services.captureFactory = [&](pbapp::CaptureBackend) -> std::unique_ptr<pbapp::DecoderCaptureSession>
    {
        captureOpened = true;
        throw std::runtime_error("Test must stop before any capture factory");
    };
    services.demodulatorFactory = [&](const pbdemodd3d11::CaptureDemodulatorConfig& config, std::shared_ptr<pbapp::DecoderDemodulator>&)
    {
        capturedConfig.set_value(config);
        return pbcapturenormalize::CaptureStatus::Failure(pbcapturenormalize::CaptureError::NativeFailure,
            pbcapturenormalize::CaptureStage::Configuration);
    };
    pbapp::DecoderRuntime runtime(std::move(services));
    auto decoderConfig = DecoderConfiguration(scratch);
    // Impossible test identity: it must not reach either injected factory.
    decoderConfig.monitorSafety->experimentMonitor.deviceName = L"PB-IMPOSSIBLE-TEST-MONITOR";
    decoderConfig.remoteMetadata.experimentMonitorIdentity = "PB-IMPOSSIBLE-TEST-MONITOR";
    REQUIRE(runtime.Start(decoderConfig));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (runtime.GetSnapshot().state != pbapp::DecoderState::Failed && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(runtime.GetSnapshot().state == pbapp::DecoderState::Failed);
    REQUIRE(runtime.GetSnapshot().errorDetail.find("display topology changed before RemoteVisual startup") != std::string::npos);
    REQUIRE(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    runtime.Stop();
    REQUIRE_FALSE(captureOpened);
    pbcapturenormalize::CaptureNormalizeConfig capture;
    pbdemodd3d11::CaptureDemodulatorConfig config;
    REQUIRE(pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4CaptureConfig(DecoderConfiguration(scratch), capture, config));
    REQUIRE(capture.capture.roiTextureCount == 2);
    REQUIRE(capture.capture.maximumInFlightFrames == 1);
    REQUIRE(capture.capture.maximumFrameAgeMilliseconds == 250);
    REQUIRE(config.visualProfileId == pbprotocol::kPam4ExperimentalProfile.visualProfileId);
    REQUIRE(config.maximumRoiWidth == 2560);
    REQUIRE(config.maximumRoiHeight == 1440);
    REQUIRE(config.slotCount == 2);
    REQUIRE(config.maximumFrameAgeMilliseconds == 250);
    REQUIRE_FALSE(config.offlinePixelsOnly);
    pbdemodd3d11::CaptureDemodulatorBudget budget;
    REQUIRE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(config, budget));
    REQUIRE(budget.demodulatorBytes == pbmodulation::ExperimentalPam4CpuDecoder::RequiredBytes());
    REQUIRE(budget.bootstrapStagingBytes == 2560ULL * 1440 * 4 * 2);
    REQUIRE(runtime.GetSnapshot().state == pbapp::DecoderState::Failed);
    REQUIRE(std::filesystem::is_empty(scratch.Path() / L"output"));
}

TEST_CASE("PAM4 report separates CPU readback observations and mixed FEC from certified throughput", "[pam4][report]")
{
    pbapp::DecoderSnapshot decoder;
    decoder.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
    decoder.pam4CpuReference = true;
    decoder.pam4DecodedObservations = 17;
    decoder.pam4FrameErasures = 2;
    decoder.pam4ReadbackDecodeWallTotal100ns = 1900000;
    decoder.pam4ReadbackDecodeWallHighWater100ns = 230000;
    decoder.admittedFrameSequenceFps = 8.0;
    const auto report = pbapp::BuildDecoderRunReportJson({}, decoder);
    REQUIRE(report.find("\"cpuReadback\":{\"active\":true,\"decodedObservations\":17,\"frameErasures\":2") != std::string::npos);
    REQUIRE(report.find("\"wallTotal100ns\":1900000") != std::string::npos);
    REQUIRE(report.find("\"admittedFrameSequenceFps\":8") != std::string::npos);
    REQUIRE(report.find("\"stageCounters\":") != std::string::npos);
    REQUIRE(report.find("\"captureFlow\":") != std::string::npos);
    REQUIRE(report.find("\"measurement\":") == std::string::npos);
    REQUIRE(report.find("\"unifiedTelemetry\":null") != std::string::npos);
    REQUIRE(report.find("\"denominatorObservedUniqueFrames\":null") != std::string::npos);
    pbapp::EncoderSnapshot encoder;
    encoder.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
    encoder.codewordsPerFrame = 11;
    encoder.innerFecInformationBytesPerLogicalFrame = 18000;
    encoder.transportPayloadCeilingBytesPerLogicalFrame = 16290;
    const auto encoderReport = pbapp::BuildEncoderRunReportJson({}, encoder);
    REQUIRE(encoderReport.find("\"experimentalPam4\":{\"controlSlots\":1,\"transportSlots\":10") != std::string::npos);
    REQUIRE(encoderReport.find("\"informationBytesPerFrame\":18000") != std::string::npos);
    REQUIRE(encoderReport.find("\"transportPayloadCeilingBytesPerFrame\":16290") != std::string::npos);
    REQUIRE(encoderReport.find("\"innerFec\":\"Robust Control; Fast Transport\"") != std::string::npos);
    REQUIRE(encoderReport.find("\"verifiedGoodput\":null") != std::string::npos);
}
