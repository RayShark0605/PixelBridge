"""Bounded, read-only criterion proof. Feasible geometry is NEVER admission."""
import argparse
import hashlib
import json
import math
from pathlib import Path


def reject_constant(value):
    raise ValueError(f"Non-finite JSON constant: {value}")


def read_rows(path):
    if path.stat().st_size > 1024 * 1024:
        raise ValueError("Evidence file exceeds 1 MiB")
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    if not 1 <= len(lines) <= 128 or any(len(line.encode("utf-8")) > 65536 for line in lines):
        raise ValueError("Evidence record count/size limit")
    rows = [json.loads(line, parse_constant=reject_constant) for line in lines]
    if any(not isinstance(row, dict) for row in rows):
        raise ValueError("Evidence record must be an object")
    return rows


def file_hash(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def original_fit(edges):
    """Independent arithmetic over exported readings; no native FitAxis call."""
    def serial_sum(values):
        # Mirror the native += order, not Python's compensated builtin sum.
        total = 0.0
        for value in values:
            total += value
        return total

    fits = []
    for axis in range(2):
        readings = edges[axis * 24:(axis + 1) * 24]
        logical_mean = serial_sum(edge[0] for edge in readings) / 24
        measured_mean = serial_sum(edge[1] for edge in readings) / 24
        numerator = serial_sum((edge[0] - logical_mean) * (edge[1] - measured_mean) for edge in readings)
        denominator = serial_sum((edge[0] - logical_mean) * (edge[0] - logical_mean) for edge in readings)
        scale = numerator / denominator
        origin = measured_mean - scale * logical_mean
        residual = max(abs(edge[1] - origin - scale * edge[0]) for edge in readings)
        fits.append([origin, scale, residual])
    return [fits[0][0], fits[1][0], fits[0][1], fits[1][1], max(fits[0][2], fits[1][2])]


def summarize_probe(row):
    probe = row["probe"]
    edges = probe["edges"]
    for key, maximum in [("markersVerified", 4), ("crossingsMeasured", 48), ("endpointsMeasured", 48), ("workUnits", 24000000)]:
        if type(probe[key]) is not int or not 0 <= probe[key] <= maximum:
            raise ValueError(f"Invalid bounded integer: {key}")
    marker_residuals = probe["markerResiduals"]
    if not isinstance(marker_residuals, list) or len(marker_residuals) != 4:
        raise ValueError("Expected four bounded marker residual slots")
    for index, value in enumerate(marker_residuals):
        if index >= probe["markersVerified"]:
            if value is not None:
                raise ValueError("Unmeasured marker residual must be null")
        elif type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value <= 0.075:
            raise ValueError("Invalid verified marker residual")
    if not isinstance(edges, list) or len(edges) != 48:
        raise ValueError("Probe must contain exactly 48 bounded edge slots")
    measured = [edge for edge in edges if edge is not None]
    if any(not isinstance(edge, list) or len(edge) != 6 for edge in measured):
        raise ValueError("Malformed edge reading")
    if any(not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value)
           for edge in measured for value in edge if value is not None):
        raise ValueError("Invalid edge numeric value")
    endpoints = sum(edge[4] is not None and edge[5] is not None for edge in measured)
    if probe["crossingsMeasured"] != len(measured) or probe["endpointsMeasured"] != endpoints:
        raise ValueError("Probe measurement counts differ from available readings")
    if (probe["firstFit"] is not None) != (probe["reason"] == "MeasuredOnlyNotAdmission"):
        raise ValueError("Unmeasured fit must be null")
    result = {"name": row["name"], "observation": row["observation"], "probeReason": probe["reason"],
              "baselineAcceptedBlocks": row["cpuAcceptedBlocks"], "canonicalResidual": None,
              "sharpEdges": None, "geometryHypothesisFitsExistingResidualBudget": None}
    if probe["reason"] != "MeasuredOnlyNotAdmission":
        return result
    if row["width"] != 1920 or row["height"] != 1080 or len(measured) != 48 or endpoints != 48 or probe["markersVerified"] != 4:
        raise ValueError("Incomplete canonical probe")
    if original_fit(edges) != probe["firstFit"]:
        raise ValueError("Independent OLS does not match native first fit")
    # For each axis: scale >= 1, origin >= 0, origin + scale*C <= C.
    # Consequently the FULL-CANVAS feasible set is the singleton (origin,scale)=(0,1).
    # This algebra constrains a hypothesis; it does not prove the input has that geometry.
    residuals = [edge[1] - edge[0] for edge in edges]
    result["canonicalResidual"] = max(abs(value) for value in residuals)
    result["geometryHypothesisFitsExistingResidualBudget"] = result["canonicalResidual"] <= 1.25
    sharp = 0
    for index, edge in enumerate(edges):
        _, crossing, black, white, before, after = edge
        rising = index % 6 % 2 != 0
        endpoint_residual = max(abs(before - (black if rising else white)), abs(after - (white if rising else black)))
        sharp += abs(crossing - round(crossing)) <= 1e-9 and endpoint_residual <= (white - black) * 1e-9
    result["sharpEdges"] = sharp
    result["firstFit"] = probe["firstFit"]
    result["maximumEndpointDeviationFromMarkerLevels"] = max(
        max(abs(edge[4] - (edge[2] if index % 6 % 2 else edge[3])),
            abs(edge[5] - (edge[3] if index % 6 % 2 else edge[2]))) for index, edge in enumerate(edges))
    return result


