#!/usr/bin/env python3
"""Build the complete bounded Step 06 LF4 impairment corpus in one command."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
from typing import Any

import blake3

from build_codec_corpus import (
    MAX_JSON_BYTES,
    SOURCE_FRAMES,
    CorpusError,
    artifact,
    build_corpus,
    canonical_json,
    check_executable,
    normalize_cli_json,
    run_bounded,
    sha256_file,
    tool_identity,
    validate_canonical_report,
    write_new,
)


SCHEMA = "PixelBridge.RemoteVisualStep06Corpus.1"
MATRIX_SCHEMA = "PixelBridge.RemoteVisualChannelMatrix.1"
TEMPORAL_SCHEMA = "PixelBridge.RemoteVisualTemporalCorpus.1"


def require_nonnegative_integer(value: Any, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise CorpusError(f"{label} must be a nonnegative integer")
    return value


def validate_matrix(report: dict[str, Any]) -> dict[str, Any]:
    cases = report["payload"].get("cases")
    summary = report["payload"].get("summary")
    if not isinstance(cases, list) or not isinstance(summary, dict):
        raise CorpusError("channel matrix cases and summary must have their canonical container types")
    classifications = {"Verified": 0, "ErasureNoFalseAccept": 0, "RejectedNoFalseAccept": 0}
    false_accepted_codewords = 0
    observed_names: set[str] = set()
    for index, case in enumerate(cases):
        if not isinstance(case, dict) or case.get("index") != index or not isinstance(case.get("name"), str) or \
                not case["name"] or case["name"] in observed_names or case.get("classification") not in classifications or \
                case.get("expectationMatched") is not True or not isinstance(case.get("evaluation"), dict):
            raise CorpusError(f"channel matrix case {index} is malformed or did not match its expectation")
        observed_names.add(case["name"])
        classifications[case["classification"]] += 1
        false_accepted_codewords += require_nonnegative_integer(
            case["evaluation"].get("falseAcceptedCodewords"), f"matrix case {index} falseAcceptedCodewords")
    if require_nonnegative_integer(summary.get("caseCount"), "matrix caseCount") != 15 or \
            require_nonnegative_integer(summary.get("verifiedCases"), "matrix verifiedCases") != 12 or \
            require_nonnegative_integer(summary.get("erasureCases"), "matrix erasureCases") != 3 or \
            require_nonnegative_integer(summary.get("rejectedCases"), "matrix rejectedCases") != 0 or \
            require_nonnegative_integer(summary.get("falseAcceptedCodewords"),
                                        "matrix falseAcceptedCodewords") != 0 or \
            require_nonnegative_integer(summary.get("expectationMismatches"),
                                        "matrix expectationMismatches") != 0 or \
            len(cases) != summary["caseCount"] or classifications["Verified"] != summary["verifiedCases"] or \
            classifications["ErasureNoFalseAccept"] != summary["erasureCases"] or \
            classifications["RejectedNoFalseAccept"] != summary["rejectedCases"] or \
            false_accepted_codewords != summary["falseAcceptedCodewords"] or \
            summary.get("truthBoundaryValid") is not True or summary.get("expectationsMatched") is not True:
        raise CorpusError("channel matrix summary does not satisfy the Step 06 truth boundary")
    return summary


def validate_temporal(report: dict[str, Any]) -> dict[str, Any]:
    scenarios = report["payload"].get("scenarios")
    stalls = report["payload"].get("stallScenario")
    summary = report["payload"].get("summary")
    if not isinstance(scenarios, list) or not isinstance(stalls, dict) or not isinstance(summary, dict):
        raise CorpusError("temporal scenarios, stalls and summary must have their canonical container types")
    event_count = 0
    admitted_blocks = 0
    suppressed_duplicates = 0
    suppressed_reordered = 0
    diagnostic_candidates = 0
    scenario_names: set[str] = set()
    for scenario_index, scenario in enumerate(scenarios):
        if not isinstance(scenario, dict) or not isinstance(scenario.get("name"), str) or \
                not scenario["name"] or scenario["name"] in scenario_names or \
                not isinstance(scenario.get("events"), list) or scenario.get("expectationMatched") is not True:
            raise CorpusError(f"temporal scenario {scenario_index} is malformed or did not match its expectation")
        scenario_names.add(scenario["name"])
        for event_index, event in enumerate(scenario["events"]):
            if not isinstance(event, dict) or event.get("expectationMatched") is not True or \
                    event.get("disposition") not in {"Invalid", "Unique", "Duplicate", "Reordered"} or \
                    not isinstance(event.get("admissionAttempted"), bool):
                raise CorpusError(f"temporal scenario {scenario_index} event {event_index} is malformed")
            event_count += 1
            admitted_blocks += require_nonnegative_integer(event.get("admittedTransportBlocks"),
                                                           "temporal admittedTransportBlocks")
            diagnostic_candidates += require_nonnegative_integer(event.get("diagnosticNonTruthCodewords"),
                                                                  "temporal diagnosticNonTruthCodewords")
            suppressed_duplicates += event["disposition"] == "Duplicate" and not event["admissionAttempted"]
            suppressed_reordered += event["disposition"] == "Reordered" and not event["admissionAttempted"]
    if set(stalls) != {"capture", "visual"} or any(not isinstance(stalls.get(name), dict) for name in stalls):
        raise CorpusError("temporal stall scenario is malformed")
    capture_stall = stalls["capture"]
    visual_stall = stalls["visual"]
    if require_nonnegative_integer(summary.get("scenarioCount"), "temporal scenarioCount") != 3 or \
            require_nonnegative_integer(summary.get("eventCount"), "temporal eventCount") != 11 or \
            require_nonnegative_integer(summary.get("admittedTransportBlocks"),
                                        "temporal admittedTransportBlocks") != 16 or \
            require_nonnegative_integer(summary.get("suppressedDuplicateEvents"),
                                        "temporal suppressedDuplicateEvents") != 3 or \
            require_nonnegative_integer(summary.get("suppressedReorderedEvents"),
                                        "temporal suppressedReorderedEvents") != 1 or \
            require_nonnegative_integer(summary.get("duplicateRefinementRecoveries"),
                                        "temporal duplicateRefinementRecoveries") != 1 or \
            require_nonnegative_integer(summary.get("wrongIdentityAcceptedTransportBlocks"),
                                        "temporal wrongIdentityAcceptedTransportBlocks") != 0 or \
            require_nonnegative_integer(summary.get("wrongIdentityDiagnosticCandidates"),
                                        "temporal wrongIdentityDiagnosticCandidates") != 4 or \
            len(scenarios) != summary["scenarioCount"] or event_count != summary["eventCount"] or \
            admitted_blocks != summary["admittedTransportBlocks"] or \
            suppressed_duplicates != summary["suppressedDuplicateEvents"] or \
            suppressed_reordered != summary["suppressedReorderedEvents"] or \
            diagnostic_candidates != summary["wrongIdentityDiagnosticCandidates"] or \
            require_nonnegative_integer(capture_stall.get("count"), "capture stall count") != 1 or \
            require_nonnegative_integer(capture_stall.get("totalMilliseconds"),
                                        "capture stall totalMilliseconds") != 1400 or \
            require_nonnegative_integer(visual_stall.get("count"), "visual stall count") != 1 or \
            require_nonnegative_integer(visual_stall.get("totalMilliseconds"),
                                        "visual stall totalMilliseconds") != 1300 or \
            summary.get("productionAdmissionSafe") is not True or summary.get("expectationsMatched") is not True:
        raise CorpusError("temporal corpus summary does not satisfy the Step 06 admission boundary")
    return summary


def build_step06_corpus(matrix_exe: Path, temporal_exe: Path, codec_probe: Path,
                        ffmpeg: Path, ffprobe: Path, output_dir: Path) -> dict[str, Any]:
    tools = {
        "channel-matrix": check_executable(matrix_exe, "channel matrix"),
        "temporal-corpus": check_executable(temporal_exe, "temporal corpus"),
        "codec-probe": check_executable(codec_probe, "codec probe"),
        "ffmpeg": check_executable(ffmpeg, "ffmpeg"),
        "ffprobe": check_executable(ffprobe, "ffprobe"),
    }
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=False, exist_ok=False)
    initial_tools = {name: tool_identity(path) for name, path in tools.items()}

    matrix_bytes = normalize_cli_json(
        run_bounded([str(tools["channel-matrix"])], maximum_stdout_bytes=MAX_JSON_BYTES), "channel matrix")
    matrix = validate_canonical_report(matrix_bytes, MATRIX_SCHEMA, "channel matrix")
    matrix_summary = validate_matrix(matrix)
    matrix_path = output_dir / "channel-matrix.json"
    write_new(matrix_path, matrix_bytes)

    temporal_bytes = normalize_cli_json(
        run_bounded([str(tools["temporal-corpus"])], maximum_stdout_bytes=MAX_JSON_BYTES), "temporal corpus")
    temporal = validate_canonical_report(temporal_bytes, TEMPORAL_SCHEMA, "temporal corpus")
    temporal_summary = validate_temporal(temporal)
    temporal_path = output_dir / "temporal-corpus.json"
    write_new(temporal_path, temporal_bytes)

    codec_dir = output_dir / "actual-codec-corpus"
    codec = build_corpus(tools["codec-probe"], tools["ffmpeg"], tools["ffprobe"], codec_dir)
    codec_cases = codec.get("cases")
    if not isinstance(codec_cases, list) or len(codec_cases) != 5:
        raise CorpusError("actual codec corpus has an unexpected case count")
    codec_false_accepted = 0
    codec_verified_frames = 0
    codec_erasure_frames = 0
    codec_rejected_frames = 0
    for case in codec_cases:
        if not isinstance(case, dict) or not isinstance(case.get("evaluationSummary"), dict):
            raise CorpusError("actual codec corpus contains a malformed case summary")
        summary = case["evaluationSummary"]
        if summary.get("truthBoundaryValid") is not True:
            raise CorpusError(f"actual codec case failed its truth boundary: {case.get('name')}")
        codec_false_accepted += require_nonnegative_integer(summary.get("falseAcceptedCodewords"),
                                                            "falseAcceptedCodewords")
        codec_verified_frames += require_nonnegative_integer(summary.get("verifiedFrames"), "verifiedFrames")
        codec_erasure_frames += require_nonnegative_integer(summary.get("erasureFrames"), "erasureFrames")
        codec_rejected_frames += require_nonnegative_integer(summary.get("rejectedFrames"), "rejectedFrames")
    if codec_false_accepted != 0:
        raise CorpusError("actual codec corpus produced nonzero false-accepted codewords")
    if codec_verified_frames + codec_erasure_frames + codec_rejected_frames != len(codec_cases) * SOURCE_FRAMES:
        raise CorpusError("actual codec corpus frame classifications do not cover every source frame")

    final_tools = {name: tool_identity(path) for name, path in tools.items()}
    if final_tools != initial_tools:
        raise CorpusError("one or more Step 06 tools changed during corpus generation")
    payload = {
        "tools": initial_tools,
        "artifacts": {
            "channelMatrix": artifact(matrix_path, output_dir),
            "temporalCorpus": artifact(temporal_path, output_dir),
            "actualCodecManifest": artifact(codec_dir / "codec-corpus-manifest.json", output_dir),
            "actualCodecSha256Sums": artifact(codec_dir / "SHA256SUMS.txt", output_dir),
        },
        "summary": {
            "channelMatrixCases": matrix_summary["caseCount"],
            "channelMatrixVerified": matrix_summary["verifiedCases"],
            "channelMatrixErasures": matrix_summary["erasureCases"],
            "temporalEvents": temporal_summary["eventCount"],
            "suppressedDuplicateEvents": temporal_summary["suppressedDuplicateEvents"],
            "suppressedReorderedEvents": temporal_summary["suppressedReorderedEvents"],
            "wrongIdentityAcceptedTransportBlocks": temporal_summary["wrongIdentityAcceptedTransportBlocks"],
            "wrongIdentityDiagnosticCandidates": temporal_summary["wrongIdentityDiagnosticCandidates"],
            "actualCodecCases": len(codec_cases),
            "actualCodecVerifiedFrames": codec_verified_frames,
            "actualCodecErasureFrames": codec_erasure_frames,
            "actualCodecRejectedFrames": codec_rejected_frames,
            "actualCodecFalseAcceptedCodewords": codec_false_accepted,
            "productionAdmissionSafe": True,
            "expectationsMatched": True,
        },
        "constraints": {
            "displayOrCaptureUsed": False,
            "decoderExpectedPayloadInput": False,
            "providerSpecificThresholds": False,
            "frozenWireChanged": False,
        },
    }
    payload_bytes = canonical_json(payload).rstrip(b"\n")
    index = {
        "schema": SCHEMA,
        "version": 1,
        "payloadBlake3": blake3.blake3(payload_bytes).hexdigest(),
        "payload": payload,
    }
    index_path = output_dir / "step06-corpus-index.json"
    write_new(index_path, canonical_json(index))
    sums = []
    root_sums_path = output_dir / "SHA256SUMS.txt"
    for path in sorted(item for item in output_dir.rglob("*") if item.is_file() and item != root_sums_path):
        sums.append(f"{sha256_file(path)}  {path.relative_to(output_dir).as_posix()}")
    write_new(root_sums_path, ("\n".join(sums) + "\n").encode("ascii"))
    return index


def parse_arguments(arguments: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel-matrix", required=True, type=Path)
    parser.add_argument("--temporal-corpus", required=True, type=Path)
    parser.add_argument("--codec-probe", required=True, type=Path)
    parser.add_argument("--ffmpeg", required=True, type=Path)
    parser.add_argument("--ffprobe", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    return parser.parse_args(arguments)


def main(arguments: list[str]) -> int:
    options = parse_arguments(arguments)
    try:
        index = build_step06_corpus(options.channel_matrix, options.temporal_corpus, options.codec_probe,
                                    options.ffmpeg, options.ffprobe, options.output_dir)
    except (CorpusError, OSError, subprocess.TimeoutExpired) as exception:
        print(f"[error] {exception}", file=sys.stderr)
        return 1
    print(json.dumps({
        "schema": index["schema"],
        "outputDir": str(options.output_dir.resolve()),
        **index["payload"]["summary"],
    }, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
