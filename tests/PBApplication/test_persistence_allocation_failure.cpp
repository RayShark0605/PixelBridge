#include "decoder_resume_store.h"

#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbstorage/output_file.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <malloc.h>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace
{

thread_local const wchar_t* allocationFailurePath = nullptr;
thread_local std::size_t allocationFailureSize = 0;
thread_local bool allocationFailureObserved = false;

[[nodiscard]] bool ShouldFailAllocation(const std::size_t byteCount) noexcept
{
    if (allocationFailurePath == nullptr || byteCount != allocationFailureSize)
    {
        return false;
    }
    const DWORD previousError = GetLastError();
    const HANDLE probe = CreateFileW(allocationFailurePath, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool locked = probe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION;
    if (probe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(probe);
    }
    SetLastError(previousError);
    if (locked)
    {
        allocationFailurePath = nullptr;
        allocationFailureObserved = true;
    }
    return locked;
}

[[nodiscard]] void* AllocateUnaligned(const std::size_t byteCount)
{
    if (ShouldFailAllocation(byteCount))
    {
        throw std::bad_alloc{};
    }
    void* const allocation = std::malloc(byteCount == 0 ? 1 : byteCount);
    if (allocation == nullptr)
    {
        throw std::bad_alloc{};
    }
    return allocation;
}

[[nodiscard]] void* AllocateAligned(const std::size_t byteCount, const std::size_t alignment)
{
    if (ShouldFailAllocation(byteCount))
    {
        throw std::bad_alloc{};
    }
    void* const allocation = _aligned_malloc(byteCount == 0 ? 1 : byteCount, alignment);
    if (allocation == nullptr)
    {
        throw std::bad_alloc{};
    }
    return allocation;
}

class AllocationFailureScope final
{
public:
    AllocationFailureScope(const std::wstring& path, const std::size_t byteCount) noexcept
    {
        allocationFailureObserved = false;
        allocationFailureSize = byteCount;
        allocationFailurePath = path.c_str();
    }
    ~AllocationFailureScope()
    {
        allocationFailurePath = nullptr;
    }
    AllocationFailureScope(const AllocationFailureScope&) = delete;
    AllocationFailureScope& operator=(const AllocationFailureScope&) = delete;
};

class ScratchDirectory final
{
public:
    explicit ScratchDirectory(const wchar_t* name) : path_(std::filesystem::path(PB_TEST_SCRATCH_ROOT) / name)
    {
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

void RequireExclusiveReopen(const std::wstring& path)
{
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    REQUIRE(CloseHandle(handle));
}

} // namespace

void* operator new(const std::size_t byteCount)
{
    return AllocateUnaligned(byteCount);
}

void* operator new[](const std::size_t byteCount)
{
    return AllocateUnaligned(byteCount);
}

void* operator new(const std::size_t byteCount, const std::align_val_t alignment)
{
    return AllocateAligned(byteCount, static_cast<std::size_t>(alignment));
}

void* operator new[](const std::size_t byteCount, const std::align_val_t alignment)
{
    return AllocateAligned(byteCount, static_cast<std::size_t>(alignment));
}

void operator delete(void* const allocation) noexcept
{
    std::free(allocation);
}

void operator delete[](void* const allocation) noexcept
{
    std::free(allocation);
}

void operator delete(void* const allocation, const std::size_t) noexcept
{
    std::free(allocation);
}

void operator delete[](void* const allocation, const std::size_t) noexcept
{
    std::free(allocation);
}

void operator delete(void* const allocation, const std::align_val_t) noexcept
{
    _aligned_free(allocation);
}

void operator delete[](void* const allocation, const std::align_val_t) noexcept
{
    _aligned_free(allocation);
}

void operator delete(void* const allocation, const std::size_t, const std::align_val_t) noexcept
{
    _aligned_free(allocation);
}

void operator delete[](void* const allocation, const std::size_t, const std::align_val_t) noexcept
{
    _aligned_free(allocation);
}

TEST_CASE("Output creation releases its part handle when allocating the final owner fails",
    "[storage][allocation][cleanup]")
{
    ScratchDirectory scratch(L"storage-owner-allocation-failure");
    pbstorage::OutputFileConfig config;
    config.outputDirectory = scratch.GetPath().wstring();
    config.sessionTag = {0xF001};
    config.maximumFileBytes = 64;
    const std::wstring partPath = (scratch.GetPath() / L"PixelBridge-000000000000f001.part").wstring();
    std::unique_ptr<pbstorage::OutputFile> output;
    pbstorage::StorageStatus status;
    {
        const AllocationFailureScope fault(partPath, sizeof(pbstorage::OutputFile));
        status = pbstorage::OutputFile::Create(config, output);
    }
    REQUIRE(allocationFailureObserved);
    REQUIRE_FALSE(status);
    REQUIRE(status.code == pbstorage::StorageErrorCode::OutOfMemory);
    REQUIRE_FALSE(output);
    RequireExclusiveReopen(partPath);
    REQUIRE(pbstorage::OutputFile::CreateOrResume(config, output));
    REQUIRE(output->GetSnapshot().resumed);
}

TEST_CASE("Resume open releases its journal handle when allocating the final owner fails",
    "[application][resume][allocation][cleanup]")
{
    ScratchDirectory scratch(L"journal-owner-allocation-failure");
    const auto policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    pbprotocol::SessionDescriptor session;
    session.protocolVersion = pbprotocol::GetProtocolVersion();
    session.digestAlgorithm = pbprotocol::DigestAlgorithm::Blake3_256;
    session.fileNameUtf8 = "empty.bin";
    const auto sessionTag = pbprotocol::DeriveSessionTag(session.sessionId);
    const auto descriptorSize = pbprotocol::GetSerializedSize(session);
    REQUIRE(descriptorSize);
    std::vector<std::byte> descriptorBytes(descriptorSize.Value());
    REQUIRE(pbprotocol::SerializeSessionDescriptor(session, policy, descriptorBytes));
    const pbprotocol::ControlRecordView record{pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor, 1, sessionTag, descriptorBytes};
    const auto controlSize = pbprotocol::GetSerializedSize(record);
    REQUIRE(controlSize);
    std::vector<std::byte> controlBytes(controlSize.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, controlBytes));
    wchar_t journalName[64]{};
    REQUIRE(swprintf_s(journalName, L"PixelBridge-%016llx.resume", static_cast<unsigned long long>(sessionTag.value)) > 0);
    const std::wstring journalPath = (scratch.GetPath() / journalName).wstring();
    pbapp::DecoderResumeLoadedState loaded;
    std::unique_ptr<pbapp::DecoderResumeStore> store;
    pbapp::DecoderResumeStoreStatus status;
    {
        const AllocationFailureScope fault(journalPath, sizeof(pbapp::DecoderResumeStore));
        status = pbapp::DecoderResumeStore::Open(scratch.GetPath(), sessionTag, controlBytes, policy, store, loaded);
    }
    REQUIRE(allocationFailureObserved);
    REQUIRE_FALSE(status);
    REQUIRE_FALSE(store);
    RequireExclusiveReopen(journalPath);
    REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), sessionTag, controlBytes, policy, store, loaded));
    REQUIRE(loaded.resumed);
}
