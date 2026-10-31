#include "feedback_file_store.h"
#include "pbprotocol/session_random.h"
#include <catch2/catch_test_macros.hpp>
#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace pbapp
{
class FeedbackFilePublisherTestAccess
{
public:
    static FeedbackStatus Publish(FeedbackFilePublisher& publisher, const FeedbackSnapshot& snapshot, void (*beforeCommit)(void*), void* context)
    {
        return publisher.PublishInternal(snapshot, beforeCommit, context);
    }
};
}

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
        for (const std::byte value : random.Value().bytes)
        {
            constexpr wchar_t digits[] = L"0123456789abcdef";
            const auto number = std::to_integer<unsigned int>(value);
            suffix.push_back(digits[number >> 4U]);
            suffix.push_back(digits[number & 15U]);
        }
        path_ = std::filesystem::path(PB_TEST_SCRATCH_ROOT) / (L"feedback-files-" + suffix);
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

pbapp::FeedbackSnapshot MakeSnapshot(const std::array<std::byte, 16>& receiverId)
{
    pbapp::FeedbackSnapshot snapshot;
    snapshot.receiverId = receiverId;
    snapshot.receiverEpoch.fill(std::byte{3});
    snapshot.snapshotSequence = 1;
    return snapshot;
}

std::vector<char> ReadRaw(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteRaw(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    output << text;
    REQUIRE(output.good());
}
}

TEST_CASE("Feedback path resolution is lexical and preserves explicit UNC file selection", "[feedback][file]")
{
    ScratchDirectory scratch;
    std::filesystem::path resolved;
    REQUIRE(pbapp::ResolveFeedbackFilePath({scratch.GetPath(), pbapp::FeedbackLocationKind::Directory}, resolved));
    REQUIRE(resolved == scratch.GetPath() / L"PixelBridgeFeedback.pbfb");
    const std::filesystem::path network = LR"(\\Client\C$\Feedback\chosen.pbfb)";
    REQUIRE(pbapp::ResolveFeedbackFilePath({network, pbapp::FeedbackLocationKind::File}, resolved));
    REQUIRE(resolved == network);
    REQUIRE_FALSE(pbapp::ResolveFeedbackFilePath({{}, pbapp::FeedbackLocationKind::Directory}, resolved));
    REQUIRE_FALSE(pbapp::ResolveFeedbackFilePath({L"relative.pbfb", pbapp::FeedbackLocationKind::File}, resolved));
    REQUIRE_FALSE(pbapp::ResolveFeedbackFilePath({scratch.GetPath() / L"state.pbfb:stream", pbapp::FeedbackLocationKind::File}, resolved));
    REQUIRE_FALSE(pbapp::ResolveFeedbackFilePath({scratch.GetPath() / L"CON.pbfb", pbapp::FeedbackLocationKind::File}, resolved));
    REQUIRE(std::filesystem::is_empty(scratch.GetPath()));
}

TEST_CASE("Feedback missing read creates no file and failed reads preserve caller output", "[feedback][file]")
{
    ScratchDirectory scratch;
    const auto path = scratch.GetPath() / L"missing.pbfb";
    std::vector<std::byte> bytes{std::byte{0xA5}};
    const auto status = pbapp::ReadFeedbackFile(path, bytes);
    REQUIRE_FALSE(status);
    REQUIRE(status.error == pbapp::FeedbackError::Unavailable);
    REQUIRE(bytes == std::vector<std::byte>{std::byte{0xA5}});
    REQUIRE(std::filesystem::is_empty(scratch.GetPath()));
}

