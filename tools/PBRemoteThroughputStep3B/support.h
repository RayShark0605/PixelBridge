#pragma once
#include "pbprotocol/blake3_digest.h"
#include <Windows.h>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pbstep3b
{
inline constexpr std::uint32_t kFrameCount = 30;
inline constexpr std::uint32_t kFrameBytes = 1920 * 1080 * 4;
inline constexpr std::uint64_t kSequenceBytes = static_cast<std::uint64_t>(kFrameBytes) * kFrameCount;
inline constexpr std::uint64_t kSourceBytes = 65536;

inline void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class Handle
{
public:
    explicit Handle(const HANDLE value) : value_(value)
    {
        Require(value_ != nullptr && value_ != INVALID_HANDLE_VALUE, "Cannot obtain required local handle");
    }
    ~Handle()
    {
        CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept
    {
        return value_;
    }
private:
    HANDLE value_;
};

inline void RequireLocal(const std::filesystem::path& path)
{
    Require(path.is_absolute() && path.root_name().wstring().size() == 2 && GetDriveTypeW(path.root_path().c_str()) == DRIVE_FIXED, "Explicit fixed-drive path required");
    auto part = path;
    while (!part.empty())
    {
        const auto attributes = GetFileAttributesW(part.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            Require((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0, "Reparse path rejected");
        }
        const auto parent = part.parent_path();
        if (parent == part)
        {
            break;
        }
        part = parent;
    }
}

inline std::string Digest(const std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto item : pbprotocol::ComputeBlake3Digest(bytes))
    {
        const auto value = std::to_integer<unsigned>(item);
        result += digits[value >> 4];
        result += digits[value & 15];
    }
    return result;
}

inline std::string JsonString(const std::string& value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char character : value)
    {
        if (character == '"' || character == '\\')
        {
            result += '\\';
        }
        if (character < 32)
        {
            result += "\\u00";
            result += digits[character >> 4];
            result += digits[character & 15];
        }
        else
        {
            result += static_cast<char>(character);
        }
    }
    return result + '"';
}

inline void WriteExact(const HANDLE file, const std::span<const std::byte> bytes)
{
    Require(bytes.size() <= kFrameBytes, "Single artifact write limit");
    DWORD written = 0;
    Require(WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size(), "Artifact write failed");
}

inline void WriteNewText(const std::filesystem::path& path, const std::string& value)
{
    RequireLocal(path);
    Require(value.size() <= 1024 * 1024, "JSON size limit");
    const Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    WriteExact(file.Get(), std::as_bytes(std::span(value)));
    Require(FlushFileBuffers(file.Get()), "Artifact flush failed");
}

void MakeFixture(const std::filesystem::path& source, const std::filesystem::path& root);
}
