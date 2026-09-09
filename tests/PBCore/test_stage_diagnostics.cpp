#include "pbcore/stage_diagnostics.h"
#include <catch2/catch_test_macros.hpp>
#include <limits>

TEST_CASE("Step2 stage diagnostics are bounded sampled observation only", "[step2][diagnostics]")
{
    pbcore::StageDiagnostics recorder(2);
    REQUIRE(recorder.Begin(pbcore::DiagnosticStage::Raster));
    recorder.End(pbcore::DiagnosticStage::Raster, 17);
    REQUIRE_FALSE(recorder.Begin(pbcore::DiagnosticStage::Raster));
    REQUIRE(recorder.Begin(pbcore::DiagnosticStage::Raster));
    recorder.End(pbcore::DiagnosticStage::Raster, 23);
    const auto snapshot = recorder.GetSnapshot();
    const auto& timing = snapshot.stages[static_cast<std::size_t>(pbcore::DiagnosticStage::Raster)];
    REQUIRE(snapshot.valid);
    REQUIRE(timing.calls == 3);
    REQUIRE(timing.samples == 2);
    REQUIRE(timing.totalNanoseconds == 40);
    REQUIRE(timing.maximumNanoseconds == 23);
    REQUIRE(pbcore::BuildStageDiagnosticsJson(snapshot).find("StageNotObserved") != std::string::npos);
}

TEST_CASE("Step2 timing failure retracts evidence and never manufactures zero", "[step2][diagnostics][negative]")
{
    pbcore::StageDiagnostics recorder;
    REQUIRE(recorder.Begin(pbcore::DiagnosticStage::Raster));
    recorder.End(pbcore::DiagnosticStage::Raster, std::numeric_limits<std::uint64_t>::max());
    REQUIRE_FALSE(recorder.GetSnapshot().valid);
    REQUIRE_FALSE(recorder.Begin(pbcore::DiagnosticStage::Raster));
    REQUIRE(pbcore::BuildStageDiagnosticsJson(recorder.GetSnapshot()).find("EvidenceInvalid") != std::string::npos);
    pbcore::StageDiagnostics invalidStride(0);
    REQUIRE_FALSE(invalidStride.GetSnapshot().valid);
    pbcore::StageDiagnostics invalidStage;
    REQUIRE_FALSE(invalidStage.Begin(pbcore::DiagnosticStage::Count));
    REQUIRE_FALSE(invalidStage.GetSnapshot().valid);
    const pbcore::DiagnosticScope disabled(nullptr, pbcore::DiagnosticStage::Raster);
}

TEST_CASE("Step2 diagnostic record cap is sticky", "[step2][diagnostics][bounds]")
{
    pbcore::StageDiagnostics recorder(4096);
    for (std::uint64_t index = 0; index < 2000000; index++)
    {
        if (recorder.Begin(pbcore::DiagnosticStage::SubmitCall))
        {
            recorder.End(pbcore::DiagnosticStage::SubmitCall, 1);
        }
    }
    REQUIRE(recorder.GetSnapshot().valid);
    REQUIRE_FALSE(recorder.Begin(pbcore::DiagnosticStage::SubmitCall));
    REQUIRE_FALSE(recorder.GetSnapshot().valid);
}
