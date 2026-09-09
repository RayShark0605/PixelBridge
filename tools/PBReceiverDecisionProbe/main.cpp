#include "support.h"
#include "recorded_pixel_replay.h"
#include "run_report.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace
{
class RawSource final : public pbapp::RecordedPixelSource
{
public:
    explicit RawSource(const std::filesystem::path& path)
        : file_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)), pixels_(pbstep3b::kFrameBytes)
    {
        LARGE_INTEGER size{};
        pbstep3b::Require(GetFileType(file_.Get()) == FILE_TYPE_DISK && GetFileSizeEx(file_.Get(), &size) &&
            size.QuadPart == static_cast<LONGLONG>(pbstep3b::kSequenceBytes), "Exactly 30 complete original BGRA frames required");
    }
    pbapp::RecordedPixelRead ReadNext(pbapp::RecordedPixelFrame& output, std::string& error) override
    {
        if (count_ == pbstep3b::kFrameCount)
        {
            return pbapp::RecordedPixelRead::EndOfFile;
        }
        DWORD read = 0;
        if (!ReadFile(file_.Get(), pixels_.data(), pbstep3b::kFrameBytes, &read, nullptr) || read != pbstep3b::kFrameBytes)
        {
            error = "Truncated raw sequence";
            return pbapp::RecordedPixelRead::Error;
        }
        output = {pixels_, 1920, 1080, 7680, count_, 1, 15, 1};
        count_++;
        return pbapp::RecordedPixelRead::Frame;
    }
private:
    pbstep3b::Handle file_;
    std::vector<std::byte> pixels_;
    std::uint32_t count_ = 0;
};
}

int wmain(const int count, wchar_t** const arguments)
{
    using namespace pbstep3b;
    try
    {
        Require(count == 4, "Use --on or --off <original-source.bgra> <new-output-root>");
        const std::wstring_view mode(arguments[1]);
        Require(mode == L"--on" || mode == L"--off", "Unknown diagnostic mode");
        const auto input = std::filesystem::absolute(arguments[2]);
        const auto root = std::filesystem::absolute(arguments[3]);
        RequireLocal(input);
        RequireLocal(root);
        const Handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = 2ULL * 1024 * 1024 * 1024;
        Require(SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
            AssignProcessToJobObject(job.Get(), GetCurrentProcess()), "Mandatory process budget unavailable");
        RawSource source(input);
        Require(std::filesystem::create_directory(root), "Output root must be new");
        std::ofstream trace(root / "frames.jsonl", std::ios::binary);
        Require(trace.good(), "Cannot open new trace");
        const auto diagnostics = std::make_shared<pbcore::StageDiagnostics>();
        const auto result = pbapp::RunRecordedPixelReplay(source, root / "output", trace, diagnostics, 31, mode == L"--on");
        std::ostringstream report;
        report << std::boolalpha << std::setprecision(17)
            << "{\"schema\":\"PixelBridge.ReceiverDecisionProbe.1\",\"classification\":\"SyntheticOfflineDiagnostic\",\"fieldStatus\":\"NOT_RUN\","
            << "\"processingDevice\":\"D3D11_WARP\",\"receiverDecisionDiagnostics\":" << (mode == L"--on")
            << ",\"liveChannelGoodput\":null,\"frames\":" << result.frames << ",\"reachedEof\":" << result.reachedEof
            << ",\"publishedAndReopened\":" << result.publishedAndReopened << ",\"processingMilliseconds\":" << result.processingMilliseconds
            << ",\"error\":" << JsonString(result.error) << ",\"receiverReport\":"
            << pbapp::BuildDecoderRunReportJson({"PBReceiverDecisionProbe", "decision-diagnostics-1", "working-tree-experiment", "unknown"}, result.decoder) << "}\n";
        WriteNewText(root / "summary.json", report.str());
        std::cout << "frames=" << result.frames << " eof=" << result.reachedEof << " published=" << result.publishedAndReopened << " error=" << result.error << '\n';
        return result.error.empty() && result.frames == 30 && result.reachedEof && result.publishedAndReopened ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
