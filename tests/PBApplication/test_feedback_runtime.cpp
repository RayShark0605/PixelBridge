#include "unified_decoder_test_support.h"
#include "feedback_file_store.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/transport_block_codec.h"
#include "pbprotocol/session_random.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <iomanip>
#include <sstream>

namespace
{
using namespace g16test;

class Scratch
{
public:
    Scratch()
    {
        const auto random = pbprotocol::GenerateRandomSessionId();
        REQUIRE(random);
        std::wstring suffix;
        for (const auto value : random.Value().bytes)
        {
            suffix += std::to_wstring(std::to_integer<unsigned int>(value));
        }
        path_ = std::filesystem::path(PB_TEST_SCRATCH_ROOT) / (L"feedback-runtime-" + suffix);
        REQUIRE(std::filesystem::create_directory(path_));
    }
    ~Scratch()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    std::filesystem::path Directory(const wchar_t* const name)
    {
        const auto result = path_ / name;
        std::filesystem::create_directory(result);
        return result;
    }
private:
    std::filesystem::path path_;
};

bool ReadSnapshot(const std::filesystem::path& directory, pbapp::FeedbackSnapshot& output)
{
    std::vector<std::byte> bytes;
    return pbapp::ReadFeedbackFile(directory / L"PixelBridgeFeedback.pbfb", bytes) && pbapp::ParseFeedbackSnapshot(bytes, output);
}

bool HasResume(const std::filesystem::path& directory)
{
    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
        if (entry.path().extension() == L".resume")
        {
            return true;
        }
    }
    return false;
}
}

