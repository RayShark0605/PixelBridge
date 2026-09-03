#include "decoder_resume_store.h"

#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/byte_io.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/descriptor_codec.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace pbapp
{
namespace
{

inline constexpr std::array<std::byte, 4> journalHeaderMagic{
    std::byte{'P'}, std::byte{'B'}, std::byte{'J'}, std::byte{'H'}};
inline constexpr std::array<std::byte, 4> journalRecordMagic{
    std::byte{'P'}, std::byte{'B'}, std::byte{'J'}, std::byte{'R'}};
inline constexpr std::uint16_t journalVersion = 1;
inline constexpr std::size_t journalHeaderFixedBytes = 24;
inline constexpr std::size_t journalRecordHeaderBytes = 32;
inline constexpr std::size_t journalRecordMinimumBytes = journalRecordHeaderBytes + 4;
inline constexpr std::uint64_t journalCompactionThresholdBytes = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::uint16_t segmentControlRecordType = 1;
inline constexpr std::uint16_t manifestControlRecordType = 2;
inline constexpr std::uint16_t acceptedBlockRecordType = 3;
inline constexpr std::uint16_t completedSegmentRecordType = 4;
inline constexpr std::uint16_t outputReservationRecordType = 5;
inline constexpr std::uint16_t publishIntentRecordType = 6;

[[nodiscard]] bool IsKnownRecordType(const std::uint16_t recordType) noexcept
{
    return recordType == segmentControlRecordType || recordType == manifestControlRecordType ||
        recordType == acceptedBlockRecordType || recordType == completedSegmentRecordType ||
        recordType == outputReservationRecordType || recordType == publishIntentRecordType;
}

template <typename Integer>
[[nodiscard]] Integer ReadLittleEndian(const std::span<const std::byte> bytes, const std::size_t offset) noexcept
{
    Integer value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); index++)
    {
        value = static_cast<Integer>(value | static_cast<Integer>(
            static_cast<Integer>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U)));
    }
    return value;
}

[[nodiscard]] bool MatchesFixedPrefix(const std::span<const std::byte> bytes, const std::size_t offset,
    const std::span<const std::byte> expected) noexcept
{
    if (bytes.size() <= offset)
    {
        return true;
    }
    const std::size_t availableBytes = (std::min)(bytes.size() - offset, expected.size());
    return std::ranges::equal(bytes.subspan(offset, availableBytes), expected.first(availableBytes));
}

[[nodiscard]] bool IsClearlyTruncatedRecordTail(const std::span<const std::byte> tail,
    const std::uint64_t previousGeneration) noexcept
{
    const std::array<std::byte, 2> versionBytes{
        static_cast<std::byte>(journalVersion & 0xFFU), static_cast<std::byte>((journalVersion >> 8U) & 0xFFU)};
    if (tail.empty() || !MatchesFixedPrefix(tail, 0, journalRecordMagic) ||
        !MatchesFixedPrefix(tail, journalRecordMagic.size(), versionBytes))
    {
        return false;
    }
    if (tail.size() > 6)
    {
        const std::uint8_t typeLowByte = std::to_integer<std::uint8_t>(tail[6]);
        if (typeLowByte < segmentControlRecordType || typeLowByte > publishIntentRecordType ||
            (tail.size() > 7 && tail[7] != std::byte{0}))
        {
            return false;
        }
    }
    if (tail.size() >= 16)
    {
        const std::uint32_t totalBytes = ReadLittleEndian<std::uint32_t>(tail, 8);
        const std::uint32_t payloadBytes = ReadLittleEndian<std::uint32_t>(tail, 12);
        if (totalBytes < journalRecordMinimumBytes || payloadBytes != totalBytes - journalRecordMinimumBytes)
        {
            return false;
        }
    }
    if (tail.size() >= 24 && ReadLittleEndian<std::uint64_t>(tail, 16) <= previousGeneration)
    {
        return false;
    }
    if (tail.size() > 24)
    {
        const std::size_t availableReservedBytes = (std::min)(tail.size() - 24, static_cast<std::size_t>(8));
        for (std::size_t index = 0; index < availableReservedBytes; index++)
        {
            if (tail[24 + index] != std::byte{0})
            {
                return false;
            }
        }
    }
    if (tail.size() >= journalRecordHeaderBytes)
    {
        const std::uint32_t totalBytes = ReadLittleEndian<std::uint32_t>(tail, 8);
        return totalBytes > tail.size();
    }
    return true;
}

[[nodiscard]] std::string NativeFailure(const char* operation, const DWORD error)
{
    return std::string(operation) + " failed; win32=" + std::to_string(error);
}

[[nodiscard]] std::wstring SessionTagText(const pbprotocol::SessionTag sessionTag)
{
    wchar_t text[17]{};
    const int count = swprintf_s(text, L"%016llx", static_cast<unsigned long long>(sessionTag.value));
    return count == 16 ? std::wstring(text, 16) : std::wstring{};
}

[[nodiscard]] DecoderResumeStoreStatus ReadFileBounded(const std::filesystem::path& path,
    const std::uint64_t maximumBytes, std::vector<std::byte>& output) noexcept
{
    output.clear();
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return DecoderResumeStoreStatus::Failure(NativeFailure("resume journal open", GetLastError()));
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE)
    {
        const DWORD error = GetLastError();
        CloseHandle(file);
        return DecoderResumeStoreStatus::Failure(NativeFailure("resume journal size", error));
    }
    if (size.QuadPart < 0)
    {
        CloseHandle(file);
        return DecoderResumeStoreStatus::Failure("resume journal size is invalid");
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > maximumBytes)
    {
        CloseHandle(file);
        return DecoderResumeStoreStatus::Failure("resume journal exceeds maxResumeBytes");
    }
    try
    {
        output.resize(static_cast<std::size_t>(size.QuadPart));
    }
    catch (const std::bad_alloc&)
    {
        CloseHandle(file);
        return DecoderResumeStoreStatus::Failure("resume journal allocation failed");
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
            return DecoderResumeStoreStatus::Failure(NativeFailure("resume journal read", error));
        }
        offset += readBytes;
    }
    const bool closed = CloseHandle(file) != FALSE;
    return closed ? DecoderResumeStoreStatus{} :
        DecoderResumeStoreStatus::Failure(NativeFailure("resume journal close", GetLastError()));
}

[[nodiscard]] DecoderResumeStoreStatus AtomicWriteFile(const std::filesystem::path& path,
    const std::span<const std::byte> bytes) noexcept
{
    const std::filesystem::path temporaryPath = path.wstring() + L".tmp";
    const HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return DecoderResumeStoreStatus::Failure(NativeFailure("resume compact create", GetLastError()));
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
            return DecoderResumeStoreStatus::Failure(NativeFailure("resume compact write", error));
        }
        offset += writtenBytes;
    }
    if (FlushFileBuffers(file) == FALSE)
    {
        const DWORD error = GetLastError();
        CloseHandle(file);
        DeleteFileW(temporaryPath.c_str());
        return DecoderResumeStoreStatus::Failure(NativeFailure("resume compact flush", error));
    }
    if (CloseHandle(file) == FALSE)
    {
        const DWORD error = GetLastError();
        DeleteFileW(temporaryPath.c_str());
        return DecoderResumeStoreStatus::Failure(NativeFailure("resume compact close", error));
    }
    if (MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        const DWORD error = GetLastError();
        DeleteFileW(temporaryPath.c_str());
        return DecoderResumeStoreStatus::Failure(NativeFailure("resume compact replace", error));
    }
    return {};
}

