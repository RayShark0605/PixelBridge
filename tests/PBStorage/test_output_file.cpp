#include "pbstorage/output_file.h"

#include "pbprotocol/blake3_digest.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace
{

class ScratchDirectory
{
public:
    explicit ScratchDirectory(const wchar_t* name)
    {
        path_ = std::filesystem::path(PB_TEST_SCRATCH_ROOT) / name;
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        error.clear();
        REQUIRE(std::filesystem::create_directories(path_, error));
        REQUIRE_FALSE(error);
    }

    ~ScratchDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& GetPath() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::byte> MakeBytes(const std::size_t count)
{
    std::vector<std::byte> bytes(count);
    for (std::size_t index = 0; index < count; index++)
    {
        bytes[index] = static_cast<std::byte>((index * 37U + 11U) & 0xFFU);
    }
    return bytes;
}

} // namespace

TEST_CASE("PBStorage publishes only an exact whole-file digest", "[storage][publish]")
{
    ScratchDirectory scratch(L"publish-success");
    const std::vector<std::byte> bytes = MakeBytes(4097);
    const pbprotocol::SessionTag sessionTag{0x123456789ABCDEF0ULL};
    pbstorage::OutputFileConfig config;
    config.outputDirectory = scratch.GetPath().wstring();
    config.sessionTag = sessionTag;
    config.fileBytes = bytes.size();
    config.maximumFileBytes = 8ULL * 1024ULL * 1024ULL;
    std::unique_ptr<pbstorage::OutputFile> output;
    REQUIRE(pbstorage::OutputFile::Create(config, output));
    REQUIRE(output != nullptr);
    const auto reserved = output->GetSnapshot();
    REQUIRE(std::filesystem::exists(reserved.partPath));
    REQUIRE_FALSE(std::filesystem::exists(reserved.finalPath));
    REQUIRE(output->Write(0, bytes));
    const pbprotocol::WholeFileDigest digest{pbprotocol::ComputeBlake3Digest(bytes)};
    REQUIRE(output->Publish(digest));
    const auto published = output->GetSnapshot();
    REQUIRE(published.published);
    REQUIRE(published.writtenBytes == bytes.size());
    REQUIRE(std::filesystem::exists(published.finalPath));
    REQUIRE_FALSE(std::filesystem::exists(published.partPath));
    REQUIRE(std::filesystem::file_size(published.finalPath) == bytes.size());
}

TEST_CASE("PBStorage preserves protocol-valid zero SessionTag identity", "[storage][session-tag]")
{
    ScratchDirectory scratch(L"zero-session-tag");
    const std::vector<std::byte> bytes = MakeBytes(33);
    pbstorage::OutputFileConfig config;
    config.outputDirectory = scratch.GetPath().wstring();
    config.sessionTag = {0};
    config.fileBytes = bytes.size();
    config.maximumFileBytes = bytes.size();
    std::unique_ptr<pbstorage::OutputFile> output;
    REQUIRE(pbstorage::OutputFile::Create(config, output));
    REQUIRE(output != nullptr);
    REQUIRE(output->GetSnapshot().finalPath.ends_with(L"PixelBridge-0000000000000000.bin"));
    REQUIRE(output->Write(0, bytes));
    const pbprotocol::WholeFileDigest digest{pbprotocol::ComputeBlake3Digest(bytes)};
    REQUIRE(output->Publish(digest));
    REQUIRE(output->GetSnapshot().published);
}

TEST_CASE("PBStorage digest failure never publishes and destruction preserves its resumable part", "[storage][errors][resume]")
{
    ScratchDirectory scratch(L"digest-failure");
    const std::vector<std::byte> bytes = MakeBytes(1024);
    std::wstring partPath;
    std::wstring finalPath;
    {
        pbstorage::OutputFileConfig config;
        config.outputDirectory = scratch.GetPath().wstring();
        config.sessionTag = {7};
        config.fileBytes = bytes.size();
        config.maximumFileBytes = bytes.size();
        std::unique_ptr<pbstorage::OutputFile> output;
        REQUIRE(pbstorage::OutputFile::Create(config, output));
        REQUIRE(output->Write(0, bytes));
        const auto snapshot = output->GetSnapshot();
        partPath = snapshot.partPath;
        finalPath = snapshot.finalPath;
        pbprotocol::WholeFileDigest wrong{};
        const auto status = output->Publish(wrong);
        REQUIRE_FALSE(status);
        REQUIRE(status.code == pbstorage::StorageErrorCode::DigestMismatch);
        REQUIRE(std::filesystem::exists(partPath));
        REQUIRE_FALSE(std::filesystem::exists(finalPath));
    }
    REQUIRE(std::filesystem::exists(partPath));
    REQUIRE_FALSE(std::filesystem::exists(finalPath));
}

TEST_CASE("PBStorage fails closed on invalid ranges and existing targets", "[storage][errors]")
{
    ScratchDirectory scratch(L"range-and-conflict");
    const std::vector<std::byte> bytes = MakeBytes(16);
    pbstorage::OutputFileConfig config;
    config.outputDirectory = scratch.GetPath().wstring();
    config.sessionTag = {9};
    config.fileBytes = bytes.size();
    config.maximumFileBytes = bytes.size();
    std::unique_ptr<pbstorage::OutputFile> output;
    REQUIRE(pbstorage::OutputFile::Create(config, output));
    REQUIRE_FALSE(output->Write(1, bytes));
    const auto snapshot = output->GetSnapshot();
    output.reset();

    const HANDLE finalHandle = CreateFileW(snapshot.finalPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(finalHandle != INVALID_HANDLE_VALUE);
    REQUIRE(CloseHandle(finalHandle));
    std::unique_ptr<pbstorage::OutputFile> conflict;
    const auto status = pbstorage::OutputFile::Create(config, conflict);
    REQUIRE_FALSE(status);
    REQUIRE(status.code == pbstorage::StorageErrorCode::TargetExists);
    REQUIRE(conflict == nullptr);
}

TEST_CASE("PBStorage accepts only contiguous non-overlapping writes", "[storage][write-order]")
{
    ScratchDirectory scratch(L"sequential-writes");
    const std::vector<std::byte> bytes = MakeBytes(32);
    pbstorage::OutputFileConfig config;
    config.outputDirectory = scratch.GetPath().wstring();
    config.sessionTag = {11};
    config.fileBytes = bytes.size();
    config.maximumFileBytes = bytes.size();
    std::unique_ptr<pbstorage::OutputFile> output;
    REQUIRE(pbstorage::OutputFile::Create(config, output));
    REQUIRE_FALSE(output->Write(16, std::span(bytes).subspan(16)));
    REQUIRE(output->GetSnapshot().writtenBytes == 0);
    REQUIRE(output->Write(0, std::span(bytes).first(16)));
    REQUIRE_FALSE(output->Write(0, std::span(bytes).first(16)));
    REQUIRE(output->GetSnapshot().writtenBytes == 16);
    REQUIRE(output->Write(16, std::span(bytes).subspan(16)));
    const pbprotocol::WholeFileDigest digest{pbprotocol::ComputeBlake3Digest(bytes)};
    REQUIRE(output->Publish(digest));
    REQUIRE(output->GetSnapshot().published);
}

TEST_CASE("PBStorage never deletes a pre-existing part file that it did not create",
    "[storage][reservation][ownership]")
{
    ScratchDirectory scratch(L"part-ownership");
    pbstorage::OutputFileConfig config;
    config.outputDirectory = scratch.GetPath().wstring();
    config.sessionTag = {13};
    config.fileBytes = 8;
    config.maximumFileBytes = 8;

    std::unique_ptr<pbstorage::OutputFile> probe;
    REQUIRE(pbstorage::OutputFile::Create(config, probe));
    const std::wstring partPath = probe->GetSnapshot().partPath;
    REQUIRE(probe->Discard());
    probe.reset();
    const HANDLE foreignPart = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(foreignPart != INVALID_HANDLE_VALUE);
    REQUIRE(CloseHandle(foreignPart));

    std::unique_ptr<pbstorage::OutputFile> conflict;
    const pbstorage::StorageStatus status = pbstorage::OutputFile::Create(config, conflict);
    REQUIRE_FALSE(status);
    REQUIRE(status.code == pbstorage::StorageErrorCode::TargetExists);
    REQUIRE(conflict == nullptr);
    REQUIRE(std::filesystem::exists(partPath));
}

TEST_CASE("PBStorage accepts flushed verified Segments out of order and rejects overlap",
    "[storage][random-write][segment]")
{
    ScratchDirectory scratch(L"random-write");
    const std::vector<std::byte> bytes = MakeBytes(96);
    pbstorage::OutputFileConfig config;
    config.outputDirectory = scratch.GetPath().wstring();
    config.sessionTag = {17};
    config.fileBytes = bytes.size();
    config.maximumFileBytes = bytes.size();
    config.originalFileNameUtf8 = "restored.bin";
    std::unique_ptr<pbstorage::OutputFile> output;
    REQUIRE(pbstorage::OutputFile::CreateOrResume(config, output));
    REQUIRE(output->GetSnapshot().partPath.ends_with(L"PixelBridge-0000000000000011.part"));

    REQUIRE(output->WriteVerifiedSegment(32, std::span(bytes).subspan(32, 32)));
    REQUIRE(output->GetSnapshot().hasPendingWrite);
    REQUIRE_FALSE(output->Publish(pbprotocol::WholeFileDigest{}));
    REQUIRE(output->FlushVerifiedSegment());
    REQUIRE(output->GetSnapshot().verifiedBytes == 32);
    REQUIRE_FALSE(output->WriteVerifiedSegment(48, std::span(bytes).subspan(48, 16)));

    REQUIRE(output->WriteVerifiedSegment(64, std::span(bytes).subspan(64, 32)));
    REQUIRE(output->FlushVerifiedSegment());
    REQUIRE(output->WriteVerifiedSegment(0, std::span(bytes).first(32)));
    REQUIRE(output->FlushVerifiedSegment());
    REQUIRE(output->GetSnapshot().verifiedBytes == bytes.size());
    const pbprotocol::WholeFileDigest digest{pbprotocol::ComputeBlake3Digest(bytes)};
    REQUIRE(output->Publish(digest));
    REQUIRE(output->GetSnapshot().finalPath.ends_with(L"restored.bin"));
}

TEST_CASE("PBStorage resumes the internal part and republishes only after revalidation",
    "[storage][resume][restart]")
{
    ScratchDirectory scratch(L"resume-restart");
    const std::vector<std::byte> bytes = MakeBytes(64);
    pbstorage::OutputFileConfig config;
    config.outputDirectory = scratch.GetPath().wstring();
    config.sessionTag = {19};
    config.fileBytes = bytes.size();
    config.maximumFileBytes = bytes.size();
    config.originalFileNameUtf8 = "session.dat";

    std::wstring partPath;
    {
        std::unique_ptr<pbstorage::OutputFile> first;
        REQUIRE(pbstorage::OutputFile::CreateOrResume(config, first));
        REQUIRE_FALSE(first->GetSnapshot().resumed);
        REQUIRE(first->WriteVerifiedSegment(0, std::span(bytes).first(32)));
        REQUIRE(first->FlushVerifiedSegment());
        partPath = first->GetSnapshot().partPath;
    }
    REQUIRE(std::filesystem::exists(partPath));

    std::unique_ptr<pbstorage::OutputFile> resumed;
    REQUIRE(pbstorage::OutputFile::CreateOrResume(config, resumed));
    REQUIRE(resumed->GetSnapshot().resumed);
    std::vector<std::byte> restoredFirstSegment(32);
    REQUIRE(resumed->ReadRange(0, restoredFirstSegment));
    REQUIRE(std::ranges::equal(restoredFirstSegment, std::span(bytes).first(32)));
    // Adoption is legal only after the application has re-read this range and
    // verified the bound Segment RawDigest. PBStorage never trusts allocation
    // contents or a stale journal on its own.
    REQUIRE(resumed->AdoptVerifiedSegment(0, restoredFirstSegment.size()));
    REQUIRE(resumed->WriteVerifiedSegment(32, std::span(bytes).subspan(32)));
    REQUIRE(resumed->FlushVerifiedSegment());
    REQUIRE_FALSE(resumed->AdoptVerifiedSegment(16, 32));
    const pbprotocol::WholeFileDigest digest{pbprotocol::ComputeBlake3Digest(bytes)};
    REQUIRE(resumed->Publish(digest));
}

TEST_CASE("PBStorage publishes a verified zero-byte file and uses deterministic collision naming",
    "[storage][empty][filename][collision]")
{
    ScratchDirectory scratch(L"empty-file");
    const std::filesystem::path existingPath = scratch.GetPath() / L"empty.txt";
    const HANDLE existing = CreateFileW(existingPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(existing != INVALID_HANDLE_VALUE);
    REQUIRE(CloseHandle(existing));

    pbstorage::OutputFileConfig config;
    config.outputDirectory = scratch.GetPath().wstring();
    config.sessionTag = {0xAB};
    config.fileBytes = 0;
    config.maximumFileBytes = 1024;
    config.originalFileNameUtf8 = "empty.txt";
    std::unique_ptr<pbstorage::OutputFile> output;
    REQUIRE(pbstorage::OutputFile::CreateOrResume(config, output));
    REQUIRE(output->GetSnapshot().finalPath.ends_with(L"empty (PixelBridge-00000000000000ab).txt"));
    REQUIRE(output->Publish(pbprotocol::GetEmptyBlake3WholeFileDigest()));
    REQUIRE(std::filesystem::file_size(output->GetSnapshot().finalPath) == 0);
    REQUIRE(std::filesystem::exists(existingPath));
}

TEST_CASE("PBStorage stops when both deterministic final-name candidates already exist",
    "[storage][filename][collision][no-overwrite]")
{
    ScratchDirectory scratch(L"second-collision");
    const std::filesystem::path primaryPath = scratch.GetPath() / L"report.bin";
    const std::filesystem::path collisionPath =
        scratch.GetPath() / L"report (PixelBridge-00000000000000cd).bin";
    const auto createExisting = [](const std::filesystem::path& path)
    {
        const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(handle != INVALID_HANDLE_VALUE);
        REQUIRE(CloseHandle(handle));
    };
    createExisting(primaryPath);
    createExisting(collisionPath);

    pbstorage::OutputFileConfig config;
    config.outputDirectory = scratch.GetPath().wstring();
    config.sessionTag = {0xCD};
    config.fileBytes = 32;
    config.maximumFileBytes = 32;
    config.originalFileNameUtf8 = "report.bin";
    std::unique_ptr<pbstorage::OutputFile> output;
    const pbstorage::StorageStatus status = pbstorage::OutputFile::CreateOrResume(config, output);
    REQUIRE_FALSE(status);
    REQUIRE(status.code == pbstorage::StorageErrorCode::TargetExists);
    REQUIRE(status.stage == pbstorage::StorageStage::Reservation);
    REQUIRE_FALSE(output);
    REQUIRE(std::filesystem::exists(primaryPath));
    REQUIRE(std::filesystem::exists(collisionPath));
    REQUIRE_FALSE(std::filesystem::exists(scratch.GetPath() / L"PixelBridge-00000000000000cd.part"));
}

TEST_CASE("PBStorage publish loses a late final-name race without overwriting either file",
    "[storage][publish][collision][no-overwrite]")
{
    ScratchDirectory scratch(L"late-publish-collision");
    const std::vector<std::byte> bytes = MakeBytes(64);
    pbstorage::OutputFileConfig config;
    config.outputDirectory = scratch.GetPath().wstring();
    config.sessionTag = {0xEF};
    config.fileBytes = bytes.size();
    config.maximumFileBytes = bytes.size();
    config.originalFileNameUtf8 = "late.bin";
    std::unique_ptr<pbstorage::OutputFile> output;
    REQUIRE(pbstorage::OutputFile::CreateOrResume(config, output));
    REQUIRE(output->WriteVerifiedSegment(0, bytes));
    REQUIRE(output->FlushVerifiedSegment());
    const pbstorage::OutputFileSnapshot beforePublish = output->GetSnapshot();

    const HANDLE lateTarget = CreateFileW(beforePublish.finalPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(lateTarget != INVALID_HANDLE_VALUE);
    const std::byte marker{0x5A};
    DWORD writtenBytes = 0;
    REQUIRE(WriteFile(lateTarget, &marker, 1, &writtenBytes, nullptr));
    REQUIRE(writtenBytes == 1);
    REQUIRE(CloseHandle(lateTarget));

    const pbprotocol::WholeFileDigest digest{pbprotocol::ComputeBlake3Digest(bytes)};
    const pbstorage::StorageStatus publishStatus = output->Publish(digest);
    REQUIRE_FALSE(publishStatus);
    REQUIRE(publishStatus.code == pbstorage::StorageErrorCode::TargetExists);
    REQUIRE(publishStatus.stage == pbstorage::StorageStage::Reservation);
    REQUIRE(std::filesystem::exists(beforePublish.partPath));
    REQUIRE(std::filesystem::file_size(beforePublish.partPath) == bytes.size());
    REQUIRE(std::filesystem::exists(beforePublish.finalPath));
    REQUIRE(std::filesystem::file_size(beforePublish.finalPath) == 1);
    REQUIRE_FALSE(output->GetSnapshot().published);
}

TEST_CASE("PBStorage rejects a resumed part whose length conflicts with the descriptor",
    "[storage][resume][conflict]")
{
    ScratchDirectory scratch(L"resume-size-conflict");
    pbstorage::OutputFileConfig config;
    config.outputDirectory = scratch.GetPath().wstring();
    config.sessionTag = {23};
    config.fileBytes = 16;
    config.maximumFileBytes = 64;
    std::unique_ptr<pbstorage::OutputFile> first;
    REQUIRE(pbstorage::OutputFile::CreateOrResume(config, first));
    const std::wstring partPath = first->GetSnapshot().partPath;
    first.reset();

    const HANDLE partHandle = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(partHandle != INVALID_HANDLE_VALUE);
    LARGE_INTEGER wrongLength{};
    wrongLength.QuadPart = 8;
    REQUIRE(SetFilePointerEx(partHandle, wrongLength, nullptr, FILE_BEGIN));
    REQUIRE(SetEndOfFile(partHandle));
    REQUIRE(CloseHandle(partHandle));

    std::unique_ptr<pbstorage::OutputFile> resumed;
    const auto status = pbstorage::OutputFile::CreateOrResume(config, resumed);
    REQUIRE_FALSE(status);
    REQUIRE(status.code == pbstorage::StorageErrorCode::ResumeMismatch);
    REQUIRE(resumed == nullptr);
    REQUIRE(std::filesystem::exists(partPath));
}
