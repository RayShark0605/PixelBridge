#include "fixtures.h"
#include "edge_probe.h"
#include "recording_media.h"
#include "pbprotocol/blake3_digest.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

extern "C"
{
#include <libavutil/mem.h>
}

namespace
{
class ProcessBudget
{
public:
    ProcessBudget() : job_(CreateJobObjectW(nullptr, nullptr))
    {
        pbg1::Require(job_ != nullptr, "Cannot create process budget");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = 512ULL * 1024 * 1024;
        if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) || !AssignProcessToJobObject(job_, GetCurrentProcess()))
        {
            CloseHandle(job_);
            job_ = nullptr;
            throw std::runtime_error("Cannot install process budget");
        }
    }
    ~ProcessBudget()
    {
        if (job_ != nullptr)
        {
            CloseHandle(job_);
        }
    }
    ProcessBudget(const ProcessBudget&) = delete;
    ProcessBudget& operator=(const ProcessBudget&) = delete;
private:
    HANDLE job_ = nullptr;
};

void WriteGeometry(const pbmodulation::LocalDesktopGeometry& geometry)
{
    std::cout << '[' << geometry.originX << ',' << geometry.originY << ',' << geometry.scaleX << ',' << geometry.scaleY << ',' << geometry.markerResidualPixels << ']';
}

std::string PixelDigest(const std::span<const std::byte> pixels)
{
    constexpr char digits[] = "0123456789abcdef";
    const auto digest = pbprotocol::ComputeBlake3Digest(pixels);
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto item : digest)
    {
        const auto value = std::to_integer<unsigned>(item);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15U]);
    }
    return result;
}

void CheckCurrentBoundary()
{
    constexpr std::array<pbmodulation::LocalDesktopGeometry, 8> inputs{{
        {0, 0, 1, 1, 0}, {0.00001, 0, 1, 1, 0}, {-0.00001, 0, 1, 1, 0}, {-0.00001, 0, 1.00000002, 1, 0},
        {0, 0, 0.99999999, 1, 0}, {1, 0, 1, 1, 0}, {0, 0, 1, 1, 0}, {0.00001, 0, 1, 1, 0}}};
    constexpr std::array<const char*, 8> names{"exact", "near-inside", "near-outside", "both-outside", "scale-only-near", "translated-one-pixel", "one-pixel-narrow-roi", "enlarged-roi"};
    constexpr std::array<bool, 8> baselineExpected{true, false, false, true, true, false, false, true};
    for (std::size_t index = 0; index < inputs.size(); index++)
    {
        pbmodulation::LocalDesktopGeometry output{999, 998, 997, 996, 995};
        const auto before = output;
        const auto width = index == 6 ? 1919U : index == 7 ? 2560U : 1920U;
        const auto height = index == 7 ? 1440U : 1080U;
        const bool accepted = pbmodulation::ResolveUnifiedVisualSamplingGeometry(inputs[index], width, height, {}, output);
        pbg1::Require(accepted == baselineExpected[index], "Existing boundary behavior changed");
        pbg1::Require(accepted || output == before, "Failed resolver mutated output");
        std::cout << "{\"type\":\"boundary\",\"name\":\"" << names[index] << "\",\"accepted\":" << accepted << ",\"input\":";
        WriteGeometry(inputs[index]);
        std::cout << ",\"output\":";
        WriteGeometry(output);
        std::cout << "}\n";
    }
}

void WriteBlocks(const std::span<const pbmodulation::UnifiedAcceptedBlock> blocks)
{
    std::cout << '[';
    bool first = true;
    for (const auto& block : blocks)
    {
        if (!first)
        {
            std::cout << ',';
        }
        first = false;
        std::cout << '[' << static_cast<unsigned>(block.codewordSlot) << ',' << static_cast<unsigned>(block.kind) << ',' << block.size
            << ",\"" << PixelDigest(std::span(block.bytes).first(block.size)) << "\"]";
    }
    std::cout << ']';
}