[[nodiscard]] DecoderResumeStoreStatus BuildJournalHeader(const pbprotocol::SessionTag sessionTag,
    const std::span<const std::byte> sessionControlRecord, std::vector<std::byte>& output) noexcept
{
    const auto totalWithoutCrc = pbprotocol::CheckedAddUint64(journalHeaderFixedBytes, sessionControlRecord.size());
    const auto totalBytes = totalWithoutCrc ? pbprotocol::CheckedAddUint64(totalWithoutCrc.Value(), 4) : totalWithoutCrc;
    if (!totalBytes || sessionControlRecord.empty() || sessionControlRecord.size() > UINT32_MAX)
    {
        return DecoderResumeStoreStatus::Failure("resume journal header size is invalid");
    }
    try
    {
        output.assign(static_cast<std::size_t>(totalBytes.Value()), std::byte{0});
    }
    catch (const std::bad_alloc&)
    {
        return DecoderResumeStoreStatus::Failure("resume journal header allocation failed");
    }
    pbprotocol::ByteWriter writer(output);
    bool valid = static_cast<bool>(writer.WriteFixedBytes(journalHeaderMagic));
    valid = valid && static_cast<bool>(writer.WriteUint16(journalVersion));
    valid = valid && static_cast<bool>(writer.WriteUint16(0));
    valid = valid && static_cast<bool>(writer.WriteUint64(sessionTag.value));
    valid = valid && static_cast<bool>(writer.WriteUint32(static_cast<std::uint32_t>(sessionControlRecord.size())));
    valid = valid && static_cast<bool>(writer.WriteUint32(0));
    valid = valid && static_cast<bool>(writer.WriteBytes(sessionControlRecord));
    if (!valid || writer.Remaining() != 4)
    {
        output.clear();
        return DecoderResumeStoreStatus::Failure("resume journal header serialization failed");
    }
    valid = static_cast<bool>(writer.WriteUint32(
        pbprotocol::ComputeCrc32c(std::span(output).first(output.size() - 4))));
    return valid && writer.Remaining() == 0 ? DecoderResumeStoreStatus{} :
        DecoderResumeStoreStatus::Failure("resume journal header CRC serialization failed");
}

[[nodiscard]] DecoderResumeStoreStatus ParseJournalHeader(const std::span<const std::byte> bytes,
    const pbprotocol::SessionTag expectedSessionTag, const std::span<const std::byte> expectedSessionControl,
    std::size_t& consumedBytes) noexcept
{
    consumedBytes = 0;
    if (bytes.size() < journalHeaderFixedBytes + 4)
    {
        return DecoderResumeStoreStatus::Failure("resume journal header is truncated");
    }
    pbprotocol::ByteReader reader(bytes);
    const auto magic = reader.ReadFixedBytes<4>();
    const auto version = reader.ReadUint16();
    const auto reserved = reader.ReadUint16();
    const auto sessionTag = reader.ReadUint64();
    const auto sessionBytes = reader.ReadUint32();
    const auto reserved2 = reader.ReadUint32();
    if (!magic || !version || !reserved || !sessionTag || !sessionBytes || !reserved2 ||
        magic.Value() != journalHeaderMagic || version.Value() != journalVersion || reserved.Value() != 0 ||
        reserved2.Value() != 0 || sessionTag.Value() != expectedSessionTag.value ||
        sessionBytes.Value() != expectedSessionControl.size())
    {
        return DecoderResumeStoreStatus::Failure("resume journal header identity is invalid");
    }
    const auto sessionControl = reader.ReadBytes(sessionBytes.Value());
    const auto storedCrc = reader.ReadUint32();
    if (!sessionControl || !storedCrc || !std::ranges::equal(sessionControl.Value(), expectedSessionControl))
    {
        return DecoderResumeStoreStatus::Failure("resume journal SessionDescriptor conflicts with current pixels");
    }
    consumedBytes = reader.Position();
    if (storedCrc.Value() != pbprotocol::ComputeCrc32c(bytes.first(consumedBytes - 4)))
    {
        return DecoderResumeStoreStatus::Failure("resume journal header CRC is invalid");
    }
    return {};
}

[[nodiscard]] DecoderResumeStoreStatus BuildRecord(const std::uint16_t recordType,
    const std::uint64_t generation, const std::span<const std::byte> payload, std::vector<std::byte>& output) noexcept
{
    const auto totalWithoutCrc = pbprotocol::CheckedAddUint64(journalRecordHeaderBytes, payload.size());
    const auto totalBytes = totalWithoutCrc ? pbprotocol::CheckedAddUint64(totalWithoutCrc.Value(), 4) : totalWithoutCrc;
    if (!totalBytes || totalBytes.Value() > UINT32_MAX || payload.size() > UINT32_MAX || generation == 0)
    {
        return DecoderResumeStoreStatus::Failure("resume journal record size or generation is invalid");
    }
    try
    {
        output.assign(static_cast<std::size_t>(totalBytes.Value()), std::byte{0});
    }
    catch (const std::bad_alloc&)
    {
        return DecoderResumeStoreStatus::Failure("resume journal record allocation failed");
    }
    pbprotocol::ByteWriter writer(output);
    bool valid = static_cast<bool>(writer.WriteFixedBytes(journalRecordMagic));
    valid = valid && static_cast<bool>(writer.WriteUint16(journalVersion));
    valid = valid && static_cast<bool>(writer.WriteUint16(recordType));
    valid = valid && static_cast<bool>(writer.WriteUint32(static_cast<std::uint32_t>(totalBytes.Value())));
    valid = valid && static_cast<bool>(writer.WriteUint32(static_cast<std::uint32_t>(payload.size())));
    valid = valid && static_cast<bool>(writer.WriteUint64(generation));
    valid = valid && static_cast<bool>(writer.WriteUint64(0));
    valid = valid && static_cast<bool>(writer.WriteBytes(payload));
    if (!valid || writer.Remaining() != 4)
    {
        output.clear();
        return DecoderResumeStoreStatus::Failure("resume journal record serialization failed");
    }
    valid = static_cast<bool>(writer.WriteUint32(
        pbprotocol::ComputeCrc32c(std::span(output).first(output.size() - 4))));
    return valid && writer.Remaining() == 0 ? DecoderResumeStoreStatus{} :
        DecoderResumeStoreStatus::Failure("resume journal record CRC serialization failed");
}

[[nodiscard]] DecoderResumeStoreStatus BuildAcceptedBlockPayload(const DecoderResumeAcceptedBlock& block,
    std::vector<std::byte>& output) noexcept
{
    const auto totalBytes = pbprotocol::CheckedAddUint64(20, block.paddedPayload.size());
    if (!totalBytes || block.paddedPayload.size() > UINT32_MAX)
    {
        return DecoderResumeStoreStatus::Failure("resume accepted block size overflow");
    }
    try
    {
        output.assign(static_cast<std::size_t>(totalBytes.Value()), std::byte{0});
    }
    catch (const std::bad_alloc&)
    {
        return DecoderResumeStoreStatus::Failure("resume accepted block allocation failed");
    }
    pbprotocol::ByteWriter writer(output);
    bool valid = static_cast<bool>(writer.WriteUint64(block.segmentOrdinal));
    valid = valid && static_cast<bool>(writer.WriteUint32(block.outerBlockId));
    valid = valid && static_cast<bool>(writer.WriteUint16(block.declaredPayloadBytes));
    valid = valid && static_cast<bool>(writer.WriteUint16(0));
    valid = valid && static_cast<bool>(writer.WriteUint32(static_cast<std::uint32_t>(block.paddedPayload.size())));
    valid = valid && static_cast<bool>(writer.WriteBytes(block.paddedPayload));
    return valid && writer.Remaining() == 0 ? DecoderResumeStoreStatus{} :
        DecoderResumeStoreStatus::Failure("resume accepted block serialization failed");
}

