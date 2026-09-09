#include "run_measurement.h"
#include "measurement_capture_consumer.h"
#include "run_report.h"
#include "unified_decoder_test_support.h"
#include "pbstorage/output_file.h"
#include "pbprotocol/session_random.h"

#include <catch2/catch_test_macros.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <limits>

namespace
{
constexpr std::size_t Index(const pbapp::RunMilestone milestone)
{
    return static_cast<std::size_t>(milestone);
}
QJsonObject Json(const std::string &text)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    REQUIRE(error.error == QJsonParseError::NoError);
    REQUIRE(document.isObject());
    return document.object();
}

pbapp::RunMeasurementSnapshot ValidTiming()
{
    pbapp::RunMeasurementSnapshot snapshot;
    snapshot.runGeneration = 1;
    for (std::size_t index = 0; index <= Index(pbapp::RunMilestone::Terminal); index++)
    {
        snapshot.offsetsNanoseconds[index] = index * 100000000ULL;
    }
    snapshot.terminalSucceeded = true;
    return snapshot;
}

std::filesystem::path FreshDirectory()
{
    const auto id = pbprotocol::GenerateRandomSessionId();
    REQUIRE(id);
    const auto root = std::filesystem::path(PB_TEST_SCRATCH_ROOT) /
                      (L"step1-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(pbprotocol::DeriveSessionTag(id.Value()).value));
    REQUIRE(std::filesystem::create_directory(root));
    return root; // Keep fixtures and failures in the new build evidence tree.
}

class ObservedCapture final : public pbapp::DecoderCaptureSession
{
  public:
    ObservedCapture(std::shared_ptr<g16test::ReceiveState> state, const pbapp::CaptureBackend backend, const bool failStop)
        : inner_(std::move(state), backend), failStop_(failStop)
    {
    }
    pbcapturenormalize::CaptureStatus Start(const pbcapturenormalize::CaptureNormalizeConfig &config,
                                            std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> consumer) noexcept override
    {
        const auto status = inner_.Start(config, consumer);
        if (status)
        {
            const pbcapturenormalize::ScreenCaptureFrame frame;
            return consumer->Submit(frame, nullptr);
        }
        return status;
    }
    pbcapturenormalize::CaptureSnapshot GetSnapshot() const noexcept override
    {
        return inner_.GetSnapshot();
    }
    pbcapturenormalize::CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept override
    {
        return inner_.GetNormalizationSnapshot();
    }
    void RequestStop() noexcept override
    {
        inner_.RequestStop();
    }
    pbcapturenormalize::CaptureStatus Stop() noexcept override
    {
        const auto status = inner_.Stop();
        return failStop_ ? pbcapturenormalize::CaptureStatus::Failure(pbcapturenormalize::CaptureError::NativeFailure,
                                                                      pbcapturenormalize::CaptureStage::Configuration)
                         : status;
    }

  private:
    g16test::CaptureSession inner_;
    bool failStop_;
};
} // namespace

