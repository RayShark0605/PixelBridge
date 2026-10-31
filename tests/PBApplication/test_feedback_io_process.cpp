#include "feedback_io_process.h"
#include "feedback_file_store.h"
#include "pbprotocol/session_random.h"
#include <catch2/catch_test_macros.hpp>
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <thread>

namespace
{
class ScratchDirectory
{
public:
    ScratchDirectory()
    {
        const auto random = pbprotocol::GenerateRandomSessionId();
        REQUIRE(random);
        std::wstring suffix;
        for (const auto value : random.Value().bytes)
        {
            constexpr wchar_t digits[] = L"0123456789abcdef";
            suffix.push_back(digits[std::to_integer<unsigned int>(value) >> 4U]);
            suffix.push_back(digits[std::to_integer<unsigned int>(value) & 15U]);
        }
        path_ = std::filesystem::path(PB_TEST_SCRATCH_ROOT) / (L"feedback io 测试 " + suffix);
        REQUIRE(std::filesystem::create_directory(path_));
    }
    ~ScratchDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& GetPath() const noexcept
    {
        return path_;
    }
private:
    std::filesystem::path path_;
};

std::uint64_t Now()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

template<typename Predicate>
pbapp::FeedbackIoResult WaitFor(pbapp::FeedbackIoProcess& process, Predicate predicate)
{
    const auto deadline = Now() + 4'000'000'000ULL;
    pbapp::FeedbackIoResult result;
    do
    {
        result = process.Poll(Now());
        if (predicate(result))
        {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (Now() < deadline);
    return result;
}

pbapp::FeedbackSnapshot MakeSnapshot(const std::uint64_t sequence)
{
    pbapp::FeedbackSnapshot snapshot;
    snapshot.receiverId.fill(std::byte{1});
    snapshot.receiverEpoch.fill(std::byte{2});
    snapshot.snapshotSequence = sequence;
    return snapshot;
}
}

TEST_CASE("Disabled feedback creates no helper and missing helper fails bounded", "[feedback][process]")
{
    pbapp::FeedbackIoProcess disabled;
    REQUIRE_FALSE(disabled.IsRunning());
    disabled.Stop();
    REQUIRE_FALSE(disabled.Poll(Now()).running);
    ScratchDirectory scratch;
    pbapp::FeedbackIoProcess absent(scratch.GetPath() / L"missing.exe");
    REQUIRE_FALSE(absent.Start(pbapp::FeedbackIoRole::Reader, {scratch.GetPath()}, 1));
    REQUIRE_FALSE(absent.IsRunning());
}

TEST_CASE("Actual isolated writer and reader exchange canonical metadata on quoted Unicode path", "[feedback][process]")
{
    ScratchDirectory scratch;
    const pbapp::FeedbackConfig config{scratch.GetPath()};
    pbapp::FeedbackIoProcess writer(PB_FEEDBACK_HELPER_PATH);
    REQUIRE(writer.Start(pbapp::FeedbackIoRole::Writer, config, 17));
    auto ready = WaitFor(writer, [](const auto& result) { return result.ready; });
    INFO(ready.status.message << " nativeError=" << ready.status.nativeError);
    REQUIRE(ready.ready);
    REQUIRE(ready.runToken == 17);
    for (std::uint64_t sequence = 1; sequence <= 50; sequence++)
    {
        REQUIRE(writer.QueueSnapshot(MakeSnapshot(sequence), 17));
    }
    REQUIRE(writer.GetPendingSnapshotCount() == 1);
    REQUIRE_FALSE(writer.QueueSnapshot(MakeSnapshot(51), 16));
    const auto published = WaitFor(writer, [](const auto& result) { return result.publishedSequence == 50; });
    INFO(published.status.message << " nativeError=" << published.status.nativeError);
    REQUIRE(published.publishedSequence == 50);
    pbapp::FeedbackIoProcess reader(PB_FEEDBACK_HELPER_PATH);
    REQUIRE(reader.Start(pbapp::FeedbackIoRole::Reader, config, 19));
    const auto received = WaitFor(reader, [](const auto& result) { return result.snapshot.has_value(); });
    REQUIRE(received.snapshot);
    REQUIRE(received.snapshot->snapshotSequence == 50);
    REQUIRE(received.snapshot->receiverId == ready.receiverId);
    REQUIRE(received.runToken == 19);
    reader.Stop();
    writer.Stop();
    REQUIRE_FALSE(reader.IsRunning());
    REQUIRE_FALSE(writer.IsRunning());
    REQUIRE(writer.Start(pbapp::FeedbackIoRole::Writer, config, 20));
    ready = WaitFor(writer, [](const auto& result) { return result.ready; });
    REQUIRE(ready.ready);
    REQUIRE(ready.receiverId == received.snapshot->receiverId);
    REQUIRE(ready.publishedSequence == 0);
    REQUIRE_FALSE(ready.snapshot);
    writer.Stop();
}

TEST_CASE("Missing feedback remains read only and helper recovers when file appears", "[feedback][process]")
{
    ScratchDirectory scratch;
    pbapp::FeedbackIoProcess reader(PB_FEEDBACK_HELPER_PATH);
    REQUIRE(reader.Start(pbapp::FeedbackIoRole::Reader, {scratch.GetPath()}, 3));
    const auto missing = WaitFor(reader, [](const auto& result) { return result.ready && !result.status; });
    REQUIRE(missing.ready);
    REQUIRE_FALSE(missing.status);
    REQUIRE(std::filesystem::is_empty(scratch.GetPath()));
    pbapp::FeedbackFilePublisher publisher;
    REQUIRE(publisher.Open(scratch.GetPath() / L"PixelBridgeFeedback.pbfb"));
    auto snapshot = MakeSnapshot(1);
    snapshot.receiverId = publisher.GetReceiverId();
    REQUIRE(publisher.Publish(snapshot));
    const auto received = WaitFor(reader, [](const auto& result) { return result.snapshot.has_value(); });
    REQUIRE(received.snapshot);
    REQUIRE(received.status);
    reader.Stop();
}

TEST_CASE("Malformed and wrong-generation helper replies are rejected", "[feedback][process]")
{
    ScratchDirectory scratch;
    for (const auto* const name : {L"bad-version.pbfb", L"bad-token.pbfb", L"bad-length.pbfb", L"exit.pbfb"})
    {
        INFO(std::filesystem::path(name).string());
        pbapp::FeedbackIoProcess process(PB_FEEDBACK_FIXTURE_PATH);
        REQUIRE(process.Start(pbapp::FeedbackIoRole::Reader,
            {scratch.GetPath() / name, pbapp::FeedbackLocationKind::File}, 8));
        const auto result = WaitFor(process, [](const auto& result) { return !result.running; });
        REQUIRE_FALSE(result.status);
        REQUIRE_FALSE(result.ready);
        REQUIRE_FALSE(result.snapshot);
        process.Stop();
        REQUIRE_FALSE(process.IsRunning());
    }
}

TEST_CASE("Blocked helper timeout and rapid stop restart retain a single child bound", "[feedback][process]")
{
    ScratchDirectory scratch;
    pbapp::FeedbackIoProcess process(PB_FEEDBACK_FIXTURE_PATH);
    const pbapp::FeedbackConfig config{scratch.GetPath() / L"blocked.pbfb", pbapp::FeedbackLocationKind::File};
    REQUIRE(process.Start(pbapp::FeedbackIoRole::Reader, config, 1));
    const auto initial = Now();
    REQUIRE(process.Poll(initial).running);
    const auto timeout = process.Poll(initial + 5'100'000'000ULL);
    REQUIRE(timeout.timedOut);
    REQUIRE_FALSE(timeout.status);
    REQUIRE_FALSE(timeout.snapshot);
    const auto beforeStop = Now();
    process.Stop();
    REQUIRE(Now() - beforeStop < 1'000'000'000ULL);
    REQUIRE_FALSE(process.IsRunning());
    for (std::uint64_t token = 2; token <= 8; token++)
    {
        REQUIRE(process.Start(pbapp::FeedbackIoRole::Reader, config, token));
        REQUIRE(process.Poll(Now()).runToken == token);
        process.Stop();
        REQUIRE_FALSE(process.IsRunning());
        REQUIRE_FALSE(process.Poll(Now()).snapshot);
    }
}

TEST_CASE("Ready helper with a blocked write operation cannot block owner polling or teardown", "[feedback][process]")
{
    ScratchDirectory scratch;
    pbapp::FeedbackIoProcess process(PB_FEEDBACK_FIXTURE_PATH);
    const pbapp::FeedbackConfig config{scratch.GetPath() / L"blocked-operation.pbfb", pbapp::FeedbackLocationKind::File};
    REQUIRE(process.Start(pbapp::FeedbackIoRole::Writer, config, 99));
    const auto ready = WaitFor(process, [](const auto& result) { return result.ready; });
    REQUIRE(ready.ready);
    REQUIRE(process.QueueSnapshot(MakeSnapshot(1), 99));
    const auto started = Now();
    REQUIRE(process.Poll(started).running);
    const auto timeout = process.Poll(started + 5'100'000'000ULL);
    REQUIRE(timeout.timedOut);
    REQUIRE_FALSE(timeout.status);
    REQUIRE_FALSE(timeout.snapshot);
    REQUIRE(Now() - started < 1'000'000'000ULL);
    process.Stop();
    REQUIRE_FALSE(process.IsRunning());
    REQUIRE(process.GetPendingSnapshotCount() == 0);
}
