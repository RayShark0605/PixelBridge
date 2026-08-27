#pragma once

#include "pbrenderd3d/data_window.h"
#include "presentation_backend.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace presentationgate
{

inline void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

inline std::string Describe(const pbrenderd3d::PresentationStatus status)
{
    return std::string(pbrenderd3d::GetPresentationErrorName(status.code)) + ":" + pbrenderd3d::GetPresentationStageName(status.stage) + ":" +
           std::to_string(status.nativeError);
}

inline std::string Utf8(const std::wstring_view text)
{
    Require(!text.empty() && text.size() <= 8192, "diagnostic string length out of bounds");
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    Require(length > 0, "diagnostic UTF-8 sizing failed");
    std::string result(static_cast<std::size_t>(length), '\0');
    Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr) == length,
            "diagnostic UTF-8 conversion failed");
    return result;
}

class Handle
{
public:
    explicit Handle(const HANDLE value = nullptr) noexcept : value_(value)
    {
    }
    ~Handle()
    {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(value_);
        }
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

class Evidence
{
public:
    Evidence(const std::filesystem::path& root, const std::string& name);
    void Note(const std::string& text);
    void Record(const std::string& event, const pbrenderd3d::DataWindowSnapshot& snapshot);
    [[nodiscard]] const std::filesystem::path& Directory() const noexcept
    {
        return directory_;
    }

private:
    std::filesystem::path directory_;
    std::ofstream notes_;
    std::ofstream snapshots_;
    std::size_t count_ = 0;
};

struct Monitor
{
    HMONITOR handle = nullptr;
    MONITORINFOEXW info{};
};

[[nodiscard]] std::vector<Monitor> GetMonitors();
[[nodiscard]] pbrenderd3d::PhysicalPoint GetOrigin(const Monitor& monitor, std::uint32_t width, std::uint32_t height);
[[nodiscard]] std::unique_ptr<pbrenderd3d::DataWindow> CreateDataWindow(const pbrenderd3d::DataWindowConfig& config,
                                                                        const pbrenderd3d::NativeBackendTestOptions& options = {});
[[nodiscard]] std::vector<std::byte> MakePixelOracle(std::uint32_t width, std::uint32_t height, std::size_t rowPitch, unsigned int seed);
inline void StampSequence(const std::span<std::byte> pixels, const std::uint64_t sequence)
{
    Require(pixels.size() >= 12, "sequence stamp requires three BGRA pixels");
    for (std::size_t index = 0; index < 8; index++)
    {
        pixels[(index / 3) * 4 + index % 3] = static_cast<std::byte>((sequence >> (index * 8)) & 255);
    }
}
void PresentAndVerify(pbrenderd3d::DataWindow& window, const pbrenderd3d::DataWindowConfig& config, std::span<const std::byte> pixels, std::size_t pitch,
                      std::uint64_t sequence, Evidence& evidence);
void RunModeSupervisor(const std::filesystem::path& root, Evidence& evidence);
int RunModeChild(int argumentCount, wchar_t* arguments[]);

template <typename Predicate>
pbrenderd3d::DataWindowSnapshot WaitFor(pbrenderd3d::DataWindow& window, Predicate predicate, const char* description,
                                        const std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do
    {
        const auto snapshot = window.GetSnapshot();
        Require(snapshot.state != pbrenderd3d::WindowState::Failed, std::string(description) + " " + Describe(snapshot.error));
        Require(snapshot.state != pbrenderd3d::WindowState::Stopped, std::string(description) + " unexpectedly stopped");
        if (predicate(snapshot))
        {
            return snapshot;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(std::string("timeout: ") + description);
}

}
