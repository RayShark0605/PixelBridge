#include "encoder_session_store.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/byte_io.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/crc32c.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

namespace pbapp
{
namespace
{

inline constexpr std::array<std::byte, 4> descriptorMagic{
    std::byte{'P'}, std::byte{'B'}, std::byte{'E'}, std::byte{'D'}};
inline constexpr std::array<std::byte, 4> runtimeMagic{
    std::byte{'P'}, std::byte{'B'}, std::byte{'E'}, std::byte{'R'}};
inline constexpr std::uint16_t storeVersion = 2;
inline constexpr std::size_t descriptorHeaderBytes = 104;
inline constexpr std::size_t runtimeHeaderBytes = 104;
inline constexpr std::uint64_t maximumDescriptorFileBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t maximumRuntimeFileBytes = 2ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t maximumIdentityStringBytes = 4096;

class StateHandle final
{
public:
    explicit StateHandle(const HANDLE handle) noexcept : handle_(handle)
    {
    }
    ~StateHandle()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            if (deleteOnClose_)
            {
                FILE_DISPOSITION_INFO disposition{TRUE};
                SetFileInformationByHandle(handle_, FileDispositionInfo, &disposition, sizeof(disposition));
            }
            CloseHandle(handle_);
        }
    }
    StateHandle(const StateHandle&) = delete;
    StateHandle& operator=(const StateHandle&) = delete;
    [[nodiscard]] bool IsValid() const noexcept
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }
    void Adopt(const HANDLE handle) noexcept
    {
        handle_ = handle;
    }
    void DeleteOnClose() noexcept
    {
        deleteOnClose_ = true;
    }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool deleteOnClose_ = false;
};

[[nodiscard]] std::unique_ptr<StateHandle> LockStateFile(const std::filesystem::path& path)
{
    auto lock = std::make_unique<StateHandle>(INVALID_HANDLE_VALUE);
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
        0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    lock->Adopt(handle);
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!lock->IsValid() || !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
    {
        return nullptr;
    }
    lock->DeleteOnClose();
    return lock;
}

// Keep every existing directory component pinned against rename/deletion,
// including the ancestors of the configured root. Never follow a junction.
[[nodiscard]] bool PinDirectories(const std::filesystem::path& directory,
    std::vector<std::unique_ptr<StateHandle>>& pins)
{
    const std::filesystem::path absolute = std::filesystem::absolute(directory).lexically_normal();
    if (absolute == absolute.root_path())
    {
        return false;
    }
    std::filesystem::path current = absolute.root_path();
    for (const auto& component : absolute.relative_path())
    {
        current /= component;
        auto pin = std::make_unique<StateHandle>(INVALID_HANDLE_VALUE);
        const HANDLE handle = CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        pin->Adopt(handle);
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!pin->IsValid() || !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            return false;
        }
        pins.push_back(std::move(pin));
    }
    return true;
}

[[nodiscard]] bool IsOrdinaryStateFile(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

class UnpublishedSessionDirectory final
{
public:
    explicit UnpublishedSessionDirectory(const std::filesystem::path& path) noexcept : path_(path)
    {
    }
    ~UnpublishedSessionDirectory()
    {
        if (published_)
        {
            return;
        }
        // Only four files created by this Create operation, never recursive
        // removal and never a source path. Pins remain held during cleanup.
        if (!pins_.empty())
        {
            try
            {
                for (const wchar_t* name : {L"descriptors.bin", L"runtime.state", L"descriptors.bin.tmp", L"runtime.state.tmp"})
                {
                    DeleteFileW((path_ / name).c_str());
                }
            }
            catch (...)
            {
                // Keep an unindexed residue rather than terminate while
                // unwinding an allocation failure or deleting unknown files.
            }
        }
        pins_.clear();
        RemoveDirectoryW(path_.c_str());
    }
    [[nodiscard]] bool Pin()
    {
        return PinDirectories(path_, pins_);
    }
    void Publish() noexcept
    {
        published_ = true;
    }
private:
    const std::filesystem::path& path_;
    std::vector<std::unique_ptr<StateHandle>> pins_;
    bool published_ = false;
};

struct ParsedDescriptorState
{
    EncoderSourceIdentity sourceIdentity;
    std::string sourcePathUtf8;
    std::string buildIdentity;
    std::string compressionIdentity;
    std::string outerFecIdentity;
    pbprotocol::SessionId sessionId{};
    std::uint64_t segmentCount = 0;
    std::vector<std::byte> descriptorBundle;
};

struct ParsedRuntimeState
{
    std::uint64_t generation = 0;
    pbprotocol::SessionId sessionId{};
    std::array<std::byte, pbprotocol::kDigestBytes> descriptorDigest{};
    std::uint64_t carouselPass = 0;
    std::uint64_t segmentOrdinal = 0;
    std::uint64_t frameSequenceLeaseEnd = 0;
    std::vector<std::uint32_t> repairIdLeaseEnds;
};

[[nodiscard]] std::string NativeFailure(const char* operation, const DWORD error)
{
    return std::string(operation) + " failed; win32=" + std::to_string(error);
}

[[nodiscard]] std::uint64_t FileTimeValue(const FILETIME& value) noexcept
{
    return static_cast<std::uint64_t>(value.dwLowDateTime) |
        (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U);
}

[[nodiscard]] std::string SessionIdHex(const pbprotocol::SessionId& sessionId)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string value(pbprotocol::kSessionIdBytes * 2, '0');
    for (std::size_t index = 0; index < sessionId.bytes.size(); index++)
    {
        const std::uint8_t byte = std::to_integer<std::uint8_t>(sessionId.bytes[index]);
        value[index * 2] = digits[byte >> 4U];
        value[index * 2 + 1] = digits[byte & 0x0FU];
    }
    return value;
}

[[nodiscard]] bool ParseHexDigit(const char character, std::uint8_t& value) noexcept
{
    if (character >= '0' && character <= '9')
    {
        value = static_cast<std::uint8_t>(character - '0');
        return true;
    }
    if (character >= 'a' && character <= 'f')
    {
        value = static_cast<std::uint8_t>(character - 'a' + 10);
        return true;
    }
    return false;
}

[[nodiscard]] bool ParseSessionIdHex(const std::string_view text, pbprotocol::SessionId& output) noexcept
{
    if (text.size() != pbprotocol::kSessionIdBytes * 2)
    {
        return false;
    }
    pbprotocol::SessionId parsed;
    for (std::size_t index = 0; index < parsed.bytes.size(); index++)
    {
        std::uint8_t high = 0;
        std::uint8_t low = 0;
        if (!ParseHexDigit(text[index * 2], high) || !ParseHexDigit(text[index * 2 + 1], low))
        {
            return false;
        }
        parsed.bytes[index] = static_cast<std::byte>((high << 4U) | low);
    }
    output = parsed;
    return true;
}

