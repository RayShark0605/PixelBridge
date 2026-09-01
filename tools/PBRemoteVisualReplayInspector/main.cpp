#include "replay_inspector_core.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <io.h>

namespace
{

struct Options
{
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    std::uint32_t maximumFrames = pbrealcapturereplay::kReplayV2DefaultMaximumCaptureFrames;
    std::uint32_t maximumMebibytes = 2048;
};

[[nodiscard]] bool ParseUnsigned(const std::wstring_view text, std::uint32_t& output) noexcept
{
    if (text.empty())
    {
        return false;
    }
    std::uint64_t value = 0;
    for (const wchar_t character : text)
    {
        if (character < L'0' || character > L'9')
        {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - L'0');
        if (value > ((std::numeric_limits<std::uint32_t>::max)() - digit) / 10ULL)
        {
            return false;
        }
        value = value * 10ULL + digit;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

[[nodiscard]] bool ParseOptions(const int argumentCount, const wchar_t* const arguments[], Options& output)
{
    Options options;
    for (int index = 1; index < argumentCount; index++)
    {
        const std::wstring_view option(arguments[index]);
        const auto nextArgument = [&]() -> const wchar_t*
        {
            index++;
            return index < argumentCount ? arguments[index] : nullptr;
        };
        if (option == L"--input")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || options.inputPath != std::filesystem::path{})
            {
                return false;
            }
            options.inputPath = value;
        }
        else if (option == L"--output")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || options.outputPath != std::filesystem::path{})
            {
                return false;
            }
            options.outputPath = value;
        }
        else if (option == L"--max-frames")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || !ParseUnsigned(value, options.maximumFrames) ||
                options.maximumFrames == 0 ||
                options.maximumFrames > pbrealcapturereplay::kReplayV2HardMaximumCaptureFrames)
            {
                return false;
            }
        }
        else if (option == L"--max-mib")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || !ParseUnsigned(value, options.maximumMebibytes) ||
                options.maximumMebibytes < 16 || options.maximumMebibytes > 16384)
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }
    if (options.inputPath.empty())
    {
        return false;
    }
    output = std::move(options);
    return true;
}

[[nodiscard]] std::string DescribeWindowsError(const DWORD error)
{
    return "Windows error " + std::to_string(error);
}

[[nodiscard]] bool WriteAll(const HANDLE file, const std::string_view contents,
    std::string& error) noexcept
{
    std::size_t offset = 0;
    while (offset < contents.size())
    {
        const std::size_t remaining = contents.size() - offset;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(remaining,
            (std::numeric_limits<DWORD>::max)()));
        DWORD written = 0;
        if (WriteFile(file, contents.data() + offset, request, &written, nullptr) == FALSE ||
            written != request)
        {
            error = DescribeWindowsError(GetLastError());
            return false;
        }
        offset += written;
    }
    return true;
}

[[nodiscard]] bool PublishNew(const std::filesystem::path& path,
    const std::string_view contents, std::string& error) noexcept
{
    try
    {
        std::filesystem::path partialPath = path;
        partialPath += L".partial";
        const HANDLE file = CreateFileW(partialPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = "cannot create output .partial with CREATE_NEW: " + DescribeWindowsError(GetLastError());
            return false;
        }
        bool success = WriteAll(file, contents, error);
        if (success && FlushFileBuffers(file) == FALSE)
        {
            error = "cannot flush output .partial: " + DescribeWindowsError(GetLastError());
            success = false;
        }
        if (CloseHandle(file) == FALSE && success)
        {
            error = "cannot close output .partial: " + DescribeWindowsError(GetLastError());
            success = false;
        }
        if (success && MoveFileExW(partialPath.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE)
        {
            error = "cannot publish create-only output: " + DescribeWindowsError(GetLastError());
            success = false;
        }
        if (!success)
        {
            DeleteFileW(partialPath.c_str());
        }
        return success;
    }
    catch (...)
    {
        error = "invalid output path";
        return false;
    }
}

void Usage()
{
    std::cerr << "usage: PBRemoteVisualReplayInspector --input SEALED.pbrv2 "
        "[--output NEW.json] [--max-frames 1..2048] [--max-mib 16..16384]\n";
}

int RunMain(const int argumentCount, const wchar_t* const arguments[])
{
    Options options;
    if (!ParseOptions(argumentCount, arguments, options))
    {
        Usage();
        return 2;
    }
    pbrealcapturereplay::ReplayV2Limits limits;
    limits.maximumCaptureFrames = options.maximumFrames;
    limits.maximumFileBytes = static_cast<std::uint64_t>(options.maximumMebibytes) * 1024ULL * 1024ULL;
    limits.maximumTotalRasterBytes = limits.maximumFileBytes;
    pbremotevisualreplayinspector::ReplayInspection inspection;
    std::string error;
    if (!pbremotevisualreplayinspector::InspectReplay(options.inputPath, limits, inspection, error))
    {
        std::cerr << "[error] " << error << '\n';
        return 1;
    }
    if (!options.outputPath.empty())
    {
        if (!PublishNew(options.outputPath, inspection.canonicalJson, error))
        {
            std::cerr << "[error] " << error << '\n';
            return 1;
        }
        std::cout << "Replay inspection published: captures=" << inspection.captureFrames <<
            " bootstrapAccepted=" << inspection.bootstrapAcceptedFrames <<
            " transportAcceptedFrames=" << inspection.transportAcceptedFrames << '\n';
        return 0;
    }
    if (_setmode(_fileno(stdout), _O_BINARY) == -1)
    {
        std::cerr << "[error] cannot set stdout binary mode\n";
        return 1;
    }
    std::cout.write(inspection.canonicalJson.data(),
        static_cast<std::streamsize>(inspection.canonicalJson.size()));
    std::cout.flush();
    return std::cout ? 0 : 1;
}

} // namespace

int wmain(const int argumentCount, const wchar_t* const arguments[])
{
    try
    {
        return RunMain(argumentCount, arguments);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[error] exception: " << exception.what() << '\n';
    }
    catch (...)
    {
        std::cerr << "[error] unknown exception\n";
    }
    return 1;
}
