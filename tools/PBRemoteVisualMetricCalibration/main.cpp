#include "calibration_pipeline.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace
{

struct Options
{
    pbremotevisualmetriccalibration::CalibrationPipelineInput pipeline;
    std::filesystem::path outputPath;
};

[[nodiscard]] bool ToUtf8(const std::wstring_view value, std::string& output) noexcept
{
    if (value.empty() || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return false;
    }
    const int sourceLength = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceLength,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        return false;
    }
    try
    {
        std::string converted(static_cast<std::size_t>(required), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceLength,
            converted.data(), required, nullptr, nullptr) != required)
        {
            return false;
        }
        output = std::move(converted);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

[[nodiscard]] bool ParseSplit(const std::wstring_view value,
    pbremotevisualmetriccalibration::DatasetSplit& output) noexcept
{
    if (value == L"train")
    {
        output = pbremotevisualmetriccalibration::DatasetSplit::Train;
        return true;
    }
    if (value == L"validation")
    {
        output = pbremotevisualmetriccalibration::DatasetSplit::Validation;
        return true;
    }
    if (value == L"holdout")
    {
        output = pbremotevisualmetriccalibration::DatasetSplit::Holdout;
        return true;
    }
    return false;
}

[[nodiscard]] bool ParseOptions(const int argumentCount, const wchar_t* const arguments[],
    Options& output)
{
    if (argumentCount < 2 || std::wstring_view(arguments[1]) != L"build-report")
    {
        return false;
    }
    Options options;
    bool hasRealDataset = false;
    bool hasRealReplay = false;
    bool hasRealReplayDigest = false;
    bool hasPresenterDigest = false;
    bool hasOutput = false;
    for (int index = 2; index < argumentCount; index++)
    {
        const std::wstring_view option(arguments[index]);
        const auto nextArgument = [&]() -> const wchar_t*
        {
            index++;
            return index < argumentCount ? arguments[index] : nullptr;
        };
        if (option == L"--codec")
        {
            const wchar_t* const split = nextArgument();
            const wchar_t* const signalProfile = nextArgument();
            const wchar_t* const runId = nextArgument();
            const wchar_t* const path = nextArgument();
            const wchar_t* const digest = nextArgument();
            pbremotevisualmetriccalibration::CodecSequenceInput sequence;
            if (split == nullptr || signalProfile == nullptr || runId == nullptr || path == nullptr ||
                digest == nullptr || !ParseSplit(split, sequence.split) ||
                !ToUtf8(signalProfile, sequence.signalProfile) || !ToUtf8(runId, sequence.runId) ||
                !ToUtf8(digest, sequence.expectedBlake3))
            {
                return false;
            }
            sequence.gray8Path = path;
            options.pipeline.codecSequences.push_back(std::move(sequence));
        }
        else if (option == L"--real-collection-dataset")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || hasRealDataset ||
                !ToUtf8(value, options.pipeline.realReplay.collectionDatasetId))
            {
                return false;
            }
            hasRealDataset = true;
        }
        else if (option == L"--real-replay")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || hasRealReplay)
            {
                return false;
            }
            options.pipeline.realReplay.replayPath = value;
            hasRealReplay = true;
        }
        else if (option == L"--real-replay-blake3")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || hasRealReplayDigest ||
                !ToUtf8(value, options.pipeline.realReplay.expectedReplayBlake3))
            {
                return false;
            }
            hasRealReplayDigest = true;
        }
        else if (option == L"--presenter-raster-blake3")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || hasPresenterDigest ||
                !ToUtf8(value, options.pipeline.realReplay.expectedPresenterRasterBlake3))
            {
                return false;
            }
            hasPresenterDigest = true;
        }
        else if (option == L"--output")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || hasOutput)
            {
                return false;
            }
            options.outputPath = value;
            hasOutput = true;
        }
        else
        {
            return false;
        }
    }
    if (options.pipeline.codecSequences.empty() || !hasRealDataset ||
        !hasRealReplay || !hasRealReplayDigest || !hasPresenterDigest || !hasOutput ||
        options.outputPath.empty())
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
    std::cerr << "usage: PBRemoteVisualMetricCalibration build-report "
        "--codec <train|validation|holdout> SIGNAL_PROFILE RUN_ID GRAY8 BLAKE3 [--codec ...] "
        "--real-collection-dataset DATASET_ID --real-replay SEALED.pbrv2 "
        "--real-replay-blake3 BLAKE3 --presenter-raster-blake3 BLAKE3 --output NEW.json\n";
}

int RunMain(const int argumentCount, const wchar_t* const arguments[])
{
    Options options;
    if (!ParseOptions(argumentCount, arguments, options))
    {
        Usage();
        return 2;
    }
    pbremotevisualmetriccalibration::CalibrationPipelineReport report;
    std::string error;
    if (!pbremotevisualmetriccalibration::BuildCalibrationEvidence(options.pipeline, report, error))
    {
        std::cerr << "[error] " << error << '\n';
        return 1;
    }
    if (!PublishNew(options.outputPath, report.canonicalJson, error))
    {
        std::cerr << "[error] " << error << '\n';
        return 1;
    }
    std::cout << "Step-07 metric calibration evidence published: selected=" <<
        report.calibration.selectedCandidateId << " gate=" <<
        (report.calibration.gatePassed ? "PASS" : "FAIL") << '\n';
    return report.calibration.gatePassed ? 0 : 3;
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