[[nodiscard]] std::wstring SourceIdentityKey(const EncoderSourceIdentity& identity)
{
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring value(16 + 1 + identity.fileId.size() * 2, L'0');
    for (std::size_t index = 0; index < 16; index++)
    {
        const std::uint32_t shift = static_cast<std::uint32_t>((15 - index) * 4);
        value[index] = digits[(identity.volumeSerialNumber >> shift) & 0x0FULL];
    }
    value[16] = L'-';
    for (std::size_t index = 0; index < identity.fileId.size(); index++)
    {
        const std::uint8_t byte = std::to_integer<std::uint8_t>(identity.fileId[index]);
        value[17 + index * 2] = digits[byte >> 4U];
        value[17 + index * 2 + 1] = digits[byte & 0x0FU];
    }
    return value;
}

[[nodiscard]] EncoderSessionStoreStatus ReadFileBounded(const std::filesystem::path& path,
    const std::uint64_t maximumBytes, std::vector<std::byte>& output) noexcept
{
    output.clear();
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return EncoderSessionStoreStatus::Failure(NativeFailure("session state open", GetLastError()));
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > maximumBytes)
    {
        const DWORD error = GetLastError();
        CloseHandle(file);
        return EncoderSessionStoreStatus::Failure(error == ERROR_SUCCESS ?
            "session state exceeds its bounded size" : NativeFailure("session state size", error));
    }
    try
    {
        output.resize(static_cast<std::size_t>(size.QuadPart));
    }
    catch (const std::bad_alloc&)
    {
        CloseHandle(file);
        return EncoderSessionStoreStatus::Failure("session state allocation failed");
    }
    std::size_t offset = 0;
    while (offset < output.size())
    {
        const DWORD requested = static_cast<DWORD>((std::min)(output.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD readBytes = 0;
        if (ReadFile(file, output.data() + offset, requested, &readBytes, nullptr) == FALSE || readBytes == 0)
        {
            const DWORD error = GetLastError();
            CloseHandle(file);
            return EncoderSessionStoreStatus::Failure(NativeFailure("session state read", error));
        }
        offset += readBytes;
    }
    std::byte extra{};
    DWORD extraBytes = 0;
    const bool exact = ReadFile(file, &extra, 1, &extraBytes, nullptr) != FALSE && extraBytes == 0;
    const bool closed = CloseHandle(file) != FALSE;
    return exact && closed ? EncoderSessionStoreStatus{} :
        EncoderSessionStoreStatus::Failure("session state changed during read or close failed");
}

[[nodiscard]] EncoderSessionStoreStatus AtomicWriteFile(const std::filesystem::path& path,
    const std::span<const std::byte> bytes) noexcept
{
    const std::filesystem::path temporaryPath = path.wstring() + L".tmp";
    const DWORD temporaryAttributes = GetFileAttributesW(temporaryPath.c_str());
    if (temporaryAttributes != INVALID_FILE_ATTRIBUTES &&
        ((temporaryAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
            !DeleteFileW(temporaryPath.c_str())))
    {
        return EncoderSessionStoreStatus::Failure("Encoder temporary state is unsafe or busy");
    }
    const HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return EncoderSessionStoreStatus::Failure(NativeFailure("session state temporary create", GetLastError()));
    }
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const DWORD requested = static_cast<DWORD>((std::min)(bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD writtenBytes = 0;
        if (WriteFile(file, bytes.data() + offset, requested, &writtenBytes, nullptr) == FALSE ||
            writtenBytes != requested)
        {
            const DWORD error = GetLastError();
            CloseHandle(file);
            DeleteFileW(temporaryPath.c_str());
            return EncoderSessionStoreStatus::Failure(NativeFailure("session state write", error));
        }
        offset += writtenBytes;
    }
    if (FlushFileBuffers(file) == FALSE)
    {
        const DWORD error = GetLastError();
        CloseHandle(file);
        DeleteFileW(temporaryPath.c_str());
        return EncoderSessionStoreStatus::Failure(NativeFailure("session state flush", error));
    }
    if (CloseHandle(file) == FALSE)
    {
        const DWORD error = GetLastError();
        DeleteFileW(temporaryPath.c_str());
        return EncoderSessionStoreStatus::Failure(NativeFailure("session state close", error));
    }
    if (MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        const DWORD error = GetLastError();
        DeleteFileW(temporaryPath.c_str());
        return EncoderSessionStoreStatus::Failure(NativeFailure("session state atomic replace", error));
    }
    return {};
}

[[nodiscard]] bool WriteStatus(pbprotocol::ByteWriter& writer, const pbprotocol::ProtocolStatus& status) noexcept
{
    static_cast<void>(writer);
    return static_cast<bool>(status);
}

[[nodiscard]] EncoderSessionStoreStatus SerializeDescriptorState(const EncoderSessionStoreCreateConfig& config,
    std::vector<std::byte>& output) noexcept
{
    const auto variableBytes1 = pbprotocol::CheckedAddUint64(config.sourcePathUtf8.size(), config.buildIdentity.size());
    const auto variableBytes2 = variableBytes1 ? pbprotocol::CheckedAddUint64(variableBytes1.Value(),
        config.compressionIdentity.size()) : variableBytes1;
    const auto variableBytes3 = variableBytes2 ? pbprotocol::CheckedAddUint64(variableBytes2.Value(),
        config.outerFecIdentity.size()) : variableBytes2;
    const auto variableBytes4 = variableBytes3 ? pbprotocol::CheckedAddUint64(variableBytes3.Value(),
        config.descriptorBundle.size()) : variableBytes3;
    const auto totalWithoutCrc = variableBytes4 ? pbprotocol::CheckedAddUint64(descriptorHeaderBytes,
        variableBytes4.Value()) : variableBytes4;
    const auto totalBytes = totalWithoutCrc ? pbprotocol::CheckedAddUint64(totalWithoutCrc.Value(), 4) : totalWithoutCrc;
    if (!totalBytes || totalBytes.Value() > maximumDescriptorFileBytes ||
        config.sourcePathUtf8.size() > maximumIdentityStringBytes || config.buildIdentity.size() > maximumIdentityStringBytes ||
        config.compressionIdentity.size() > maximumIdentityStringBytes || config.outerFecIdentity.size() > maximumIdentityStringBytes ||
        config.sourcePathUtf8.size() > UINT32_MAX || config.buildIdentity.size() > UINT32_MAX ||
        config.compressionIdentity.size() > UINT32_MAX || config.outerFecIdentity.size() > UINT32_MAX)
    {
        return EncoderSessionStoreStatus::Failure("Encoder descriptor state exceeds its bounded format");
    }
    try
    {
        output.assign(static_cast<std::size_t>(totalBytes.Value()), std::byte{0});
    }
    catch (const std::bad_alloc&)
    {
        return EncoderSessionStoreStatus::Failure("Encoder descriptor state allocation failed");
    }
    pbprotocol::ByteWriter writer(output);
    const auto sourcePathBytes = std::as_bytes(std::span(config.sourcePathUtf8));
    const auto buildBytes = std::as_bytes(std::span(config.buildIdentity));
    const auto compressionBytes = std::as_bytes(std::span(config.compressionIdentity));
    const auto outerFecBytes = std::as_bytes(std::span(config.outerFecIdentity));
    bool valid = WriteStatus(writer, writer.WriteFixedBytes(descriptorMagic));
    valid = valid && WriteStatus(writer, writer.WriteUint16(storeVersion));
    valid = valid && WriteStatus(writer, writer.WriteUint16(0));
    valid = valid && WriteStatus(writer, writer.WriteUint64(totalBytes.Value()));
    valid = valid && WriteStatus(writer, writer.WriteFixedBytes(config.sessionId.bytes));
    valid = valid && WriteStatus(writer, writer.WriteUint64(config.sourceIdentity.volumeSerialNumber));
    valid = valid && WriteStatus(writer, writer.WriteFixedBytes(config.sourceIdentity.fileId));
    valid = valid && WriteStatus(writer, writer.WriteUint64(config.sourceIdentity.fileBytes));
    valid = valid && WriteStatus(writer, writer.WriteUint64(config.sourceIdentity.lastWriteTime));
    valid = valid && WriteStatus(writer, writer.WriteUint64(config.segmentCount));
    valid = valid && WriteStatus(writer, writer.WriteUint32(static_cast<std::uint32_t>(sourcePathBytes.size())));
    valid = valid && WriteStatus(writer, writer.WriteUint32(static_cast<std::uint32_t>(buildBytes.size())));
    valid = valid && WriteStatus(writer, writer.WriteUint32(static_cast<std::uint32_t>(compressionBytes.size())));
    valid = valid && WriteStatus(writer, writer.WriteUint32(static_cast<std::uint32_t>(outerFecBytes.size())));
    valid = valid && WriteStatus(writer, writer.WriteUint64(config.descriptorBundle.size()));
    valid = valid && writer.Position() == descriptorHeaderBytes;
    valid = valid && WriteStatus(writer, writer.WriteBytes(sourcePathBytes));
    valid = valid && WriteStatus(writer, writer.WriteBytes(buildBytes));
    valid = valid && WriteStatus(writer, writer.WriteBytes(compressionBytes));
    valid = valid && WriteStatus(writer, writer.WriteBytes(outerFecBytes));
    valid = valid && WriteStatus(writer, writer.WriteBytes(config.descriptorBundle));
    if (!valid || writer.Remaining() != 4)
    {
        output.clear();
        return EncoderSessionStoreStatus::Failure("Encoder descriptor state serialization invariant failed");
    }
    const std::uint32_t crc = pbprotocol::ComputeCrc32c(std::span(output).first(output.size() - 4));
    valid = WriteStatus(writer, writer.WriteUint32(crc));
    return valid && writer.Remaining() == 0 ? EncoderSessionStoreStatus{} :
        EncoderSessionStoreStatus::Failure("Encoder descriptor state CRC serialization failed");
}

[[nodiscard]] EncoderSessionStoreStatus ParseDescriptorState(const std::span<const std::byte> bytes,
    ParsedDescriptorState& output) noexcept
{
    if (bytes.size() < descriptorHeaderBytes + 4 || bytes.size() > maximumDescriptorFileBytes)
    {
        return EncoderSessionStoreStatus::Failure("Encoder descriptor state size is invalid");
    }
    pbprotocol::ByteReader reader(bytes);
    const auto magic = reader.ReadFixedBytes<4>();
    const auto version = reader.ReadUint16();
    const auto reserved = reader.ReadUint16();
    const auto totalBytes = reader.ReadUint64();
    const auto sessionId = reader.ReadFixedBytes<pbprotocol::kSessionIdBytes>();
    const auto volumeSerial = reader.ReadUint64();
    const auto fileId = reader.ReadFixedBytes<16>();
    const auto fileBytes = reader.ReadUint64();
    const auto lastWriteTime = reader.ReadUint64();
    const auto segmentCount = reader.ReadUint64();
    const auto sourcePathBytes = reader.ReadUint32();
    const auto buildBytes = reader.ReadUint32();
    const auto compressionBytes = reader.ReadUint32();
    const auto outerFecBytes = reader.ReadUint32();
    const auto descriptorBytes = reader.ReadUint64();
    if (!magic || !version || !reserved || !totalBytes || !sessionId || !volumeSerial ||
        !fileId || !fileBytes || !lastWriteTime || !segmentCount || !sourcePathBytes || !buildBytes ||
        !compressionBytes || !outerFecBytes || !descriptorBytes || magic.Value() != descriptorMagic ||
        version.Value() != storeVersion || reserved.Value() != 0 ||
        totalBytes.Value() != bytes.size() || reader.Position() != descriptorHeaderBytes ||
        sourcePathBytes.Value() > maximumIdentityStringBytes || buildBytes.Value() > maximumIdentityStringBytes ||
        compressionBytes.Value() > maximumIdentityStringBytes || outerFecBytes.Value() > maximumIdentityStringBytes)
    {
        return EncoderSessionStoreStatus::Failure("Encoder descriptor state header is invalid");
    }
    const auto variableBytes1 = pbprotocol::CheckedAddUint64(sourcePathBytes.Value(), buildBytes.Value());
    const auto variableBytes2 = variableBytes1 ? pbprotocol::CheckedAddUint64(variableBytes1.Value(),
        compressionBytes.Value()) : variableBytes1;
    const auto variableBytes3 = variableBytes2 ? pbprotocol::CheckedAddUint64(variableBytes2.Value(),
        outerFecBytes.Value()) : variableBytes2;
    const auto variableBytes4 = variableBytes3 ? pbprotocol::CheckedAddUint64(variableBytes3.Value(),
        descriptorBytes.Value()) : variableBytes3;
    if (!variableBytes4 || variableBytes4.Value() != reader.Remaining() - 4)
    {
        return EncoderSessionStoreStatus::Failure("Encoder descriptor state variable lengths are invalid");
    }
    const auto sourcePath = reader.ReadBytes(sourcePathBytes.Value());
    const auto build = reader.ReadBytes(buildBytes.Value());
    const auto compression = reader.ReadBytes(compressionBytes.Value());
    const auto outerFec = reader.ReadBytes(outerFecBytes.Value());
    const auto bundle = reader.ReadBytes(static_cast<std::size_t>(descriptorBytes.Value()));
    const auto storedCrc = reader.ReadUint32();
    if (!sourcePath || !build || !compression || !outerFec || !bundle || !storedCrc ||
        !reader.RequireFullyConsumed() || storedCrc.Value() != pbprotocol::ComputeCrc32c(bytes.first(bytes.size() - 4)))
    {
        return EncoderSessionStoreStatus::Failure("Encoder descriptor state body or CRC is invalid");
    }
    const auto ToString = [](const std::span<const std::byte> value)
    {
        return std::string(reinterpret_cast<const char*>(value.data()), value.size());
    };
    ParsedDescriptorState parsed;
    parsed.sourceIdentity = {volumeSerial.Value(), fileId.Value(), fileBytes.Value(), lastWriteTime.Value()};
    parsed.sourcePathUtf8 = ToString(sourcePath.Value());
    parsed.buildIdentity = ToString(build.Value());
    parsed.compressionIdentity = ToString(compression.Value());
    parsed.outerFecIdentity = ToString(outerFec.Value());
    parsed.sessionId.bytes = sessionId.Value();
    parsed.segmentCount = segmentCount.Value();
    try
    {
        parsed.descriptorBundle.assign(bundle.Value().begin(), bundle.Value().end());
    }
    catch (const std::bad_alloc&)
    {
        return EncoderSessionStoreStatus::Failure("Encoder descriptor bundle allocation failed");
    }
    if (!pbprotocol::ValidateUtf8(parsed.sourcePathUtf8) || !pbprotocol::ValidateUtf8(parsed.buildIdentity) ||
        !pbprotocol::ValidateUtf8(parsed.compressionIdentity) || !pbprotocol::ValidateUtf8(parsed.outerFecIdentity))
    {
        return EncoderSessionStoreStatus::Failure("Encoder descriptor state contains invalid UTF-8 identity text");
    }
    output = std::move(parsed);
    return {};
}

[[nodiscard]] EncoderSessionStoreStatus SerializeRuntimeState(const ParsedRuntimeState& state,
    std::vector<std::byte>& output) noexcept
{
    const auto repairBytes = pbprotocol::CheckedMultiplyUint64(state.repairIdLeaseEnds.size(), 4);
    const auto totalWithoutCrc = repairBytes ? pbprotocol::CheckedAddUint64(runtimeHeaderBytes,
        repairBytes.Value()) : repairBytes;
    const auto totalBytes = totalWithoutCrc ? pbprotocol::CheckedAddUint64(totalWithoutCrc.Value(), 4) : totalWithoutCrc;
    if (!totalBytes || totalBytes.Value() > maximumRuntimeFileBytes)
    {
        return EncoderSessionStoreStatus::Failure("Encoder runtime state exceeds its bounded format");
    }
    try
    {
        output.assign(static_cast<std::size_t>(totalBytes.Value()), std::byte{0});
    }
    catch (const std::bad_alloc&)
    {
        return EncoderSessionStoreStatus::Failure("Encoder runtime state allocation failed");
    }
    pbprotocol::ByteWriter writer(output);
    bool valid = WriteStatus(writer, writer.WriteFixedBytes(runtimeMagic));
    valid = valid && WriteStatus(writer, writer.WriteUint16(storeVersion));
    valid = valid && WriteStatus(writer, writer.WriteUint16(0));
    valid = valid && WriteStatus(writer, writer.WriteUint64(totalBytes.Value()));
    valid = valid && WriteStatus(writer, writer.WriteUint64(state.generation));
    valid = valid && WriteStatus(writer, writer.WriteFixedBytes(state.sessionId.bytes));
    valid = valid && WriteStatus(writer, writer.WriteFixedBytes(state.descriptorDigest));
    valid = valid && WriteStatus(writer, writer.WriteUint64(state.carouselPass));
    valid = valid && WriteStatus(writer, writer.WriteUint64(state.segmentOrdinal));
    valid = valid && WriteStatus(writer, writer.WriteUint64(state.frameSequenceLeaseEnd));
    valid = valid && WriteStatus(writer, writer.WriteUint64(state.repairIdLeaseEnds.size()));
    valid = valid && writer.Position() == runtimeHeaderBytes;
    for (const std::uint32_t repairIdLeaseEnd : state.repairIdLeaseEnds)
    {
        valid = valid && WriteStatus(writer, writer.WriteUint32(repairIdLeaseEnd));
    }
    if (!valid || writer.Remaining() != 4)
    {
        output.clear();
        return EncoderSessionStoreStatus::Failure("Encoder runtime state serialization invariant failed");
    }
    valid = WriteStatus(writer, writer.WriteUint32(
        pbprotocol::ComputeCrc32c(std::span(output).first(output.size() - 4))));
    return valid && writer.Remaining() == 0 ? EncoderSessionStoreStatus{} :
        EncoderSessionStoreStatus::Failure("Encoder runtime state CRC serialization failed");
}

[[nodiscard]] EncoderSessionStoreStatus ParseRuntimeState(const std::span<const std::byte> bytes,
    const std::uint64_t expectedSegmentCount, ParsedRuntimeState& output) noexcept
{
    if (bytes.size() < runtimeHeaderBytes + 4 || bytes.size() > maximumRuntimeFileBytes)
    {
        return EncoderSessionStoreStatus::Failure("Encoder runtime state size is invalid");
    }
    pbprotocol::ByteReader reader(bytes);
    const auto magic = reader.ReadFixedBytes<4>();
    const auto version = reader.ReadUint16();
    const auto reserved = reader.ReadUint16();
    const auto totalBytes = reader.ReadUint64();
    const auto generation = reader.ReadUint64();
    const auto sessionId = reader.ReadFixedBytes<pbprotocol::kSessionIdBytes>();
    const auto descriptorDigest = reader.ReadFixedBytes<pbprotocol::kDigestBytes>();
    const auto carouselPass = reader.ReadUint64();
    const auto segmentOrdinal = reader.ReadUint64();
    const auto frameLeaseEnd = reader.ReadUint64();
    const auto repairCount = reader.ReadUint64();
    if (!magic || !version || !reserved || !totalBytes || !generation || !sessionId || !descriptorDigest ||
        !carouselPass || !segmentOrdinal || !frameLeaseEnd || !repairCount || magic.Value() != runtimeMagic ||
        version.Value() != storeVersion || reserved.Value() != 0 || totalBytes.Value() != bytes.size() ||
        generation.Value() == 0 || repairCount.Value() != expectedSegmentCount ||
        (expectedSegmentCount == 0 ? segmentOrdinal.Value() != 0 : segmentOrdinal.Value() >= expectedSegmentCount) ||
        reader.Position() != runtimeHeaderBytes)
    {
        return EncoderSessionStoreStatus::Failure("Encoder runtime state header is invalid");
    }
    const auto repairBytes = pbprotocol::CheckedMultiplyUint64(repairCount.Value(), 4);
    if (!repairBytes || repairBytes.Value() != reader.Remaining() - 4)
    {
        return EncoderSessionStoreStatus::Failure("Encoder runtime state repair lease length is invalid");
    }
    ParsedRuntimeState parsed;
    parsed.generation = generation.Value();
    parsed.sessionId.bytes = sessionId.Value();
    parsed.descriptorDigest = descriptorDigest.Value();
    parsed.carouselPass = carouselPass.Value();
    parsed.segmentOrdinal = segmentOrdinal.Value();
    parsed.frameSequenceLeaseEnd = frameLeaseEnd.Value();
    try
    {
        parsed.repairIdLeaseEnds.reserve(static_cast<std::size_t>(repairCount.Value()));
    }
    catch (const std::bad_alloc&)
    {
        return EncoderSessionStoreStatus::Failure("Encoder runtime repair lease allocation failed");
    }
    for (std::uint64_t index = 0; index < repairCount.Value(); index++)
    {
        const auto repairLeaseEnd = reader.ReadUint32();
        if (!repairLeaseEnd)
        {
            return EncoderSessionStoreStatus::Failure("Encoder runtime repair lease is truncated");
        }
        parsed.repairIdLeaseEnds.push_back(repairLeaseEnd.Value());
    }
    const auto storedCrc = reader.ReadUint32();
    if (!storedCrc || !reader.RequireFullyConsumed() ||
        storedCrc.Value() != pbprotocol::ComputeCrc32c(bytes.first(bytes.size() - 4)))
    {
        return EncoderSessionStoreStatus::Failure("Encoder runtime state CRC is invalid");
    }
    output = std::move(parsed);
    return {};
}

[[nodiscard]] EncoderSessionStoreStatus RoundLeaseEnd(const std::uint64_t currentEnd,
    const std::uint64_t requiredExclusive, std::uint64_t& output) noexcept
{
    if (requiredExclusive <= currentEnd)
    {
        output = currentEnd;
        return {};
    }
    const auto roundedInput = pbprotocol::CheckedAddUint64(requiredExclusive, encoderDurableIdLeaseSize - 1);
    if (!roundedInput)
    {
        return EncoderSessionStoreStatus::Failure("durable ID lease endpoint overflow");
    }
    output = roundedInput.Value() / encoderDurableIdLeaseSize * encoderDurableIdLeaseSize;
    if (output < requiredExclusive)
    {
        return EncoderSessionStoreStatus::Failure("durable ID lease rounding overflow");
    }
    return {};
}

} // namespace

struct EncoderSessionStore::Implementation
{
    std::unique_ptr<StateHandle> ownerLock;
    std::filesystem::path rootDirectory;
    std::filesystem::path sessionDirectory;
    std::filesystem::path runtimePath;
    ParsedDescriptorState descriptor;
    ParsedRuntimeState runtime;
    bool resumed = false;
};

EncoderSessionStore::EncoderSessionStore(std::unique_ptr<Implementation> implementation) noexcept :
    implementation_(std::move(implementation))
{
}

EncoderSessionStore::~EncoderSessionStore() = default;

EncoderSessionStoreStatus ResolveEncoderSessionRoot(const std::filesystem::path& configuredRoot,
    std::filesystem::path& output) noexcept
{
    try
    {
        if (!configuredRoot.empty())
        {
            output = configuredRoot;
            return {};
        }
        DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (required <= 1)
        {
            return EncoderSessionStoreStatus::Failure(NativeFailure("LOCALAPPDATA query", GetLastError()));
        }
        std::wstring localAppData(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), required);
        if (written == 0 || written >= required)
        {
            return EncoderSessionStoreStatus::Failure(NativeFailure("LOCALAPPDATA read", GetLastError()));
        }
        localAppData.resize(written);
        output = std::filesystem::path(localAppData) / L"PixelBridge" / L"EncoderSessions";
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return EncoderSessionStoreStatus::Failure("Encoder session root allocation failed");
    }
    catch (...)
    {
        return EncoderSessionStoreStatus::Failure("Encoder session root resolution failed");
    }
}

