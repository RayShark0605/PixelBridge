#include "application_model.h"
#include "local_desktop_runtime.h"
#include "run_report.h"
#include "sender_carousel_scheduler.h"
#include "pbmodulation/experimental_pam4_wide.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
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
        path_ = root / (L"pam4-wide-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(nextOrdinal.fetch_add(1)));
        REQUIRE(path_.parent_path() == root);
        REQUIRE(std::filesystem::create_directory(path_));
        REQUIRE(std::filesystem::create_directory(path_ / L"output"));
        REQUIRE(std::filesystem::create_directory(path_ / L"state"));
    }
    ~Scratch()
    {
        const auto root = std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal();
        if (path_.parent_path() == root && path_.filename().wstring().starts_with(L"pam4-wide-"))
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

void WriteFixture(const std::filesystem::path& path, const std::size_t size, const bool compressible = false)
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
    monitor.physicalRect = protectedMonitor ? RECT{-2560, 0, 0, 1440} : RECT{0, 0, 2560, 1600};
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
    config.visualProfile = pbapp::VisualProfile::ExperimentalPam4Wide;
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

pbapp::ExperimentalPam4FileProbeOptions WideOptions()
{
    pbapp::ExperimentalPam4FileProbeOptions options;
    options.visualProfile = pbapp::VisualProfile::ExperimentalPam4Wide;
    return options;
}

void CheckComplete(const pbapp::ExperimentalPam4FileProbeSnapshot& result, const std::size_t rawBytes)
{
    REQUIRE(result.sourceStable);
    REQUIRE(result.independentlyReopenedEqual);
    REQUIRE(result.decoder.finalReopenVerified == true);
    REQUIRE(result.decoder.wholeFileDigestVerified);
    REQUIRE(result.decoder.finalPublishSucceeded);
    REQUIRE(result.decoder.verifiedRawBytes == rawBytes);
    REQUIRE(result.decoder.visualProfileId == pbprotocol::kPam4WideExperimentalProfile.visualProfileId);
    REQUIRE(result.decoder.visualLayoutVersion == 16);
    REQUIRE(result.decoder.codewordsPerFrame == 20);
    REQUIRE(result.decoder.codedDataBytesPerFrame == 40500);
    REQUIRE(result.peakSenderSegments <= 6);
    REQUIRE(result.peakReceiverDecoders <= 8);
    REQUIRE(result.peakReceiverOrphanBytes == 0);
    REQUIRE(result.decoder.outerConflictRejections == 0);
    REQUIRE(result.decoder.outerResourceRejections == 0);
    REQUIRE(result.decoder.pam4EvaluatedSlots == result.decoder.pam4AvailableObservations * 20);
}

}

TEST_CASE("Wide requires an independent explicit token and exact twenty-slot layout", "[pam4-wide][scheduler][configuration]")
{
    REQUIRE(pbapp::senderUnifiedMaximumCodewordSlotCount == 18);
    REQUIRE(pbmodulation::kUnifiedMaximumFrameSlotCount == 18);
    REQUIRE(pbapp::maximumMixedFrameSlotCount == 20);
    REQUIRE_FALSE(pbapp::IsUnifiedVisualFamily(pbapp::VisualProfile::ExperimentalPam4Wide));
    REQUIRE(pbapp::UsesMixedSlotCarousel(pbapp::VisualProfile::ExperimentalPam4Wide));
    REQUIRE(pbapp::ParseVisualProfileToken(std::string_view("experimental-pam4-wide")) == pbapp::VisualProfile::ExperimentalPam4Wide);
    REQUIRE(pbapp::ParseVisualProfileToken(std::wstring_view(L"experimental-pam4-wide")) == pbapp::VisualProfile::ExperimentalPam4Wide);
    REQUIRE(std::string_view(pbapp::GetVisualProfileName(pbapp::VisualProfile::ExperimentalPam4Wide)) == "PB-Experimental-Pam4-Wide-1");
    REQUIRE_FALSE(pbapp::ParseVisualProfileToken(std::string_view("pam4-wide")));
    for (const auto& option : pbapp::GetVisualProfileOptions())
    {
        REQUIRE_FALSE(pbapp::IsExperimentalPam4Family(option.profile));
    }
    pbapp::SenderUnifiedCarouselSchedulerConfig config;
    config.frameCodewordSlots = 20;
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create(config, scheduler));
    config.slotLayout = pbapp::SenderMixedSlotLayout::ExperimentalPam4;
    REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create(config, scheduler));
    config.slotLayout = pbapp::SenderMixedSlotLayout::ExperimentalPam4Wide;
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create(config, scheduler));
    for (const std::uint32_t count : {0U, 1U, 11U, 15U, 18U, 19U, 21U, (std::numeric_limits<std::uint32_t>::max)()})
    {
        config.frameCodewordSlots = count;
        REQUIRE_FALSE(pbapp::SenderUnifiedCarouselScheduler::Create(config, scheduler));
    }
}

