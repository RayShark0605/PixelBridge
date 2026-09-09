#include "held_presentation.h"
#include "run_contract.h"
#include "run_measurement.h"
#include "support.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

namespace
{
using pbstep3b::Require;
using pbexperiment::Treatment;
using pbrenderd3d::PresentationErrorCode;

class TestPresentation final : public pbapp::EncoderPresentation
{
public:
    pbrenderd3d::DataWindowSnapshot snapshot;
    std::vector<std::byte> lastPixels;
    std::vector<std::uint64_t> sequences;
    pbrenderd3d::PresentationStatus nextSubmitStatus;

    TestPresentation()
    {
        snapshot.state = pbrenderd3d::WindowState::Running;
        snapshot.timing.presentationEpoch = 1;
    }
    pbrenderd3d::DataWindowSnapshot GetSnapshot() const override
    {
        return snapshot;
    }
    pbrenderd3d::PresentationStatus SubmitFrame(const pbrenderd3d::CanonicalBgraFrameView& frame) override
    {
        if (!nextSubmitStatus)
        {
            return nextSubmitStatus;
        }
        lastPixels.assign(frame.pixels.begin(), frame.pixels.end());
        sequences.push_back(frame.frameSequence);
        snapshot.submittedFrames++;
        snapshot.pendingFrame = true;
        return {};
    }
    void Present(const std::uint64_t sequence, const pbpresenttiming::PresentOutcome outcome = pbpresenttiming::PresentOutcome::Success)
    {
        snapshot.pendingFrame = false;
        snapshot.totalPresentCalls++;
        snapshot.totalSuccessfulPresents += outcome == pbpresenttiming::PresentOutcome::Success ? 1U : 0U;
        snapshot.timing.lastPresent = {sequence, 10, 11, outcome, {}};
    }
    void RequestStop() noexcept override
    {
        snapshot.state = pbrenderd3d::WindowState::Stopped;
        snapshot.pendingFrame = false;
        snapshot.inFlightFrame = false;
    }
    void Stop() noexcept override
    {
        RequestStop();
    }
};

struct Fixture
{
    pbrenderd3d::DataWindowConfig config;
    std::uint64_t now = 100;
    TestPresentation* native = nullptr;
    std::shared_ptr<pbexperiment::PresentationEvidence> evidence = std::make_shared<pbexperiment::PresentationEvidence>();
    std::unique_ptr<pbexperiment::HeldPresentation> presenter;
    std::array<std::byte, 8> pixels{std::byte{0}, std::byte{255}, std::byte{0}, std::byte{29},
        std::byte{100}, std::byte{100}, std::byte{100}, std::byte{255}};