TEST_CASE("Decoder feedback exports only stored commits and listens after verified publication", "[feedback][runtime][decoder]")
{
    Scratch scratch;
    const auto source = scratch.Directory(L"source");
    const auto output = scratch.Directory(L"output");
    const auto feedback = scratch.Directory(L"feedback");
    const auto bytes = RawBytes(4096);
    const auto frames = MakeFrames(source, bytes, 3);
    auto controls = frames.front();
    controls.demodulation.acceptedUnifiedBlockCount = 0;
    for (std::uint32_t index = 0; index < frames.front().demodulation.acceptedUnifiedBlockCount; index++)
    {
        const auto& block = frames.front().demodulation.acceptedUnifiedBlocks[index];
        if (block.kind == pbmodulation::UnifiedSlotKind::Control)
        {
            controls.demodulation.acceptedUnifiedBlocks[controls.demodulation.acceptedUnifiedBlockCount++] = block;
        }
    }
    auto state = std::make_shared<ReceiveState>();
    pbapp::DecoderRuntime runtime(Services(state));
    auto config = pbapp::MakeUnifiedDecoderConfig(output.wstring(), Region());
    config.feedback = pbapp::FeedbackConfig{feedback};
    REQUIRE(runtime.Start(config));
    state->Push(controls);
    pbapp::FeedbackSnapshot received;
    REQUIRE(WaitFor([&]() { return ReadSnapshot(feedback, received) && received.identityBound; }));
    REQUIRE(received.phase == pbapp::FeedbackPhase::Receiving);
    REQUIRE(pbapp::GetFeedbackCompletedSegmentCount(received) == 0);
    REQUIRE_FALSE(received.wholeDigestVerified);
    REQUIRE_FALSE(received.finalPublishSucceeded);
    REQUIRE_FALSE(received.finalReopenVerified);
    for (const auto& frame : frames)
    {
        state->Push(frame);
    }
    REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().state == pbapp::DecoderState::Completed; }));
    const auto completed = runtime.GetSnapshot();
    INFO(completed.errorDetail);
    REQUIRE(completed.feedback.listening);
    REQUIRE_FALSE(runtime.Start(config));
    REQUIRE(completed.wholeFileDigestCheck == true);
    REQUIRE(completed.finalRenameSucceeded == true);
    REQUIRE(completed.finalReopenVerified == true);
    REQUIRE(HasResume(output));
    REQUIRE(WaitFor([&]() { return ReadSnapshot(feedback, received) && received.phase == pbapp::FeedbackPhase::Completed; }));
    REQUIRE(received.identity.segmentCount == 1);
    REQUIRE(pbapp::GetFeedbackCompletedSegmentCount(received) == 1);
    REQUIRE(received.wholeDigestVerified);
    REQUIRE(received.finalPublishSucceeded);
    REQUIRE(received.finalReopenVerified);
    const auto first = received;
    const std::filesystem::path finalPath(std::u8string(completed.outputPath.begin(), completed.outputPath.end()));
    const HANDLE viewingReader = CreateFileW(finalPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(viewingReader != INVALID_HANDLE_VALUE);
    CloseHandle(viewingReader);
    const HANDLE modifyingWriter = CreateFileW(finalPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (modifyingWriter != INVALID_HANDLE_VALUE)
    {
        CloseHandle(modifyingWriter);
    }
    REQUIRE(modifyingWriter == INVALID_HANDLE_VALUE);
    REQUIRE_FALSE(MoveFileExW(finalPath.c_str(), (output / L"moved.bin").c_str(), 0));
    REQUIRE_FALSE(DeleteFileW(finalPath.c_str()));
    auto bootstrapOnly = frames.back();
    auto futureBootstrap = pbprotocol::ParseBootstrapRecord(bootstrapOnly.bootstrapRecord);
    REQUIRE(futureBootstrap);
    futureBootstrap.Value().frameSequence = 1'000'000;
    REQUIRE(pbprotocol::SerializeBootstrapRecord(futureBootstrap.Value(), bootstrapOnly.bootstrapRecord));
    bootstrapOnly.bootstrap.canonical44 = bootstrapOnly.bootstrapRecord;
    bootstrapOnly.kind = pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly;
    state->Push(bootstrapOnly);
    const auto nextSnapshot = received.snapshotSequence;
    REQUIRE(WaitFor([&]() { return ReadSnapshot(feedback, received) && received.snapshotSequence > nextSnapshot; }));
    REQUIRE(received.observedFrameSequence == first.observedFrameSequence);
    const auto moreFrames = MakeFrames(source, bytes, 1, nullptr, {}, 0, true);
    state->Push(moreFrames.front());
    REQUIRE(WaitFor([&]() { return ReadSnapshot(feedback, received) && received.observedFrameSequence > first.observedFrameSequence; }));
    REQUIRE(runtime.GetSnapshot().activeReceptionElapsedMilliseconds == completed.activeReceptionElapsedMilliseconds);
    REQUIRE(runtime.GetSnapshot().recoveryRuntimeMilliseconds == completed.recoveryRuntimeMilliseconds);
    REQUIRE(runtime.GetSnapshot().feedback.listenerElapsedMilliseconds.value_or(0) > 0);
    REQUIRE(runtime.GetSnapshot().runEndedUnixMilliseconds == completed.runEndedUnixMilliseconds);
    REQUIRE(runtime.GetSnapshot().verifiedSegmentCount == completed.verifiedSegmentCount);
    REQUIRE(runtime.GetSnapshot().outerUniqueSymbols == completed.outerUniqueSymbols);
    runtime.Stop();
    REQUIRE(runtime.GetSnapshot().state == pbapp::DecoderState::Completed);
    REQUIRE_FALSE(runtime.GetSnapshot().feedback.listening);
    REQUIRE(runtime.GetSnapshot().feedback.listenerStoppedUnixMilliseconds);
    REQUIRE(runtime.GetSnapshot().recoveryRuntimeMilliseconds == completed.recoveryRuntimeMilliseconds);
    const HANDLE releasedWriter = CreateFileW(finalPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(releasedWriter != INVALID_HANDLE_VALUE);
    CloseHandle(releasedWriter);

    state = std::make_shared<ReceiveState>();
    pbapp::DecoderRuntime restarted(Services(state));
    REQUIRE(restarted.Start(config));
    state->Push(moreFrames.front());
    REQUIRE(WaitFor([&]() { return restarted.GetSnapshot().state == pbapp::DecoderState::Completed; }));
    REQUIRE(restarted.GetSnapshot().outputRecoveredAfterPublish);
    const bool restartedReceipt = WaitFor([&]() { return ReadSnapshot(feedback, received) && received.receiverEpoch != first.receiverEpoch && received.phase == pbapp::FeedbackPhase::Completed; });
    INFO("state=" << static_cast<int>(restarted.GetSnapshot().feedback.state) << " error=" << static_cast<int>(restarted.GetSnapshot().feedback.lastError) <<
        " status=" << restarted.GetSnapshot().feedback.statusMessage << " flags=" << restarted.GetSnapshot().wholeFileDigestCheck.value_or(false) <<
        restarted.GetSnapshot().finalRenameSucceeded.value_or(false) << restarted.GetSnapshot().finalReopenVerified.value_or(false) <<
        " count=" << restarted.GetSnapshot().feedback.confirmedSegments << " filephase=" << static_cast<int>(received.phase));
    REQUIRE(restartedReceipt);
    REQUIRE(received.receiverId == first.receiverId);
    REQUIRE(pbapp::GetFeedbackCompletedSegmentCount(received) == 1);
    restarted.Stop();
}

TEST_CASE("Feedback failure cannot undo Decoder success and disabled cleanup stays unchanged", "[feedback][runtime][decoder]")
{
    Scratch scratch;
    const auto frames = MakeFrames(scratch.Directory(L"source"), RawBytes(32));
    for (const bool enabled : {false, true})
    {
        const auto output = scratch.Directory(enabled ? L"enabled" : L"disabled");
        const auto state = std::make_shared<ReceiveState>();
        pbapp::DecoderRuntime runtime(Services(state));
        auto config = pbapp::MakeUnifiedDecoderConfig(output.wstring(), Region());
        if (enabled)
        {
            config.feedback = pbapp::FeedbackConfig{output / L"missing-parent"};
        }
        REQUIRE(runtime.Start(config));
        state->Push(frames.front());
        REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().state == pbapp::DecoderState::Completed; }));
        REQUIRE(runtime.GetSnapshot().wholeFileDigestCheck == true);
        REQUIRE(runtime.GetSnapshot().feedback.requested == enabled);
        REQUIRE(HasResume(output) == enabled);
        runtime.Stop();
        REQUIRE(runtime.GetSnapshot().state == pbapp::DecoderState::Completed);
    }
}