TEST_CASE("Wide schedules nineteen distinct equations only after the same-frame descriptor", "[pam4-wide][scheduler]")
{
    const std::uint32_t count = GENERATE(0U, 1U, 21U, 645U);
    const std::uint64_t pass = GENERATE(0ULL, 1ULL);
    CAPTURE(count, pass);
    pbapp::SenderUnifiedCarouselSchedulerConfig config;
    config.frameCodewordSlots = 20;
    config.slotLayout = pbapp::SenderMixedSlotLayout::ExperimentalPam4Wide;
    config.systematicBlockCount = count;
    config.wirehair = count >= 2;
    config.carouselPass = pass;
    config.logicalFramesPerSecond = 30;
    pbapp::SenderUnifiedCarouselScheduler scheduler;
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create(config, scheduler));
    std::set<std::uint64_t> equations;
    std::uint32_t maximumEquations = 0;
    for (std::uint64_t frameIndex = 0; frameIndex < 2000 && !scheduler.IsComplete(); frameIndex++)
    {
        pbapp::SenderUnifiedScheduledFrame frame;
        REQUIRE(scheduler.PrepareFrameAt(frameIndex, frameIndex * 33333333, frame));
        const auto snapshot = scheduler.GetSnapshot();
        pbapp::SenderUnifiedScheduledFrame retry;
        REQUIRE(scheduler.PrepareFrameAt(frameIndex, frameIndex * 33333333, retry));
        REQUIRE(retry == frame);
        REQUIRE(snapshot == scheduler.GetSnapshot());
        REQUIRE(frame.slotCount == 20);
        REQUIRE(frame.controlSlotCount == 1);
        REQUIRE(frame.transportSlotCount == 19);
        std::array<pbmodulation::UnifiedSlotAssignment, 20> assignments{};
        for (std::uint32_t slot = 0; slot < frame.slotCount; slot++)
        {
            assignments[slot] = frame.slots[slot].assignment;
            const auto& scheduled = frame.slots[slot];
            if (scheduled.transportDisposition == pbapp::SenderUnifiedTransportSlotDisposition::ScheduledEquation)
            {
                REQUIRE(frame.slots[0].assignment.controlPriority == pbmodulation::UnifiedControlPriority::CurrentSegmentDescriptor);
                REQUIRE(equations.insert(scheduled.equationIndex).second);
                REQUIRE(scheduled.repairEquation == (scheduled.equationIndex >= snapshot.systematicEquationCount));
            }
        }
        REQUIRE(pbmodulation::ValidateExperimentalPam4WideSlotPlan(assignments));
        maximumEquations = std::max(maximumEquations, frame.scheduledEquationCount);
        REQUIRE(scheduler.CommitPreparedFrame());
    }
    REQUIRE(scheduler.IsComplete());
    REQUIRE(scheduler.GetSnapshot().committedEquationCount == equations.size());
    REQUIRE(scheduler.GetSnapshot().scheduledEquationCount == equations.size());
    // The unchanged high-rate incremental repair policy has a sixteen-symbol
    // minimum: K=21/pass1 ends before filling a whole nineteen-symbol frame.
    const std::uint32_t expectedPeak = count == 21 && pass == 1 ? 16U : count >= 21 ? 19U : count;
    REQUIRE(maximumEquations == expectedPeak);
}

