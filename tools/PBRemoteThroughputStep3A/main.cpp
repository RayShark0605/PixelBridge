#include "scenario_source.h"
#include "recorded_pixel_replay.h"
#include "run_report.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace
{
int RunCase(const std::filesystem::path& fixture, const pbstep3a::Scenario scenario, const std::filesystem::path& root)
{
    using namespace pbstep3a;
    RequireLocalPath(fixture);
    RequireLocalPath(root);
    Require(std::filesystem::create_directory(root), "Case evidence root must be new");
    std::ofstream trace(root / "frames.jsonl", std::ios::binary);
    std::ofstream transforms(root / "transforms.jsonl", std::ios::binary);
    Require(trace.good() && transforms.good(), "Cannot create case traces");
    pbapp::RecordedPixelReplayResult result;
    const auto diagnostics = std::make_shared<pbcore::StageDiagnostics>();
    try
    {
        ScenarioSource source(fixture, scenario, transforms);
        // The unchanged Step2 implementation owns WARP, all admission and the
        // production ReceiverPipeline. It receives no expected source/payload.
        result = pbapp::RunRecordedPixelReplay(source, root / "output", trace, diagnostics, 16);
    }
    catch (const std::exception& error)
    {
        result.error = error.what();
    }
    trace.flush();
    transforms.flush();
    if ((!trace.good() || !transforms.good()) && result.error.empty())
    {
        result.error = "Cannot flush evidence traces";
    }
    std::ostringstream summary;
    summary << std::boolalpha << std::setprecision(17)
        << "{\"schema\":\"PixelBridge.Step3A.Run.1\",\"classification\":\"SyntheticOfflineDiagnostic\",\"fieldStatus\":\"NOT_RUN\","
        << "\"scenario\":\"" << ScenarioName(scenario) << "\",\"inputContract\":\"OfflinePixels\",\"processingDevice\":\"D3D11_WARP\","
        << "\"originalCaptureBackend\":null,\"originalCursorState\":null,\"originalCaptureClock\":null,\"codec\":null,\"bitrate\":null,"
        << "\"liveChannelGoodput\":null,\"simulatedVerifiedGoodput\":null,\"temporalModel\":\"FrozenOrdinal15Hz_ObservationTicks30Hz_NotMeasuredSenderTime\","
        << "\"frames\":" << result.frames << ",\"expectedObservations\":" << ObservationCount(scenario) << ",\"reachedEof\":" << result.reachedEof
        << ",\"prefixLimitReached\":" << result.prefixLimitReached << ",\"publishedAndReopened\":" << result.publishedAndReopened
        << ",\"processingMilliseconds\":" << result.processingMilliseconds << ",\"syntheticPtsSpanSeconds\":" << result.timeline.SpanSeconds()
        << ",\"error\":" << JsonString(result.error) << ",\"knownGeometryFailure\":" << (scenario == Scenario::MarkerPlusOne)
        << ",\"stageTimingNote\":\"mediaDecode includes synthetic input read and transform, not codec decode; stage durations are inclusive\","
        << "\"diagnostics\":" << pbcore::BuildStageDiagnosticsJson(diagnostics->GetSnapshot())
        << ",\"receiverReport\":" << pbapp::BuildDecoderRunReportJson({"PBRemoteThroughputStep3A", "sealed-Step2", "unsealed-tool", "unknown"}, result.decoder) << "}\n";
    WriteNewText(root / "summary.json", summary.str());
    std::cout << "case=" << ScenarioName(scenario) << " observations=" << result.frames << " publishedAndReopened=" << result.publishedAndReopened << " error=" << result.error << '\n';
    return result.error.empty() ? 0 : 1;
}
}

int wmain(const int count, wchar_t** const arguments)
{
    try
    {
        const pbstep3a::ProcessBudget budget;
        if (count == 2 && std::wstring_view(arguments[1]) == L"--self-test")
        {
            pbstep3a::RunSelfChecks();
            std::cout << "{\"schema\":\"PixelBridge.Step3A.SelfCheck.1\",\"checks\":12,\"passed\":true}\n";
            return 0;
        }
        if (count == 3 && std::wstring_view(arguments[1]) == L"--make-fixture")
        {
            pbstep3a::MakeFixture(std::filesystem::absolute(arguments[2]));
            return 0;
        }
        if (count == 5 && std::wstring_view(arguments[1]) == L"--run-case")
        {
            const auto scenario = pbstep3a::ParseScenario(arguments[3]);
            return RunCase(std::filesystem::absolute(arguments[2]), scenario, std::filesystem::absolute(arguments[4]));
        }
        std::cerr << "Use --self-test, --make-fixture <new-root> or --run-case <frozen-pixels-root> <fixed-case> <new-output-root>\n";
        return 2;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