TEST_CASE("Feedback target overlapping the output cannot steal a reservation after visual initialization", "[feedback][runtime][decoder][path]")
{
    Scratch scratch;
    const auto source = scratch.Directory(L"source");
    const auto output = scratch.Directory(L"output");
    const auto frames = MakeFrames(source, RawBytes(32), 3);
    auto controls = frames.front();
    controls.demodulation.acceptedUnifiedBlockCount = 0;
    for (std::uint32_t index = 0; index < frames.front().demodulation.acceptedUnifiedBlockCount; index++)
    {
        const auto& block = frames.front().demodulation.acceptedUnifiedBlocks[index];
        if (block.kind == pbmodulation::UnifiedSlotKind::Control)
        {
            controls.demodulation.acceptedUnifiedBlocks[controls.demodulation.acceptedUnifiedBlockCount++] = block;
        }
    }
    const auto feedbackPath = output / L"g16-source.bin";
    const auto occupiedTemporary = feedbackPath.wstring() + L".tmp";
    {
        std::ofstream file(occupiedTemporary, std::ios::binary);
        file << "owned test interposition";
        REQUIRE(file.good());
    }
    const auto state = std::make_shared<ReceiveState>();
    pbapp::DecoderRuntime runtime(Services(state));
    auto config = pbapp::MakeUnifiedDecoderConfig(output.wstring(), Region());
    config.feedback = pbapp::FeedbackConfig{feedbackPath, pbapp::FeedbackLocationKind::File};
    REQUIRE(runtime.Start(config));
    state->Push(controls);
    REQUIRE(WaitFor([&]()
    {
        for (const auto& entry : std::filesystem::directory_iterator(output))
        {
            if (entry.path().extension() == L".part")
            {
                return true;
            }
        }
        return false;
    }));
    REQUIRE_FALSE(std::filesystem::exists(feedbackPath));
    REQUIRE(std::filesystem::remove(occupiedTemporary));
    REQUIRE(WaitFor([&]()
    {
        if (runtime.GetSnapshot().feedback.lastError == pbapp::FeedbackError::Conflict)
        {
            return true;
        }
        std::vector<std::byte> bytes;
        return static_cast<bool>(pbapp::ReadFeedbackFile(feedbackPath, bytes));
    }));
    for (const auto& frame : frames)
    {
        state->Push(frame);
    }
    REQUIRE(WaitFor([&]()
    {
        const auto current = runtime.GetSnapshot().state;
        return current == pbapp::DecoderState::Completed || current == pbapp::DecoderState::Failed;
    }));
    const auto completed = runtime.GetSnapshot();
    INFO(completed.errorDetail);
    REQUIRE(completed.state == pbapp::DecoderState::Completed);
    REQUIRE(completed.feedback.lastError == pbapp::FeedbackError::Conflict);
    REQUIRE_FALSE(completed.feedback.terminalPublished);
    REQUIRE(std::filesystem::file_size(feedbackPath) == 32);
    runtime.Stop();
}

