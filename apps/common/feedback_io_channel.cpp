#include "feedback_io_channel.h"
#include "pbprotocol/byte_io.h"
#include <algorithm>
#include <limits>
#include <string_view>

namespace pbapp::detail
{
namespace
{
constexpr std::array<std::byte, 4> ioMagic{std::byte{'P'}, std::byte{'B'}, std::byte{'I'}, std::byte{'P'}};

bool ParseNumber(const std::wstring_view text, std::uint64_t& output)
{
    if (text.empty() || text.size() > 20)
    {
        return false;
    }
    std::uint64_t number = 0;
    for (const wchar_t character : text)
    {
        if (character < L'0' || character > L'9' || number > (UINT64_MAX - static_cast<unsigned int>(character - L'0')) / 10U)
        {
            return false;
        }
        number = number * 10U + static_cast<unsigned int>(character - L'0');
    }
    output = number;
    return true;
}
}

FeedbackStatus EncodeFeedbackIoMessage(const FeedbackIoMessage& message, std::vector<std::byte>& output)
{
    if (message.body.size() > maximumFeedbackSnapshotBytes)
    {
        return {FeedbackError::LimitExceeded, "Helper message exceeds bound"};
    }
    std::vector<std::byte> bytes(feedbackIoHeaderBytes + message.body.size());
    pbprotocol::ByteWriter writer(bytes);
    if (!writer.WriteFixedBytes(ioMagic) || !writer.WriteUint16(1) || !writer.WriteUint16(static_cast<std::uint16_t>(message.type)) ||
        !writer.WriteUint32(static_cast<std::uint32_t>(message.body.size())) || !writer.WriteUint32(static_cast<std::uint32_t>(message.error)) ||
        !writer.WriteUint32(message.nativeError) || !writer.WriteUint32(0) || !writer.WriteUint64(message.runToken) ||
        !writer.WriteFixedBytes(message.nonce) || !writer.WriteBytes(message.body))
    {
        return {FeedbackError::InvalidFormat, "Could not serialize helper message"};
    }
    output.swap(bytes);
    return {};
}

FeedbackStatus GetFeedbackIoMessageSize(const std::span<const std::byte> header, std::size_t& output) noexcept
{
    if (header.size() < feedbackIoHeaderBytes)
    {
        return {FeedbackError::InvalidFormat, "Truncated helper header"};
    }
    pbprotocol::ByteReader reader(header.first(feedbackIoHeaderBytes));
    const auto magic = reader.ReadFixedBytes<4>();
    const auto version = reader.ReadUint16();
    const auto type = reader.ReadUint16();
    const auto bodyBytes = reader.ReadUint32();
    const auto error = reader.ReadUint32();
    const auto nativeError = reader.ReadUint32();
    const auto reserved = reader.ReadUint32();
    if (!magic || !version || !type || !bodyBytes || !error || !nativeError || !reserved || magic.Value() != ioMagic ||
        version.Value() != 1 || type.Value() < 1 || type.Value() > 5 || bodyBytes.Value() > maximumFeedbackSnapshotBytes ||
        error.Value() > static_cast<std::uint32_t>(FeedbackError::Superseded) || reserved.Value() != 0)
    {
        return {FeedbackError::InvalidFormat, "Invalid helper version, kind or length"};
    }
    output = feedbackIoHeaderBytes + bodyBytes.Value();
    return {};
}

FeedbackStatus DecodeFeedbackIoMessage(const std::span<const std::byte> bytes, FeedbackIoMessage& output)
{
    std::size_t size = 0;
    const auto status = GetFeedbackIoMessageSize(bytes, size);
    if (!status || bytes.size() != size)
    {
        return {FeedbackError::InvalidFormat, "Invalid helper message length"};
    }
    pbprotocol::ByteReader reader(bytes);
    const auto prefix = reader.ReadBytes(6);
    const auto type = reader.ReadUint16();
    const auto bodyBytes = reader.ReadUint32();
    const auto error = reader.ReadUint32();
    const auto nativeError = reader.ReadUint32();
    const auto reserved = reader.ReadUint32();
    const auto token = reader.ReadUint64();
    const auto nonce = reader.ReadFixedBytes<16>();
    if (!prefix || !type || !bodyBytes || !error || !nativeError || !reserved || !token || !nonce)
    {
        return {FeedbackError::InvalidFormat, "Invalid helper message fields"};
    }
    FeedbackIoMessage message;
    message.type = static_cast<FeedbackIoMessageType>(type.Value());
    message.error = static_cast<FeedbackError>(error.Value());
    message.nativeError = nativeError.Value();
    message.runToken = token.Value();
    message.nonce = nonce.Value();
    message.body.assign(bytes.begin() + feedbackIoHeaderBytes, bytes.end());
    output = std::move(message);
    return {};
}

bool ParseFeedbackIoLaunch(const int argumentCount, wchar_t** const arguments, FeedbackIoLaunch& output)
{
    if (argumentCount != 6 || (std::wstring_view(arguments[1]) != L"--read" && std::wstring_view(arguments[1]) != L"--write"))
    {
        return false;
    }
    std::uint64_t handleNumber = 0;
    std::uint64_t token = 0;
    const std::wstring_view nonceText(arguments[5]);
    if (!ParseNumber(arguments[3], handleNumber) || handleNumber == 0 || handleNumber > UINTPTR_MAX ||
        !ParseNumber(arguments[4], token) || token == 0 || nonceText.size() != 32)
    {
        return false;
    }
    FeedbackIoLaunch launch;
    launch.writer = std::wstring_view(arguments[1]) == L"--write";
    launch.path = arguments[2];
    launch.pipe = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handleNumber));
    launch.runToken = token;
    for (std::size_t index = 0; index < 16; index++)
    {
        unsigned int value = 0;
        for (std::size_t digitIndex = 0; digitIndex < 2; digitIndex++)
        {
            const wchar_t character = nonceText[index * 2U + digitIndex];
            if (!((character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f')))
            {
                return false;
            }
            value = value * 16U + static_cast<unsigned int>(character <= L'9' ? character - L'0' : character - L'a' + 10);
        }
        launch.nonce[index] = static_cast<std::byte>(value);
    }
    if (GetFileType(launch.pipe) != FILE_TYPE_PIPE || std::ranges::all_of(launch.nonce, [](const auto value) { return value == std::byte{0}; }))
    {
        return false;
    }
    output = std::move(launch);
    return true;
}

bool WriteFeedbackIoMessage(const HANDLE pipe, const FeedbackIoMessage& message)
{
    std::vector<std::byte> bytes;
    if (!EncodeFeedbackIoMessage(message, bytes))
    {
        return false;
    }
    std::size_t position = 0;
    while (position < bytes.size())
    {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes.data() + position, static_cast<DWORD>(bytes.size() - position), &written, nullptr) || written == 0)
        {
            return false;
        }
        position += written;
    }
    return true;
}
}