TEST_CASE("Feedback publisher keeps old readers valid across bounded no-replace publication", "[feedback][file]")
{
    ScratchDirectory scratch;
    const auto path = scratch.GetPath() / L"status.pbfb";
    pbapp::FeedbackFilePublisher publisher;
    REQUIRE(publisher.Open(path));
    auto snapshot = MakeSnapshot(publisher.GetReceiverId());
    const auto firstPublished = publisher.Publish(snapshot);
    INFO(firstPublished.message << " nativeError=" << firstPublished.nativeError);
    REQUIRE(firstPublished);
    const HANDLE oldReader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    REQUIRE(oldReader != INVALID_HANDLE_VALUE);
    snapshot.snapshotSequence = 2;
    const auto published = publisher.Publish(snapshot);
    std::array<std::byte, 1024> oldBytes{};
    DWORD oldByteCount = 0;
    const bool oldRead = ReadFile(oldReader, oldBytes.data(), static_cast<DWORD>(oldBytes.size()), &oldByteCount, nullptr) != FALSE;
    snapshot.snapshotSequence = 3;
    const auto thirdPublished = publisher.Publish(snapshot);
    CloseHandle(oldReader);
    INFO(published.message << " nativeError=" << published.nativeError);
    REQUIRE(published);
    REQUIRE(thirdPublished);
    REQUIRE(oldRead);
    pbapp::FeedbackSnapshot oldSnapshot;
    REQUIRE(pbapp::ParseFeedbackSnapshot(std::span(oldBytes).first(oldByteCount), oldSnapshot));
    REQUIRE(oldSnapshot.snapshotSequence == 1);
    std::vector<std::byte> bytes;
    REQUIRE(pbapp::ReadFeedbackFile(path, bytes));
    pbapp::FeedbackSnapshot decoded;
    REQUIRE(pbapp::ParseFeedbackSnapshot(bytes, decoded));
    REQUIRE(decoded == snapshot);
    REQUIRE_FALSE(std::filesystem::exists(path.wstring() + L".tmp"));
    REQUIRE_FALSE(std::filesystem::exists(path.wstring() + L".previous"));
    const auto writerId = publisher.GetReceiverId();
    publisher.Close();
    REQUIRE_FALSE(std::filesystem::exists(path.wstring() + L".lock"));
    REQUIRE(publisher.Open(path));
    REQUIRE(publisher.GetReceiverId() == writerId);
}

