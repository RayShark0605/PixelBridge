#include "channel_matrix_core.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>

namespace
{

[[nodiscard]] bool WritePartialNoOverwrite(const std::filesystem::path& partialPath, const std::string& contents,
    std::string& error)
{
#if defined(_WIN32)
    const HANDLE file = CreateFileW(partialPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "cannot create new partial output file";
        return false;
    }
    bool success = true;
    std::size_t offset = 0;
    while (offset < contents.size())
    {
        const std::size_t remaining = contents.size() - offset;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(remaining,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (WriteFile(file, contents.data() + offset, requested, &written, nullptr) == FALSE || written == 0)
        {
            success = false;
            break;
        }
        offset += written;
    }
    if (success && FlushFileBuffers(file) == FALSE)
    {
        success = false;
    }
    if (CloseHandle(file) == FALSE)
    {
        success = false;
    }
    if (!success)
    {
        error = "cannot write and flush complete partial output file";
    }
    return success;
#else
    std::ofstream stream(partialPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!stream)
    {
        error = "cannot create partial output file";
        return false;
    }
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.flush();
    if (!stream)
    {
        error = "cannot write complete output file";
        return false;
    }
    return true;
#endif
}

[[nodiscard]] bool PublishNoOverwrite(const std::filesystem::path& outputPath, const std::string& contents,
    std::string& error)
{
    std::error_code filesystemError;
    if (std::filesystem::exists(outputPath, filesystemError) || filesystemError)
    {
        error = filesystemError ? "cannot inspect output path" : "output path already exists";
        return false;
    }
    std::filesystem::path partialPath = outputPath;
    partialPath += ".partial";
    if (std::filesystem::exists(partialPath, filesystemError) || filesystemError)
    {
        error = filesystemError ? "cannot inspect partial output path" : "partial output path already exists";
        return false;
    }
    if (!WritePartialNoOverwrite(partialPath, contents, error))
    {
        std::filesystem::remove(partialPath, filesystemError);
        return false;
    }
    std::filesystem::rename(partialPath, outputPath, filesystemError);
    if (filesystemError)
    {
        error = "cannot publish output file without overwrite";
        std::filesystem::remove(partialPath, filesystemError);
        return false;
    }
    return true;
}

int RunMain(const int argumentCount, char* arguments[])
{
    std::filesystem::path outputPath;
    if (argumentCount == 3 && std::string(arguments[1]) == "--output")
    {
        outputPath = arguments[2];
        if (outputPath.empty())
        {
            std::cerr << "usage: PBRemoteVisualChannelMatrix [--output <new-json-file>]\n";
            return 2;
        }
    }
    else if (argumentCount != 1)
    {
        std::cerr << "usage: PBRemoteVisualChannelMatrix [--output <new-json-file>]\n";
        return 2;
    }

    pbremotevisualmatrix::ChannelMatrixReport report;
    std::string error;
    if (!pbremotevisualmatrix::BuildDefaultChannelMatrix(report, error))
    {
        std::cerr << "[error] " << error << "\n";
        return 1;
    }
    if (outputPath.empty())
    {
        std::cout << report.canonicalJson;
    }
    else if (!PublishNoOverwrite(outputPath, report.canonicalJson, error))
    {
        std::cerr << "[error] " << error << ": " << outputPath.string() << "\n";
        return 2;
    }
    return report.truthBoundaryValid && report.expectationsMatched ? 0 : 1;
}

} // namespace

int main(const int argumentCount, char* arguments[])
{
    try
    {
        return RunMain(argumentCount, arguments);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[error] exception: " << exception.what() << "\n";
    }
    catch (...)
    {
        std::cerr << "[error] unknown exception\n";
    }
    return 1;
}