EncoderSessionStoreStatus EncoderSessionStore::FindMatching(const std::filesystem::path& rootDirectory,
    const EncoderSourceIdentity& sourceIdentity, const std::string_view buildIdentity,
    const std::string_view compressionIdentity, const std::string_view outerFecIdentity,
    std::unique_ptr<EncoderSessionStore>& output, bool& found) noexcept
{
    output.reset();
    found = false;
    try
    {
        const std::filesystem::path indexPath = rootDirectory / L"SourceIndex" /
            (SourceIdentityKey(sourceIdentity) + L".txt");
        if (!std::filesystem::exists(indexPath))
        {
            return {};
        }
        const auto indexLock = LockStateFile(rootDirectory / L"SourceIndex" / L"index.lock");
        if (!indexLock)
        {
            return EncoderSessionStoreStatus::Failure("Encoder source index is busy");
        }
        std::vector<std::byte> indexBytes;
        EncoderSessionStoreStatus status = ReadFileBounded(indexPath, 64, indexBytes);
        if (!status)
        {
            return status;
        }
        if (indexBytes.size() != pbprotocol::kSessionIdBytes * 2 + 1 || indexBytes.back() != std::byte{'\n'})
        {
            return EncoderSessionStoreStatus::Failure("Encoder source index is malformed");
        }
        const std::string sessionText(reinterpret_cast<const char*>(indexBytes.data()), indexBytes.size() - 1);
        pbprotocol::SessionId indexedSessionId;
        if (!ParseSessionIdHex(sessionText, indexedSessionId))
        {
            return EncoderSessionStoreStatus::Failure("Encoder source index SessionId is malformed");
        }
        const std::filesystem::path sessionDirectory = rootDirectory / std::filesystem::path(sessionText);
        auto ownerLock = LockStateFile(sessionDirectory / L"owner.lock");
        if (!ownerLock)
        {
            return EncoderSessionStoreStatus::Failure("Encoder Session has another live owner");
        }
        if (std::filesystem::exists(sessionDirectory / L"ended.descriptors"))
        {
            std::vector<std::byte> endedBytes;
            ParsedDescriptorState endedDescriptor;
            status = ReadFileBounded(sessionDirectory / L"ended.descriptors", maximumDescriptorFileBytes, endedBytes);
            if (status)
            {
                status = ParseDescriptorState(endedBytes, endedDescriptor);
            }
            if (!status || endedDescriptor.sessionId != indexedSessionId ||
                endedDescriptor.sourceIdentity.volumeSerialNumber != sourceIdentity.volumeSerialNumber ||
                endedDescriptor.sourceIdentity.fileId != sourceIdentity.fileId)
            {
                return EncoderSessionStoreStatus::Failure("Encoder end marker identity/checksum is invalid");
            }
            return {};
        }
        std::vector<std::byte> descriptorBytes;
        status = ReadFileBounded(sessionDirectory / L"descriptors.bin", maximumDescriptorFileBytes, descriptorBytes);
        if (!status)
        {
            return status;
        }
        ParsedDescriptorState descriptor;
        status = ParseDescriptorState(descriptorBytes, descriptor);
        if (!status)
        {
            return status;
        }
        if (descriptor.sessionId != indexedSessionId ||
            descriptor.sourceIdentity.volumeSerialNumber != sourceIdentity.volumeSerialNumber ||
            descriptor.sourceIdentity.fileId != sourceIdentity.fileId)
        {
            return EncoderSessionStoreStatus::Failure("Encoder source index conflicts with persisted source identity");
        }
        if (descriptor.sourceIdentity.fileBytes != sourceIdentity.fileBytes ||
            descriptor.sourceIdentity.lastWriteTime != sourceIdentity.lastWriteTime)
        {
            return {};
        }
        if (descriptor.buildIdentity != buildIdentity || descriptor.compressionIdentity != compressionIdentity ||
            descriptor.outerFecIdentity != outerFecIdentity)
        {
            return {};
        }
        std::vector<std::byte> runtimeBytes;
        status = ReadFileBounded(sessionDirectory / L"runtime.state", maximumRuntimeFileBytes, runtimeBytes);
        if (!status)
        {
            return status;
        }
        ParsedRuntimeState runtime;
        status = ParseRuntimeState(runtimeBytes, descriptor.segmentCount, runtime);
        if (!status)
        {
            return status;
        }
        if (runtime.sessionId != descriptor.sessionId ||
            runtime.descriptorDigest != pbprotocol::ComputeBlake3Digest(descriptor.descriptorBundle))
        {
            return EncoderSessionStoreStatus::Failure("Encoder runtime state conflicts with immutable descriptors");
        }
        auto implementation = std::make_unique<Implementation>();
        implementation->ownerLock = std::move(ownerLock);
        implementation->rootDirectory = rootDirectory;
        implementation->sessionDirectory = sessionDirectory;
        implementation->runtimePath = sessionDirectory / L"runtime.state";
        implementation->descriptor = std::move(descriptor);
        implementation->runtime = std::move(runtime);
        implementation->resumed = true;
        output = std::unique_ptr<EncoderSessionStore>(new EncoderSessionStore(std::move(implementation)));
        found = true;
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return EncoderSessionStoreStatus::Failure("Encoder persisted session allocation failed");
    }
    catch (const std::exception& exception)
    {
        return EncoderSessionStoreStatus::Failure(exception.what());
    }
    catch (...)
    {
        return EncoderSessionStoreStatus::Failure("Encoder persisted session load failed");
    }
}

