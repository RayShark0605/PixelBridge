#pragma once

#include "screen_region_internal.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace screenregiongate
{
using namespace pbscreenregion;
using namespace pbscreenregion::detail;

inline void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

inline std::string Describe(const ScreenRegionStatus status)
{
    return std::string(GetScreenRegionErrorName(status.code)) + " stage=" + std::to_string(static_cast<unsigned int>(status.stage)) +
           " native=" + std::to_string(status.nativeError);
}

struct OracleMonitor
{
    HMONITOR monitor = nullptr;
    RECT rect{};
    UINT dpi = 0;
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
};

std::vector<OracleMonitor> ReadOracle();
void MovePointer(POINT point);
void MouseButton(DWORD flags);
void Escape();
RECT SmallRect(const OracleMonitor& monitor);

class RestorePointer
{
public:
    RestorePointer()
    {
        Require(GetPhysicalCursorPos(&original_) != FALSE, "cannot save physical cursor position");
    }
    ~RestorePointer()
    {
        if (!restored_)
        {
            (void)SetPhysicalCursorPos(original_.x, original_.y);
        }
    }
    RestorePointer(const RestorePointer&) = delete;
    RestorePointer& operator=(const RestorePointer&) = delete;
    void Restore()
    {
        Require(SetPhysicalCursorPos(original_.x, original_.y) != FALSE, "cannot restore physical cursor position");
        POINT actual{};
        // A successful positioning call is not an input-processing barrier.
        // Observe the exact saved point, without resending the warp or accepting
        // a tolerance; fail if it has not settled before the bounded deadline.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        do
        {
            Require(GetPhysicalCursorPos(&actual) != FALSE, "cannot verify physical cursor restoration");
            if (actual.x == original_.x && actual.y == original_.y)
            {
                restored_ = true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        RECT clip{};
        const BOOL clipped = GetClipCursor(&clip);
        throw std::runtime_error("physical cursor restoration mismatch: saved=" + std::to_string(original_.x) + "," + std::to_string(original_.y) +
                                 "; actual=" + std::to_string(actual.x) + "," + std::to_string(actual.y) + "; clip-valid=" + std::to_string(clipped) +
                                 "; clip=" + std::to_string(clip.left) + "," + std::to_string(clip.top) + "," + std::to_string(clip.right) + "," +
                                 std::to_string(clip.bottom));
    }

private:
    POINT original_{};
    bool restored_ = false;
};

class Evidence
{
public:
    Evidence(const std::filesystem::path& root, const char* name);
    void Note(const std::string& message);
    void Record(const char* event, const ScreenCaptureRegion& region);
    const std::filesystem::path& Directory() const noexcept
    {
        return directory_;
    }

private:
    std::filesystem::path directory_;
    std::ofstream notes_;
    std::ofstream records_;
    std::size_t count_ = 0;
};

void RunCliGate(const std::filesystem::path& decoder, Evidence& evidence);
void RunSupervisedNativeGate(const std::filesystem::path& evidenceRoot);

} // namespace screenregiongate