TEST_CASE("Feedback publisher refuses unrelated targets temporary files and concurrent writers", "[feedback][file]")
{
    ScratchDirectory scratch;
    const auto path = scratch.GetPath() / L"status.pbfb";
    WriteRaw(path, "unrelated user content");
    const auto original = ReadRaw(path);
    pbapp::FeedbackFilePublisher publisher;
    REQUIRE_FALSE(publisher.Open(path));
    REQUIRE(ReadRaw(path) == original);
    REQUIRE(std::filesystem::remove(path));
    REQUIRE(publisher.Open(path));
    pbapp::FeedbackFilePublisher second;
    REQUIRE_FALSE(second.Open(path));
    const auto snapshot = MakeSnapshot(publisher.GetReceiverId());
    WriteRaw(path.wstring() + L".tmp", "unrelated temporary name");
    const auto oldTemporary = ReadRaw(path.wstring() + L".tmp");
    REQUIRE_FALSE(publisher.Publish(snapshot));
    REQUIRE(ReadRaw(path.wstring() + L".tmp") == oldTemporary);
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("Feedback publisher rejects hardlink directory and reparse aliases without changing targets", "[feedback][file]")
{
    ScratchDirectory scratch;
    const auto original = scratch.GetPath() / L"original.pbfb";
    pbapp::FeedbackFilePublisher source;
    REQUIRE(source.Open(original));
    REQUIRE(source.Publish(MakeSnapshot(source.GetReceiverId())));
    source.Close();
    const auto originalBytes = ReadRaw(original);
    const auto hardlink = scratch.GetPath() / L"hardlink.pbfb";
    REQUIRE(CreateHardLinkW(hardlink.c_str(), original.c_str(), nullptr));
    pbapp::FeedbackFilePublisher publisher;
    REQUIRE_FALSE(publisher.Open(hardlink));
    std::vector<std::byte> bytes;
    REQUIRE_FALSE(pbapp::ReadFeedbackFile(hardlink, bytes));
    REQUIRE(ReadRaw(original) == originalBytes);
    const auto directory = scratch.GetPath() / L"directory.pbfb";
    REQUIRE(std::filesystem::create_directory(directory));
    REQUIRE_FALSE(publisher.Open(directory));
    REQUIRE_FALSE(pbapp::ReadFeedbackFile(directory, bytes));
    const auto symbolic = scratch.GetPath() / L"symbolic.pbfb";
    if (CreateSymbolicLinkW(symbolic.c_str(), original.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
    {
        REQUIRE_FALSE(publisher.Open(symbolic));
        REQUIRE_FALSE(pbapp::ReadFeedbackFile(symbolic, bytes));
        REQUIRE(ReadRaw(original) == originalBytes);
    }
    else
    {
        REQUIRE(GetLastError() == ERROR_PRIVILEGE_NOT_HELD);
    }
}

TEST_CASE("Feedback malformed oversized and externally replaced primary files never become trusted", "[feedback][file]")
{
    ScratchDirectory scratch;
    const auto path = scratch.GetPath() / L"status.pbfb";
    WriteRaw(path, std::string(65537, 'X'));
    std::vector<std::byte> bytes{std::byte{0xA5}};
    REQUIRE_FALSE(pbapp::ReadFeedbackFile(path, bytes));
    REQUIRE(bytes == std::vector<std::byte>{std::byte{0xA5}});
    REQUIRE(std::filesystem::remove(path));
    pbapp::FeedbackFilePublisher publisher;
    REQUIRE(publisher.Open(path));
    auto snapshot = MakeSnapshot(publisher.GetReceiverId());
    REQUIRE(publisher.Publish(snapshot));
    WriteRaw(path, "replacement user file");
    const auto replacement = ReadRaw(path);
    snapshot.snapshotSequence = 2;
    REQUIRE_FALSE(publisher.Publish(snapshot));
    REQUIRE(ReadRaw(path) == replacement);
    REQUIRE_FALSE(pbapp::ReadFeedbackFile(path, bytes));
}

TEST_CASE("Feedback publisher pins selected parent and rejects a different writer identity", "[feedback][file]")
{
    ScratchDirectory scratch;
    const auto parent = scratch.GetPath() / L"selected";
    REQUIRE(std::filesystem::create_directory(parent));
    const auto path = parent / L"status.pbfb";
    pbapp::FeedbackFilePublisher publisher;
    REQUIRE(publisher.Open(path));
    auto snapshot = MakeSnapshot(publisher.GetReceiverId());
    snapshot.receiverId[0] ^= std::byte{1};
    REQUIRE_FALSE(publisher.Publish(snapshot));
    REQUIRE_FALSE(std::filesystem::exists(path));
    REQUIRE_FALSE(MoveFileExW(parent.c_str(), (scratch.GetPath() / L"moved").c_str(), 0));
}

TEST_CASE("Feedback commit never overwrites an unrelated file inserted after ownership validation", "[feedback][file][race]")
{
    ScratchDirectory scratch;
    const auto path = scratch.GetPath() / L"status.pbfb";
    const auto unrelated = scratch.GetPath() / L"unrelated.txt";
    WriteRaw(unrelated, "concurrent user content must survive");
    const auto original = ReadRaw(unrelated);
    pbapp::FeedbackFilePublisher publisher;
    REQUIRE(publisher.Open(path));
    auto snapshot = MakeSnapshot(publisher.GetReceiverId());
    REQUIRE(publisher.Publish(snapshot));
    const auto oldPrimary = ReadRaw(path);
    struct Interposition
    {
        std::filesystem::path path;
        std::filesystem::path unrelated;
        bool inserted = false;
    } context{path, unrelated};
    snapshot.snapshotSequence++;
    const auto status = pbapp::FeedbackFilePublisherTestAccess::Publish(publisher, snapshot, [](void* const value)
    {
        auto& action = *static_cast<Interposition*>(value);
        action.inserted = MoveFileExW(action.unrelated.c_str(), action.path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    }, &context);
    REQUIRE(context.inserted);
    REQUIRE_FALSE(status);
    REQUIRE(ReadRaw(path) == original);
    REQUIRE(ReadRaw(path.wstring() + L".previous") == oldPrimary);
    REQUIRE_FALSE(std::filesystem::exists(path.wstring() + L".tmp"));
    REQUIRE_FALSE(publisher.Publish(snapshot));
    REQUIRE(ReadRaw(path) == original);
    publisher.Close();
    REQUIRE_FALSE(publisher.Open(path));
    REQUIRE(ReadRaw(path.wstring() + L".previous") == oldPrimary);
}

TEST_CASE("Feedback retirement never adopts an occupied previous name", "[feedback][file][race]")
{
    ScratchDirectory scratch;
    const auto path = scratch.GetPath() / L"status.pbfb";
    const auto previous = path.wstring() + L".previous";
    pbapp::FeedbackFilePublisher publisher;
    REQUIRE(publisher.Open(path));
    auto snapshot = MakeSnapshot(publisher.GetReceiverId());
    REQUIRE(publisher.Publish(snapshot));
    const auto primaryBytes = ReadRaw(path);
    WriteRaw(previous, "unrelated occupied previous name");
    const auto previousBytes = ReadRaw(previous);
    snapshot.snapshotSequence++;
    REQUIRE_FALSE(publisher.Publish(snapshot));
    REQUIRE(ReadRaw(path) == primaryBytes);
    REQUIRE(ReadRaw(previous) == previousBytes);
    REQUIRE_FALSE(std::filesystem::exists(path.wstring() + L".tmp"));
    publisher.Close();
    REQUIRE_FALSE(publisher.Open(path));
    REQUIRE(ReadRaw(previous) == previousBytes);
}