def analyze(repo, fixtures_path, recording_path):
    historic = repo / "artifacts/geometry-g1-20260908-run02"
    manifest = json.loads((historic / "FINAL_MANIFEST.json").read_text(encoding="utf-8"))
    known_hashes = {str(Path(item["path"]).resolve()): item["sha256"] for item in manifest["files"]}
    old_paths = [historic / "runs/fixtures-final-02/fixtures.jsonl", historic / "runs/recording-final-02/recording-prefix.jsonl"]
    if any(file_hash(path) != known_hashes[str(path.resolve())] for path in old_paths):
        raise ValueError("Historical sealed traces changed")
    all_new = [read_rows(fixtures_path), read_rows(recording_path)]
    all_old = [read_rows(path) for path in old_paths]
    fixtures = [row for row in all_new[0] if row.get("type") == "fixture"]
    recording = [row for row in all_new[1] if row.get("type") == "recording"]
    old_fixtures = [row for row in all_old[0] if row.get("type") == "fixture"]
    old_recording = [row for row in all_old[1] if row.get("type") == "recording"]
    checks = {
        "exactRecordShape": len(all_new[0]) == 35 and len(all_new[1]) == 37,
        "fixtureCount25": len(fixtures) == 25,
        "recordingCount36": len(recording) == 36,
        "fixtureNamesUnique": len({row["name"] for row in fixtures}) == len(fixtures),
        "recordingOrder": [row["observation"] for row in recording] == list(range(1, 37)),
    }
    fields = ["name", "observation", "pts", "width", "height", "pixelsIdenticalToPristine", "pixelBlake3", "bootstrapAccepted",
              "geometryAccepted", "geometryFit", "fixedMatchesContinuous", "cpuBootstrapErasure", "cpuFrameErasure", "cpuAcceptedBlocks", "baselinePayloads"]
    checks["all25FixtureBaselineFieldsExact"] = len(fixtures) == len(old_fixtures) and all(
        all(left[field] == right[field] for field in fields) for left, right in zip(fixtures, old_fixtures))
    checks["all36RecordingBaselineFieldsExact"] = len(recording) == len(old_recording) and all(
        all(left[field] == right[field] for field in fields) for left, right in zip(recording, old_recording))
    checks["eightNumericBoundaryRecordsExact"] = [r for r in all_new[0] if r.get("type") == "boundary"] == [r for r in all_old[0] if r.get("type") == "boundary"]
    checks["mediaIdentityExact"] = [r for r in all_new[1] if r.get("type") == "media"] == [r for r in all_old[1] if r.get("type") == "media"]
    checks["probeContractChecks10"] = [r for r in all_new[0] if r.get("type") == "probe-contract"] == [{"type": "probe-contract", "checks": 10, "passed": True}]
    checks["countsMatchPayloadLists"] = all(row["cpuAcceptedBlocks"] == len(row["baselinePayloads"]) for row in fixtures + recording)
    checks["noAdmissionCandidate"] = all(row["admissionCandidateImplemented"] is False for row in fixtures + recording)
    checks["finalStatusExact"] = [row for row in all_new[0] if row.get("type") == "status"] == [{
        "type": "status", "baselineBoundaryChecks": 8, "fixtureObservations": 25,
        "productionChanged": False, "candidateImplemented": False, "candidateVariants": 0}]
    fixture_probes = [summarize_probe(row) for row in fixtures]
    recording_probes = [summarize_probe(row) for row in recording]
    probes = {row["name"]: row for row in fixture_probes}
    geometry_negative = ["crop-pad-opposite-left", "crop-pad-opposite-right", "crop-pad-opposite-top", "crop-pad-opposite-bottom"]
    must_reject = geometry_negative + ["crop-left", "crop-right", "crop-top", "crop-bottom", "point-downscale-075",
                                     "point-half-edge-blur-horizontal", "point-half-edge-blur-vertical", "bootstrap-identity-tear", "bootstrap-erased", "expected-identity-conflict"]
    checks["fourteenMustRejectCasesRemainZero"] = all(probes[name]["baselineAcceptedBlocks"] == 0 for name in must_reject)
    checks["fourRealTranslationsAreCounterexamples"] = all(
        probes[name]["canonicalResidual"] == 1 and probes[name]["sharpEdges"] == 48 and probes[name]["geometryHypothesisFitsExistingResidualBudget"] is True for name in geometry_negative)
    minimal = probes["one-marker-edge-plus-one"]
    checks["minimalReproductionExactlyOneNonSharpEdge"] = minimal["sharpEdges"] == 47 and minimal["baselineAcceptedBlocks"] == 0 and minimal["geometryHypothesisFitsExistingResidualBudget"] is True
    checks["allRecordingEdgesMeasured"] = all(row["probeReason"] == "MeasuredOnlyNotAdmission" for row in recording_probes)
    checks["noSharpRecordingEdge"] = all(row["sharpEdges"] == 0 for row in recording_probes)
    checks["recordingBaseline16And240"] = sum(row["cpuAcceptedBlocks"] > 0 for row in recording) == 16 and sum(row["cpuAcceptedBlocks"] for row in recording) == 240
    checks["fixtureBaseline10And148"] = sum(row["cpuAcceptedBlocks"] > 0 for row in fixtures) == 10 and sum(row["cpuAcceptedBlocks"] for row in fixtures) == 148
    checks["allRecordingResidualHypothesesFeasible"] = all(row["geometryHypothesisFitsExistingResidualBudget"] is True for row in recording_probes)
    return {"schema": "PixelBridge.G1B.CriterionProof.1", "checks": checks, "allChecksPassed": all(checks.values()),
            "verdict": "CRITERION_INSUFFICIENT_NO_ADMISSION_CANDIDATE" if all(checks.values()) else "EVIDENCE_INVALID",
            "candidateVariants": 0, "fieldStatus": "NOT_RUN", "fixtureProbes": fixture_probes, "recordingProbes": recording_probes,
            "minimalReproduction": minimal, "geometryCounterexamples": [probes[name] for name in geometry_negative],
            "maximumRecordingCanonicalResidual": max(row["canonicalResidual"] for row in recording_probes),
            "interpretation": "This rejects residual-only constrained canonical fitting as a sufficient admission criterion. It does not prove all estimators impossible, nor demonstrate payload acceptance by an alternative decoder. No new tolerance/noise model is inferred from the observations."}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--recording", type=Path, required=True)
    parser.add_argument("--output-new", type=Path, required=True)
    args = parser.parse_args()
    output = args.output_new.resolve()
    if output.exists() or not output.is_relative_to(args.repo.resolve() / "artifacts"):
        raise RuntimeError("Analysis output must be a new workspace artifact")
    try:
        result = analyze(args.repo.resolve(), args.fixtures, args.recording)
    except (KeyError, ValueError, TypeError, IndexError, OSError) as error:
        result = {"schema": "PixelBridge.G1B.CriterionProof.1", "allChecksPassed": False, "verdict": "EVIDENCE_INVALID", "error": str(error)}
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")
    print(json.dumps({key: value for key, value in result.items() if key not in {"fixtureProbes", "recordingProbes"}}, ensure_ascii=False))
    if not result["allChecksPassed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