TEST_CASE("Wide actual compositor pixels recover zero tiny multisegment and compressed files", "[pam4-wide][file]")
{
    const std::uint32_t fixtureIndex = GENERATE(0U, 1U, 2U, 3U, 4U);
    constexpr std::array<std::pair<std::size_t, bool>, 5> fixtures{{{0, false}, {1024, false}, {150000, false},
        {2U * 1024U * 1024U + 1024U, false}, {2U * 1024U * 1024U + 1024U, true}}};
    const auto fixture = fixtures[fixtureIndex];
    Scratch scratch;
    const auto source = scratch.Path() / L"fixture.bin";
    WriteFixture(source, fixture.first, fixture.second);
    const auto options = WideOptions();
    pbapp::ExperimentalPam4FileProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(), scratch.Path() / L"state",
        (scratch.Path() / L"output").wstring(), options, result);
    INFO(status.message);
    REQUIRE(status);
    CheckComplete(result, fixture.first);
    if (fixture.first != 0)
    {
        REQUIRE(result.maximumDataBlocksPerFrame == 19);
        if (fixture.first >= 150000 && !fixture.second)
        {
            REQUIRE(result.lastSlotReceiverCalled);
        }
        if (fixture.first == 1024)
        {
            // DirectRepeat can finish in slot1. Later slots must not mutate
            // the receiver after whole-file publication just for coverage.
            REQUIRE_FALSE(result.lastSlotReceiverCalled);
        }
    }
    std::cout << "Wide pixels bytes=" << fixture.first << " compressed=" << fixture.second << " frames=" << result.generatedFrames
        << " segments=" << result.segmentCount << " lastSlot=" << result.lastSlotReceiverCalled << '\n';
}

TEST_CASE("Wide bounded late join loss and duplicate observations still finish independent file reopen", "[pam4-wide][file][loss]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"loss.bin";
    constexpr std::size_t bytes = 2U * 1024U * 1024U + 1024U;
    WriteFixture(source, bytes);
    auto options = WideOptions();
    options.joinAfterFrames = 40;
    options.dropEveryFrames = 5;
    options.duplicateEveryFrames = 7;
    pbapp::ExperimentalPam4FileProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(), scratch.Path() / L"state",
        (scratch.Path() / L"output").wstring(), options, result);
    INFO(status.message);
    REQUIRE(status);
    CheckComplete(result, bytes);
    REQUIRE(result.skippedFrames >= 40);
    REQUIRE(result.duplicateObservations != 0);
    REQUIRE(result.decoder.pam4FrameObservations == result.decodedFrames + result.duplicateObservations);
}

TEST_CASE("Wide corrupt handoffs reject before allocating file output", "[pam4-wide][negative]")
{
    const auto fault = GENERATE(pbapp::ExperimentalPam4ProbeFault::ForeignObservationIdentity,
        pbapp::ExperimentalPam4ProbeFault::DuplicateControlSlot, pbapp::ExperimentalPam4ProbeFault::OversizedBlockCount,
        pbapp::ExperimentalPam4ProbeFault::ForeignSlotCount, pbapp::ExperimentalPam4ProbeFault::ForeignResultKind,
        pbapp::ExperimentalPam4ProbeFault::SlotOutOfRange, pbapp::ExperimentalPam4ProbeFault::OtherProfileBlocks);
    Scratch scratch;
    const auto source = scratch.Path() / L"rejected.bin";
    WriteFixture(source, 1024);
    auto options = WideOptions();
    options.handoffFaultForTest = fault;
    pbapp::ExperimentalPam4FileProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(), scratch.Path() / L"state",
        (scratch.Path() / L"output").wstring(), options, result);
    REQUIRE_FALSE(status);
    REQUIRE_FALSE(status.message.empty());
    REQUIRE_FALSE(result.independentlyReopenedEqual);
    REQUIRE(std::filesystem::is_empty(scratch.Path() / L"output"));
}

TEST_CASE("Wide last slot corruption cannot publish a file", "[pam4-wide][negative]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"last-slot.bin";
    WriteFixture(source, 150000);
    auto options = WideOptions();
    options.handoffFaultForTest = pbapp::ExperimentalPam4ProbeFault::CorruptLastSlotTransport;
    pbapp::ExperimentalPam4FileProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(), scratch.Path() / L"state",
        (scratch.Path() / L"output").wstring(), options, result);
    REQUIRE_FALSE(status);
    REQUIRE(status.message.find("Transport independent parse failed") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(scratch.Path() / L"output" / L"last-slot.bin"));
}