EncoderSessionStoreStatus EncoderSessionStore::Create(const EncoderSessionStoreCreateConfig& config,
    std::unique_ptr<EncoderSessionStore>& output) noexcept
{
    output.reset();
    try
    {
        if (config.rootDirectory.empty() || config.segmentCount > 65536 ||
            config.descriptorBundle.empty() || config.sourceIdentity.fileBytes > 500ULL * 1024ULL * 1024ULL * 1024ULL)
        {
            return EncoderSessionStoreStatus::Failure("Encoder session create configuration is invalid");
        }
        std::error_code directoryError;
        std::filesystem::create_directories(config.rootDirectory / L"SourceIndex", directoryError);
        if (directoryError)
        {
            return EncoderSessionStoreStatus::Failure("Encoder SourceIndex directory creation failed");
        }
        std::vector<std::unique_ptr<StateHandle>> rootPins;
        if (!PinDirectories(config.rootDirectory / L"SourceIndex", rootPins))
        {
            return EncoderSessionStoreStatus::Failure("Encoder Session root is unsafe or a reparse point");
        }
        const auto indexLock = LockStateFile(config.rootDirectory / L"SourceIndex" / L"index.lock");
        if (!indexLock)
        {
            return EncoderSessionStoreStatus::Failure("Encoder source index is busy");
        }
        std::vector<std::byte> descriptorBytes;
        EncoderSessionStoreStatus status = SerializeDescriptorState(config, descriptorBytes);
        if (!status)
        {
            return status;
        }
        const std::string sessionText = SessionIdHex(config.sessionId);
        const std::filesystem::path sessionDirectory = config.rootDirectory / std::filesystem::path(sessionText);
        if (std::filesystem::exists(sessionDirectory, directoryError) || directoryError)
        {
            return EncoderSessionStoreStatus::Failure(directoryError ?
                "Encoder session directory inspection failed" : "Encoder SessionId directory already exists");
        }
        if (!std::filesystem::create_directory(sessionDirectory, directoryError) || directoryError)
        {
            return EncoderSessionStoreStatus::Failure("Encoder session directory creation failed");
        }
        UnpublishedSessionDirectory cleanup(sessionDirectory);
        if (!cleanup.Pin())
        {
            return EncoderSessionStoreStatus::Failure("Encoder new Session directory could not be pinned");
        }
        auto ownerLock = LockStateFile(sessionDirectory / L"owner.lock");
        if (!ownerLock)
        {
            return EncoderSessionStoreStatus::Failure("Encoder new Session ownership failed");
        }
        status = AtomicWriteFile(sessionDirectory / L"descriptors.bin", descriptorBytes);
        if (!status)
        {
            return status;
        }
        ParsedRuntimeState runtime;
        runtime.generation = 1;
        runtime.sessionId = config.sessionId;
        runtime.descriptorDigest = pbprotocol::ComputeBlake3Digest(config.descriptorBundle);
        runtime.repairIdLeaseEnds.resize(static_cast<std::size_t>(config.segmentCount), 0);
        std::vector<std::byte> runtimeBytes;
        status = SerializeRuntimeState(runtime, runtimeBytes);
        if (!status)
        {
            return status;
        }
        const std::filesystem::path runtimePath = sessionDirectory / L"runtime.state";
        status = AtomicWriteFile(runtimePath, runtimeBytes);
        if (!status)
        {
            return status;
        }
        auto implementation = std::make_unique<Implementation>();
        implementation->ownerLock = std::move(ownerLock);
        implementation->rootDirectory = config.rootDirectory;
        implementation->sessionDirectory = sessionDirectory;
        implementation->runtimePath = runtimePath;
        implementation->descriptor.sourceIdentity = config.sourceIdentity;
        implementation->descriptor.sourcePathUtf8 = config.sourcePathUtf8;
        implementation->descriptor.buildIdentity = config.buildIdentity;
        implementation->descriptor.compressionIdentity = config.compressionIdentity;
        implementation->descriptor.outerFecIdentity = config.outerFecIdentity;
        implementation->descriptor.sessionId = config.sessionId;
        implementation->descriptor.segmentCount = config.segmentCount;
        implementation->descriptor.descriptorBundle = config.descriptorBundle;
        implementation->runtime = std::move(runtime);
        auto candidate = std::unique_ptr<EncoderSessionStore>(new EncoderSessionStore(std::move(implementation)));
        const std::string indexText = sessionText + "\n";
        status = AtomicWriteFile(config.rootDirectory / L"SourceIndex" /
            (SourceIdentityKey(config.sourceIdentity) + L".txt"), std::as_bytes(std::span(indexText)));
        if (!status)
        {
            return status;
        }
        cleanup.Publish();
        output = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return EncoderSessionStoreStatus::Failure("Encoder session creation allocation failed");
    }
    catch (const std::exception& exception)
    {
        return EncoderSessionStoreStatus::Failure(exception.what());
    }
    catch (...)
    {
        return EncoderSessionStoreStatus::Failure("Encoder session creation failed");
    }
}