void WriteProbe(const pbg1b::EdgeProbe& probe)
{
    std::cout << "{\"reason\":\"" << probe.reason << "\",\"markersVerified\":" << probe.markersVerified
        << ",\"crossingsMeasured\":" << probe.crossingsMeasured << ",\"endpointsMeasured\":" << probe.endpointsMeasured
        << ",\"workUnits\":" << probe.workUnits << ",\"firstFit\":";
    if (probe.firstFitMeasured)
    {
        WriteGeometry(probe.firstFit);
    }
    else
    {
        std::cout << "null";
    }
    std::cout << ",\"markerResiduals\":[";
    for (std::size_t role = 0; role < probe.markerResiduals.size(); role++)
    {
        if (role != 0)
        {
            std::cout << ',';
        }
        if (role < probe.markersVerified)
        {
            std::cout << probe.markerResiduals[role];
        }
        else
        {
            std::cout << "null";
        }
    }
    // Fixed ordering: horizontal then vertical; TL/TR/BL/BR; six canonical edges.
    std::cout << "],\"edges\":[";
    for (std::size_t index = 0; index < probe.edges.size(); index++)
    {
        if (index != 0)
        {
            std::cout << ',';
        }
        const auto& edge = probe.edges[index];
        if (!edge.crossingMeasured)
        {
            std::cout << "null";
            continue;
        }
        std::cout << '[' << edge.logical << ',' << edge.crossing << ',' << edge.black << ',' << edge.white << ',';
        if (edge.endpointsMeasured)
        {
            std::cout << edge.before << ',' << edge.after;
        }
        else
        {
            std::cout << "null,null";
        }
        std::cout << ']';
    }
    std::cout << "]}";
}

std::uint32_t Observe(const char* const name, const pbmodulation::LumaView& view,
    const pbg1::Fixture* const reference, const std::uint32_t observation = 0, const std::int64_t pts = 0,
    const pbmodulation::UnifiedExpectedFrameIdentity& expectedIdentity = {})
{
    using namespace pbmodulation;
    const LocalDesktopBootstrapBinding binding{kUnifiedVisualProfile.productProfile.visualProfileId, kUnifiedVisualProfile.productProfile.visualLayoutVersion};
    const auto bootstrap = DecodeLocalDesktopBootstrap(view, binding);
    const auto fixed = DecodeLocalDesktopFixedCanvasBootstrap(view, binding);
    LocalDesktopGeometry geometry;
    const bool geometryAccepted = bootstrap.IsAccepted() && ResolveUnifiedVisualSamplingGeometry(bootstrap.geometry, view.width, view.height, {}, geometry);
    auto created = UnifiedVisualCpuOracle::Create(UnifiedVisualCpuOracle::RequiredBytes());
    pbg1::Require(static_cast<bool>(created), "CPU oracle allocation");
    auto oracle = std::move(created).Value();
    const auto decoded = oracle.DecodeMixedFrame(view, expectedIdentity);
    const auto baselineBlocks = oracle.GetAcceptedBlocks();
    for (const auto& block : baselineBlocks)
    {
        pbg1::Require(block.codewordSlot < kUnifiedCodewordCount && block.size <= block.bytes.size(), "Invalid accepted block bounds");
        if (reference != nullptr)
        {
            const auto& expected = reference->expectedBlocks[block.codewordSlot];
            pbg1::Require(block.size == expected.size() && std::equal(expected.begin(), expected.end(), block.bytes.begin()), "False accepted fixture payload");
        }
    }
    const bool identical = reference != nullptr && view.width == reference->frame.width && view.height == reference->frame.height &&
        std::ranges::equal(view.pixels, reference->frame.pixels);
    const auto probe = pbg1b::InspectCanonicalEdges(view);
    std::cout << "{\"type\":\"" << (reference == nullptr ? "recording" : "fixture") << "\",\"name\":\"" << name << "\",\"observation\":" << observation
        << ",\"pts\":" << pts << ",\"width\":" << view.width << ",\"height\":" << view.height
        << ",\"pixelsIdenticalToPristine\":" << identical << ",\"pixelBlake3\":\"" << PixelDigest(view.pixels)
        << "\",\"bootstrapAccepted\":" << bootstrap.IsAccepted() << ",\"geometryAccepted\":" << geometryAccepted << ",\"geometryFit\":";
    WriteGeometry(bootstrap.geometry);
    std::cout << ",\"fixedMatchesContinuous\":" << (fixed.IsAccepted() == bootstrap.IsAccepted() && fixed.geometry == bootstrap.geometry)
        << ",\"cpuBootstrapErasure\":" << static_cast<unsigned>(decoded.bootstrap.erasure) << ",\"cpuFrameErasure\":" << static_cast<unsigned>(decoded.frameErasure)
        << ",\"cpuAcceptedBlocks\":" << decoded.acceptedBlocks << ",\"baselinePayloads\":";
    WriteBlocks(baselineBlocks);
    std::cout << ",\"probe\":";
    WriteProbe(probe);
    std::cout << ",\"admissionCandidateImplemented\":false}\n";
    return decoded.acceptedBlocks;
}

