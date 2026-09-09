#include "probe.h"
#include "support.h"
#include "recording_media.h"
#include "pbmodulation/unified_visual.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
extern "C"
{
#include <libavutil/mem.h>
}

namespace
{
using pbstep3b::Require;
using pbstep3b::RequireLocal;
using pbstep3b::Handle;

void WriteGeometry(std::ostream& stream, const pbmodulation::LocalDesktopGeometry& geometry)
{
    stream << '[' << geometry.originX << ',' << geometry.originY << ',' << geometry.scaleX << ',' << geometry.scaleY << ',' << geometry.markerResidualPixels << ']';
}

void Observe(std::ostream& stream, const pbapp::RecordedPixelFrame& frame, const bool raw, const std::uint32_t ordinal,
    const pbgeomprobe::Probe* const suppliedProbe = nullptr, const std::uint32_t candidateOrdinal = 0)
{
    const pbmodulation::LumaView view{frame.bgra, frame.width, frame.height, frame.rowPitch, pbmodulation::LumaPixelFormat::Bgra8};
    const auto probe = suppliedProbe ? *suppliedProbe : pbgeomprobe::Inspect(view);
    pbmodulation::LocalDesktopGeometry resolved{999, 998, 997, 996, 995};
    const auto untouched = resolved;
    const bool measured = std::string_view(probe.reason) == "MeasuredOnly_NoAdmissionCandidate";
    const bool accepted = measured && pbmodulation::ResolveUnifiedVisualSamplingGeometry(probe.finalGeometry, frame.width, frame.height, {}, resolved);
    Require(accepted || resolved == untouched, "Rejected resolver modified output");
    std::ostringstream row;
    row << std::boolalpha << std::setprecision(17) << "{\"schema\":\"PixelBridge.GeometryCodec.Sample.1\",\"kind\":\"" << (raw ? "raw" : "codec")
        << "\",\"ordinal\":" << ordinal << ",\"pts\":" << frame.pts << ",\"timeBase\":[" << frame.timeBaseNumerator << ',' << frame.timeBaseDenominator << "]"
        << ",\"pixelBlake3\":\"" << pbstep3b::Digest(frame.bgra) << "\",\"reason\":\"" << probe.reason << "\",\"roleCounts\":[";
    for (std::size_t index = 0; index < 4; index++)
    {
        row << (index == 0 ? "" : ",") << probe.roleCounts[index];
    }
    row << "],\"markers\":[";
    for (std::size_t index = 0; index < 4; index++)
    {
        row << (index == 0 ? "[" : ",[");
        for (std::size_t item = 0; item < probe.markers[index].size(); item++)
        {
            row << (item == 0 ? "" : ",") << probe.markers[index][item];
        }
        row << ']';
    }
    row << "],\"seed\":";
    WriteGeometry(row, probe.seed);
    row << ",\"iterations\":[";
    for (std::uint32_t index = 0; index < probe.iterationCount; index++)
    {
        const auto& iteration = probe.iterations[index];
        row << (index == 0 ? "" : ",") << "{\"input\":";
        WriteGeometry(row, iteration.input);
        row << ",\"fit\":";
        WriteGeometry(row, iteration.fitted);
        row << ",\"movement\":" << iteration.movement << ",\"edges\":[";
        for (std::size_t edgeIndex = 0; edgeIndex < iteration.edges.size(); edgeIndex++)
        {
            const auto& edge = iteration.edges[edgeIndex];
            Require(edge.measured, "Incomplete measured edge");
            row << (edgeIndex == 0 ? "[" : ",[") << edge.logical << ',' << edge.predicted << ',' << edge.perpendicular << ',' << edge.black << ',' << edge.white << ','
                << edge.crossing << ',' << edge.leftPosition << ',' << edge.rightPosition << ',' << edge.leftValue << ',' << edge.rightValue << ',' << edge.before << ',' << edge.after << ']';
        }
        row << "]}";
    }
    row << "],\"refined\":";
    WriteGeometry(row, probe.refined);
    row << ",\"finalGeometry\":";
    WriteGeometry(row, probe.finalGeometry);
    row << ",\"originalRefineMatched\":" << probe.originalRefineMatched << ",\"originalCoverageMatched\":" << probe.originalCoverageMatched
        << ",\"resolverAccepted\":" << accepted << ",\"resolvedGeometry\":";
    if (accepted)
    {
        WriteGeometry(row, resolved);
    }
    else
    {
        row << "null";
    }
    row << ",\"work\":{\"locate\":" << probe.locateWork << ",\"reference\":" << probe.referenceWork << ",\"fit\":" << probe.fitWork << ",\"edges\":" << probe.edgeWork
        << "},\"candidateOrdinal\":";
    if (suppliedProbe)
    {
        row << candidateOrdinal;
    }
    else
    {
        row << "null";
    }
    row << ",\"bootstrapEvaluation\":";
    if (probe.evaluationAttempted)
    {
        row << "{\"erasure\":" << static_cast<unsigned>(probe.evaluation.erasure) << ",\"canonicalBlake3\":\"" << pbstep3b::Digest(probe.evaluation.canonical44)
            << "\",\"copyFecDecoded\":[" << probe.evaluation.copies[0].fecDecoded << ',' << probe.evaluation.copies[1].fecDecoded << "]}";
    }
    else
    {
        row << "null";
    }
    row << ",\"bootstrapEvaluationPermitted\":" << (suppliedProbe != nullptr) << ",\"newPayloadFecOrReceiverCalls\":0,\"admissionCandidateImplemented\":false}\n";
    const auto data = row.str();
    Require(data.size() <= 65536, "Sample row byte limit");
    stream << data;
    stream.flush();
    Require(stream.good(), "Sample trace write failed");
    Require(measured || (suppliedProbe && probe.evaluationAttempted && !probe.evaluation.IsAccepted()), "Geometry probe stopped: " + std::string(probe.reason));
}

void RunAmbiguity(const std::filesystem::path& codecPath, const std::filesystem::path& root)
{
    RequireLocal(codecPath);
    RequireLocal(root);
    Require(std::filesystem::file_size(codecPath) <= 16ULL * 1024 * 1024, "Codec byte budget");
    Require(std::filesystem::create_directory(root), "New ambiguity output required");
    std::ofstream samples(root / "samples.jsonl", std::ios::binary);
    std::ofstream mediaTrace(root / "media-prefix.jsonl", std::ios::binary);
    Require(samples.good() && mediaTrace.good(), "Cannot create ambiguity traces");
    std::string error;
    auto media = pbstep2::RecordingMedia::Open(codecPath, error);
    Require(media != nullptr, "Original media open: " + error);
    for (std::uint32_t ordinal = 0; ordinal < 16; ordinal++)
    {
        pbapp::RecordedPixelFrame frame;
        const auto status = media->ReadNext(frame, error);
        Require(status == pbapp::RecordedPixelRead::Frame && error.empty(), "Original media prefix failed: " + error);
        Require(frame.width == 1920 && frame.height == 1080 && frame.rowPitch == 7680 && frame.bgra.size() == pbstep3b::kFrameBytes &&
            frame.pts == (static_cast<std::int64_t>(ordinal) * 1000 + 7) / 15 && frame.timeBaseNumerator == 1 && frame.timeBaseDenominator == 1000, "Media contract changed");
        mediaTrace << "{\"ordinal\":" << ordinal << ",\"pts\":" << frame.pts << ",\"pixelBlake3\":\"" << pbstep3b::Digest(frame.bgra) << "\"}\n";
        if (ordinal != 15)
        {
            continue;
        }
        const pbmodulation::LumaView view{frame.bgra, 1920, 1080, 7680, pbmodulation::LumaPixelFormat::Bgra8};
        std::array<pbgeomprobe::Probe, 2> probes{};
        for (std::uint32_t index = 0; index < probes.size(); index++)
        {
            probes[index] = pbgeomprobe::InspectCandidate(view, index);
            Observe(samples, frame, false, ordinal, &probes[index], index);
        }
        const pbmodulation::LocalDesktopBootstrapBinding binding{pbmodulation::kUnifiedVisualProfile.productProfile.visualProfileId, pbmodulation::kUnifiedVisualProfile.productProfile.visualLayoutVersion};
        const auto original = pbmodulation::DecodeLocalDesktopBootstrap(view, binding);
        Require(original.markerCandidates == 5 && original.geometryCandidates <= 2 && original.IsAccepted(), "Original bounded candidate decision differs");
        std::uint32_t acceptedCount = 0;
        Require(pbgeomprobe::SameSeed(probes[0].seed, probes[1].seed) && original.geometryCandidates == 1, "Original seed deduplication differs");
        for (std::size_t index = 0; index < probes.size(); index++)
        {
            const auto& probe = probes[index];
            if (index != 0 && pbgeomprobe::SameSeed(probes[0].seed, probe.seed))
            {
                continue;
            }
            if (probe.evaluationAttempted && probe.evaluation.IsAccepted())
            {
                acceptedCount++;
                Require(probe.evaluation.geometry == original.geometry && probe.evaluation.canonical44 == original.canonical44, "Candidate and original decision differ");
            }
        }
        Require(acceptedCount == 1, "Do not choose an ambiguous candidate");
        pbmodulation::LocalDesktopGeometry resolved;
        const bool geometryAccepted = pbmodulation::ResolveUnifiedVisualSamplingGeometry(original.geometry, 1920, 1080, {}, resolved);
        std::ostringstream summary;
        summary << std::boolalpha << std::setprecision(17) << "{\"schema\":\"PixelBridge.GeometryCodec.Ambiguity.1\",\"cpuPixelSamples\":1,\"decodedMediaFrames\":16,\"originalMarkerCandidates\":" << original.markerCandidates
            << ",\"originalGeometryCandidates\":" << original.geometryCandidates << ",\"genericBootstrapAccepted\":true,\"acceptedCandidateCount\":" << acceptedCount << ",\"originalGeometry\":";
        WriteGeometry(summary, original.geometry);
        summary << ",\"secondSeedDeduplicatedBeforeEvaluation\":true,\"bothStandaloneEvaluationsAreDiagnosticsOnly\":true,\"originalCanonicalBlake3\":\"" << pbstep3b::Digest(original.canonical44) << "\",\"resolverAccepted\":" << geometryAccepted
            << ",\"bootstrapCandidateEvaluationsUpperBound\":4,\"bootstrapRsDecodeCopiesUpperBound\":8,\"newPayloadFecOrReceiverCalls\":0,\"newWarpObservations\":0,\"intentionalPrefix\":true,\"eofClaimed\":false,\"media\":" << media->GetIdentityJson() << "}\n";
        pbstep3b::WriteNewText(root / "summary.json", summary.str());
    }
    mediaTrace.flush();
    Require(mediaTrace.good(), "Media trace flush failed");
    std::cout << "one additional pixel sample; original two-candidate Bootstrap decision only; no data FEC/Receiver/WARP\n";
}

void CheckSeedGuards(const std::filesystem::path& output)
{
    const pbmodulation::LocalDesktopGeometry first{-0.01315789473684248, 0, 1.0002741228070176, 1, 0.5};
    const pbmodulation::LocalDesktopGeometry second{0.01315789473684248, 0, 0.9997258771929824, 1, 0.5};
    Require(pbgeomprobe::SameSeed(first, second), "Recorded seeds must deduplicate");
    Require(pbgeomprobe::SameSeed(first, first), "Identical seed must deduplicate");
    const pbmodulation::LocalDesktopGeometry exact{0, 0, 1, 1, 0};
    const pbmodulation::LocalDesktopGeometry boundary{1.5, 0, 1, 1, 0};
    Require(!pbgeomprobe::SameSeed(exact, boundary), "Strict origin dedup boundary changed");
    const pbmodulation::LocalDesktopGeometry scaleOutside{0, 0, 1.0009765625, 1, 0};
    Require(!pbgeomprobe::SameSeed(exact, scaleOutside), "Scale-normalized dedup changed");
    pbstep3b::WriteNewText(output, "{\"schema\":\"PixelBridge.GeometryCodec.SeedGuards.1\",\"passed\":true,\"checks\":4,\"pixelSamples\":0,\"mediaFrames\":0,\"bootstrapFecCalls\":0,\"productPredicateUnchanged\":true}\n");
}

void Run(const std::filesystem::path& rawPath, const std::filesystem::path& codecPath, const std::filesystem::path& root)
{
    RequireLocal(rawPath);
    RequireLocal(codecPath);
    RequireLocal(root);
    Require(std::filesystem::file_size(codecPath) <= 16ULL * 1024 * 1024, "Codec byte budget");
    const Handle raw(CreateFileW(rawPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    Require(GetFileType(raw.Get()) == FILE_TYPE_DISK && GetFileSizeEx(raw.Get(), &size) && size.QuadPart == static_cast<LONGLONG>(pbstep3b::kSequenceBytes), "Exact sealed raw sequence required");
    Require(std::filesystem::create_directory(root), "New probe output directory required");
    std::ofstream samples(root / "samples.jsonl", std::ios::binary);
    std::ofstream mediaTrace(root / "media-prefix.jsonl", std::ios::binary);
    Require(samples.good() && mediaTrace.good(), "Cannot create probe traces");
    std::vector<std::byte> pixels(pbstep3b::kFrameBytes);
    for (const std::uint32_t ordinal : {0U, 1U, 15U})
    {
        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(ordinal) * pbstep3b::kFrameBytes;
        DWORD read = 0;
        Require(SetFilePointerEx(raw.Get(), offset, nullptr, FILE_BEGIN) && ReadFile(raw.Get(), pixels.data(), pbstep3b::kFrameBytes, &read, nullptr) && read == pbstep3b::kFrameBytes, "Raw sample read failed");
        Observe(samples, {pixels, 1920, 1080, 7680, ordinal, 1, 15, 1}, true, ordinal);
    }
    std::string error;
    auto media = pbstep2::RecordingMedia::Open(codecPath, error);
    Require(media != nullptr, "Original media open: " + error);
    for (std::uint32_t ordinal = 0; ordinal < 16; ordinal++)
    {
        pbapp::RecordedPixelFrame frame;
        const auto status = media->ReadNext(frame, error);
        Require(status == pbapp::RecordedPixelRead::Frame && error.empty(), "Original media prefix failed: " + error);
        Require(frame.width == 1920 && frame.height == 1080 && frame.rowPitch == 7680 && frame.bgra.size() == pbstep3b::kFrameBytes &&
            frame.pts == (static_cast<std::int64_t>(ordinal) * 1000 + 7) / 15 && frame.timeBaseNumerator == 1 && frame.timeBaseDenominator == 1000 &&
            frame.duration >= 66 && frame.duration <= 67, "Original media pixel/PTS contract differs");
        mediaTrace << "{\"ordinal\":" << ordinal << ",\"pts\":" << frame.pts << ",\"pixelBlake3\":\"" << pbstep3b::Digest(frame.bgra) << "\"}\n";
        if (ordinal == 0 || ordinal == 1 || ordinal == 15)
        {
            Observe(samples, frame, false, ordinal);
        }
    }
    mediaTrace.flush();
    Require(mediaTrace.good(), "Media trace flush failed");
    pbstep3b::WriteNewText(root / "summary.json", "{\"schema\":\"PixelBridge.GeometryCodec.Run.1\",\"samples\":6,\"decodedMediaFrames\":16,\"intentionalPrefix\":true,\"eofClaimed\":false,\"newWarpObservations\":0,\"newBootstrapFecCalls\":0,\"newPayloadFecOrReceiverCalls\":0,\"admissionCandidateImplemented\":false,\"fieldStatus\":\"NOT_RUN\",\"media\":" + media->GetIdentityJson() + "}\n");
    std::cout << "six geometry samples; sixteen media frames; no Bootstrap/payload FEC, Receiver or WARP\n";
}
}

int wmain(const int count, wchar_t** const arguments)
{
    try
    {
        const Handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = 512ULL * 1024 * 1024;
        Require(SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) && AssignProcessToJobObject(job.Get(), GetCurrentProcess()), "Mandatory 512 MiB process budget failed");
        av_max_alloc(32ULL * 1024 * 1024);
        if (count == 3 && std::wstring_view(arguments[1]) == L"--check-seed-guards")
        {
            CheckSeedGuards(std::filesystem::path(arguments[2]));
            return 0;
        }
        if (count == 4 && std::wstring_view(arguments[1]) == L"--ambiguity")
        {
            RunAmbiguity(std::filesystem::path(arguments[2]), std::filesystem::path(arguments[3]));
            return 0;
        }
        Require(count == 5 && std::wstring_view(arguments[1]) == L"--run", "Use --run <sealed-raw> <sealed-codec> <new-output>, or --ambiguity <sealed-codec> <new-output>");
        Run(std::filesystem::path(arguments[2]), std::filesystem::path(arguments[3]), std::filesystem::path(arguments[4]));
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
