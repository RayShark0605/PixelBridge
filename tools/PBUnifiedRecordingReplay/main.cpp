#include "recording_media.h"
#include "recorded_pixel_replay.h"
#include "run_report.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

extern "C"
{
#include <libavutil/mem.h>
}

namespace pbstep2
{
void MakeRecordingFixture(const std::filesystem::path& root);
}

namespace
{
std::string JsonString(const std::string& value)
{
    std::string output = "\"";
    for (const unsigned char character : value)
    {
        if (character == '\\' || character == '"')
        {
            output += '\\';
        }
        if (character >= 32)
        {
            output += static_cast<char>(character);
        }
        else
        {
            output += '?';
        }
    }
    return output + '"';
}

class ProcessBudget
{
public:
    explicit ProcessBudget(const bool fixtureProducer = false)
    {
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = 2ULL * 1024 * 1024 * 1024;
        if (job_ == nullptr || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job_, GetCurrentProcess()))
        {
            if (job_ != nullptr)
            {
                CloseHandle(job_);
                job_ = nullptr;
            }
            throw std::runtime_error("Cannot establish mandatory 2 GiB process commit limit");
        }
        // Per-allocation limit is supplementary, not an aggregate codec cap.
        av_max_alloc((fixtureProducer ? 192ULL : 32ULL) * 1024 * 1024);
    }
    ~ProcessBudget()
    {
        if (job_ != nullptr)
        {
            CloseHandle(job_);
        }
    }

private:
    HANDLE job_ = nullptr;
};
} // namespace

int wmain(const int argumentCount, wchar_t** const arguments)
{
    try
    {
        if (argumentCount == 3 && std::wstring_view(arguments[1]) == L"--make-fixture")
        {
            const ProcessBudget budget(true);
            pbstep2::MakeRecordingFixture(std::filesystem::absolute(arguments[2]));
            return 0;
        }
        if (argumentCount != 4 || (std::wstring_view(arguments[1]) != L"--replay" && std::wstring_view(arguments[1]) != L"--replay-no-diagnostics" &&
                                      std::wstring_view(arguments[1]) != L"--replay-prefix"))
        {
            std::cerr << "Usage: PBUnifiedRecordingReplay --replay <local.mkv> <new-evidence-directory>\n";
            return 2;
        }
        const ProcessBudget budget;
        const auto root = std::filesystem::absolute(arguments[3]);
        if (!std::filesystem::create_directory(root))
        {
            throw std::runtime_error("Evidence root must be new");
        }
        std::ofstream trace(root / "frames.jsonl", std::ios::binary);
        if (!trace)
        {
            throw std::runtime_error("Cannot create trace");
        }
        std::string error;
        auto media = pbstep2::RecordingMedia::Open(std::filesystem::absolute(arguments[2]), error);
        auto diagnostics = std::wstring_view(arguments[1]) != L"--replay-no-diagnostics" ? std::make_shared<pbcore::StageDiagnostics>() : nullptr;
        pbapp::RecordedPixelReplayResult result;
        if (media)
        {
            result = pbapp::RunRecordedPixelReplay(*media, root / "output", trace, diagnostics, std::wstring_view(arguments[1]) == L"--replay-prefix" ? 36 : 7200);
        }
        else
        {
            result.error = error;
        }
        std::ofstream report(root / "summary.json", std::ios::binary);
        report << "{\"schema\":\"PixelBridge.RecordingReplay.1\",\"classification\":\"OfflineRecordingDiagnostic\","
               << "\"fieldStatus\":\"NOT_RUN\",\"inputContract\":\"OfflinePixels\",\"processingDevice\":\"D3D11_WARP\","
               << "\"originalCaptureBackend\":null,\"originalCursorState\":null,\"originalCaptureClock\":null,"
               << "\"liveChannelGoodput\":null,\"falseAcceptanceRate\":null,\"processCommitLimitBytes\":2147483648,"
               << "\"frames\":" << result.frames << ",\"reachedEof\":" << (result.reachedEof ? "true" : "false")
               << ",\"prefixLimitReached\":" << (result.prefixLimitReached ? "true" : "false")
               << ",\"publishedAndReopened\":" << (result.publishedAndReopened ? "true" : "false") << ",\"processingMilliseconds\":" << result.processingMilliseconds
               << ",\"ptsSpanSeconds\":" << result.timeline.SpanSeconds() << ",\"error\":" << JsonString(result.error)
               << ",\"media\":" << (media ? media->GetIdentityJson() : "null")
               << ",\"diagnostics\":" << (diagnostics ? pbcore::BuildStageDiagnosticsJson(diagnostics->GetSnapshot()) : "null")
               << ",\"receiverReport\":" << pbapp::BuildDecoderRunReportJson({"PBUnifiedRecordingReplay", "Step2-experiment", "unsealed", "unknown"}, result.decoder)
               << "}\n";
        report.flush();
        if (!report.good())
        {
            throw std::runtime_error("Cannot finish report");
        }
        std::cout << "frames=" << result.frames << " eof=" << result.reachedEof << " publishedAndReopened=" << result.publishedAndReopened << " error=" << result.error
                  << '\n';
        return result.error.empty() ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