[[nodiscard]] DecoderResumeStoreStatus ParseAcceptedBlockPayload(const std::span<const std::byte> payload,
    const pbprotocol::ReceiverResourcePolicy& policy, DecoderResumeAcceptedBlock& output) noexcept
{
    pbprotocol::ByteReader reader(payload);
    const auto ordinal = reader.ReadUint64();
    const auto blockId = reader.ReadUint32();
    const auto declaredBytes = reader.ReadUint16();
    const auto reserved = reader.ReadUint16();
    const auto paddedBytes = reader.ReadUint32();
    if (!ordinal || !blockId || !declaredBytes || !reserved || !paddedBytes || reserved.Value() != 0 ||
        ordinal.Value() >= policy.maxSegmentCount || paddedBytes.Value() == 0 ||
        paddedBytes.Value() > policy.maxOuterBlockBytes || declaredBytes.Value() == 0 ||
        declaredBytes.Value() > paddedBytes.Value())
    {
        return DecoderResumeStoreStatus::Failure("resume accepted block header is invalid");
    }
    const auto paddedPayload = reader.ReadBytes(paddedBytes.Value());
    if (!paddedPayload || !reader.RequireFullyConsumed())
    {
        return DecoderResumeStoreStatus::Failure("resume accepted block payload is truncated");
    }
    DecoderResumeAcceptedBlock parsed;
    parsed.segmentOrdinal = ordinal.Value();
    parsed.outerBlockId = blockId.Value();
    parsed.declaredPayloadBytes = declaredBytes.Value();
    try
    {
        parsed.paddedPayload.assign(paddedPayload.Value().begin(), paddedPayload.Value().end());
    }
    catch (const std::bad_alloc&)
    {
        return DecoderResumeStoreStatus::Failure("resume accepted block payload allocation failed");
    }
    output = std::move(parsed);
    return {};
}

[[nodiscard]] DecoderResumeStoreStatus BuildCompletedPayload(
    const pbprotocol::ResumeCompletedSegmentRecord& completed, std::vector<std::byte>& output) noexcept
{
    try
    {
        output.assign(72, std::byte{0});
    }
    catch (const std::bad_alloc&)
    {
        return DecoderResumeStoreStatus::Failure("resume completed record allocation failed");
    }
    pbprotocol::ByteWriter writer(output);
    bool valid = static_cast<bool>(writer.WriteFixedBytes(completed.sessionId.bytes));
    valid = valid && static_cast<bool>(writer.WriteUint64(completed.segmentOrdinal));
    valid = valid && static_cast<bool>(writer.WriteUint64(completed.rawOffset));
    valid = valid && static_cast<bool>(writer.WriteUint64(completed.rawSize));
    valid = valid && static_cast<bool>(writer.WriteFixedBytes(completed.rawDigest.bytes));
    return valid && writer.Remaining() == 0 ? DecoderResumeStoreStatus{} :
        DecoderResumeStoreStatus::Failure("resume completed record serialization failed");
}

[[nodiscard]] DecoderResumeStoreStatus ParseCompletedPayload(const std::span<const std::byte> payload,
    pbprotocol::ResumeCompletedSegmentRecord& output) noexcept
{
    if (payload.size() != 72)
    {
        return DecoderResumeStoreStatus::Failure("resume completed record size is invalid");
    }
    pbprotocol::ByteReader reader(payload);
    const auto sessionId = reader.ReadFixedBytes<pbprotocol::kSessionIdBytes>();
    const auto ordinal = reader.ReadUint64();
    const auto rawOffset = reader.ReadUint64();
    const auto rawSize = reader.ReadUint64();
    const auto rawDigest = reader.ReadFixedBytes<pbprotocol::kDigestBytes>();
    if (!sessionId || !ordinal || !rawOffset || !rawSize || !rawDigest || !reader.RequireFullyConsumed())
    {
        return DecoderResumeStoreStatus::Failure("resume completed record is malformed");
    }
    output = {pbprotocol::SessionId{sessionId.Value()}, ordinal.Value(), rawOffset.Value(), rawSize.Value(),
        pbprotocol::RawDigest{rawDigest.Value()}};
    return {};
}

[[nodiscard]] DecoderResumeStoreStatus ParseOutputReservationPayload(const std::span<const std::byte> payload,
    std::string& output) noexcept
{
    if (payload.empty())
    {
        return DecoderResumeStoreStatus::Failure("resume output reservation filename is empty");
    }
    try
    {
        output.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
    }
    catch (const std::bad_alloc&)
    {
        return DecoderResumeStoreStatus::Failure("resume output reservation filename allocation failed");
    }
    const pbprotocol::ProtocolStatus validationStatus = pbprotocol::ValidateFileNameUtf8(output);
    if (!validationStatus)
    {
        output.clear();
        return DecoderResumeStoreStatus::Failure("resume output reservation filename is invalid");
    }
    return {};
}

[[nodiscard]] DecoderResumeStoreStatus ParsePublishIntentPayload(const std::span<const std::byte> payload,
    pbprotocol::WholeFileDigest& output) noexcept
{
    if (payload.size() != pbprotocol::kDigestBytes)
    {
        return DecoderResumeStoreStatus::Failure("resume publish intent digest size is invalid");
    }
    std::copy(payload.begin(), payload.end(), output.bytes.begin());
    return {};
}

[[nodiscard]] bool IsCompletedOrdinal(const std::vector<pbprotocol::ResumeCompletedSegmentRecord>& completed,
    const std::uint64_t segmentOrdinal) noexcept
{
    return std::ranges::any_of(completed, [segmentOrdinal](const auto& record)
    {
        return record.segmentOrdinal == segmentOrdinal;
    });
}

[[nodiscard]] std::size_t CountActiveSegments(const std::vector<DecoderResumeAcceptedBlock>& blocks) noexcept
{
    std::array<std::uint64_t, 4> ordinals{};
    std::size_t count = 0;
    for (const DecoderResumeAcceptedBlock& block : blocks)
    {
        bool found = false;
        for (std::size_t index = 0; index < count; index++)
        {
            found = found || ordinals[index] == block.segmentOrdinal;
        }
        if (!found)
        {
            if (count == ordinals.size())
            {
                return count + 1;
            }
            ordinals[count] = block.segmentOrdinal;
            count++;
        }
    }
    return count;
}