TEST_CASE("Decoder defers feedback writes until part and journal paths can be excluded", "[feedback][runtime][decoder][path]")
{
    const auto extension = GENERATE(L".part", L".resume", L".resume.tmp");
    Scratch scratch;
    const auto source = scratch.Directory(L"source");
    const auto output = scratch.Directory(L"output");
    const auto frames = MakeFrames(source, RawBytes(32));
    const auto bootstrap = pbprotocol::ParseBootstrapRecord(frames.front().bootstrapRecord);
    REQUIRE(bootstrap);
    std::wostringstream name;
    name << L"PixelBridge-" << std::hex << std::setw(16) << std::setfill(L'0') << bootstrap.Value().sessionTag.value << extension;
    const auto feedbackPath = output / name.str();
    const auto state = std::make_shared<ReceiveState>();
    pbapp::DecoderRuntime runtime(Services(state));
    auto config = pbapp::MakeUnifiedDecoderConfig(output.wstring(), Region());
    config.feedback = pbapp::FeedbackConfig{feedbackPath, pbapp::FeedbackLocationKind::File};
    REQUIRE(runtime.Start(config));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE_FALSE(std::filesystem::exists(feedbackPath));
    REQUIRE_FALSE(std::filesystem::exists(feedbackPath.wstring() + L".lock"));
    state->Push(frames.front());
    REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().state == pbapp::DecoderState::Completed; }));
    REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().feedback.lastError == pbapp::FeedbackError::Conflict; }));
    REQUIRE_FALSE(runtime.GetSnapshot().feedback.terminalPublished);
    REQUIRE(runtime.GetSnapshot().wholeFileDigestCheck == true);
    REQUIRE(runtime.GetSnapshot().finalReopenVerified == true);
    runtime.Stop();
}

TEST_CASE("Retained final is revalidated and corruption cannot generate a new terminal receipt", "[feedback][runtime][decoder]")
{
    Scratch scratch;
    const auto frames = MakeFrames(scratch.Directory(L"source"), RawBytes(32));
    const auto output = scratch.Directory(L"output");
    const auto feedback = scratch.Directory(L"feedback");
    auto config = pbapp::MakeUnifiedDecoderConfig(output.wstring(), Region());
    config.feedback = pbapp::FeedbackConfig{feedback};
    std::string finalPath;
    {
        const auto state = std::make_shared<ReceiveState>();
        pbapp::DecoderRuntime runtime(Services(state));
        REQUIRE(runtime.Start(config));
        state->Push(frames.front());
        REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().state == pbapp::DecoderState::Completed; }));
        finalPath = runtime.GetSnapshot().outputPath;
        runtime.Stop();
    }
    const auto path = std::filesystem::path(std::u8string(finalPath.begin(), finalPath.end()));
    bool missing = false;
    SECTION("Same-length digest corruption")
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        const std::string corruptBytes(32, '\0');
        file.write(corruptBytes.data(), static_cast<std::streamsize>(corruptBytes.size()));
        REQUIRE(file.good());
    }
    SECTION("Missing published object")
    {
        missing = true;
        REQUIRE(std::filesystem::remove(path));
    }
    const auto state = std::make_shared<ReceiveState>();
    pbapp::DecoderRuntime runtime(Services(state));
    REQUIRE(runtime.Start(config));
    state->Push(frames.front());
    REQUIRE(WaitFor([&]() { return runtime.GetSnapshot().state == pbapp::DecoderState::Failed; }));
    REQUIRE_FALSE(runtime.GetSnapshot().feedback.terminalPublished);
    REQUIRE_FALSE(runtime.GetSnapshot().finalPublishSucceeded);
    runtime.Stop();
    REQUIRE(std::filesystem::exists(path) != missing);
    if (!missing)
    {
        REQUIRE(std::filesystem::file_size(path) == 32);
    }
}