TEST_CASE("Step1 main clock is start accepted through final reopen with honest unavailable cases", "[step1][application][report]")
{
    auto snapshot = ValidTiming();
    SECTION("unit and denominator")
    {
        const auto report = Json(pbapp::BuildRunMeasurementJson(snapshot, 900, 450, true, false));
        CHECK(report["receiverTimingEligible"].toBool());
        CHECK(report["elapsedNanoseconds"].toDouble() == 900000000.0);
        CHECK(report["verifiedRawGoodputBytesPerSecond"].toDouble() == 1000.0);
        CHECK(report["verifiedEncodedGoodputBytesPerSecond"].toDouble() == 500.0);
    }
    SECTION("capture delivered synchronously before ready is legal")
    {
        snapshot.offsetsNanoseconds[Index(pbapp::RunMilestone::CaptureReady)] = 350000000;
        CHECK(Json(pbapp::BuildRunMeasurementJson(snapshot, 900, 450, true, false))["receiverTimingEligible"].toBool());
    }
    SECTION("missing milestone is not zero")
    {
        snapshot.offsetsNanoseconds[Index(pbapp::RunMilestone::FirstVisualObservation)].reset();
        const auto report = Json(pbapp::BuildRunMeasurementJson(snapshot, 900, 450, true, false));
        CHECK(report["unavailableReason"] == "MissingMilestone");
        CHECK(report["verifiedRawGoodputBytesPerSecond"].isNull());
    }
    SECTION("rename before digest is invalid")
    {
        snapshot.offsetsNanoseconds[Index(pbapp::RunMilestone::WholeDigestVerified)] = 950000000;
        CHECK(Json(pbapp::BuildRunMeasurementJson(snapshot, 900, 450, true, false))["unavailableReason"] == "InvalidMilestoneOrder");
    }
    SECTION("zero time is invalid")
    {
        snapshot.offsetsNanoseconds[Index(pbapp::RunMilestone::FinalReopenVerified)] = 0;
        CHECK(Json(pbapp::BuildRunMeasurementJson(snapshot, 900, 450, true, false))["unavailableReason"] == "InvalidDuration");
    }
    SECTION("zero bytes have no equations or last segment")
    {
        snapshot.offsetsNanoseconds[Index(pbapp::RunMilestone::FirstUsefulEquation)].reset();
        snapshot.offsetsNanoseconds[Index(pbapp::RunMilestone::LastSegmentStored)].reset();
        const auto report = Json(pbapp::BuildRunMeasurementJson(snapshot, 0, 0, true, false));
        CHECK(report["receiverTimingEligible"].toBool());
        CHECK(report["verifiedEncodedGoodputBytesPerSecond"].toDouble(-1) == 0);
        CHECK(report["zeroBytePayloadMilestones"] == "NotApplicable");
    }
    SECTION("resume and missing encoded coverage")
    {
        CHECK(Json(pbapp::BuildRunMeasurementJson(snapshot, 900, 450, true, true))["unavailableReason"] == "ResumeHasNoFullRunTimingCoverage");
        CHECK(Json(pbapp::BuildRunMeasurementJson(snapshot, 900, {}, true, false))["unavailableReason"] == "MissingVerifiedByteCoverage");
    }
    SECTION("payload success is not clean terminal evidence")
    {
        snapshot.terminalSucceeded = false;
        CHECK_FALSE(Json(pbapp::BuildRunMeasurementJson(snapshot, 900, 450, true, false))["receiverTimingEligible"].toBool());
    }
}

TEST_CASE("Step1 observation and queue limits are sticky bounded and do not wrap", "[step1]")
{
    const auto recorder = std::make_unique<pbapp::RunMeasurementRecorder>();
    recorder->Begin(1, 100);
    SECTION("clock regression")
    {
        recorder->RecordAt(pbapp::RunMilestone::CaptureReady, 99);
        CHECK(recorder->GetSnapshot().failure == pbapp::MeasurementFailure::InvalidClock);
    }
    SECTION("duration cap")
    {
        recorder->CheckDuration(100 + pbapp::step1MaximumDurationNanoseconds + 1);
        CHECK(recorder->GetSnapshot().failure == pbapp::MeasurementFailure::DurationLimit);
    }
    SECTION("large clock cannot overflow subtraction")
    {
        recorder->RecordAt(pbapp::RunMilestone::CaptureReady, (std::numeric_limits<std::uint64_t>::max)());
        CHECK(recorder->GetSnapshot().failure == pbapp::MeasurementFailure::DurationLimit);
    }
    SECTION("milestones preserve the first observation")
    {
        recorder->RecordAt(pbapp::RunMilestone::CaptureReady, 105);
        recorder->RecordAt(pbapp::RunMilestone::CaptureReady, 110);
        CHECK(recorder->GetSnapshot().offsetsNanoseconds[Index(pbapp::RunMilestone::CaptureReady)] == 5);
        recorder->Begin(2, 200);
        CHECK(recorder->GetSnapshot().runGeneration == 1);
        CHECK(recorder->GetSnapshot().failure == pbapp::MeasurementFailure::ReusedRecorder);
    }
    SECTION("full queue loses evidence rather than blocking producer")
    {
        for (std::size_t index = 0; index <= pbapp::step1SubmittedQueueCapacity; index++)
        {
            recorder->RecordSubmitted({1, index, 2, 3, 4}, 100 + index);
        }
        CHECK(recorder->GetSnapshot().failure == pbapp::MeasurementFailure::QueueFull);
        CHECK(recorder->GetSnapshot().submittedRecords == pbapp::step1SubmittedQueueCapacity);
    }
    SECTION("record cap with a draining consumer")
    {
        pbapp::SubmittedFrameIdentity identity;
        for (std::uint64_t index = 0; index < pbapp::step1MaximumSubmittedRecords; index++)
        {
            recorder->RecordSubmitted({(std::numeric_limits<std::uint64_t>::max)(), index, 2, 3, 4}, 100 + index);
            REQUIRE(recorder->TakeSubmitted(identity));
            REQUIRE(identity.frameSequence == index);
        }
        recorder->RecordSubmitted({}, 100);
        CHECK(recorder->GetSnapshot().failure == pbapp::MeasurementFailure::RecordLimit);
        CHECK(Json(pbapp::BuildSubmittedFrameJson(identity))["sessionTag"].toString() == "18446744073709551615");
    }
}