[[nodiscard]] std::optional<pbprotocol::SegmentDescriptor> FindBoundSegmentDescriptor(
    const std::vector<std::vector<std::byte>>& segmentControls,
    const pbprotocol::SessionDescriptor& session, const pbprotocol::ReceiverResourcePolicy& policy,
    const std::uint64_t segmentOrdinal) noexcept
{
    for (const std::vector<std::byte>& controlBytes : segmentControls)
    {
        const auto control = pbprotocol::ParseControlRecord(controlBytes);
        if (!control || control.Value().recordType != pbprotocol::ControlRecordType::SegmentDescriptor)
        {
            return std::nullopt;
        }
        const auto descriptor = pbprotocol::ParseSegmentDescriptor(control.Value().payload, session, policy);
        if (!descriptor)
        {
            return std::nullopt;
        }
        if (descriptor.Value().segmentOrdinal == segmentOrdinal)
        {
            return descriptor.Value();
        }
    }
    return std::nullopt;
}

} // namespace

struct DecoderResumeStore::Implementation
{
    HANDLE file = INVALID_HANDLE_VALUE;
    std::filesystem::path path;
    pbprotocol::ReceiverResourcePolicy policy;
    pbprotocol::SessionTag sessionTag{};
    pbprotocol::SessionDescriptor session;
    std::vector<std::byte> sessionControl;
    std::vector<std::vector<std::byte>> segmentControls;
    std::vector<std::byte> manifestControl;
    std::vector<pbprotocol::ResumeCompletedSegmentRecord> completedSegments;
    std::vector<DecoderResumeAcceptedBlock> activeBlocks;
    std::vector<DecoderResumeAcceptedBlock> pendingBlocks;
    std::optional<std::string> outputReservationFileNameUtf8;
    std::optional<pbprotocol::WholeFileDigest> publishIntent;
    std::uint64_t generation = 0;
    std::uint64_t fileBytes = 0;
    bool resumed = false;
    bool hadTruncatedTail = false;
    bool terminalFailure = false;
};

DecoderResumeStore::DecoderResumeStore(std::unique_ptr<Implementation> implementation) noexcept :
    implementation_(std::move(implementation))
{
}

DecoderResumeStore::~DecoderResumeStore()
{
    if (implementation_ && implementation_->file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(implementation_->file);
        implementation_->file = INVALID_HANDLE_VALUE;
    }
}

