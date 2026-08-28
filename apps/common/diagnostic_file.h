#pragma once

#include <Windows.h>

#include <cstddef>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pbdiagnostic
{

inline void FlushDiagnosticOutput(std::ostream& output)
{
    output.flush();
    if (!output)
    {
        throw std::runtime_error("diagnostic stdout flush failed");
    }
}

// Application diagnostics only. Creation is exclusive, storage is bounded and
// Finish is explicit so flush/close errors cannot masquerade as a completed log.
class DiagnosticFile
{
public:
    static constexpr std::size_t maximumBytes = 16 * 1024 * 1024;

    explicit DiagnosticFile(const wchar_t* path)
    {
        if (path != nullptr)
        {
            handle_ = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ == INVALID_HANDLE_VALUE)
            {
                throw std::runtime_error("cannot create new telemetry file; win32=" + std::to_string(GetLastError()));
            }
        }
    }
    DiagnosticFile(const DiagnosticFile&) = delete;
    DiagnosticFile& operator=(const DiagnosticFile&) = delete;
    [[nodiscard]] bool IsEnabled() const noexcept
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }
    ~DiagnosticFile()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
        }
    }
    void Write(const std::string_view text)
    {
        if (handle_ == INVALID_HANDLE_VALUE)
        {
            return;
        }
        if (text.size() > maximumBytes - bytesWritten_)
        {
            throw std::runtime_error("telemetry reached its 16 MiB limit; stopping instead of growing an unbounded log");
        }
        DWORD written = 0;
        if (!WriteFile(handle_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) || written != text.size())
        {
            throw std::runtime_error("telemetry write failed");
        }
        bytesWritten_ += written;
    }
    void Finish()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            if (!FlushFileBuffers(handle_))
            {
                throw std::runtime_error("telemetry flush failed");
            }
            const HANDLE handle = handle_;
            handle_ = INVALID_HANDLE_VALUE;
            if (!CloseHandle(handle))
            {
                throw std::runtime_error("telemetry close failed");
            }
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::size_t bytesWritten_ = 0;
};

} // namespace pbdiagnostic