EncoderSessionStoreStatus EncoderSessionStore::EndAndDelete(const std::filesystem::path& rootDirectory,
    const std::string_view sessionIdHex) noexcept
{
    try
    {
        pbprotocol::SessionId requestedId;
        if (rootDirectory.empty() || !ParseSessionIdHex(sessionIdHex, requestedId))
        {
            return EncoderSessionStoreStatus::Failure("Encoder deletion requires an exact root and SessionId");
        }
        const std::filesystem::path root = std::filesystem::absolute(rootDirectory).lexically_normal();
        const std::filesystem::path sessionDirectory = root / std::string(sessionIdHex);
        std::vector<std::unique_ptr<StateHandle>> rootPins;
        std::vector<std::unique_ptr<StateHandle>> sessionPins;
        if (!PinDirectories(root / L"SourceIndex", rootPins))
        {
            return EncoderSessionStoreStatus::Failure("Encoder deletion root is missing, unsafe or a reparse point");
        }
        const auto indexLock = LockStateFile(root / L"SourceIndex" / L"index.lock");
        if (!indexLock)
        {
            return EncoderSessionStoreStatus::Failure("Encoder source index is busy");
        }
        if (!std::filesystem::exists(sessionDirectory))
        {
            return EncoderSessionStoreStatus::Failure("Encoder Session directory is missing");
        }
        if (!PinDirectories(sessionDirectory, sessionPins))
        {
            return EncoderSessionStoreStatus::Failure("Encoder Session directory is unsafe or a reparse point");
        }
        auto ownerLock = LockStateFile(sessionDirectory / L"owner.lock");
        if (!ownerLock)
        {
            return EncoderSessionStoreStatus::Failure("Stop the live Encoder Session owner before deleting it");
        }
        constexpr std::array<std::wstring_view, 7> allowedNames{
            L"descriptors.bin", L"runtime.state", L"descriptors.bin.tmp", L"runtime.state.tmp",
            L"ended.descriptors", L"ended.descriptors.tmp", L"owner.lock"};
        for (const auto& entry : std::filesystem::directory_iterator(sessionDirectory))
        {
            const std::wstring name = entry.path().filename().wstring();
            if (std::ranges::find(allowedNames, name) == allowedNames.end() || !IsOrdinaryStateFile(entry.path()))
            {
                return EncoderSessionStoreStatus::Failure("Encoder deletion refuses unexpected files or reparse points");
            }
        }
        const std::filesystem::path marker = sessionDirectory / L"ended.descriptors";
        const bool alreadyEnded = std::filesystem::exists(marker);
        std::vector<std::byte> descriptorBytes;
        EncoderSessionStoreStatus status = ReadFileBounded(alreadyEnded ? marker : sessionDirectory / L"descriptors.bin",
            maximumDescriptorFileBytes, descriptorBytes);
        ParsedDescriptorState descriptor;
        if (status)
        {
            status = ParseDescriptorState(descriptorBytes, descriptor);
        }
        if (!status || descriptor.sessionId != requestedId)
        {
            return EncoderSessionStoreStatus::Failure("Encoder deletion descriptor identity/checksum is invalid");
        }
        const std::filesystem::path indexPath = root / L"SourceIndex" /
            (SourceIdentityKey(descriptor.sourceIdentity) + L".txt");
        bool removeIndex = false;
        if (std::filesystem::exists(indexPath))
        {
            std::vector<std::byte> indexBytes;
            status = IsOrdinaryStateFile(indexPath) ? ReadFileBounded(indexPath, 64, indexBytes) :
                EncoderSessionStoreStatus::Failure("Unsafe source index");
            pbprotocol::SessionId indexedId;
            if (!status || indexBytes.size() != 33 || indexBytes.back() != std::byte{'\n'} ||
                !ParseSessionIdHex(std::string_view(reinterpret_cast<const char*>(indexBytes.data()), 32), indexedId))
            {
                return EncoderSessionStoreStatus::Failure("Encoder deletion refuses a malformed source index");
            }
            removeIndex = indexedId == requestedId;
        }
        // An interruption after this durable marker cannot resume the ended
        // Session. Keeping the validated descriptor in the marker makes a
        // partial deletion retryable without inventing an index identity.
        if (!alreadyEnded)
        {
            status = AtomicWriteFile(marker, descriptorBytes);
            if (!status)
            {
                return status;
            }
        }
        if (removeIndex && !DeleteFileW(indexPath.c_str()))
        {
            return EncoderSessionStoreStatus::Failure(NativeFailure("Encoder index deletion", GetLastError()));
        }
        for (const std::wstring_view name : {L"runtime.state", L"runtime.state.tmp", L"descriptors.bin",
            L"descriptors.bin.tmp", L"ended.descriptors.tmp"})
        {
            if (!DeleteFileW((sessionDirectory / name).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            {
                return EncoderSessionStoreStatus::Failure(NativeFailure("Encoder state deletion", GetLastError()));
            }
        }
        if (!DeleteFileW(marker.c_str()))
        {
            return EncoderSessionStoreStatus::Failure(NativeFailure("Encoder end marker deletion", GetLastError()));
        }
        ownerLock.reset();
        sessionPins.clear();
        if (!RemoveDirectoryW(sessionDirectory.c_str()))
        {
            return EncoderSessionStoreStatus::Failure(NativeFailure("Encoder Session directory deletion", GetLastError()));
        }
        return {};
    }
    catch (const std::exception& exception)
    {
        return EncoderSessionStoreStatus::Failure(exception.what());
    }
    catch (...)
    {
        return EncoderSessionStoreStatus::Failure("Encoder explicit Session deletion failed");
    }
}

const pbprotocol::SessionId& EncoderSessionStore::GetSessionId() const noexcept
{
    return implementation_->descriptor.sessionId;
}

bool EncoderSessionStore::MatchesDescriptorBundle(const std::span<const std::byte> descriptorBundle) const noexcept
{
    return std::ranges::equal(implementation_->descriptor.descriptorBundle, descriptorBundle);
}

std::uint64_t EncoderSessionStore::GetFrameSequenceStart() const noexcept
{
    return implementation_->runtime.frameSequenceLeaseEnd;
}

std::uint64_t EncoderSessionStore::GetFrameSequenceLeaseEnd() const noexcept
{
    return implementation_->runtime.frameSequenceLeaseEnd;
}

std::uint32_t EncoderSessionStore::GetRepairIdStart(const std::uint64_t segmentOrdinal,
    const std::uint32_t systematicBlockCount) const noexcept
{
    if (segmentOrdinal >= implementation_->runtime.repairIdLeaseEnds.size())
    {
        return systematicBlockCount;
    }
    return (std::max)(systematicBlockCount,
        implementation_->runtime.repairIdLeaseEnds[static_cast<std::size_t>(segmentOrdinal)]);
}

std::uint32_t EncoderSessionStore::GetRepairIdLeaseEnd(const std::uint64_t segmentOrdinal) const noexcept
{
    return segmentOrdinal < implementation_->runtime.repairIdLeaseEnds.size() ?
        implementation_->runtime.repairIdLeaseEnds[static_cast<std::size_t>(segmentOrdinal)] : 0;
}

std::uint64_t EncoderSessionStore::GetCarouselPass() const noexcept
{
    return implementation_->runtime.carouselPass;
}

std::uint64_t EncoderSessionStore::GetSegmentOrdinal() const noexcept
{
    return implementation_->runtime.segmentOrdinal;
}

std::uint64_t EncoderSessionStore::GetGeneration() const noexcept
{
    return implementation_->runtime.generation;
}

bool EncoderSessionStore::WasResumed() const noexcept
{
    return implementation_->resumed;
}

const std::filesystem::path& EncoderSessionStore::GetSessionDirectory() const noexcept
{
    return implementation_->sessionDirectory;
}

EncoderSessionStoreStatus EncoderSessionStore::EnsureFrameSequenceLease(const std::uint64_t requiredExclusive) noexcept
{
    std::uint64_t newEnd = 0;
    EncoderSessionStoreStatus status = RoundLeaseEnd(implementation_->runtime.frameSequenceLeaseEnd,
        requiredExclusive, newEnd);
    if (!status || newEnd == implementation_->runtime.frameSequenceLeaseEnd)
    {
        return status;
    }
    const std::uint64_t originalEnd = implementation_->runtime.frameSequenceLeaseEnd;
    implementation_->runtime.frameSequenceLeaseEnd = newEnd;
    status = PersistRuntimeState();
    if (!status)
    {
        implementation_->runtime.frameSequenceLeaseEnd = originalEnd;
    }
    return status;
}

EncoderSessionStoreStatus EncoderSessionStore::EnsureRepairIdLease(const std::uint64_t segmentOrdinal,
    const std::uint64_t requiredExclusive) noexcept
{
    if (segmentOrdinal >= implementation_->runtime.repairIdLeaseEnds.size() ||
        requiredExclusive > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()))
    {
        return EncoderSessionStoreStatus::Failure("repair ID lease request is outside its bounded Segment/uint32 range");
    }
    const std::size_t index = static_cast<std::size_t>(segmentOrdinal);
    std::uint64_t newEnd = 0;
    EncoderSessionStoreStatus status = RoundLeaseEnd(implementation_->runtime.repairIdLeaseEnds[index],
        requiredExclusive, newEnd);
    if (!status || newEnd == implementation_->runtime.repairIdLeaseEnds[index])
    {
        return status;
    }
    if (newEnd > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()))
    {
        return EncoderSessionStoreStatus::Failure("repair ID durable lease endpoint exhausted uint32");
    }
    const std::uint32_t originalEnd = implementation_->runtime.repairIdLeaseEnds[index];
    implementation_->runtime.repairIdLeaseEnds[index] = static_cast<std::uint32_t>(newEnd);
    status = PersistRuntimeState();
    if (!status)
    {
        implementation_->runtime.repairIdLeaseEnds[index] = originalEnd;
    }
    return status;
}