TEST_CASE("Wide resumed partial files retain explicit memory budgets and final verification", "[pam4-wide][file][resume]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"restart.bin";
    constexpr std::size_t bytes = 2U * 1024U * 1024U + 4096U;
    WriteFixture(source, bytes);
    auto options = WideOptions();
    options.restartReceiverAfterFrames = 55;
    options.budgetBoundDecoders = true;
    options.memoryBudget = pbapp::DecoderMemoryBudget{2048 * pbapp::decoderMemoryMebibyte, 1024 * pbapp::decoderMemoryMebibyte};
    pbapp::ExperimentalPam4FileProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(), scratch.Path() / L"state",
        (scratch.Path() / L"output").wstring(), options, result);
    INFO(status.message);
    REQUIRE(status);
    CheckComplete(result, bytes);
    REQUIRE(result.receiverRestarts == 1);
    REQUIRE(result.verifiedRawBytesBeforeRestart > 0);
    REQUIRE(result.verifiedRawBytesBeforeRestart < bytes);
    REQUIRE(result.decoder.resumeStateLoaded);
    REQUIRE(result.decoder.resumeVerificationSucceeded == true);
    REQUIRE(result.decoder.budgetBoundDecoderAdmission);
    REQUIRE(result.decoder.customDecoderMemoryBudget);
    REQUIRE(result.decoder.outerTotalDecoderByteLimit == options.memoryBudget->totalDecoderBytes);
    REQUIRE(result.decoder.outerPerDecoderByteLimit == options.memoryBudget->perDecoderBytes);
    REQUIRE(result.decoder.receiverResumeByteLimit == pbapp::CalculateDecoderResumeBudgetBytes(*options.memoryBudget));
}

TEST_CASE("Wide WARP mapped staging returns all twenty slots and retires domain leases", "[pam4-wide][capture][file][offline-warp]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"native.bin";
    WriteFixture(source, 150000);
    auto options = WideOptions();
    options.nativeReadback = true;
    options.recreateDomainAfterFrames = 8;
    pbapp::ExperimentalPam4FileProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(), scratch.Path() / L"state",
        (scratch.Path() / L"output").wstring(), options, result);
    INFO(status.message);
    REQUIRE(status);
    CheckComplete(result, 150000);
    REQUIRE(result.nativeReadbackMatchedReference);
    REQUIRE(result.nativeReadback.pam4CpuReference);
    REQUIRE(result.nativeReadback.domainStarts == 2);
    REQUIRE(result.nativeReadback.invalidations == 2);
    REQUIRE(result.nativeReadback.pam4FrameErasures == 0);
    REQUIRE(result.nativeReadback.pam4DecodedObservations == result.decodedFrames);
    REQUIRE(result.nativeReadback.bootstrapReadbackBytes == result.decodedFrames * 2560ULL * 1600 * 4);
    REQUIRE(result.nativeReadback.stagedGpuSubmissions == 0);
    REQUIRE(result.nativeReadback.stagedGpuCompletions == 0);
    REQUIRE(pbdemodd3d11::AreCaptureDemodulatorResourcesRetired(result.nativeReadback));
    REQUIRE(result.decoder.captureEpochResets == 1);
    REQUIRE(result.lastSlotReceiverCalled);
}

TEST_CASE("Wide capture resource accounting includes the enlarged result and precise CPU workspace", "[pam4-wide][capture][budget]")
{
    Scratch scratch;
    pbcapturenormalize::CaptureNormalizeConfig capture;
    pbdemodd3d11::CaptureDemodulatorConfig config;
    REQUIRE(pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4CaptureConfig(DecoderConfiguration(scratch), capture, config));
    REQUIRE(config.visualProfileId == pbprotocol::kPam4WideExperimentalProfile.visualProfileId);
    REQUIRE(config.slotCount == 2);
    REQUIRE(capture.capture.roiTextureCount == 2);
    pbdemodd3d11::CaptureDemodulatorBudget budget;
    REQUIRE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(config, budget));
    REQUIRE(budget.demodulatorBytes == pbmodulation::ExperimentalPam4WideCpuDecoder::RequiredBytes());
    REQUIRE(budget.referenceScratchBytes == sizeof(pbdemodd3d11::CaptureDemodulatorResult));
    REQUIRE(budget.resultQueueBytes == config.resultQueueCapacity * sizeof(pbdemodd3d11::CaptureDemodulatorResult));
    REQUIRE(budget.bootstrapStagingBytes == 2560ULL * 1440 * 4 * 2);
    const auto original = budget;
    config.maximumResidentBytes = original.totalBytes - 1;
    REQUIRE_FALSE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(config, budget));
    REQUIRE(budget == original);
    config.maximumResidentBytes = original.totalBytes;
    REQUIRE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(config, budget));
    config.evaluationMode = pbdesktoplevels::EvaluationMode::DiagnosticTruth;
    REQUIRE_FALSE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(config, budget));
}