void CheckProbeBoundaries(const pbg1::Fixture& fixture)
{
    using namespace pbmodulation;
    const auto empty = pbg1b::InspectCanonicalEdges({});
    pbg1::Require(std::string_view(empty.reason) == "InvalidView" && empty.crossingsMeasured == 0, "Empty view not rejected");
    auto shortView = fixture.frame.View();
    shortView.pixels = shortView.pixels.first(10);
    pbg1::Require(std::string_view(pbg1b::InspectCanonicalEdges(shortView).reason) == "InvalidView", "Short view not rejected");
    auto badPitch = fixture.frame.View();
    badPitch.rowPitch = 4;
    pbg1::Require(std::string_view(pbg1b::InspectCanonicalEdges(badPitch).reason) == "InvalidView", "Bad pitch not rejected");
    LocalDesktopDecodePolicy smallWork;
    smallWork.maximumWorkUnits = 1;
    const auto exhausted = pbg1b::InspectCanonicalEdges(fixture.frame.View(), smallWork);
    pbg1::Require(exhausted.workUnits <= 1 && exhausted.crossingsMeasured == 0 && !exhausted.firstFitMeasured, "Probe exceeded work budget");
    LocalDesktopDecodePolicy markerBudget;
    markerBudget.maximumMarkers = 3;
    pbg1::Require(std::string_view(pbg1b::InspectCanonicalEdges(fixture.frame.View(), markerBudget).reason) == "CallerPolicyExcludesHypothesis", "Marker budget ignored");
    LocalDesktopDecodePolicy strictScale;
    strictScale.minimumScale = 1.125;
    pbg1::Require(std::string_view(pbg1b::InspectCanonicalEdges(fixture.frame.View(), strictScale).reason) == "CallerPolicyExcludesHypothesis", "Caller scale ignored");
    LocalDesktopDecodePolicy invalid;
    invalid.maximumGeometryResidualPixels = 2;
    pbg1::Require(std::string_view(pbg1b::InspectCanonicalEdges(fixture.frame.View(), invalid).reason) == "InvalidPolicy", "Relaxed invalid policy accepted");
    const auto positive = pbg1b::InspectCanonicalEdges(fixture.frame.View());
    pbg1::Require(positive.crossingsMeasured == 48 && positive.endpointsMeasured == 48 && positive.markersVerified == 4 && positive.firstFitMeasured, "Positive probe incomplete");
    pbg1::Require(positive.firstFit == LocalDesktopGeometry{0, 0, 1, 1, 0}, "Pristine fit mismatch");
    for (const auto& edge : positive.edges)
    {
        pbg1::Require(edge.crossing == edge.logical, "Pristine crossing mismatch");
    }
    std::cout << "{\"type\":\"probe-contract\",\"checks\":10,\"passed\":true}\n";
}
void RunRecording(const wchar_t* const path)
{
    std::string error;
    auto media = pbstep2::RecordingMedia::Open(path, error);
    pbg1::Require(media != nullptr, error.c_str());
    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < 36; index++)
    {
        pbg1::Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(120), "Recording prefix time budget");
        pbapp::RecordedPixelFrame frame;
        pbg1::Require(media->ReadNext(frame, error) == pbapp::RecordedPixelRead::Frame, error.c_str());
        const pbmodulation::LumaView view{frame.bgra, frame.width, frame.height, frame.rowPitch, pbmodulation::LumaPixelFormat::Bgra8};
        Observe("recording-prefix", view, nullptr, index + 1, frame.pts);
    }
    std::cout << "{\"type\":\"media\",\"identity\":" << media->GetIdentityJson() << "}\n";
}
}