    explicit Fixture(const Treatment treatment = Treatment::NeutralChroma)
    {
        config.width = 2;
        config.height = 1;
        auto backend = std::make_unique<TestPresentation>();
        native = backend.get();
        presenter = std::make_unique<pbexperiment::HeldPresentation>(config, treatment, std::move(backend), evidence, [this]() { return now; });
    }
    pbrenderd3d::CanonicalBgraFrameView Frame(const std::uint64_t sequence) const
    {
        return {pixels, 2, 1, 8, sequence, 1};
    }
};

void TestTransform()
{
    pbrenderd3d::DataWindowConfig config;
    config.width = 2;
    config.height = 2;
    const std::array<std::byte, 20> source{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{13},
        std::byte{0}, std::byte{255}, std::byte{0}, std::byte{29}, std::byte{66}, std::byte{67}, std::byte{68}, std::byte{69},
        std::byte{0}, std::byte{0}, std::byte{255}, std::byte{37}, std::byte{100}, std::byte{100}, std::byte{100}, std::byte{255}};
    const pbrenderd3d::CanonicalBgraFrameView view{source, 2, 2, 12, 0, 1};
    std::array<std::byte, 16> destination{};
    Require(static_cast<bool>(pbexperiment::TransformFrame(config, view, Treatment::NeutralChroma, destination)), "strided transform");
    const std::array<std::uint8_t, 4> expected{18, 182, 54, 100};
    const std::array<std::uint8_t, 4> alpha{13, 29, 37, 255};
    for (std::size_t index = 0; index < expected.size(); index++)
    {
        for (std::size_t channel = 0; channel < 3; channel++)
        {
            Require(std::to_integer<std::uint8_t>(destination[index * 4 + channel]) == expected[index], "luma value");
        }
        Require(std::to_integer<std::uint8_t>(destination[index * 4 + 3]) == alpha[index], "alpha immutable");
    }
    Require(static_cast<bool>(pbexperiment::TransformFrame(config, view, Treatment::ColorControl, destination)), "color control");
    Require(std::equal(destination.begin(), destination.begin() + 8, source.begin()) &&
        std::equal(destination.begin() + 8, destination.end(), source.begin() + 12), "row pitch respected");
    const auto before = destination;
    auto truncated = view;
    truncated.pixels = truncated.pixels.first(19);
    Require(!pbexperiment::TransformFrame(config, truncated, Treatment::NeutralChroma, destination) && before == destination, "truncation transactional");
    truncated = view;
    truncated.rowPitch = (std::numeric_limits<std::size_t>::max)();
    Require(!pbexperiment::TransformFrame(config, truncated, Treatment::NeutralChroma, destination) && before == destination, "pitch overflow");
    Require(!pbexperiment::TransformFrame(config, view, static_cast<Treatment>(255), destination) && before == destination, "unknown treatment");
    Require(!pbexperiment::TransformFrame(config, view, Treatment::NeutralChroma, std::span(destination).first(15)) && before == destination, "output bound");
    const pbrenderd3d::CanonicalBgraFrameView alias{destination, 2, 2, 8, 0, 1};
    Require(!pbexperiment::TransformFrame(config, alias, Treatment::NeutralChroma, destination) && before == destination, "alias rejected");
}

void TestHoldAndQueue()
{
    Fixture fixture;
    Require(static_cast<bool>(fixture.presenter->SubmitFrame(fixture.Frame(7))), "queue first");
    Require(fixture.native->sequences.empty() && fixture.evidence->recordCount == 1, "queue not native present");
    auto snapshot = fixture.presenter->GetSnapshot();
    Require(snapshot.pendingFrame && snapshot.submittedFrames == 1 && fixture.native->sequences == std::vector<std::uint64_t>{7}, "forward exact identity");
    Require(fixture.native->lastPixels[0] == std::byte{182} && fixture.native->lastPixels[3] == std::byte{29}, "native sees transformed pixels");
    Require(!fixture.presenter->SubmitFrame(fixture.Frame(8)) && fixture.evidence->recordCount == 1, "native busy rejects new queue");
    fixture.native->Present(7);
    fixture.now = 110;
    snapshot = fixture.presenter->GetSnapshot();
    Require(!snapshot.pendingFrame && fixture.evidence->observedFrames == 1, "no fabricated pending while timer only");
    Require(snapshot.totalSuccessfulPresents == 1, "native present count untouched");
    Require(static_cast<bool>(fixture.presenter->SubmitFrame(fixture.Frame(8))), "one pending during hold");
    Require(!fixture.presenter->SubmitFrame(fixture.Frame(9)) && fixture.evidence->recordCount == 2, "no replace or second queued frame");
    fixture.now = 110 + pbexperiment::minimumObservedPresentHoldNanoseconds - 1;
    snapshot = fixture.presenter->GetSnapshot();
    Require(snapshot.pendingFrame && fixture.native->sequences.size() == 1, "hold minimum enforced");
    fixture.native->Present(7);
    fixture.native->snapshot.inFlightFrame = true;
    fixture.now++;
    snapshot = fixture.presenter->GetSnapshot();
    Require(fixture.native->sequences == std::vector<std::uint64_t>{7, 8}, "repeated Present does not reset hold or starve separate pending buffer");
    Require(fixture.evidence->observedFrames == 1 && snapshot.totalSuccessfulPresents == 2, "repeats not new logical data");
    fixture.native->Present(8);
    fixture.now++;
    static_cast<void>(fixture.presenter->GetSnapshot());
    Require(!fixture.presenter->SubmitFrame(fixture.Frame(8)), "same sequence rejected");
    Require(static_cast<bool>(fixture.presenter->SubmitFrame(fixture.Frame(9))), "queue before stop");
    fixture.presenter->Stop();
    fixture.presenter->Stop();
    snapshot = fixture.presenter->GetSnapshot();
    Require(snapshot.state == pbrenderd3d::WindowState::Stopped && !snapshot.pendingFrame && !snapshot.inFlightFrame, "bounded clean stop");
    Require(fixture.evidence->discardedOnStop == 1 && fixture.evidence->records[2].discardedOnStop &&
        !fixture.evidence->records[2].forwardedNanoseconds, "discard not fake forwarded");
    Require(!fixture.presenter->SubmitFrame(fixture.Frame(10)), "stopped cannot accept");
    std::ostringstream json;
    pbexperiment::WritePresentationEvidenceJson(json, *fixture.evidence);
    Require(json.str().find("\"offlineH2TimingEquivalent\":false") != std::string::npos, "timing authority explicit");
}

void TestFailureBoundaries()
{
    {
        Fixture fixture;
        Require(static_cast<bool>(fixture.presenter->SubmitFrame(fixture.Frame(0))), "epoch setup");
        fixture.native->snapshot.timing.presentationEpoch = 2;
        fixture.native->snapshot.timing.epochReason = pbpresenttiming::EpochReason::StatisticsDisjoint;
        Require(fixture.presenter->GetSnapshot().error.code == PresentationErrorCode::EpochMismatch && fixture.native->sequences.empty(), "stale queue cannot forward");
        Require(fixture.evidence->nativeAtEpochMismatch && fixture.evidence->nativeAtEpochMismatch->timing.epochReason ==
            pbpresenttiming::EpochReason::StatisticsDisjoint, "native epoch cause retained");
    }
    {
        Fixture fixture;
        auto frame = fixture.Frame(0);
        frame.presentationEpoch = 9;
        Require(fixture.presenter->SubmitFrame(frame).code == PresentationErrorCode::EpochMismatch && fixture.evidence->recordCount == 0, "foreign epoch admission");
    }
    {
        Fixture fixture;
        Require(static_cast<bool>(fixture.presenter->SubmitFrame(fixture.Frame(0))), "timeout setup");
        static_cast<void>(fixture.presenter->GetSnapshot());
        fixture.native->Present(0, pbpresenttiming::PresentOutcome::Failure);
        fixture.now += pbexperiment::nativePresentTimeoutNanoseconds;
        Require(fixture.presenter->GetSnapshot().error.code == PresentationErrorCode::Timeout && fixture.evidence->observedFrames == 0, "failed Present not success");
    }
    {
        Fixture fixture;
        Require(static_cast<bool>(fixture.presenter->SubmitFrame(fixture.Frame(0))), "native reject setup");
        fixture.native->nextSubmitStatus = pbrenderd3d::PresentationStatus::Failure(PresentationErrorCode::DeviceLost, pbrenderd3d::PresentationStage::Upload);
        Require(fixture.presenter->GetSnapshot().error.code == PresentationErrorCode::DeviceLost && fixture.evidence->forwardedFrames == 0, "native error retained");
    }
    {
        Fixture fixture;
        Require(static_cast<bool>(fixture.presenter->SubmitFrame(fixture.Frame(0))), "clock setup");
        fixture.now--;
        Require(fixture.presenter->GetSnapshot().error.code == PresentationErrorCode::ContractViolation, "clock regression");
    }
    {
        Fixture fixture;
        fixture.now = (std::numeric_limits<std::uint64_t>::max)() - 10;
        Require(static_cast<bool>(fixture.presenter->SubmitFrame(fixture.Frame(0))), "overflow setup");
        static_cast<void>(fixture.presenter->GetSnapshot());
        fixture.native->Present(0);
        fixture.now++;
        Require(fixture.presenter->GetSnapshot().error.code == PresentationErrorCode::ResourceLimit && fixture.evidence->observedFrames == 0, "deadline overflow");
    }
    {
        Fixture fixture;
        fixture.evidence->recordCount = pbexperiment::maximumPresentationRecords;
        Require(fixture.presenter->SubmitFrame(fixture.Frame(0)).code == PresentationErrorCode::ResourceLimit, "fixed evidence bound");
    }
    {
        Fixture fixture(Treatment::ColorControl);
        Require(static_cast<bool>(fixture.presenter->SubmitFrame(fixture.Frame(0))), "color setup");
        static_cast<void>(fixture.presenter->GetSnapshot());
        Require(std::equal(fixture.native->lastPixels.begin(), fixture.native->lastPixels.end(), fixture.pixels.begin()), "reference color unchanged");
    }
}

void TestDrainedStatisticsEpoch()
{
    Fixture fixture;
    fixture.native->snapshot.candidateContractSatisfied = true;
    Require(static_cast<bool>(fixture.presenter->SubmitFrame(fixture.Frame(0))), "statistics setup");
    static_cast<void>(fixture.presenter->GetSnapshot());
    fixture.native->Present(0);
    // Reproduce the captured native trace: successful Present followed by
    // statistics-disjoint invalidates source and resets lastPresent evidence.
    fixture.native->snapshot.timing.lastPresent.reset();
    fixture.native->snapshot.timing.presentationEpoch = 2;
    fixture.native->snapshot.timing.epochReason = pbpresenttiming::EpochReason::StatisticsDisjoint;
    auto snapshot = fixture.presenter->GetSnapshot();
    Require(snapshot.state == pbrenderd3d::WindowState::Running && !snapshot.pendingFrame &&
        fixture.evidence->drainedStatisticsEpochCount == 1 && fixture.evidence->records[0].invalidatedAtStatisticsEpoch &&
        fixture.evidence->observedFrames == 0, "drained old forward stays unobserved");
    Require(fixture.presenter->SubmitFrame(fixture.Frame(1)).code == PresentationErrorCode::EpochMismatch, "cannot retag old-epoch input");
    auto fresh = fixture.Frame(1);
    fresh.presentationEpoch = 2;
    Require(static_cast<bool>(fixture.presenter->SubmitFrame(fresh)), "fresh runtime frame can enter new epoch");
    fixture.native->snapshot.timing.presentationEpoch = 3;
    fixture.native->snapshot.timing.epochReason = pbpresenttiming::EpochReason::StatisticsRecovered;
    snapshot = fixture.presenter->GetSnapshot();
    Require(snapshot.state == pbrenderd3d::WindowState::Running && !snapshot.pendingFrame &&
        fixture.evidence->records[1].invalidatedAtStatisticsEpoch && !fixture.evidence->records[1].forwardedNanoseconds &&
        fixture.native->sequences == std::vector<std::uint64_t>{0}, "queued old pixels dropped, not replayed");
    fixture.native->snapshot.timing.presentationEpoch = 4;
    Require(fixture.presenter->GetSnapshot().error.code == PresentationErrorCode::EpochMismatch, "statistics reset count bounded to two");
    for (std::uint32_t scenario = 0; scenario < 3; scenario++)
    {
        Fixture changed;
        changed.native->snapshot.candidateContractSatisfied = true;
        Require(static_cast<bool>(changed.presenter->SubmitFrame(changed.Frame(0))), "changed surface setup");
        changed.native->snapshot.timing.presentationEpoch = 2;
        changed.native->snapshot.timing.epochReason = pbpresenttiming::EpochReason::StatisticsDisjoint;
        if (scenario == 0)
        {
            changed.native->snapshot.environment.modeChangeSerial = 1;
        }
        else if (scenario == 1)
        {
            changed.native->snapshot.activeFrame = true;
        }
        else
        {
            changed.native->snapshot.timing.epochReason = pbpresenttiming::EpochReason::DeviceLost;
        }
        Require(changed.presenter->GetSnapshot().error.code == PresentationErrorCode::EpochMismatch, "unsafe epoch cannot recover");
    }
}

void TestRunContract()
{
    Require(pbexperiment::ParseRunSeconds(L"5", false) == 5 && pbexperiment::ParseRunSeconds(L"60", false) == 60, "short budget preserved");
    Require(pbexperiment::ParseRunSeconds(L"61", true) == 61 && pbexperiment::ParseRunSeconds(L"900", true) == 900, "explicit comparison boundary");
    for (const bool comparison : {false, true})
    {
        for (const auto text : {L"", L"0", L"4", L"05", L"090", L"901", L"1000", L"-5", L"+5", L"5 ", L" 5", L"5.0", L"5x", L"\uff15"})
        {
            bool rejected = false;
            try
            {
                static_cast<void>(pbexperiment::ParseRunSeconds(text, comparison));
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            Require(rejected, "noncanonical or out-of-budget duration must reject");
        }
    }
    bool shortRejected = false;
    try
    {
        static_cast<void>(pbexperiment::ParseRunSeconds(L"61", false));
    }
    catch (const std::invalid_argument&)
    {
        shortRejected = true;
    }
    Require(shortRejected, "comparison budget cannot silently extend short entry");
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    const auto submittedRow = pbapp::BuildSubmittedFrameJson({maximum, maximum, maximum, maximum, maximum, maximum}) + '\n';
    Require(submittedRow.size() <= pbexperiment::maximumSubmittedEvidenceRowBytes, "actual original serializer maximum decimal width");
    Require(submittedRow.size() * pbexperiment::maximumSubmittedEvidenceRecords < pbexperiment::maximumEvidenceFileBytes, "all submitted rows fit existing 4 MiB cap");
    const auto evidence = std::make_unique<pbexperiment::PresentationEvidence>();
    evidence->recordCount = pbexperiment::maximumPresentationRecords;
    for (auto& record : evidence->records)
    {
        record = {maximum, maximum, maximum, maximum, maximum, (std::numeric_limits<std::int64_t>::min)(), maximum, false, false};
    }
    std::ostringstream presentation;
    pbexperiment::WritePresentationEvidenceJson(presentation, *evidence);
    // Three snapshots have fixed keys/scalars and fixed Win32 name arrays;
    // each has a conservative 64 KiB allowance, not a variable record list.
    Require(presentation.str().size() + 3 * 65536 < pbexperiment::maximumEvidenceFileBytes, "maximum-width held records leave bounded snapshot reserve");
    std::cout << "{\"submittedMaxWidthRowBytes\":" << submittedRow.size() << ",\"submittedMaxRowsBytes\":"
        << submittedRow.size() * pbexperiment::maximumSubmittedEvidenceRecords << ",\"heldMaxWidthRecordsBytes\":" << presentation.str().size()
        << ",\"heldSnapshotReserveBytes\":196608,\"evidenceFileCapBytes\":" << pbexperiment::maximumEvidenceFileBytes << "}\n";
}

void TestComparisonVirtualClock()
{
    constexpr std::uint64_t frameCount = 6750;
    static_assert(frameCount + 4 < pbexperiment::maximumPresentationRecords);
    Fixture fixture;
    for (std::uint64_t index = 0; index < frameCount; index++)
    {
        Require(static_cast<bool>(fixture.presenter->SubmitFrame(fixture.Frame(index))), "bounded virtual comparison queue");
        static_cast<void>(fixture.presenter->GetSnapshot());
        fixture.native->Present(index);
        static_cast<void>(fixture.presenter->GetSnapshot());
        Require(fixture.evidence->observedFrames == index + 1, "one observed identity per virtual Present");
        fixture.now += pbexperiment::minimumObservedPresentHoldNanoseconds;
    }
    Require(fixture.evidence->recordCount == frameCount && fixture.evidence->forwardedFrames == frameCount && fixture.evidence->failure, "virtual budget retains complete identity evidence");
    fixture.presenter->Stop();
}

void VerifyFrozenPair(const std::filesystem::path& originalPath, const std::filesystem::path& grayPath)
{
    pbstep3b::RequireLocal(originalPath);
    pbstep3b::RequireLocal(grayPath);
    Require(std::filesystem::file_size(originalPath) == pbstep3b::kSequenceBytes &&
        std::filesystem::file_size(grayPath) == 2 * pbstep3b::kSequenceBytes, "frozen pair dimensions");
    const pbstep3b::Handle original(CreateFileW(originalPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    const pbstep3b::Handle gray(CreateFileW(grayPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    std::vector<std::byte> source(pbstep3b::kFrameBytes), transformed(pbstep3b::kFrameBytes), reference(pbstep3b::kFrameBytes);
    const pbrenderd3d::DataWindowConfig config;
    for (std::uint32_t index = 0; index < 30; index++)
    {
        DWORD read = 0;
        Require(ReadFile(original.Get(), source.data(), pbstep3b::kFrameBytes, &read, nullptr) && read == pbstep3b::kFrameBytes, "original read");
        Require(static_cast<bool>(pbexperiment::TransformFrame(config, {source, 1920, 1080, 7680, index, 1}, Treatment::NeutralChroma, transformed)), "full frame transform");
        for (std::uint32_t repeat = 0; repeat < 2; repeat++)
        {
            Require(ReadFile(gray.Get(), reference.data(), pbstep3b::kFrameBytes, &read, nullptr) && read == pbstep3b::kFrameBytes, "gray read");
            Require(reference == transformed, "C++ transform differs from frozen independent Python pixels");
        }
    }
    std::cout << "{\"status\":\"FROZEN_GRAY_PAIR_BYTE_EXACT\",\"distinctFrames\":30,\"pixels\":62208000,\"newCodecRuns\":0,\"newGpuObservations\":0}\n";
}
} // namespace

int wmain(const int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--self-test")
        {
            TestTransform();
            TestHoldAndQueue();
            TestFailureBoundaries();
            TestDrainedStatisticsEpoch();
            TestRunContract();
            TestComparisonVirtualClock();
            std::cout << "{\"status\":\"PASS\",\"testGroups\":6,\"nativeScreens\":0,\"codecRuns\":0,\"gpuObservations\":0}\n";
            return 0;
        }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--verify-frozen-pair")
        {
            VerifyFrozenPair(argv[2], argv[3]);
            return 0;
        }
        throw std::invalid_argument("Use --self-test or --verify-frozen-pair original30.bgra gray60.bgra");
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