DecoderResumeStoreStatus DecoderResumeStore::Open(const std::filesystem::path& outputDirectory,
    const pbprotocol::SessionTag sessionTag, const std::span<const std::byte> sessionControlRecord,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy, std::unique_ptr<DecoderResumeStore>& output,
    DecoderResumeLoadedState& loaded) noexcept
{
    output.reset();
    loaded = {};
    try
    {
        if (outputDirectory.empty() || sessionControlRecord.empty() || resourcePolicy.maxResumeBytes == 0 ||
            resourcePolicy.maxActiveOuterFecDecoders == 0 || resourcePolicy.maxActiveOuterFecDecoders > 4)
        {
            return DecoderResumeStoreStatus::Failure("resume store configuration is invalid");
        }
        const auto control = pbprotocol::ParseControlRecord(sessionControlRecord);
        if (!control || control.Value().recordType != pbprotocol::ControlRecordType::SessionDescriptor ||
            control.Value().sessionTag != sessionTag)
        {
            return DecoderResumeStoreStatus::Failure("resume store requires a canonical SessionDescriptor ControlRecord");
        }
        const auto parsedSession = pbprotocol::ParseSessionDescriptor(control.Value().payload, resourcePolicy);
        if (!parsedSession || pbprotocol::DeriveSessionTag(parsedSession.Value().sessionId) != sessionTag)
        {
            return DecoderResumeStoreStatus::Failure("resume store SessionDescriptor is invalid");
        }
        auto implementation = std::make_unique<Implementation>();
        implementation->path = outputDirectory / (L"PixelBridge-" + SessionTagText(sessionTag) + L".resume");
        implementation->policy = resourcePolicy;
        implementation->sessionTag = sessionTag;
        implementation->session = parsedSession.Value();
        implementation->sessionControl.assign(sessionControlRecord.begin(), sessionControlRecord.end());

        if (!std::filesystem::exists(implementation->path))
        {
            std::vector<std::byte> header;
            DecoderResumeStoreStatus status = BuildJournalHeader(sessionTag, sessionControlRecord, header);
            if (!status || header.size() > resourcePolicy.maxResumeBytes)
            {
                return status ? DecoderResumeStoreStatus::Failure("resume header exceeds maxResumeBytes") : status;
            }
            status = AtomicWriteFile(implementation->path, header);
            if (!status)
            {
                return status;
            }
            implementation->fileBytes = header.size();
        }
        else
        {
            implementation->resumed = true;
            std::vector<std::byte> document;
            DecoderResumeStoreStatus status = ReadFileBounded(implementation->path,
                resourcePolicy.maxResumeBytes, document);
            if (!status)
            {
                return status;
            }
            std::size_t position = 0;
            status = ParseJournalHeader(document, sessionTag, sessionControlRecord, position);
            if (!status)
            {
                return status;
            }
            std::vector<std::optional<pbprotocol::SegmentDescriptor>> parsedSegments(
                static_cast<std::size_t>(parsedSession.Value().segmentCount));
            while (position < document.size())
            {
                const std::size_t remaining = document.size() - position;
                if (remaining < journalRecordMinimumBytes)
                {
                    if (!IsClearlyTruncatedRecordTail(std::span(document).subspan(position),
                        implementation->generation))
                    {
                        return DecoderResumeStoreStatus::Failure(
                            "resume journal tail is not a valid truncated record prefix");
                    }
                    implementation->hadTruncatedTail = true;
                    break;
                }
                pbprotocol::ByteReader recordHeader(std::span(document).subspan(position));
                const auto magic = recordHeader.ReadFixedBytes<4>();
                const auto version = recordHeader.ReadUint16();
                const auto type = recordHeader.ReadUint16();
                const auto totalBytes = recordHeader.ReadUint32();
                const auto payloadBytes = recordHeader.ReadUint32();
                const auto generation = recordHeader.ReadUint64();
                const auto reserved = recordHeader.ReadUint64();
                if (!magic || !version || !type || !totalBytes || !payloadBytes || !generation || !reserved)
                {
                    return DecoderResumeStoreStatus::Failure("resume journal record header is truncated");
                }
                if (magic.Value() != journalRecordMagic || version.Value() != journalVersion ||
                    reserved.Value() != 0 || totalBytes.Value() < journalRecordMinimumBytes ||
                    payloadBytes.Value() != totalBytes.Value() - journalRecordMinimumBytes ||
                    generation.Value() <= implementation->generation)
                {
                    return DecoderResumeStoreStatus::Failure("resume journal record header is invalid or non-monotonic");
                }
                if (!IsKnownRecordType(type.Value()))
                {
                    return DecoderResumeStoreStatus::Failure(
                        "resume journal contains an unknown mandatory record type");
                }
                if (totalBytes.Value() > remaining)
                {
                    implementation->hadTruncatedTail = true;
                    break;
                }
                const std::span<const std::byte> record = std::span(document).subspan(position, totalBytes.Value());
                const std::span<const std::byte> payload = record.subspan(journalRecordHeaderBytes,
                    payloadBytes.Value());
                pbprotocol::ByteReader crcReader(record.last(4));
                const auto storedCrc = crcReader.ReadUint32();
                if (!storedCrc || storedCrc.Value() != pbprotocol::ComputeCrc32c(record.first(record.size() - 4)))
                {
                    return DecoderResumeStoreStatus::Failure("resume journal internal record CRC is invalid");
                }
                if (implementation->publishIntent && type.Value() != publishIntentRecordType)
                {
                    return DecoderResumeStoreStatus::Failure(
                        "resume journal mutates durable state after publish intent");
                }
                implementation->generation = generation.Value();
                if (type.Value() == segmentControlRecordType)
                {
                    const auto parsedControl = pbprotocol::ParseControlRecord(payload);
                    if (!parsedControl || parsedControl.Value().recordType != pbprotocol::ControlRecordType::SegmentDescriptor ||
                        parsedControl.Value().sessionTag != sessionTag)
                    {
                        return DecoderResumeStoreStatus::Failure("resume SegmentDescriptor ControlRecord is invalid");
                    }
                    const auto descriptor = pbprotocol::ParseSegmentDescriptor(parsedControl.Value().payload,
                        implementation->session, resourcePolicy);
                    if (!descriptor || descriptor.Value().segmentOrdinal >= parsedSegments.size())
                    {
                        return DecoderResumeStoreStatus::Failure("resume SegmentDescriptor payload is invalid");
                    }
                    auto& existing = parsedSegments[static_cast<std::size_t>(descriptor.Value().segmentOrdinal)];
                    if (existing && *existing != descriptor.Value())
                    {
                        return DecoderResumeStoreStatus::Failure("resume SegmentDescriptor conflict");
                    }
                    if (!existing)
                    {
                        existing = descriptor.Value();
                        implementation->segmentControls.emplace_back(payload.begin(), payload.end());
                    }
                }
                else if (type.Value() == manifestControlRecordType)
                {
                    const auto parsedControl = pbprotocol::ParseControlRecord(payload);
                    if (!parsedControl || parsedControl.Value().recordType != pbprotocol::ControlRecordType::FinalManifest ||
                        parsedControl.Value().sessionTag != sessionTag ||
                        !pbprotocol::ParseFinalManifest(parsedControl.Value().payload, implementation->session, resourcePolicy))
                    {
                        return DecoderResumeStoreStatus::Failure("resume FinalManifest ControlRecord is invalid");
                    }
                    if (!implementation->manifestControl.empty() &&
                        !std::ranges::equal(implementation->manifestControl, payload))
                    {
                        return DecoderResumeStoreStatus::Failure("resume FinalManifest conflict");
                    }
                    implementation->manifestControl.assign(payload.begin(), payload.end());
                }
                else if (type.Value() == acceptedBlockRecordType)
                {
                    DecoderResumeAcceptedBlock block;
                    status = ParseAcceptedBlockPayload(payload, resourcePolicy, block);
                    if (!status || block.segmentOrdinal >= parsedSegments.size() ||
                        !parsedSegments[static_cast<std::size_t>(block.segmentOrdinal)] ||
                        IsCompletedOrdinal(implementation->completedSegments, block.segmentOrdinal))
                    {
                        return status ? DecoderResumeStoreStatus::Failure(
                            "resume accepted block lacks a live bound SegmentDescriptor") : status;
                    }
                    const pbprotocol::SegmentDescriptor& descriptor =
                        *parsedSegments[static_cast<std::size_t>(block.segmentOrdinal)];
                    if (block.paddedPayload.size() != descriptor.outerBlockBytes)
                    {
                        return DecoderResumeStoreStatus::Failure(
                            "resume accepted block size conflicts with its SegmentDescriptor");
                    }
                    const auto existing = std::ranges::find_if(implementation->activeBlocks, [&block](const auto& value)
                    {
                        return value.segmentOrdinal == block.segmentOrdinal && value.outerBlockId == block.outerBlockId;
                    });
                    if (existing != implementation->activeBlocks.end() && *existing != block)
                    {
                        return DecoderResumeStoreStatus::Failure("resume accepted block payload conflict");
                    }
                    if (existing == implementation->activeBlocks.end())
                    {
                        implementation->activeBlocks.push_back(std::move(block));
                    }
                }
                else if (type.Value() == completedSegmentRecordType)
                {
                    pbprotocol::ResumeCompletedSegmentRecord completed;
                    status = ParseCompletedPayload(payload, completed);
                    if (!status || completed.sessionId != implementation->session.sessionId ||
                        completed.segmentOrdinal >= parsedSegments.size() ||
                        !parsedSegments[static_cast<std::size_t>(completed.segmentOrdinal)])
                    {
                        return status ? DecoderResumeStoreStatus::Failure(
                            "resume completed Segment lacks its bound descriptor") : status;
                    }
                    const pbprotocol::SegmentDescriptor& descriptor =
                        *parsedSegments[static_cast<std::size_t>(completed.segmentOrdinal)];
                    if (completed.rawOffset != descriptor.rawOffset || completed.rawSize != descriptor.rawSize ||
                        completed.rawDigest != descriptor.rawDigest)
                    {
                        return DecoderResumeStoreStatus::Failure("resume completed Segment conflicts with descriptor");
                    }
                    const auto existing = std::ranges::find_if(implementation->completedSegments,
                        [&completed](const auto& value)
                        {
                            return value.segmentOrdinal == completed.segmentOrdinal;
                        });
                    if (existing != implementation->completedSegments.end() && *existing != completed)
                    {
                        return DecoderResumeStoreStatus::Failure("resume completed Segment duplicate conflict");
                    }
                    if (existing == implementation->completedSegments.end())
                    {
                        implementation->completedSegments.push_back(completed);
                    }
                    std::erase_if(implementation->activeBlocks, [&completed](const auto& block)
                    {
                        return block.segmentOrdinal == completed.segmentOrdinal;
                    });
                }
                else if (type.Value() == outputReservationRecordType)
                {
                    std::string finalFileNameUtf8;
                    status = ParseOutputReservationPayload(payload, finalFileNameUtf8);
                    if (!status)
                    {
                        return status;
                    }
                    if (implementation->outputReservationFileNameUtf8 &&
                        *implementation->outputReservationFileNameUtf8 != finalFileNameUtf8)
                    {
                        return DecoderResumeStoreStatus::Failure("resume output reservation conflict");
                    }
                    implementation->outputReservationFileNameUtf8 = std::move(finalFileNameUtf8);
                }
                else if (type.Value() == publishIntentRecordType)
                {
                    pbprotocol::WholeFileDigest publishIntent;
                    status = ParsePublishIntentPayload(payload, publishIntent);
                    if (!status || !implementation->outputReservationFileNameUtf8 ||
                        implementation->manifestControl.empty() ||
                        implementation->completedSegments.size() != implementation->session.segmentCount)
                    {
                        return status ? DecoderResumeStoreStatus::Failure(
                            "resume publish intent lacks a complete durable output state") : status;
                    }
                    const auto manifestControl = pbprotocol::ParseControlRecord(implementation->manifestControl);
                    const auto manifest = manifestControl ? pbprotocol::ParseFinalManifest(
                        manifestControl.Value().payload, implementation->session, resourcePolicy) :
                        pbprotocol::ProtocolResult<pbprotocol::FinalManifest>::Failure(
                            pbprotocol::ProtocolErrorCode::InternalInvariantViolation, 0);
                    if (!manifest || manifest.Value().wholeFileDigest != publishIntent)
                    {
                        return DecoderResumeStoreStatus::Failure(
                            "resume publish intent digest conflicts with FinalManifest");
                    }
                    if (implementation->publishIntent && *implementation->publishIntent != publishIntent)
                    {
                        return DecoderResumeStoreStatus::Failure("resume publish intent conflict");
                    }
                    implementation->publishIntent = publishIntent;
                }
                if (CountActiveSegments(implementation->activeBlocks) > resourcePolicy.maxActiveOuterFecDecoders)
                {
                    return DecoderResumeStoreStatus::Failure("resume journal exceeds the active Segment limit");
                }
                position += totalBytes.Value();
            }
            implementation->fileBytes = position;
        }

        implementation->file = CreateFileW(implementation->path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (implementation->file == INVALID_HANDLE_VALUE)
        {
            return DecoderResumeStoreStatus::Failure(NativeFailure("resume journal append open", GetLastError()));
        }
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(implementation->fileBytes);
        if (SetFilePointerEx(implementation->file, end, nullptr, FILE_BEGIN) == FALSE ||
            SetEndOfFile(implementation->file) == FALSE)
        {
            const DWORD error = GetLastError();
            CloseHandle(implementation->file);
            return DecoderResumeStoreStatus::Failure(NativeFailure("resume journal tail repair", error));
        }
        auto store = std::unique_ptr<DecoderResumeStore>(new DecoderResumeStore(std::move(implementation)));
        if (store->implementation_->hadTruncatedTail)
        {
            const DecoderResumeStoreStatus compactStatus = store->Compact();
            if (!compactStatus)
            {
                return compactStatus;
            }
        }
        loaded.segmentControlRecords = store->implementation_->segmentControls;
        loaded.manifestControlRecord = store->implementation_->manifestControl;
        loaded.completedSegments = store->implementation_->completedSegments;
        loaded.activeBlocks = store->implementation_->activeBlocks;
        loaded.outputReservationFileNameUtf8 = store->implementation_->outputReservationFileNameUtf8;
        loaded.publishIntent = store->implementation_->publishIntent;
        loaded.resumed = store->implementation_->resumed;
        loaded.hadTruncatedTail = store->implementation_->hadTruncatedTail;
        loaded.generation = store->implementation_->generation;
        output = std::move(store);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return DecoderResumeStoreStatus::Failure("resume store allocation failed");
    }
    catch (const std::exception& exception)
    {
        return DecoderResumeStoreStatus::Failure(exception.what());
    }
    catch (...)
    {
        return DecoderResumeStoreStatus::Failure("resume store open failed");
    }
}

DecoderResumeStoreStatus DecoderResumeStore::RecordSegmentControl(
    const std::span<const std::byte> controlRecord) noexcept
{
    if (!implementation_ || implementation_->terminalFailure)
    {
        return DecoderResumeStoreStatus::Failure("resume store is unavailable");
    }
    const auto control = pbprotocol::ParseControlRecord(controlRecord);
    if (!control || control.Value().recordType != pbprotocol::ControlRecordType::SegmentDescriptor ||
        control.Value().sessionTag != implementation_->sessionTag)
    {
        return DecoderResumeStoreStatus::Failure("resume SegmentDescriptor ControlRecord is invalid");
    }
    const auto descriptor = pbprotocol::ParseSegmentDescriptor(control.Value().payload,
        implementation_->session, implementation_->policy);
    if (!descriptor)
    {
        return DecoderResumeStoreStatus::Failure("resume SegmentDescriptor payload is invalid");
    }
    for (const std::vector<std::byte>& existing : implementation_->segmentControls)
    {
        const auto existingControl = pbprotocol::ParseControlRecord(existing);
        const auto existingDescriptor = existingControl ? pbprotocol::ParseSegmentDescriptor(
            existingControl.Value().payload, implementation_->session, implementation_->policy) :
            pbprotocol::ProtocolResult<pbprotocol::SegmentDescriptor>::Failure(
                pbprotocol::ProtocolErrorCode::InternalInvariantViolation, 0);
        if (existingDescriptor && existingDescriptor.Value().segmentOrdinal == descriptor.Value().segmentOrdinal)
        {
            return existingDescriptor.Value() == descriptor.Value() ? DecoderResumeStoreStatus{} :
                DecoderResumeStoreStatus::Failure("resume SegmentDescriptor conflict");
        }
    }
    DecoderResumeStoreStatus status = AppendRecord(segmentControlRecordType, controlRecord, true);
    if (status)
    {
        try
        {
            implementation_->segmentControls.emplace_back(controlRecord.begin(), controlRecord.end());
        }
        catch (const std::bad_alloc&)
        {
            implementation_->terminalFailure = true;
            return DecoderResumeStoreStatus::Failure("resume SegmentDescriptor memory update failed after durable append");
        }
    }
    return status;
}

DecoderResumeStoreStatus DecoderResumeStore::RecordManifestControl(
    const std::span<const std::byte> controlRecord) noexcept
{
    if (!implementation_ || implementation_->terminalFailure)
    {
        return DecoderResumeStoreStatus::Failure("resume store is unavailable");
    }
    if (!implementation_->manifestControl.empty())
    {
        return std::ranges::equal(implementation_->manifestControl, controlRecord) ? DecoderResumeStoreStatus{} :
            DecoderResumeStoreStatus::Failure("resume FinalManifest conflict");
    }
    const auto control = pbprotocol::ParseControlRecord(controlRecord);
    if (!control || control.Value().recordType != pbprotocol::ControlRecordType::FinalManifest ||
        control.Value().sessionTag != implementation_->sessionTag ||
        !pbprotocol::ParseFinalManifest(control.Value().payload, implementation_->session, implementation_->policy))
    {
        return DecoderResumeStoreStatus::Failure("resume FinalManifest ControlRecord is invalid");
    }
    DecoderResumeStoreStatus status = AppendRecord(manifestControlRecordType, controlRecord, true);
    if (status)
    {
        try
        {
            implementation_->manifestControl.assign(controlRecord.begin(), controlRecord.end());
        }
        catch (const std::bad_alloc&)
        {
            implementation_->terminalFailure = true;
            return DecoderResumeStoreStatus::Failure("resume FinalManifest memory update failed after durable append");
        }
    }
    return status;
}

DecoderResumeStoreStatus DecoderResumeStore::RecordOutputReservation(std::string finalFileNameUtf8) noexcept
{
    if (!implementation_ || implementation_->terminalFailure)
    {
        return DecoderResumeStoreStatus::Failure("resume store is unavailable");
    }
    const pbprotocol::ProtocolStatus validationStatus = pbprotocol::ValidateFileNameUtf8(finalFileNameUtf8);
    if (!validationStatus)
    {
        return DecoderResumeStoreStatus::Failure("resume output reservation filename is invalid");
    }
    if (implementation_->outputReservationFileNameUtf8)
    {
        return *implementation_->outputReservationFileNameUtf8 == finalFileNameUtf8 ? DecoderResumeStoreStatus{} :
            DecoderResumeStoreStatus::Failure("resume output reservation conflict");
    }
    const std::span<const char> characters(finalFileNameUtf8.data(), finalFileNameUtf8.size());
    const DecoderResumeStoreStatus status = AppendRecord(outputReservationRecordType,
        std::as_bytes(characters), true);
    if (!status)
    {
        return status;
    }
    try
    {
        implementation_->outputReservationFileNameUtf8 = std::move(finalFileNameUtf8);
    }
    catch (const std::bad_alloc&)
    {
        implementation_->terminalFailure = true;
        return DecoderResumeStoreStatus::Failure(
            "resume output reservation memory update failed after durable append");
    }
    return {};
}

DecoderResumeStoreStatus DecoderResumeStore::RecordAcceptedBlock(const DecoderResumeAcceptedBlock& block) noexcept
{
    const std::optional<pbprotocol::SegmentDescriptor> descriptor = implementation_ ?
        FindBoundSegmentDescriptor(implementation_->segmentControls, implementation_->session,
            implementation_->policy, block.segmentOrdinal) : std::nullopt;
    if (!implementation_ || implementation_->terminalFailure || block.segmentOrdinal >= implementation_->session.segmentCount ||
        block.paddedPayload.empty() || block.paddedPayload.size() > implementation_->policy.maxOuterBlockBytes ||
        block.declaredPayloadBytes == 0 || block.declaredPayloadBytes > block.paddedPayload.size() ||
        IsCompletedOrdinal(implementation_->completedSegments, block.segmentOrdinal) || !descriptor ||
        block.paddedPayload.size() != descriptor->outerBlockBytes)
    {
        return DecoderResumeStoreStatus::Failure("resume accepted block is outside its bounded active state");
    }
    const auto existing = std::ranges::find_if(implementation_->activeBlocks, [&block](const auto& value)
    {
        return value.segmentOrdinal == block.segmentOrdinal && value.outerBlockId == block.outerBlockId;
    });
    if (existing != implementation_->activeBlocks.end())
    {
        return *existing == block ? DecoderResumeStoreStatus{} :
            DecoderResumeStoreStatus::Failure("resume accepted block conflict");
    }
    try
    {
        implementation_->activeBlocks.push_back(block);
        if (CountActiveSegments(implementation_->activeBlocks) > implementation_->policy.maxActiveOuterFecDecoders)
        {
            implementation_->activeBlocks.pop_back();
            return DecoderResumeStoreStatus::Failure("resume active Segment limit exceeded");
        }
        implementation_->pendingBlocks.push_back(block);
    }
    catch (const std::bad_alloc&)
    {
        if (!implementation_->activeBlocks.empty() && implementation_->activeBlocks.back() == block)
        {
            implementation_->activeBlocks.pop_back();
        }
        return DecoderResumeStoreStatus::Failure("resume accepted block allocation failed");
    }
    return {};
}

DecoderResumeStoreStatus DecoderResumeStore::Checkpoint() noexcept
{
    if (!implementation_ || implementation_->terminalFailure)
    {
        return DecoderResumeStoreStatus::Failure("resume store is unavailable");
    }
    for (const DecoderResumeAcceptedBlock& block : implementation_->pendingBlocks)
    {
        std::vector<std::byte> payload;
        DecoderResumeStoreStatus status = BuildAcceptedBlockPayload(block, payload);
        if (!status)
        {
            return status;
        }
        status = AppendRecord(acceptedBlockRecordType, payload, false);
        if (!status)
        {
            return status;
        }
    }
    if (!implementation_->pendingBlocks.empty() && FlushFileBuffers(implementation_->file) == FALSE)
    {
        implementation_->terminalFailure = true;
        return DecoderResumeStoreStatus::Failure(NativeFailure("resume checkpoint flush", GetLastError()));
    }
    implementation_->pendingBlocks.clear();
    return implementation_->fileBytes >= journalCompactionThresholdBytes ? Compact() : DecoderResumeStoreStatus{};
}

DecoderResumeStoreStatus DecoderResumeStore::RecordCompletedSegment(
    const pbprotocol::ResumeCompletedSegmentRecord& completedRecord) noexcept
{
    const std::optional<pbprotocol::SegmentDescriptor> descriptor = implementation_ ?
        FindBoundSegmentDescriptor(implementation_->segmentControls, implementation_->session,
            implementation_->policy, completedRecord.segmentOrdinal) : std::nullopt;
    if (!implementation_ || implementation_->terminalFailure ||
        completedRecord.sessionId != implementation_->session.sessionId ||
        completedRecord.segmentOrdinal >= implementation_->session.segmentCount || !descriptor ||
        completedRecord.rawOffset != descriptor->rawOffset || completedRecord.rawSize != descriptor->rawSize ||
        completedRecord.rawDigest != descriptor->rawDigest)
    {
        return DecoderResumeStoreStatus::Failure("resume completed Segment is outside its Session");
    }
    const auto existing = std::ranges::find_if(implementation_->completedSegments,
        [&completedRecord](const auto& value)
        {
            return value.segmentOrdinal == completedRecord.segmentOrdinal;
        });
    if (existing != implementation_->completedSegments.end())
    {
        return *existing == completedRecord ? DecoderResumeStoreStatus{} :
            DecoderResumeStoreStatus::Failure("resume completed Segment conflict");
    }
    DecoderResumeStoreStatus status = Checkpoint();
    if (!status)
    {
        return status;
    }
    std::vector<std::byte> payload;
    status = BuildCompletedPayload(completedRecord, payload);
    if (!status)
    {
        return status;
    }
    status = AppendRecord(completedSegmentRecordType, payload, true);
    if (!status)
    {
        return status;
    }
    try
    {
        implementation_->completedSegments.push_back(completedRecord);
    }
    catch (const std::bad_alloc&)
    {
        implementation_->terminalFailure = true;
        return DecoderResumeStoreStatus::Failure("resume completed memory update failed after durable append");
    }
    std::erase_if(implementation_->activeBlocks, [&completedRecord](const auto& block)
    {
        return block.segmentOrdinal == completedRecord.segmentOrdinal;
    });
    return Compact();
}

DecoderResumeStoreStatus DecoderResumeStore::RecordPublishIntent(
    const pbprotocol::WholeFileDigest& wholeFileDigest) noexcept
{
    if (!implementation_ || implementation_->terminalFailure ||
        !implementation_->outputReservationFileNameUtf8 || implementation_->manifestControl.empty() ||
        implementation_->completedSegments.size() != implementation_->session.segmentCount)
    {
        return DecoderResumeStoreStatus::Failure("resume publish intent requires a complete durable output state");
    }
    const auto manifestControl = pbprotocol::ParseControlRecord(implementation_->manifestControl);
    const auto manifest = manifestControl ? pbprotocol::ParseFinalManifest(manifestControl.Value().payload,
        implementation_->session, implementation_->policy) :
        pbprotocol::ProtocolResult<pbprotocol::FinalManifest>::Failure(
            pbprotocol::ProtocolErrorCode::InternalInvariantViolation, 0);
    if (!manifest || manifest.Value().wholeFileDigest != wholeFileDigest)
    {
        return DecoderResumeStoreStatus::Failure("resume publish intent digest conflicts with FinalManifest");
    }
    if (implementation_->publishIntent)
    {
        return *implementation_->publishIntent == wholeFileDigest ? DecoderResumeStoreStatus{} :
            DecoderResumeStoreStatus::Failure("resume publish intent conflict");
    }
    DecoderResumeStoreStatus status = Checkpoint();
    if (!status)
    {
        return status;
    }
    status = AppendRecord(publishIntentRecordType, wholeFileDigest.bytes, true);
    if (status)
    {
        implementation_->publishIntent = wholeFileDigest;
    }
    return status;
}

DecoderResumeStoreStatus DecoderResumeStore::RemoveAfterPublish() noexcept
{
    if (!implementation_)
    {
        return DecoderResumeStoreStatus::Failure("resume store is unavailable");
    }
    if (implementation_->file != INVALID_HANDLE_VALUE)
    {
        const HANDLE file = std::exchange(implementation_->file, INVALID_HANDLE_VALUE);
        if (CloseHandle(file) == FALSE)
        {
            return DecoderResumeStoreStatus::Failure(NativeFailure("resume final close", GetLastError()));
        }
    }
    if (DeleteFileW(implementation_->path.c_str()) == FALSE && GetLastError() != ERROR_FILE_NOT_FOUND)
    {
        return DecoderResumeStoreStatus::Failure(NativeFailure("resume final delete", GetLastError()));
    }
    return {};
}

DecoderResumeStoreStatus DecoderResumeStore::AppendRecord(const std::uint16_t recordType,
    const std::span<const std::byte> payload, const bool flush) noexcept
{
    if (!implementation_ || implementation_->file == INVALID_HANDLE_VALUE || implementation_->terminalFailure ||
        implementation_->generation == (std::numeric_limits<std::uint64_t>::max)())
    {
        return DecoderResumeStoreStatus::Failure("resume append state is invalid");
    }
    std::vector<std::byte> record;
    DecoderResumeStoreStatus status = BuildRecord(recordType, implementation_->generation + 1, payload, record);
    const auto newFileBytes = pbprotocol::CheckedAddUint64(implementation_->fileBytes, record.size());
    if (!status || !newFileBytes || newFileBytes.Value() > implementation_->policy.maxResumeBytes)
    {
        return status ? DecoderResumeStoreStatus::Failure("resume journal maxResumeBytes exceeded") : status;
    }
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(implementation_->fileBytes);
    if (SetFilePointerEx(implementation_->file, end, nullptr, FILE_BEGIN) == FALSE)
    {
        implementation_->terminalFailure = true;
        return DecoderResumeStoreStatus::Failure(NativeFailure("resume append seek", GetLastError()));
    }
    DWORD writtenBytes = 0;
    if (record.size() > (std::numeric_limits<DWORD>::max)() ||
        WriteFile(implementation_->file, record.data(), static_cast<DWORD>(record.size()), &writtenBytes, nullptr) == FALSE ||
        writtenBytes != record.size() || (flush && FlushFileBuffers(implementation_->file) == FALSE))
    {
        implementation_->terminalFailure = true;
        return DecoderResumeStoreStatus::Failure(NativeFailure("resume append write/flush", GetLastError()));
    }
    implementation_->generation++;
    implementation_->fileBytes = newFileBytes.Value();
    return {};
}

DecoderResumeStoreStatus DecoderResumeStore::Compact() noexcept
{
    if (!implementation_ || implementation_->terminalFailure || !implementation_->pendingBlocks.empty())
    {
        return DecoderResumeStoreStatus::Failure("resume compaction state is invalid");
    }
    std::vector<std::byte> document;
    DecoderResumeStoreStatus status = BuildJournalHeader(implementation_->sessionTag,
        implementation_->sessionControl, document);
    if (!status)
    {
        return status;
    }
    std::uint64_t generation = implementation_->generation;
    auto AppendCompactRecord = [&](const std::uint16_t type, const std::span<const std::byte> payload)
        -> DecoderResumeStoreStatus
    {
        if (generation == (std::numeric_limits<std::uint64_t>::max)())
        {
            return DecoderResumeStoreStatus::Failure("resume compact generation exhausted");
        }
        std::vector<std::byte> record;
        DecoderResumeStoreStatus recordStatus = BuildRecord(type, generation + 1, payload, record);
        const auto newSize = pbprotocol::CheckedAddUint64(document.size(), record.size());
        if (!recordStatus || !newSize || newSize.Value() > implementation_->policy.maxResumeBytes)
        {
            return recordStatus ? DecoderResumeStoreStatus::Failure("resume compact document exceeds maxResumeBytes") :
                recordStatus;
        }
        try
        {
            document.insert(document.end(), record.begin(), record.end());
        }
        catch (const std::bad_alloc&)
        {
            return DecoderResumeStoreStatus::Failure("resume compact document allocation failed");
        }
        generation++;
        return {};
    };
    if (implementation_->outputReservationFileNameUtf8)
    {
        const std::span<const char> characters(implementation_->outputReservationFileNameUtf8->data(),
            implementation_->outputReservationFileNameUtf8->size());
        status = AppendCompactRecord(outputReservationRecordType, std::as_bytes(characters));
        if (!status)
        {
            return status;
        }
    }
    for (const std::vector<std::byte>& control : implementation_->segmentControls)
    {
        status = AppendCompactRecord(segmentControlRecordType, control);
        if (!status)
        {
            return status;
        }
    }
    if (!implementation_->manifestControl.empty())
    {
        status = AppendCompactRecord(manifestControlRecordType, implementation_->manifestControl);
        if (!status)
        {
            return status;
        }
    }
    for (const pbprotocol::ResumeCompletedSegmentRecord& completed : implementation_->completedSegments)
    {
        std::vector<std::byte> payload;
        status = BuildCompletedPayload(completed, payload);
        if (!status || !(status = AppendCompactRecord(completedSegmentRecordType, payload)))
        {
            return status;
        }
    }
    for (const DecoderResumeAcceptedBlock& block : implementation_->activeBlocks)
    {
        std::vector<std::byte> payload;
        status = BuildAcceptedBlockPayload(block, payload);
        if (!status || !(status = AppendCompactRecord(acceptedBlockRecordType, payload)))
        {
            return status;
        }
    }
    if (implementation_->publishIntent)
    {
        status = AppendCompactRecord(publishIntentRecordType, implementation_->publishIntent->bytes);
        if (!status)
        {
            return status;
        }
    }
    if (implementation_->file != INVALID_HANDLE_VALUE)
    {
        const HANDLE file = std::exchange(implementation_->file, INVALID_HANDLE_VALUE);
        if (CloseHandle(file) == FALSE)
        {
            implementation_->terminalFailure = true;
            return DecoderResumeStoreStatus::Failure(NativeFailure("resume compact source close", GetLastError()));
        }
    }
    status = AtomicWriteFile(implementation_->path, document);
    if (!status)
    {
        implementation_->terminalFailure = true;
        return status;
    }
    implementation_->file = CreateFileW(implementation_->path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (implementation_->file == INVALID_HANDLE_VALUE)
    {
        implementation_->terminalFailure = true;
        return DecoderResumeStoreStatus::Failure(NativeFailure("resume compact reopen", GetLastError()));
    }
    implementation_->generation = generation;
    implementation_->fileBytes = document.size();
    return {};
}

const std::filesystem::path& DecoderResumeStore::GetPath() const noexcept
{
    return implementation_->path;
}

std::uint64_t DecoderResumeStore::GetGeneration() const noexcept
{
    return implementation_->generation;
}

std::uint64_t DecoderResumeStore::GetFileBytes() const noexcept
{
    return implementation_->fileBytes;
}

std::size_t DecoderResumeStore::GetActiveBlockCount() const noexcept
{
    return implementation_->activeBlocks.size();
}

std::size_t DecoderResumeStore::GetPendingBlockCount() const noexcept
{
    return implementation_->pendingBlocks.size();
}

bool DecoderResumeStore::WasResumed() const noexcept
{
    return implementation_->resumed;
}

bool DecoderResumeStore::HadTruncatedTail() const noexcept
{
    return implementation_->hadTruncatedTail;
}

} // namespace pbapp
