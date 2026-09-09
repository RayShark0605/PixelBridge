#include "fixtures.h"
#include "candidate.h"
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

void WriteMeasuredDouble(const double value, const std::uint32_t measurements)
{
    if (measurements == 0)
    {
        std::cout << "null";
    }
    else
    {
        std::cout << value;
    }
}

bool HasObservationConflict(const pbmodulation::LocalDesktopObservation& continuous,
    const pbmodulation::LocalDesktopObservation& candidate, const std::span<const pbmodulation::UnifiedAcceptedBlock> baselineBlocks,
    const std::span<const pbmodulation::UnifiedAcceptedBlock> candidateBlocks)
{
    if (continuous.IsAccepted() && candidate.IsAccepted() && continuous.canonical44 != candidate.canonical44)
    {
        return true;
    }
    for (const auto& left : baselineBlocks)
    {
        for (const auto& right : candidateBlocks)
        {
            if (left.codewordSlot == right.codewordSlot && (left.kind != right.kind || left.size != right.size ||
                !std::equal(left.bytes.begin(), left.bytes.begin() + left.size, right.bytes.begin())))
            {
                return true;
            }
        }
    }
    return false;
}

std::pair<std::uint32_t, std::uint32_t> Observe(const char* const name, const pbmodulation::LumaView& view,
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
    const std::vector<UnifiedAcceptedBlock> baselineBlocks(oracle.GetAcceptedBlocks().begin(), oracle.GetAcceptedBlocks().end());
    const auto candidate = pbg1::ValidateCanonicalCandidate(view);
    const auto candidateDecoded = oracle.DecodeG1CanonicalFrame(view, expectedIdentity);
    const auto candidateBlocks = oracle.GetAcceptedBlocks();
    const auto VerifyBlocks = [reference](const std::span<const UnifiedAcceptedBlock> blocks)
    {
        for (const auto& block : blocks)
        {
            pbg1::Require(block.codewordSlot < kUnifiedCodewordCount && block.size <= block.bytes.size(), "Invalid accepted block bounds");
            if (reference != nullptr)
            {
                const auto& expected = reference->expectedBlocks[block.codewordSlot];
                pbg1::Require(block.size == expected.size() && std::equal(expected.begin(), expected.end(), block.bytes.begin()), "False accepted fixture payload");
            }
        }
    };
    VerifyBlocks(baselineBlocks);
    VerifyBlocks(candidateBlocks);
    const bool conflict = HasObservationConflict(bootstrap, candidate.bootstrap, baselineBlocks, candidateBlocks);
    pbg1::Require(!conflict, "Conflicting valid candidate interpretations; stop experiment");
    const bool identical = reference != nullptr && view.width == reference->frame.width && view.height == reference->frame.height &&
        std::ranges::equal(view.pixels, reference->frame.pixels);
    std::cout << "{\"type\":\"" << (reference == nullptr ? "recording" : "fixture") << "\",\"name\":\"" << name << "\",\"observation\":" << observation
        << ",\"pts\":" << pts << ",\"width\":" << view.width << ",\"height\":" << view.height
        << ",\"pixelsIdenticalToPristine\":" << identical << ",\"pixelBlake3\":\"" << PixelDigest(view.pixels)
        << "\",\"bootstrapAccepted\":" << bootstrap.IsAccepted() << ",\"geometryAccepted\":" << geometryAccepted << ",\"geometryFit\":";
    WriteGeometry(bootstrap.geometry);
    std::cout << ",\"fixedMatchesContinuous\":" << (fixed.IsAccepted() == bootstrap.IsAccepted() && fixed.geometry == bootstrap.geometry)
        << ",\"cpuBootstrapErasure\":" << static_cast<unsigned>(decoded.bootstrap.erasure) << ",\"cpuFrameErasure\":" << static_cast<unsigned>(decoded.frameErasure)
        << ",\"cpuAcceptedBlocks\":" << decoded.acceptedBlocks << ",\"baselinePayloads\":";
    WriteBlocks(baselineBlocks);
    std::cout << ",\"candidate\":{\"reason\":\"" << candidate.reason << "\",\"sharpEdges\":" << candidate.sharpEdges << ",\"crossingsMeasured\":" << candidate.crossingsMeasured
        << ",\"endpointsMeasured\":" << candidate.endpointsMeasured << ",\"originsMeasured\":" << candidate.originsMeasured << ",\"crossingRoundoff\":";
    WriteMeasuredDouble(candidate.maximumCrossingRoundoff, candidate.crossingsMeasured);
    std::cout << ",\"endpointResidual\":";
    WriteMeasuredDouble(candidate.maximumEndpointResidual, candidate.endpointsMeasured);
    std::cout << ",\"originResidual\":";
    WriteMeasuredDouble(candidate.maximumOriginResidual, candidate.originsMeasured);
    std::cout << ",\"bootstrapAccepted\":" << candidate.bootstrap.IsAccepted() << ",\"bootstrapErasure\":" << static_cast<unsigned>(candidate.bootstrap.erasure)
        << ",\"workUnits\":" << candidate.bootstrap.workUnits << ",\"frameErasure\":" << static_cast<unsigned>(candidateDecoded.frameErasure)
        << ",\"acceptedBlocks\":" << candidateDecoded.acceptedBlocks << ",\"payloads\":";
    WriteBlocks(candidateBlocks);
    std::cout << "},\"conflict\":" << conflict << ",\"newCandidateBlocksWhenBaselineEmpty\":" << (baselineBlocks.empty() ? candidateBlocks.size() : 0) << "}\n";
    return {decoded.acceptedBlocks, candidateDecoded.acceptedBlocks};
}