int wmain(const int count, wchar_t** const arguments)
{
    try
    {
        const ProcessBudget budget;
        av_max_alloc(32ULL * 1024 * 1024);
        std::cout << std::boolalpha << std::setprecision(17);
        if (count == 3 && std::wstring_view(arguments[1]) == L"--recording")
        {
            RunRecording(arguments[2]);
            return 0;
        }
        pbg1::Require(count == 1, "Use no arguments for fixtures or --recording with an explicit local path");
        CheckCurrentBoundary();
        const auto fixture = pbg1::BuildFixture(40);
        const auto CheckFixture = [&fixture](const char* const name, const pbg1::PixelFrame& frame, const int baselineExpected)
        {
            const auto result = Observe(name, frame.View(), &fixture);
            pbg1::Require(baselineExpected < 0 || result == static_cast<std::uint32_t>(baselineExpected), "Baseline fixture classification changed");
        };
        CheckFixture("pristine-1x", fixture.frame, 15);
        CheckFixture("intensity-plus-one", pbg1::OffsetIntensity(fixture.frame), 15);
        CheckProbeBoundaries(fixture);
        constexpr std::array<const char*, 4> cropNames{"crop-left", "crop-right", "crop-top", "crop-bottom"};
        constexpr std::array<const char*, 4> restoreNames{"crop-pad-same-left", "crop-pad-same-right", "crop-pad-same-top", "crop-pad-same-bottom"};
        constexpr std::array<const char*, 4> shiftNames{"crop-pad-opposite-left", "crop-pad-opposite-right", "crop-pad-opposite-top", "crop-pad-opposite-bottom"};
        for (std::uint32_t edge = 0; edge < 4; edge++)
        {
            const auto cropped = pbg1::CropEdge(fixture.frame, edge);
            CheckFixture(cropNames[edge], cropped, 0);
            const auto equivalent = pbg1::PadEdge(cropped, edge);
            pbg1::Require(equivalent.pixels == fixture.frame.pixels, "Equivalent fixture pixels changed");
            CheckFixture(restoreNames[edge], equivalent, 15);
            CheckFixture(shiftNames[edge], pbg1::PadEdge(cropped, edge ^ 1U), 0);
        }
        CheckFixture("point-downscale-075", pbg1::DownscalePoint(fixture.frame), 0);
        CheckFixture("blur-horizontal", pbg1::BlurAxis(fixture.frame, true), 15);
        CheckFixture("blur-vertical", pbg1::BlurAxis(fixture.frame, false), 15);
        const auto upscaled = pbg1::UpscalePoint(fixture.frame);
        CheckFixture("point-upscale-1125", upscaled, 15);
        CheckFixture("point-half-edge-blur-horizontal", pbg1::BlurAxis(upscaled, true), 0);
        CheckFixture("point-half-edge-blur-vertical", pbg1::BlurAxis(upscaled, false), 0);
        CheckFixture("one-marker-edge-plus-one", pbg1::PerturbMarkerEdge(fixture.frame), -1);
        const auto other = pbg1::BuildFixture(41);
        CheckFixture("bootstrap-identity-tear", pbg1::ReplaceRegion(fixture.frame, other.frame, pbmodulation::kLocalDesktopBootstrapRegions[1]), 0);
        CheckFixture("bootstrap-erased", pbg1::EraseBootstrap(fixture.frame), 0);
        CheckFixture("local-freshness-tear", pbg1::ReplaceRegion(fixture.frame, other.frame, pbmodulation::kLocalDesktopTimingRegions[0]), -1);
        pbmodulation::UnifiedExpectedFrameIdentity conflictingIdentity;
        conflictingIdentity.requireFrameSequence = true;
        conflictingIdentity.frameSequence = 41;
        const auto conflict = Observe("expected-identity-conflict", fixture.frame.View(), &fixture, 0, 0, conflictingIdentity);
        pbg1::Require(conflict == 0, "Expected identity conflict not rejected");
        std::cout << "{\"type\":\"status\",\"baselineBoundaryChecks\":8,\"fixtureObservations\":25,\"productionChanged\":false,\"candidateImplemented\":false,\"candidateVariants\":0}\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