TEST_CASE("Step1 source audit reuses immutable prescan including RAW zstd zero and short tail", "[step1]")
{
    const auto root = FreshDirectory();
    std::vector<std::byte> bytes;
    SECTION("zero")
    {
    }
    SECTION("RAW")
    {
        bytes = g16test::RawBytes(20000);
    }
    SECTION("zstd and short second segment")
    {
        bytes.assign(8 * 1024 * 1024 + 1, std::byte{0x41});
    }
    const auto sender = std::make_shared<pbapp::RunMeasurementRecorder>();
    pbapp::EncoderSnapshot encoderSnapshot;
    const auto frames = g16test::MakeFrames(root / L"sender", bytes, bytes.empty() ? 1 : 2, &encoderSnapshot, sender);
    REQUIRE(encoderSnapshot.measurement);
    REQUIRE(encoderSnapshot.measurement->terminalSucceeded == true);
    REQUIRE(encoderSnapshot.measurement->sourceLedgerComplete);
    REQUIRE(encoderSnapshot.measurement->sourceBytes == bytes.size());
    pbapp::SubmittedFrameIdentity submitted;
    for (const auto &frame : frames)
    {
        REQUIRE(sender->TakeSubmitted(submitted));
        const auto bootstrap = pbprotocol::ParseBootstrapRecord(frame.bootstrapRecord);
        REQUIRE(bootstrap);
        CHECK(submitted.frameSequence == bootstrap.Value().frameSequence);
        CHECK(submitted.sessionTag == bootstrap.Value().sessionTag.value);
    }
    CHECK_FALSE(sender->TakeSubmitted(submitted));
    std::string ledger;
    const auto source = root / L"sender" / L"g16-source.bin";
    REQUIRE(pbapp::AuditUnifiedSource(source.wstring(), ledger));
    CHECK(ledger == pbapp::BuildSourceLedgerJson(*encoderSnapshot.measurement));
    const HANDLE writer = CreateFileW(source.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(writer != INVALID_HANDLE_VALUE);
    std::string unchanged = "unchanged";
    CHECK_FALSE(pbapp::AuditUnifiedSource(source.wstring(), unchanged));
    CHECK(unchanged == "unchanged");
    REQUIRE(CloseHandle(writer));
}

TEST_CASE("Step1 measurement cannot change production small file recovery or certify failed cleanup", "[step1][application][report]")
{
    const auto root = FreshDirectory();
    std::vector<std::byte> bytes;
    SECTION("empty")
    {
    }
    SECTION("RAW")
    {
        bytes = g16test::RawBytes(20000);
    }
    const auto frames = g16test::MakeFrames(root / L"sender", bytes, bytes.empty() ? 1 : 2);
    QJsonObject referenceTelemetry;
    for (int mode = 0; mode < 4; mode++)
    {
        const auto state = std::make_shared<g16test::ReceiveState>();
        for (const auto &frame : frames)
        {
            state->Push(frame);
        }
        auto services = g16test::Services(state);
        services.captureFactory = [state, mode](const pbapp::CaptureBackend backend)
        {
            return std::make_unique<ObservedCapture>(state, backend, mode == 2);
        };
        const auto output = root / std::to_wstring(mode);
        REQUIRE(std::filesystem::create_directory(output));
        auto config = pbapp::MakeUnifiedDecoderConfig(output.wstring(), g16test::Region());
        if (mode != 0)
        {
            config.measurement = std::make_shared<pbapp::RunMeasurementRecorder>();
        }
        if (mode == 3)
        {
            config.measurement->Fail(pbapp::MeasurementFailure::QueueFull);
        }
        pbapp::DecoderRuntime runtime(services);
        REQUIRE(runtime.Start(config));
        REQUIRE(g16test::WaitFor([&]()
                                 { return runtime.GetSnapshot().state == pbapp::DecoderState::Completed; }));
        runtime.Stop();
        const auto snapshot = runtime.GetSnapshot();
        REQUIRE(g16test::VerifyOutput(snapshot, bytes));
        const auto report = Json(pbapp::BuildDecoderRunReportJson({"Decoder", "test", "test", "test"}, snapshot));
        if (mode == 0)
        {
            CHECK_FALSE(report.contains("measurement"));
            referenceTelemetry = report["unifiedTelemetry"].toObject();
        }
        else
        {
            REQUIRE(snapshot.measurement);
            const auto measured = report["measurement"].toObject();
            CHECK(measured["receiverTimingEligible"].toBool() == (mode == 1));
            CHECK(snapshot.measurement->terminalSucceeded == (mode != 2));
            CHECK(report["unifiedTelemetry"].toObject()["uniqueFrames"] == referenceTelemetry["uniqueFrames"]);
            CHECK(report["unifiedTelemetry"].toObject()["lanes"] == referenceTelemetry["lanes"]);
            if (mode == 2)
            {
                CHECK_FALSE(snapshot.errorDetail.empty());
            }
        }
    }
}

TEST_CASE("Step1 storage timestamps observe exact publication phases without fabricating recovered history", "[step1][application][report]")
{
    const auto root = FreshDirectory();
    const auto bytes = g16test::RawBytes(128);
    pbstorage::OutputFileConfig config{root.wstring(), pbprotocol::SessionTag{101}, bytes.size(), 4096, "output.bin"};
    config.observePublishTiming = true;
    std::unique_ptr<pbstorage::OutputFile> output;
    REQUIRE(pbstorage::OutputFile::Create(config, output));
    REQUIRE(output->WriteVerifiedSegment(0, bytes));
    REQUIRE(output->FlushVerifiedSegment());
    const pbprotocol::WholeFileDigest digest{pbprotocol::ComputeBlake3Digest(bytes)};
    SECTION("digest rejection")
    {
        auto wrong = digest;
        wrong.bytes[0] ^= std::byte{1};
        REQUIRE_FALSE(output->Publish(wrong));
        CHECK_FALSE(output->GetSnapshot().wholeDigestVerifiedAt);
        CHECK_FALSE(output->GetSnapshot().finalRenameSucceededAt);
        CHECK_FALSE(output->GetSnapshot().finalReopenVerifiedAt);
    }
    SECTION("final target conflict")
    {
        std::ofstream existing(std::filesystem::path(output->GetSnapshot().finalPath));
        existing << "protected";
        existing.close();
        REQUIRE_FALSE(output->Publish(digest));
        CHECK(output->GetSnapshot().wholeDigestVerifiedAt.has_value());
        CHECK_FALSE(output->GetSnapshot().finalRenameSucceededAt);
    }
    SECTION("success then resume has no prior timing")
    {
        const auto name = std::filesystem::path(output->GetSnapshot().finalPath).filename().string();
        REQUIRE(output->Publish(digest));
        const auto snapshot = output->GetSnapshot();
        REQUIRE(snapshot.wholeDigestVerifiedAt);
        REQUIRE(snapshot.finalRenameSucceededAt);
        REQUIRE(snapshot.finalReopenVerifiedAt);
        CHECK(*snapshot.wholeDigestVerifiedAt <= *snapshot.finalRenameSucceededAt);
        CHECK(*snapshot.finalRenameSucceededAt <= *snapshot.finalReopenVerifiedAt);
        output.reset();
        REQUIRE(pbstorage::OutputFile::CreateOrResume(config, {name}, digest, output));
        CHECK(output->GetSnapshot().recoveredPublished);
        CHECK_FALSE(output->GetSnapshot().wholeDigestVerifiedAt);
        CHECK_FALSE(output->GetSnapshot().finalRenameSucceededAt);
        CHECK_FALSE(output->GetSnapshot().finalReopenVerifiedAt);
    }
}

TEST_CASE("Step1 orphan caching is not a useful equation until bound decoder admission", "[step1]")
{
    const auto root = FreshDirectory();
    const auto bytes = g16test::RawBytes(20000);
    const auto frames = g16test::MakeFrames(root / L"sender", bytes, 2);
    const auto first = std::make_unique<pbdemodd3d11::CaptureDemodulatorResult>(frames.front());
    std::uint32_t kept = 0;
    std::uint32_t omitted = 0;
    for (std::uint32_t index = 0; index < first->demodulation.acceptedUnifiedBlockCount; index++)
    {
        const auto &block = first->demodulation.acceptedUnifiedBlocks[index];
        if (block.kind == pbmodulation::UnifiedSlotKind::Control)
        {
            const auto record = pbprotocol::ParseControlRecord(std::span(block.bytes).first(block.size));
            REQUIRE(record);
            if (record.Value().recordType == pbprotocol::ControlRecordType::SegmentDescriptor)
            {
                omitted++;
                continue;
            }
        }
        first->demodulation.acceptedUnifiedBlocks[kept++] = block;
    }
    REQUIRE(omitted > 0);
    first->demodulation.acceptedUnifiedBlockCount = kept;
    const auto state = std::make_shared<g16test::ReceiveState>();
    state->Push(*first);
    const auto output = root / L"receiver";
    REQUIRE(std::filesystem::create_directory(output));
    auto config = pbapp::MakeUnifiedDecoderConfig(output.wstring(), g16test::Region());
    config.measurement = std::make_shared<pbapp::RunMeasurementRecorder>();
    auto services = g16test::Services(state);
    services.captureFactory = [state](const pbapp::CaptureBackend backend)
    { return std::make_unique<ObservedCapture>(state, backend, false); };
    pbapp::DecoderRuntime runtime(services);
    REQUIRE(runtime.Start(config));
    REQUIRE(g16test::WaitFor([&]()
                             { return runtime.GetSnapshot().outerOrphanCachedBlockCount > 0; }));
    const auto pending = runtime.GetSnapshot();
    REQUIRE(pending.measurement);
    CHECK(pending.measurement->offsetsNanoseconds[Index(pbapp::RunMilestone::FirstControlAccepted)].has_value());
    CHECK_FALSE(pending.measurement->offsetsNanoseconds[Index(pbapp::RunMilestone::FirstUsefulEquation)].has_value());
    state->Push(frames.back());
    REQUIRE(g16test::WaitFor([&]()
                             { return runtime.GetSnapshot().state == pbapp::DecoderState::Completed; }));
    runtime.Stop();
    const auto completed = runtime.GetSnapshot();
    CHECK(g16test::VerifyOutput(completed, bytes));
    CHECK(completed.measurement->offsetsNanoseconds[Index(pbapp::RunMilestone::FirstUsefulEquation)].has_value());
}

TEST_CASE("Step1 capture observation forwards reservation status cancellation and stages unchanged", "[step1]")
{
    class Probe final : public pbcapturenormalize::ScreenCaptureConsumer
    {
      public:
        int calls = 0;
        bool sawCancellation = false;
        pbcapturenormalize::CaptureStatus failure = pbcapturenormalize::CaptureStatus::Failure(
            pbcapturenormalize::CaptureError::NativeFailure, pbcapturenormalize::CaptureStage::Configuration);
        std::uint64_t ReservedBytes() const noexcept override
        {
            return 512;
        }
        pbcapturenormalize::CaptureStatus ValidateConfiguration(const pbcapturenormalize::CaptureConfig &) const noexcept override
        {
            return failure;
        }
        pbcapturenormalize::CaptureStatus DomainStarted(const pbcapturenormalize::ScreenCaptureDomain &,
                                                        const pbcapturenormalize::CaptureEnvironment &, ID3D11Device *) override
        {
            calls++;
            return failure;
        }
        void DomainInvalidated(const pbcapturenormalize::ScreenCaptureDomain &) noexcept override
        {
            calls++;
        }
        pbcapturenormalize::CaptureStatus Submit(const pbcapturenormalize::ScreenCaptureFrame &, ID3D11DeviceContext *) override
        {
            calls++;
            return failure;
        }
        pbcapturenormalize::CaptureConsumerCompletion CompleteStage(const pbcapturenormalize::ScreenCaptureFrameMetadata &,
                                                                    ID3D11Texture2D *, ID3D11DeviceContext *, const bool cancelled) override
        {
            calls++;
            sawCancellation = cancelled;
            return {failure, !cancelled};
        }
        pbcapturenormalize::CaptureStatus Completed(const pbcapturenormalize::ScreenCaptureFrameMetadata &,
                                                    ID3D11DeviceContext *, bool) override
        {
            calls++;
            return failure;
        }
        void Erased(const pbcapturenormalize::CaptureErasure &) noexcept override
        {
            calls++;
        }
    };
    const auto primary = std::make_shared<Probe>();
    const auto recorder = std::make_shared<pbapp::RunMeasurementRecorder>();
    recorder->Begin(1, pbapp::MeasurementNowNanoseconds());
    pbapp::detail::MeasurementCaptureConsumer wrapper(primary, recorder);
    CHECK(wrapper.ReservedBytes() == 512);
    CHECK_FALSE(wrapper.ValidateConfiguration({}));
    CHECK_FALSE(wrapper.DomainStarted({}, {}, nullptr));
    wrapper.DomainInvalidated({});
    CHECK_FALSE(wrapper.Submit({}, nullptr));
    const auto completion = wrapper.CompleteStage({}, nullptr, nullptr, true);
    CHECK_FALSE(completion.status);
    CHECK(primary->sawCancellation);
    CHECK_FALSE(wrapper.Completed({}, nullptr, true));
    wrapper.Erased({});
    CHECK(primary->calls == 6);
    CHECK(recorder->GetSnapshot().offsetsNanoseconds[Index(pbapp::RunMilestone::FirstVisualObservation)].has_value());
}