EncoderSessionStoreStatus EncoderSessionStore::UpdateCarouselPosition(const std::uint64_t carouselPass,
    const std::uint64_t segmentOrdinal) noexcept
{
    if ((!implementation_->runtime.repairIdLeaseEnds.empty() &&
         segmentOrdinal >= implementation_->runtime.repairIdLeaseEnds.size()) ||
        (implementation_->runtime.repairIdLeaseEnds.empty() && segmentOrdinal != 0))
    {
        return EncoderSessionStoreStatus::Failure("Carousel position is outside persisted SegmentCount");
    }
    if (carouselPass == implementation_->runtime.carouselPass && segmentOrdinal == implementation_->runtime.segmentOrdinal)
    {
        return {};
    }
    const std::uint64_t originalPass = implementation_->runtime.carouselPass;
    const std::uint64_t originalOrdinal = implementation_->runtime.segmentOrdinal;
    implementation_->runtime.carouselPass = carouselPass;
    implementation_->runtime.segmentOrdinal = segmentOrdinal;
    EncoderSessionStoreStatus status = PersistRuntimeState();
    if (!status)
    {
        implementation_->runtime.carouselPass = originalPass;
        implementation_->runtime.segmentOrdinal = originalOrdinal;
    }
    return status;
}

EncoderSessionStoreStatus EncoderSessionStore::PersistRuntimeState() noexcept
{
    if (implementation_->runtime.generation == (std::numeric_limits<std::uint64_t>::max)())
    {
        return EncoderSessionStoreStatus::Failure("Encoder session generation exhausted");
    }
    implementation_->runtime.generation++;
    std::vector<std::byte> runtimeBytes;
    EncoderSessionStoreStatus status = SerializeRuntimeState(implementation_->runtime, runtimeBytes);
    if (!status)
    {
        implementation_->runtime.generation--;
        return status;
    }
    status = AtomicWriteFile(implementation_->runtimePath, runtimeBytes);
    if (!status)
    {
        implementation_->runtime.generation--;
    }
    return status;
}

} // namespace pbapp