TEST_CASE("Wide fixed presentation rejects incompatible widths sizes sampling and display authority", "[pam4-wide][configuration]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"source.bin";
    WriteFixture(source, 1024);
    pbapp::EncoderConfig config;
    config.sourcePath = source.wstring();
    config.visualProfile = pbapp::VisualProfile::ExperimentalPam4Wide;
    config.compressionEnabled = true;
    config.compressionLevel = 3;
    config.controlRepetitions = 4;
    config.logicalVisualFps = 30;
    config.singleMonitorFullscreen = TestMonitor(false);
    config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{0, 0};
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "OfflineValidationOnly";
    config.remoteMetadata.experimentMonitorIdentity = R"(\\.\DISPLAY2)";
    config.fullscreenRasterWidth = 2560;
    REQUIRE(pbapp::ValidateEncoderConfig(config));
    for (std::uint32_t invalid = 0; invalid < 9; invalid++)
    {
        auto candidate = config;
        switch (invalid)
        {
        case 0: candidate.fullscreenRasterWidth = 0; break;
        case 1: candidate.fullscreenRasterWidth = 1920; break;
        case 2: candidate.fullscreenRasterWidth = 2576; break;
        case 3: candidate.fullscreenNativeSize = true; break;
        case 4: candidate.fullscreenSampling = pbapp::FullscreenSamplingMode::Linear; break;
        case 5: candidate.singleMonitorFullscreen->physicalRect.right = 2559; break;
        case 6: candidate.singleMonitorFullscreen->physicalRect.bottom = 1439; break;
        case 7: candidate.singleMonitorFullscreen.reset(); break;
        case 8: candidate.measurement = std::make_shared<pbapp::RunMeasurementRecorder>(); break;
        }
        CAPTURE(invalid);
        REQUIRE_FALSE(pbapp::ValidateEncoderConfig(candidate));
    }
    auto decoder = DecoderConfiguration(scratch);
    REQUIRE(pbapp::ValidateDecoderConfig(decoder));
    decoder.budgetBoundDecoders = true;
    REQUIRE(pbapp::ValidateDecoderConfig(decoder));
    decoder.captureBackend = pbapp::CaptureBackend::Wgc;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(decoder));
}

TEST_CASE("Wide reports exact independent capacities without certified frame metrics", "[pam4-wide][report]")
{
    pbapp::DecoderSnapshot decoder;
    decoder.visualProfile = pbapp::VisualProfile::ExperimentalPam4Wide;
    decoder.pam4AvailableObservations = 4;
    decoder.pam4EvaluatedSlots = 80;
    const auto decoded = pbapp::BuildDecoderRunReportJson({}, decoder);
    REQUIRE(decoded.find("\"experimentalPam4Wide\":{\"slotsPerAvailableObservation\":20") != std::string::npos);
    REQUIRE(decoded.find("\"evaluatedSlots\":80") != std::string::npos);
    REQUIRE(decoded.find("\"experimentalPam4\":") == std::string::npos);
    REQUIRE(decoded.find("\"unifiedTelemetry\":null") != std::string::npos);
    REQUIRE(decoded.find("\"denominatorObservedUniqueFrames\":null") != std::string::npos);
    pbapp::EncoderSnapshot encoder;
    encoder.visualProfile = pbapp::VisualProfile::ExperimentalPam4Wide;
    encoder.codewordsPerFrame = 20;
    encoder.innerFecInformationBytesPerLogicalFrame = 32985;
    encoder.transportPayloadCeilingBytesPerLogicalFrame = 30951;
    const auto encoded = pbapp::BuildEncoderRunReportJson({}, encoder);
    REQUIRE(encoded.find("\"experimentalPam4Wide\":{\"controlSlots\":1,\"transportSlots\":19,\"nativeCenteredRaster\":false") != std::string::npos);
    REQUIRE(encoded.find("\"informationBytesPerFrame\":32985") != std::string::npos);
    REQUIRE(encoded.find("\"transportPayloadCeilingBytesPerFrame\":30951") != std::string::npos);
    REQUIRE(encoded.find("\"verifiedGoodput\":null") != std::string::npos);
}

TEST_CASE("Wide actual six-Segment temporal carousel serves nineteen data equations per visit", "[pam4-wide][scheduler][phase]")
{
    pbapp::MixedSlotTemporalOrderProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeMixedSlotTemporalOrder(pbapp::VisualProfile::ExperimentalPam4Wide, result);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(result.peakSenderSegments <= 6);
    for (std::size_t index = 0; index < result.segmentOrdinals.size(); index++)
    {
        REQUIRE(result.segmentOrdinals[index] == (index / 6 + index % 6) % 6);
        if (index >= 72)
        {
            REQUIRE(result.scheduledEquations[index] == 19);
        }
    }
}
