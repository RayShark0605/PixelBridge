#pragma once

#include "pbmodulation/unified_visual.h"
#include <Windows.h>
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace pbstep3a
{
inline constexpr std::uint32_t kFixtureFrames = 4;
inline constexpr std::size_t kSourceBytes = 512;
inline constexpr std::uint64_t kTransformSeed = 0x535445503341ULL;
inline constexpr std::uint64_t kMaximumTraceBytes = 1024ULL * 1024;

void Require(bool condition, const std::string& message);
[[nodiscard]] std::string Digest(std::span<const std::byte> bytes);
[[nodiscard]] std::string JsonString(const std::string& value);
void WriteNew(const std::filesystem::path& path, std::span<const std::byte> bytes);
void WriteNewText(const std::filesystem::path& path, const std::string& text);
void RequireLocalPath(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path FramePath(const std::filesystem::path& root, std::uint32_t index);

class Handle final
{
public:
    explicit Handle(HANDLE value) noexcept : value_(value)
    {
    }
    ~Handle();
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept
    {
        return value_;
    }
private:
    HANDLE value_;
};

class LockedRaster final
{
public:
    explicit LockedRaster(const std::filesystem::path& path);
    [[nodiscard]] std::vector<std::byte> Read();
private:
    Handle handle_;
};

class ProcessBudget final
{
public:
    ProcessBudget();
private:
    Handle job_;
};

void MakeFixture(const std::filesystem::path& root);
void RunSelfChecks();
}
