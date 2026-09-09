#include "support.h"
#include "local_desktop_runtime.h"
#include "recorded_pixel_replay.h"
#include "recording_media.h"
#include "run_report.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <streambuf>
#include <vector>
extern "C"
{
#include <libavutil/mem.h>
}

namespace pbstep3b
{
namespace
{
class LimitedBuffer final : public std::streambuf
{
public:
    LimitedBuffer(std::streambuf* destination, const std::uint64_t limit) : destination_(destination), limit_(limit)
    {
    }
protected:
    std::streamsize xsputn(const char* bytes, const std::streamsize count) override
    {
        if (count < 0 || static_cast<std::uint64_t>(count) > limit_ - written_)
        {
            return 0;
        }
        const auto actual = destination_->sputn(bytes, count);
        if (actual > 0)
        {
            written_ += static_cast<std::uint64_t>(actual);
        }
        return actual;
    }
    int_type overflow(const int_type character) override
    {
        if (traits_type::eq_int_type(character, traits_type::eof()))
        {
            return traits_type::not_eof(character);
        }
        const char value = traits_type::to_char_type(character);
        return xsputn(&value, 1) == 1 ? character : traits_type::eof();
    }
    int sync() override
    {
        return destination_->pubsync();
    }
private:
    std::streambuf* destination_;
    std::uint64_t limit_;
    std::uint64_t written_ = 0;
};

class RawSource final : public pbapp::RecordedPixelSource
{
public:
    explicit RawSource(const std::filesystem::path& path) : file_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)), pixels_(kFrameBytes)
    {
        LARGE_INTEGER size{};
        Require(GetFileType(file_.Get()) == FILE_TYPE_DISK && GetFileSizeEx(file_.Get(), &size) && size.QuadPart == static_cast<LONGLONG>(kSequenceBytes), "Raw sequence must contain exactly 30 complete frames");
    }
    pbapp::RecordedPixelRead ReadNext(pbapp::RecordedPixelFrame& output, std::string& error) override
    {
        if (count_ == kFrameCount)
        {
            return pbapp::RecordedPixelRead::EndOfFile;
        }
        DWORD read = 0;
        if (!ReadFile(file_.Get(), pixels_.data(), kFrameBytes, &read, nullptr) || read != kFrameBytes)
        {
            error = "Truncated raw sequence";
            return pbapp::RecordedPixelRead::Error;
        }
        output = {pixels_, 1920, 1080, 7680, count_, 1, 15, 1};
        count_++;
        return pbapp::RecordedPixelRead::Frame;
    }
private:
    Handle file_;
    std::vector<std::byte> pixels_;
    std::uint32_t count_ = 0;
};

class AuditedSource final : public pbapp::RecordedPixelSource
{
public:
    AuditedSource(pbapp::RecordedPixelSource& source, std::ostream& audit, const bool raw) : source_(source), audit_(audit), raw_(raw)
    {
    }
    pbapp::RecordedPixelRead ReadNext(pbapp::RecordedPixelFrame& output, std::string& error) override
    {
        const auto result = source_.ReadNext(output, error);
        if (result == pbapp::RecordedPixelRead::EndOfFile)
        {
            if (count_ != kFrameCount)
            {
                error = "Media EOF before exact 30-frame boundary";
                return pbapp::RecordedPixelRead::Error;
            }
            return result;
        }
        if (result != pbapp::RecordedPixelRead::Frame)
        {
            return result;
        }
        const std::int64_t expectedPts = raw_ ? count_ : (static_cast<std::int64_t>(count_) * 1000 + 7) / 15;
        if (count_ >= kFrameCount || output.width != 1920 || output.height != 1080 || output.rowPitch != 7680 || output.bgra.size() != kFrameBytes ||
            output.pts != expectedPts || output.timeBaseNumerator != 1 || output.timeBaseDenominator != (raw_ ? 15 : 1000) ||
            (raw_ ? output.duration != 1 : output.duration < 66 || output.duration > 67))
        {
            error = "Fixed raster/frame/PTS/duration contract violated";
            return pbapp::RecordedPixelRead::Error;
        }
        audit_ << "{\"ordinal\":" << count_ << ",\"pts\":" << output.pts << ",\"duration\":" << output.duration
            << ",\"timeBaseNumerator\":1,\"timeBaseDenominator\":" << output.timeBaseDenominator << ",\"pixelBlake3\":\"" << Digest(output.bgra) << "\"}\n";
        if (!audit_.good())
        {
            error = "Pixel audit write failure";
            return pbapp::RecordedPixelRead::Error;
        }
        count_++;
        return result;
    }
private:
    pbapp::RecordedPixelSource& source_;
    std::ostream& audit_;
    bool raw_;
    std::uint32_t count_ = 0;
};