void CheckCandidateBoundaries(const pbg1::Fixture& fixture)
{
    using namespace pbmodulation;
    pbg1::Require(!pbg1::ValidateCanonicalCandidate({}).bootstrap.IsAccepted(), "Candidate accepted empty view");
    auto shortView = fixture.frame.View();
    shortView.pixels = shortView.pixels.first(10);
    pbg1::Require(!pbg1::ValidateCanonicalCandidate(shortView).bootstrap.IsAccepted(), "Candidate accepted short footprint");
    LocalDesktopDecodePolicy smallWork;
    smallWork.maximumWorkUnits = 1;
    const auto exhausted = pbg1::ValidateCanonicalCandidate(fixture.frame.View(), smallWork);
    pbg1::Require(!exhausted.bootstrap.IsAccepted() && exhausted.bootstrap.workUnits <= 1 && exhausted.bootstrap.quality == 0, "Candidate exceeded work budget");
    pbg1::Require(std::ranges::all_of(exhausted.bootstrap.canonical44, [](const auto value) { return value == std::byte{0}; }), "Candidate retained failed identity");
    LocalDesktopDecodePolicy markerBudget;
    markerBudget.maximumMarkers = 3;
    pbg1::Require(!pbg1::ValidateCanonicalCandidate(fixture.frame.View(), markerBudget).bootstrap.IsAccepted(), "Candidate exceeded marker budget");
    LocalDesktopDecodePolicy strictScale;
    strictScale.minimumScale = 1.125;
    pbg1::Require(!pbg1::ValidateCanonicalCandidate(fixture.frame.View(), strictScale).bootstrap.IsAccepted(), "Candidate ignored caller scale");
    LocalDesktopDecodePolicy invalid;
    invalid.maximumGeometryResidualPixels = 2;
    pbg1::Require(!pbg1::ValidateCanonicalCandidate(fixture.frame.View(), invalid).bootstrap.IsAccepted(), "Candidate accepted relaxed invalid policy");
    LocalDesktopGeometry resolved;
    LocalDesktopGeometry second;
    const auto positive = pbg1::ValidateCanonicalCandidate(fixture.frame.View());
    pbg1::Require(positive.bootstrap.IsAccepted() && positive.sharpEdges == 48, "Positive candidate not fully validated");
    pbg1::Require(ResolveUnifiedVisualSamplingGeometry(positive.bootstrap.geometry, 1920, 1080, {}, resolved) &&
        ResolveUnifiedVisualSamplingGeometry(resolved, 1920, 1080, {}, second) && resolved == second, "Canonical resolution not idempotent");
    auto conflicting = positive.bootstrap;
    conflicting.canonical44[0] ^= std::byte{1};
    pbg1::Require(HasObservationConflict(positive.bootstrap, conflicting, {}, {}), "Candidate identity conflict not rejected");
    UnifiedAcceptedBlock left;
    left.size = 1;
    UnifiedAcceptedBlock right = left;
    right.bytes[0] = std::byte{1};
    pbg1::Require(HasObservationConflict({}, {}, std::span(&left, 1), std::span(&right, 1)), "Candidate payload conflict not rejected");
    std::cout << "{\"type\":\"candidate-contract\",\"checks\":11,\"passed\":true}\n";
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
        const auto CheckFixture = [&fixture](const char* const name, const pbg1::PixelFrame& frame, const int baselineExpected, const int candidateExpected)
        {
            const auto result = Observe(name, frame.View(), &fixture);
            pbg1::Require(baselineExpected < 0 || result.first == static_cast<std::uint32_t>(baselineExpected), "Baseline fixture classification changed");
            pbg1::Require(candidateExpected < 0 || result.second == static_cast<std::uint32_t>(candidateExpected), "Candidate fixture classification failed");
        };
        CheckFixture("pristine-1x", fixture.frame, 15, 15);
        CheckFixture("intensity-plus-one", pbg1::OffsetIntensity(fixture.frame), 15, 15);
        CheckCandidateBoundaries(fixture);
        constexpr std::array<const char*, 4> cropNames{"crop-left", "crop-right", "crop-top", "crop-bottom"};
        constexpr std::array<const char*, 4> restoreNames{"crop-pad-same-left", "crop-pad-same-right", "crop-pad-same-top", "crop-pad-same-bottom"};
        constexpr std::array<const char*, 4> shiftNames{"crop-pad-opposite-left", "crop-pad-opposite-right", "crop-pad-opposite-top", "crop-pad-opposite-bottom"};
        for (std::uint32_t edge = 0; edge < 4; edge++)
        {
            const auto cropped = pbg1::CropEdge(fixture.frame, edge);
            CheckFixture(cropNames[edge], cropped, 0, 0);
            const auto equivalent = pbg1::PadEdge(cropped, edge);
            pbg1::Require(equivalent.pixels == fixture.frame.pixels, "Equivalent fixture pixels changed");
            CheckFixture(restoreNames[edge], equivalent, 15, 15);
            CheckFixture(shiftNames[edge], pbg1::PadEdge(cropped, edge ^ 1U), 0, 0);
        }
        CheckFixture("point-downscale-075", pbg1::DownscalePoint(fixture.frame), 0, 0);
        CheckFixture("blur-horizontal", pbg1::BlurAxis(fixture.frame, true), 15, 0);
        CheckFixture("blur-vertical", pbg1::BlurAxis(fixture.frame, false), 15, 0);
        const auto upscaled = pbg1::UpscalePoint(fixture.frame);
        CheckFixture("point-upscale-1125", upscaled, 15, 0);
        CheckFixture("point-half-edge-blur-horizontal", pbg1::BlurAxis(upscaled, true), 0, 0);
        CheckFixture("point-half-edge-blur-vertical", pbg1::BlurAxis(upscaled, false), 0, 0);
        CheckFixture("one-marker-edge-plus-one", pbg1::PerturbMarkerEdge(fixture.frame), -1, 0);
        const auto other = pbg1::BuildFixture(41);
        CheckFixture("bootstrap-identity-tear", pbg1::ReplaceRegion(fixture.frame, other.frame, pbmodulation::kLocalDesktopBootstrapRegions[1]), 0, 0);
        CheckFixture("bootstrap-erased", pbg1::EraseBootstrap(fixture.frame), 0, 0);
        CheckFixture("local-freshness-tear", pbg1::ReplaceRegion(fixture.frame, other.frame, pbmodulation::kLocalDesktopTimingRegions[0]), -1, -1);
        pbmodulation::UnifiedExpectedFrameIdentity conflictingIdentity;
        conflictingIdentity.requireFrameSequence = true;
        conflictingIdentity.frameSequence = 41;
        const auto conflict = Observe("expected-identity-conflict", fixture.frame.View(), &fixture, 0, 0, conflictingIdentity);
        pbg1::Require(conflict.first == 0 && conflict.second == 0, "Expected identity conflict not rejected");
        std::cout << "{\"type\":\"status\",\"baselineBoundaryChecks\":8,\"fixtureObservations\":25,\"productionChanged\":false,\"candidateImplemented\":true,\"candidateVariants\":1}\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
