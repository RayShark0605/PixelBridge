"""Compare G1 evidence with independently preserved baseline observations."""
import argparse
import collections
import json
from pathlib import Path


def rows(path):
    if path.stat().st_size > 1024 * 1024:
        raise RuntimeError("Trace exceeds the 1 MiB analysis limit")
    lines = [line for line in path.read_text(encoding="utf-8-sig").splitlines() if line]
    if len(lines) > 128 or any(len(line.encode("utf-8")) > 64 * 1024 for line in lines):
        raise RuntimeError("Trace record count or record size exceeds analysis limits")
    return [json.loads(line) for line in lines]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output-new", type=Path, required=True)
    parser.add_argument("--fixture-run", default="fixtures-01")
    parser.add_argument("--recording-run", default="recording-01")
    args = parser.parse_args()
    fixture_rows = rows(args.run / "runs" / args.fixture_run / "fixtures.jsonl")
    recording_rows = rows(args.run / "runs" / args.recording_run / "recording-prefix.jsonl")
    fixtures = [row for row in fixture_rows if row["type"] == "fixture"]
    recording = [row for row in recording_rows if row["type"] == "recording"]
    fixture_by_name = {row["name"]: row for row in fixtures}
    old_fixtures = [row for row in rows(args.repo / "artifacts/geometry-g1-20260908-run01/runs/baseline-01/fixtures.stdout") if row["type"] == "fixture"]
    sealed_prefix = rows(args.repo / "artifacts/remote-step2-20260908-run01/runs/recording-prefix-02/frames.jsonl")
    geometry_audit = [row for row in rows(args.repo / "artifacts/geometry-admission-audit-20260908-run01/probe/observations.jsonl") if "observation" in row]
    baseline_keys = ["width", "height", "pixelsIdenticalToPristine", "pixelBlake3", "bootstrapAccepted", "geometryAccepted", "geometryFit", "cpuBootstrapErasure", "cpuFrameErasure", "cpuAcceptedBlocks"]
    checks = {
        "fixtureCount25": len(fixtures) == len(fixture_by_name) == 25,
        "recordingCount36": len(recording) == len(sealed_prefix) == len(geometry_audit) == 36,
        "original17FixtureBaselineFieldsUnchanged": all(all(fixture_by_name[old["name"]][key] == old[key] for key in baseline_keys) for old in old_fixtures),
        "recordingOrderAndPtsUnchanged": all(new["observation"] == old["observation"] == index + 1 and new["pts"] == old["pts"] for index, (new, old) in enumerate(zip(recording, sealed_prefix))),
        "recordingOriginalBootstrapGeometryUnchanged": all(len(new["geometryFit"]) == 5 and len(old["geometryFit"]) == 4 and new["geometryFit"][:4] == old["geometryFit"] for new, old in zip(recording, sealed_prefix)),
        "recordingAllFiveFitFieldsMatchIndependentAudit": all(new["geometryFit"] == old["general"]["fit"] for new, old in zip(recording, geometry_audit)),
        "recordingBaselineAcceptanceUnchanged": all(new["geometryAccepted"] == old["bootstrapAccepted"] and new["cpuAcceptedBlocks"] == old["acceptedBlocks"] for new, old in zip(recording, sealed_prefix)),
        "recordingBaselinePayloadDigestsUnchanged": all(new["baselinePayloads"] == old["acceptedPayloadDigests"] for new, old in zip(recording, sealed_prefix)),
        "recordingCpuRejectionStageUnchanged": all(new["cpuBootstrapErasure"] == old["cpuOracle"]["bootstrap"]["erasure"] and new["cpuFrameErasure"] == old["cpuOracle"]["frameErasure"] for new, old in zip(recording, geometry_audit)),
        "noConflictingInterpretation": all(not row["conflict"] for row in fixtures + recording),
        "candidateAndBaselinePayloadsIdenticalWhenCandidateRecovers": all(row["candidate"]["payloads"] == row["baselinePayloads"] for row in fixtures + recording if row["candidate"]["acceptedBlocks"]),
        "pixelEquivalentCasesAcceptedByBoth": all(fixture_by_name["crop-pad-same-" + edge]["pixelsIdenticalToPristine"] and fixture_by_name["crop-pad-same-" + edge]["cpuAcceptedBlocks"] == fixture_by_name["crop-pad-same-" + edge]["candidate"]["acceptedBlocks"] == 15 for edge in ["left", "right", "top", "bottom"]),
        "localFreshnessErasurePreserved": fixture_by_name["local-freshness-tear"]["baselinePayloads"] == fixture_by_name["local-freshness-tear"]["candidate"]["payloads"] and fixture_by_name["local-freshness-tear"]["cpuAcceptedBlocks"] == 13,
        "identityMismatchStillRejected": fixture_by_name["expected-identity-conflict"]["cpuAcceptedBlocks"] == fixture_by_name["expected-identity-conflict"]["candidate"]["acceptedBlocks"] == 0,
        "onePixelPhotometricReproduction": fixture_by_name["one-marker-edge-plus-one"]["bootstrapAccepted"] and not fixture_by_name["one-marker-edge-plus-one"]["geometryAccepted"] and fixture_by_name["one-marker-edge-plus-one"]["cpuAcceptedBlocks"] == 0,
        "recordingSourceIdentityUnchanged": recording_rows[-1]["identity"]["sourceBlake3"] == "f321c148cc75444fb860e80d21a0083d0b3ef3a79311940558f426bdfba7b0a0",
    }
    negative_names = ["crop-" + edge for edge in ["left", "right", "top", "bottom"]]
    negative_names += ["crop-pad-opposite-" + edge for edge in ["left", "right", "top", "bottom"]]
    negative_names += ["point-downscale-075", "point-half-edge-blur-horizontal", "point-half-edge-blur-vertical", "bootstrap-identity-tear", "bootstrap-erased", "expected-identity-conflict"]
    checks["fourteenMustRejectFixturesStillRejected"] = all(fixture_by_name[name]["cpuAcceptedBlocks"] == fixture_by_name[name]["candidate"]["acceptedBlocks"] == 0 for name in negative_names)
    checks["candidateContractChecksPassed"] = any(row["type"] == "candidate-contract" and row["checks"] == 11 and row["passed"] for row in fixture_rows)
    checks["reportedBlockCountsMatchPayloadLists"] = all(row["cpuAcceptedBlocks"] == len(row["baselinePayloads"]) and row["candidate"]["acceptedBlocks"] == len(row["candidate"]["payloads"]) for row in fixtures + recording)
    checks["unmeasuredCandidateMetricsAreNull"] = all(
        (row["candidate"][value] is None) == (row["candidate"][count] == 0)
        for row in fixtures + recording
        for value, count in [("crossingRoundoff", "crossingsMeasured"), ("endpointResidual", "endpointsMeasured"), ("originResidual", "originsMeasured")]
    )
    first_candidate_rows = [row for row in rows(args.run / "runs/fixtures-01/fixtures.jsonl") + rows(args.run / "runs/recording-01/recording-prefix.jsonl") if row["type"] in {"fixture", "recording"}]
    candidate_behavior_keys = ["reason", "sharpEdges", "bootstrapAccepted", "bootstrapErasure", "workUnits", "frameErasure", "acceptedBlocks", "payloads"]
    checks["diagnosticAvailabilityChangePreservesCandidateBehavior"] = len(first_candidate_rows) == len(fixtures + recording) and all(
        new["pixelBlake3"] == old["pixelBlake3"] and new["baselinePayloads"] == old["baselinePayloads"] and all(new["candidate"][key] == old["candidate"][key] for key in candidate_behavior_keys)
        for new, old in zip(fixtures + recording, first_candidate_rows)
    )
    def summary(observations):
        return {
            "observations": len(observations),
            "baselineWithAcceptedBlocks": sum(row["cpuAcceptedBlocks"] > 0 for row in observations),
            "candidateWithAcceptedBlocks": sum(row["candidate"]["acceptedBlocks"] > 0 for row in observations),
            "baselineAcceptedBlocks": sum(row["cpuAcceptedBlocks"] for row in observations),
            "candidateAcceptedBlocks": sum(row["candidate"]["acceptedBlocks"] for row in observations),
            "newCandidateBlocksWhenBaselineEmpty": sum(row["newCandidateBlocksWhenBaselineEmpty"] for row in observations),
            "candidateReasons": dict(collections.Counter(row["candidate"]["reason"] for row in observations)),
        }
    verdict = "EVIDENCE_INVALID" if not all(checks.values()) else "CANDIDATE_REQUIRES_REVIEW" if any(row["newCandidateBlocksWhenBaselineEmpty"] for row in fixtures + recording) else "NOT_PROMOTED_NO_NEW_ADMISSION"
    report = {"schema": "PixelBridge.G1.ResultAnalysis.1", "checks": checks, "allChecksPassed": all(checks.values()),
              "fixtures": summary(fixtures), "recordingPrefix": summary(recording),
              "minimalReproduction": fixture_by_name["one-marker-edge-plus-one"],
              "verdict": verdict, "fieldStatus": "NOT_RUN",
              "interpretation": "A candidate is never promoted automatically. Invalid evidence requires correction; no-new-admission results do not support G2; any new admission still requires explicit review and user confirmation."}
    with args.output_new.open("x", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
        output.write("\n")
    print(json.dumps({key: report[key] for key in ["checks", "fixtures", "recordingPrefix", "verdict"]}, indent=2))
    if not report["allChecksPassed"]:
        raise RuntimeError("Evidence comparison failed")


if __name__ == "__main__":
    main()