int Replay(const std::wstring_view mode, const std::filesystem::path& input, const std::filesystem::path& root)
{
    RequireLocal(input);
    RequireLocal(root);
    const bool raw = mode == L"--raw" || mode == L"--raw-prefix";
    const bool prefix = mode == L"--raw-prefix";
    const bool mediaOnly = mode == L"--media-check";
    Require(std::filesystem::file_size(input) <= (raw ? kSequenceBytes : 16ULL * 1024 * 1024), "Input resource limit");
    Require(std::filesystem::create_directory(root), "Replay root must be new");
    std::ofstream frames(root / "frames.jsonl", std::ios::binary);
    std::ofstream pixels(root / "pixels.jsonl", std::ios::binary);
    Require(frames.good() && pixels.good(), "Cannot create traces");
    LimitedBuffer frameBuffer(frames.rdbuf(), 4ULL * 1024 * 1024);
    LimitedBuffer pixelBuffer(pixels.rdbuf(), 1024 * 1024);
    std::ostream trace(&frameBuffer);
    std::ostream pixelTrace(&pixelBuffer);
    pbapp::RecordedPixelReplayResult result;
    const auto diagnostics = std::make_shared<pbcore::StageDiagnostics>();
    std::unique_ptr<pbapp::RecordedPixelSource> source;
    pbstep2::RecordingMedia* media = nullptr;
    try
    {
        if (raw)
        {
            source = std::make_unique<RawSource>(input);
        }
        else
        {
            auto recording = pbstep2::RecordingMedia::Open(input, result.error);
            media = recording.get();
            source = std::move(recording);
        }
        if (source)
        {
            AuditedSource audited(*source, pixelTrace, raw);
            if (mediaOnly)
            {
                for (;;)
                {
                    pbapp::RecordedPixelFrame frame;
                    const auto read = audited.ReadNext(frame, result.error);
                    if (read == pbapp::RecordedPixelRead::EndOfFile)
                    {
                        result.reachedEof = true;
                        break;
                    }
                    if (read == pbapp::RecordedPixelRead::Error)
                    {
                        break;
                    }
                    result.frames++;
                }
            }
            else
            {
                // Unchanged sealed Step2 owns WARP and every production admission,
                // conflict, resource, digest, publication and reopen decision.
                result = pbapp::RunRecordedPixelReplay(audited, root / "output", trace, diagnostics, prefix ? 3 : 31);
            }
        }
    }
    catch (const std::exception& exception)
    {
        result.error = exception.what();
    }
    trace.flush();
    pixelTrace.flush();
    if ((!trace.good() || !pixelTrace.good()) && result.error.empty())
    {
        result.error = "Trace flush failure";
    }
    std::ostringstream report;
    report << std::boolalpha << std::setprecision(17)
        << "{\"schema\":\"PixelBridge.Step3B.Run.1\",\"classification\":\"SyntheticOfflineDiagnostic\",\"fieldStatus\":\"NOT_RUN\",\"inputContract\":\"OfflinePixels\","
        << "\"processingDevice\":" << JsonString(mediaOnly ? "SoftwareMediaOnly_NoDemod" : "D3D11_WARP") << ",\"raw\":" << raw << ",\"intentionalPrefix\":" << prefix << ",\"mediaOnly\":" << mediaOnly
        << ",\"liveChannelGoodput\":null,\"simulatedVerifiedGoodput\":null,\"originalCaptureClock\":null,\"frames\":" << result.frames << ",\"reachedEof\":" << result.reachedEof
        << ",\"prefixLimitReached\":" << result.prefixLimitReached << ",\"publishedAndReopened\":" << result.publishedAndReopened << ",\"processingMilliseconds\":" << result.processingMilliseconds
        << ",\"ptsSpanSeconds\":" << result.timeline.SpanSeconds() << ",\"error\":" << JsonString(result.error) << ",\"media\":" << (media ? media->GetIdentityJson() : "null")
        << ",\"diagnostics\":" << pbcore::BuildStageDiagnosticsJson(diagnostics->GetSnapshot()) << ",\"receiverReport\":"
        << pbapp::BuildDecoderRunReportJson({"PBRemoteThroughputStep3B", "sealed-Step2", "tool-only", "unknown"}, result.decoder) << "}\n";
    WriteNewText(root / "summary.json", report.str());
    std::cout << "frames=" << result.frames << " eof=" << result.reachedEof << " published=" << result.publishedAndReopened << " error=" << result.error << '\n';
    return result.error.empty() ? 0 : 1;
}
}
}

int wmain(const int count, wchar_t** const arguments)
{
    using namespace pbstep3b;
    try
    {
        const Handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = 2ULL * 1024 * 1024 * 1024;
        Require(SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) && AssignProcessToJobObject(job.Get(), GetCurrentProcess()), "Mandatory process budget unavailable");
        av_max_alloc(32ULL * 1024 * 1024);
        Require(count == 4, "Use --audit, --make-fixture, --raw, --raw-prefix, --codec or --media-check <input> <new-output>");
        const std::wstring_view mode(arguments[1]);
        const auto input = std::filesystem::absolute(arguments[2]);
        const auto output = std::filesystem::absolute(arguments[3]);
        RequireLocal(input);
        RequireLocal(output);
        if (mode == L"--audit")
        {
            std::string ledger;
            const auto status = pbapp::AuditUnifiedSource(input.wstring(), ledger);
            Require(static_cast<bool>(status), status.message);
            WriteNewText(output, ledger + '\n');
            return 0;
        }
        if (mode == L"--make-fixture")
        {
            MakeFixture(input, output);
            return 0;
        }
        Require(mode == L"--raw" || mode == L"--raw-prefix" || mode == L"--codec" || mode == L"--media-check", "Unknown fixed mode");
        return Replay(mode, input, output);
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
