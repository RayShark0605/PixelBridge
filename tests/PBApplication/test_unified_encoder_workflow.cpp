#include "local_desktop_runtime.h"
#include "encoder_session_store.h"
#include "sender_carousel_scheduler.h"
#include "pbmodulation/unified_visual.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/control_plane_receiver.h"
#include "pbprotocol/transport_block_codec.h"
#include "pbreceiver/receiver_ingress.h"
#include "pbstorage/output_file.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>

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
        const std::scoped_lock lock(state_->mutex);
        pbrenderd3d::DataWindowSnapshot snapshot;
        snapshot.state = state_->stopped ? pbrenderd3d::WindowState::Stopped : pbrenderd3d::WindowState::Running;
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
    REQUIRE(std::string_view(pbapp::GetVisualProfileName(config.visualProfile)) == "PB-Unified-LC4-V1");
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

TEST_CASE("Unified prescan visits two fixed size Segments and reports automatic compression", "[application][g15][runtime][prescan]")
{
    Scratch scratch;
    const auto source = scratch.Path() / L"source.bin";
    const std::vector<std::byte> bytes(8 * 1024 * 1024 + 1, std::byte{0x35});
    WriteBytes(source, bytes);
    auto state = std::make_shared<PresentationState>();
    state->maximumFrames = 2;
    pbapp::EncoderRuntime runtime([state](const pbrenderd3d::DataWindowConfig&)
    {
        return std::make_unique<MockPresentation>(state);
    });
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 60);
    config.sessionStateRoot = scratch.Path() / L"sessions";
    REQUIRE(runtime.Start(config));
    REQUIRE(WaitFor([&]()
    {
        return runtime.GetSnapshot().submittedFrames == 2;
    }));
    runtime.Stop();
    const auto stopped = runtime.GetSnapshot();
    REQUIRE(stopped.state == pbapp::EncoderState::Stopped);
    REQUIRE(stopped.preparedSegmentCount == 2);
    REQUIRE(stopped.preparedSourceBytes == bytes.size());
    REQUIRE(stopped.zstdSegmentCount == 1);
    REQUIRE(stopped.rawSegmentCount == 1);
    REQUIRE(stopped.cycleCount == 1);
    const auto output = scratch.Path() / L"output";
    REQUIRE(std::filesystem::create_directory(output));
    VerifyPublishedPixels(state->frames, bytes, output);
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
    REQUIRE(frame.controlSlotCount == 12);
    REQUIRE(frame.firstEquationIndex == 0);
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(clock.Commit(0));
    REQUIRE(clock.RequestFramesPerSecond(1, second));
    REQUIRE(clock.Acquire(9 * second, tick));
    REQUIRE(tick.disposition == pbapp::SenderLogicalFrameTickDisposition::Ready);
    REQUIRE(scheduler.PrepareFrameAt(tick.logicalTickOrdinal, 9 * second, frame));
    REQUIRE(frame.controlSlotCount == 0);
    REQUIRE(frame.firstEquationIndex == 19);
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
    REQUIRE(frame.controlSlotCount == 12);
    REQUIRE(frame.firstEquationIndex == 50);
    REQUIRE(scheduler.CommitPreparedFrame());
    REQUIRE(clock.Commit(10 * second));
    REQUIRE_FALSE(scheduler.PrepareFrameAt(tick.logicalTickOrdinal + 1, 9 * second, frame));
    REQUIRE(scheduler.GetSnapshot().committedFrameCount == 3);
    REQUIRE(scheduler.GetSnapshot().committedEquationCount == 69);
    REQUIRE(scheduler.GetSnapshot().controlBurstCount == 2);
    REQUIRE(pbapp::SenderUnifiedCarouselScheduler::Create({64000, 4, 15, true}, scheduler));
    REQUIRE(scheduler.PrepareFrameAt(0, (std::numeric_limits<std::uint64_t>::max)() - 1, frame));
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