TEST_CASE("Encoder automatically stops only after Decoder observes a successful Submit in the current Start", "[feedback][runtime][encoder]")
{
    Scratch scratch;
    const auto source = scratch.Directory(L"source");
    const auto initialFrames = MakeFrames(source, RawBytes(32));
    const auto output = scratch.Directory(L"output");
    const auto feedback = scratch.Directory(L"feedback");
    const auto state = std::make_shared<ReceiveState>();
    pbapp::DecoderRuntime decoder(Services(state));
    auto decoderConfig = pbapp::MakeUnifiedDecoderConfig(output.wstring(), Region());
    decoderConfig.feedback = pbapp::FeedbackConfig{feedback};
    REQUIRE(decoder.Start(decoderConfig));
    state->Push(initialFrames.front());
    REQUIRE(WaitFor([&]() { return decoder.GetSnapshot().feedback.terminalPublished; }));
    const auto pixels = std::make_shared<PixelFrames>();
    pbapp::EncoderRuntime encoder([pixels](const auto&) { return std::make_unique<PixelCollector>(pixels); });
    auto encoderConfig = pbapp::MakeUnifiedEncoderConfig((source / L"g16-source.bin").wstring(), 60);
    encoderConfig.sessionStateRoot = source / L"encoder-state";
    encoderConfig.feedback = pbapp::FeedbackConfig{feedback};
    REQUIRE(encoder.Start(encoderConfig));
    REQUIRE(WaitFor([&]() { const std::scoped_lock lock(pixels->mutex); return !pixels->frames.empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(encoder.GetSnapshot().state == pbapp::EncoderState::Broadcasting);
    REQUIRE_FALSE(encoder.GetSnapshot().feedback.terminalAccepted);
    auto oracleResult = pbmodulation::UnifiedVisualCpuOracle::Create(pbmodulation::UnifiedVisualCpuOracle::RequiredBytes());
    REQUIRE(oracleResult);
    auto oracle = std::move(oracleResult).Value();
    pbdemodd3d11::CaptureDemodulatorResult result;
    {
        const std::scoped_lock lock(pixels->mutex);
        result.kind = pbdemodd3d11::CaptureDemodulatorResultKind::UnifiedFrame;
        result.geometryStatus = pbdemodd3d11::CaptureDemodulatorGeometryStatus::ExactCanvas;
        const auto observation = oracle.DecodeMixedFrame({pixels->frames.front(), 1920, 1080, 1920 * 4, pbmodulation::LumaPixelFormat::Bgra8});
        REQUIRE(observation.IsFrameAvailable());
        result.bootstrap = observation.bootstrap;
        REQUIRE(pbprotocol::SerializeBootstrapRecord(observation.bootstrapRecord, result.bootstrapRecord));
        result.demodulation.visualProfileId = observation.bootstrapRecord.visualProfileId;
        result.demodulation.unifiedObservation = observation;
        const auto blocks = oracle.GetAcceptedBlocks();
        result.demodulation.acceptedUnifiedBlockCount = static_cast<std::uint32_t>(blocks.size());
        std::copy(blocks.begin(), blocks.end(), result.demodulation.acceptedUnifiedBlocks.begin());
    }
    state->Push(result);
    REQUIRE(WaitFor([&]() { return encoder.GetSnapshot().state == pbapp::EncoderState::Stopped; }));
    REQUIRE(encoder.GetSnapshot().stopReason == pbapp::EncoderStopReason::ReceiverVerified);
    REQUIRE(encoder.GetSnapshot().feedback.terminalAccepted);
    REQUIRE(encoder.GetSnapshot().feedback.receiverReportedCompletionMilliseconds == decoder.GetSnapshot().recoveryRuntimeMilliseconds);
    REQUIRE_FALSE(encoder.GetSnapshot().stoppedByTimeout);
    encoder.Stop();
    decoder.Stop();
}
