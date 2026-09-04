#include "unified_decoder_test_support.h"
#include "pbprotocol/transport_block_codec.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <iostream>

namespace
{
using namespace g16test;

class Scratch final
{
public:
    Scratch() : root_(std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal()),
        path_(root_ / (L"g16-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())))
    {
        REQUIRE(path_.parent_path() == root_);
        REQUIRE(std::filesystem::create_directory(path_));
    }
    ~Scratch()
    {
        if (path_.parent_path() == root_ && path_.filename().wstring().starts_with(L"g16-"))
        {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    std::filesystem::path Directory(const wchar_t* name) const
    {
        const auto path = path_ / name;
        std::filesystem::create_directories(path);
        return path;
    }
private:
    std::filesystem::path root_;
    std::filesystem::path path_;
};

bool CompletedOrFailed(const pbapp::DecoderRuntime& runtime)
{
    const auto state = runtime.GetSnapshot().state;
    return state == pbapp::DecoderState::Completed || state == pbapp::DecoderState::Failed;
}
} // namespace

TEST_CASE("G16 Unified Decoder policy selects real automatic runtime and bounded physical ROI", "[application][g16][model]")
{
    Scratch scratch;
    auto config = pbapp::MakeUnifiedDecoderConfig(scratch.Directory(L"out").wstring(), Region());
    REQUIRE(config.visualProfile == pbapp::VisualProfile::UnifiedLc4);
    REQUIRE(config.captureBackend == pbapp::CaptureBackend::Auto);
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.region.physicalRect = {0, 0, 1440, 810};
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.region.physicalRect = {0, 0, 3840, 2160};
    const auto maximumRoiValidation = pbapp::ValidateDecoderConfig(config);
    INFO(maximumRoiValidation.message);
    REQUIRE(maximumRoiValidation);
    pbdemodd3d11::CaptureDemodulatorConfig budgetConfig;
    budgetConfig.visualProfileId = pbprotocol::kUnifiedVisualProfileId;
    budgetConfig.slotCount = 4;
    budgetConfig.resultQueueCapacity = 128;
    budgetConfig.maximumResidentBytes = 256ULL * 1024 * 1024;
    pbdemodd3d11::CaptureDemodulatorBudget budget;
    REQUIRE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(budgetConfig, budget));
    std::cout << "G16 3840x2160 demod reservation: " << budget.totalBytes << " bytes; cap=" << budgetConfig.maximumResidentBytes << '\n';
    config.region.physicalRect = {0, 0, 1439, 810};
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.region = Region();
    config.captureBackend = pbapp::CaptureBackend::Dxgi;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.captureBackend = pbapp::CaptureBackend::Auto;
    config.replayOutputPath = L"not-a-product-option.pbrv2";
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.replayOutputPath.clear();
    const auto state = std::make_shared<ReceiveState>();
    auto services = Services(state);
    services.outputConfirmationThresholdBytes = (std::numeric_limits<std::uint64_t>::max)();
    pbapp::DecoderRuntime invalid(std::move(services));
    REQUIRE_FALSE(invalid.Start(config));
    REQUIRE(state->sessionStarts == 0);
}

TEST_CASE("G16 actual mixed pixels reach Decoder runtime final publish and reopen", "[application][g16][runtime]")
{
    Scratch scratch;
    std::vector<std::byte> bytes;
    SECTION("zero-byte formal Session and FinalManifest")
    {
    }
    SECTION("RAW Wirehair and arbitrarily ordered compact accepted array")
    {
        bytes = RawBytes(20000);
    }
    auto frames = MakeFrames(scratch.Directory(L"tx"), bytes);
    auto& blocks = frames[0].demodulation.acceptedUnifiedBlocks;
    std::reverse(blocks.begin(), blocks.begin() + frames[0].demodulation.acceptedUnifiedBlockCount);
    const auto state = std::make_shared<ReceiveState>();
    state->Push(frames[0]);
    pbapp::DecoderRuntime runtime(Services(state));
    const auto config = pbapp::MakeUnifiedDecoderConfig(scratch.Directory(L"out").wstring(), Region());
    REQUIRE(runtime.Start(config));
    REQUIRE(WaitFor([&]()
    {
        return CompletedOrFailed(runtime);
    }));
    runtime.Stop();
    const auto snapshot = runtime.GetSnapshot();
    INFO(snapshot.errorDetail);
    REQUIRE(VerifyOutput(snapshot, bytes));
    REQUIRE(snapshot.originalFileNameUtf8 == "g16-source.bin");
    REQUIRE(snapshot.verifiedSegmentCount == (bytes.empty() ? 0 : 1));
    REQUIRE(snapshot.requestedBackend == pbapp::CaptureBackend::Auto);
    REQUIRE(snapshot.actualBackend == pbapp::CaptureBackend::Wgc);
    REQUIRE(snapshot.geometryStatus == "ExactCanvas");
    REQUIRE(snapshot.observedLocatorGeometry.samples > 0);
    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(std::u8string(snapshot.resumeStatePath.begin(), snapshot.resumeStatePath.end()))));
    REQUIRE(state->requestedDemod.visualProfileId == pbprotocol::kUnifiedVisualProfileId);
    REQUIRE(state->requestedDemod.maximumRoiWidth == 1920);
}

TEST_CASE("G16 pending output locks its Session and applies one explicit small-threshold decision", "[application][g16][confirmation]")
{
    Scratch scratch;
    const auto bytes = RawBytes(20000);
    const auto frames = MakeFrames(scratch.Directory(L"tx"), bytes);
    const auto foreignFrames = MakeFrames(scratch.Directory(L"foreign-tx"), bytes);
    const auto directory = scratch.Directory(L"out");
    const auto state = std::make_shared<ReceiveState>();
    auto services = Services(state);
    services.outputConfirmationThresholdBytes = 4096;
    state->Push(frames[0]);
    pbapp::DecoderRuntime runtime(std::move(services));
    REQUIRE(runtime.Start(pbapp::MakeUnifiedDecoderConfig(directory.wstring(), Region())));
    REQUIRE(WaitFor([&]()
    {
        return runtime.GetSnapshot().state == pbapp::DecoderState::AwaitingLargeOutputConfirmation || CompletedOrFailed(runtime);
    }));
    const auto pending = runtime.GetSnapshot();
    INFO(pending.errorDetail);
    REQUIRE(pending.state == pbapp::DecoderState::AwaitingLargeOutputConfirmation);
    REQUIRE(pending.largeOutputConfirmationRequestId != 0);
    REQUIRE(std::filesystem::is_empty(directory));
    state->Push(foreignFrames[0]);
    state->Push(frames[0]);
    REQUIRE(WaitFor([&]()
    {
        return state->Delivered() >= 3 || CompletedOrFailed(runtime);
    }));
    REQUIRE(runtime.GetSnapshot().sessionIdHex == pending.sessionIdHex);
    REQUIRE(runtime.GetSnapshot().largeOutputConfirmationRequestId == pending.largeOutputConfirmationRequestId);
    REQUIRE(std::filesystem::is_empty(directory));
    REQUIRE_FALSE(runtime.ResolveLargeOutputConfirmation(pending.runGeneration + 1, pending.largeOutputConfirmationRequestId, true));
    REQUIRE_FALSE(runtime.ResolveLargeOutputConfirmation(pending.runGeneration, pending.largeOutputConfirmationRequestId + 1, true));
    SECTION("accept then publish through retained same-frame Control and Transport")
    {
        REQUIRE(runtime.ResolveLargeOutputConfirmation(pending.runGeneration, pending.largeOutputConfirmationRequestId, true));
        state->Push(frames[0]);
        REQUIRE(WaitFor([&]()
        {
            return CompletedOrFailed(runtime);
        }));
        runtime.Stop();
        INFO(runtime.GetSnapshot().errorDetail);
        REQUIRE(VerifyOutput(runtime.GetSnapshot(), bytes));
    }
    SECTION("reject creates no output or resume state")
    {
        REQUIRE(runtime.ResolveLargeOutputConfirmation(pending.runGeneration, pending.largeOutputConfirmationRequestId, false));
        runtime.Stop();
        REQUIRE(runtime.GetSnapshot().state == pbapp::DecoderState::Stopped);
        REQUIRE(runtime.GetSnapshot().largeOutputConfirmationState == pbapp::LargeOutputConfirmationState::Rejected);
        REQUIRE_FALSE(runtime.GetSnapshot().finalPublishSucceeded);
        REQUIRE(std::filesystem::is_empty(directory));
    }
}

TEST_CASE("G16 Stop and capture fallback preserve verified Segments and restart recovers them", "[application][g16][resume]")
{
    Scratch scratch;
    std::vector<std::byte> bytes(8ULL * 1024 * 1024 + 1, std::byte{0x31});
    bytes.back() = std::byte{0xF3};
    const auto frames = MakeFrames(scratch.Directory(L"tx"), bytes, 2);
    const auto foreign = MakeFrames(scratch.Directory(L"foreign"), RawBytes(100));
    const auto directory = scratch.Directory(L"out");
    const auto config = pbapp::MakeUnifiedDecoderConfig(directory.wstring(), Region());
    std::string sessionId;
    bool failInitialization = false;
    SECTION("initialization failure then DXGI recovery")
    {
        failInitialization = true;
    }
    SECTION("AccessLost after a verified Segment")
    {
    }
    {
        const auto state = std::make_shared<ReceiveState>();
        state->failWgc = failInitialization;
        state->Push(frames[0]);
        pbapp::DecoderRuntime runtime(Services(state));
        REQUIRE(runtime.Start(config));
        REQUIRE(WaitFor([&]()
        {
            return runtime.GetSnapshot().verifiedSegmentCount == 1 || CompletedOrFailed(runtime);
        }));
        INFO(runtime.GetSnapshot().errorDetail);
        REQUIRE(runtime.GetSnapshot().verifiedSegmentCount == 1);
        if (!failInitialization)
        {
            REQUIRE(runtime.GetSnapshot().actualBackend == pbapp::CaptureBackend::Wgc);
            const std::scoped_lock lock(state->mutex);
            state->capture.state = pbcapturenormalize::CaptureState::Failed;
            state->capture.error = pbcapturenormalize::CaptureStatus::Failure(
                pbcapturenormalize::CaptureError::AccessLost, pbcapturenormalize::CaptureStage::Callback);
        }
        REQUIRE(WaitFor([&]()
        {
            return runtime.GetSnapshot().actualBackend == pbapp::CaptureBackend::Dxgi || CompletedOrFailed(runtime);
        }));
        REQUIRE(runtime.GetSnapshot().actualBackend == pbapp::CaptureBackend::Dxgi);
        REQUIRE(runtime.GetSnapshot().captureFallbackReason == (failInitialization ? "InitializationFailure" : "AccessLost"));
        REQUIRE(runtime.GetSnapshot().verifiedSegmentCount == 1);
        const auto before = runtime.GetSnapshot();
        sessionId = before.sessionIdHex;
        REQUIRE(WaitFor([&]()
        {
            return runtime.GetSnapshot().captureStallActive;
        }));
        REQUIRE(pbapp::IsDecoderStateActive(runtime.GetSnapshot().state));
        REQUIRE(runtime.GetSnapshot().verifiedRawBytes == before.verifiedRawBytes);
        const auto deliveredBeforeDuplicates = state->Delivered();
        state->Push(frames[0]);
        state->Push(frames[0]);
        state->Push(foreign[0]);
        REQUIRE(WaitFor([&]()
        {
            return state->Delivered() >= deliveredBeforeDuplicates + 3 || CompletedOrFailed(runtime);
        }));
        runtime.Stop();
        const auto stopped = runtime.GetSnapshot();
        REQUIRE(stopped.state == pbapp::DecoderState::Stopped);
        REQUIRE(stopped.sessionIdHex == sessionId);
        REQUIRE(stopped.verifiedRawBytes == before.verifiedRawBytes);
        REQUIRE(stopped.verifiedSegmentCount == 1);
        REQUIRE(stopped.duplicateFrameSequences >= 1);
        REQUIRE_FALSE(stopped.finalPublishSucceeded);
        REQUIRE(std::filesystem::exists(std::filesystem::path(std::u8string(stopped.resumeStatePath.begin(), stopped.resumeStatePath.end()))));
    }
    const auto state = std::make_shared<ReceiveState>();
    state->Push(frames[1]);
    pbapp::DecoderRuntime resumed(Services(state));
    REQUIRE(resumed.Start(config));
    REQUIRE(WaitFor([&]()
    {
        return CompletedOrFailed(resumed);
    }));
    resumed.Stop();
    INFO(resumed.GetSnapshot().errorDetail);
    REQUIRE(VerifyOutput(resumed.GetSnapshot(), bytes));
    REQUIRE(resumed.GetSnapshot().sessionIdHex == sessionId);
    REQUIRE(resumed.GetSnapshot().resumeStateLoaded);
    REQUIRE(resumed.GetSnapshot().verifiedSegmentCount == 2);
}

TEST_CASE("G16 mixed handoff rejects malformed capacity slot identity and same-frame conflicts", "[application][g16][negative]")
{
    Scratch scratch;
    auto frames = MakeFrames(scratch.Directory(L"tx"), RawBytes(100000));
    auto changed = frames[0];
    bool conflictAfterAdmission = false;
    bool advanceConflictingFrame = false;
    SECTION("accepted count out of range")
    {
        changed.demodulation.acceptedUnifiedBlockCount = pbmodulation::kUnifiedCodewordCount + 1;
    }
    SECTION("duplicate slot index")
    {
        changed.demodulation.acceptedUnifiedBlocks[1].codewordSlot = changed.demodulation.acceptedUnifiedBlocks[0].codewordSlot;
    }
    SECTION("foreign demod profile")
    {
        changed.demodulation.visualProfileId++;
    }
    SECTION("same frame valid Transport bytes conflict")
    {
        conflictAfterAdmission = true;
    }
    SECTION("same Outer identity conflicts in a newer frame")
    {
        conflictAfterAdmission = true;
        advanceConflictingFrame = true;
    }
    if (conflictAfterAdmission)
    {
        auto& block = changed.demodulation.acceptedUnifiedBlocks[12];
        REQUIRE(block.kind == pbmodulation::UnifiedSlotKind::Transport);
        const auto parsed = pbprotocol::ParseTransportBlock(std::span(block.bytes).first(block.size));
        REQUIRE(parsed);
        auto payload = std::vector<std::byte>(parsed.Value().payload.begin(), parsed.Value().payload.end());
        payload[0] ^= std::byte{1};
        REQUIRE(pbprotocol::SerializeTransportBlock(parsed.Value().header, payload, std::span(block.bytes).first(block.size)));
    }
    if (advanceConflictingFrame)
    {
        auto& bootstrap = changed.demodulation.unifiedObservation.bootstrapRecord;
        bootstrap.frameSequence++;
        REQUIRE(pbprotocol::SerializeBootstrapRecord(bootstrap, changed.bootstrapRecord));
    }
    const auto state = std::make_shared<ReceiveState>();
    if (conflictAfterAdmission)
    {
        state->Push(frames[0]);
    }
    state->Push(changed);
    pbapp::DecoderRuntime runtime(Services(state));
    REQUIRE(runtime.Start(pbapp::MakeUnifiedDecoderConfig(scratch.Directory(L"out").wstring(), Region())));
    REQUIRE(WaitFor([&]()
    {
        return CompletedOrFailed(runtime);
    }));
    runtime.Stop();
    REQUIRE(runtime.GetSnapshot().state == pbapp::DecoderState::Failed);
    REQUIRE_FALSE(runtime.GetSnapshot().finalPublishSucceeded);
    REQUIRE(runtime.GetSnapshot().verifiedRawBytes == 0);
}
