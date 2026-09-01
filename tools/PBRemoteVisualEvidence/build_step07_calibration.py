#!/usr/bin/env python3
"""Build and independently seal the Step 07 LF4 soft-metric calibration Gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import stat as stat_module
import subprocess
import sys
from typing import Any

import blake3

from build_codec_corpus import CorpusError, canonical_json, check_executable, run_bounded, tool_identity, write_new


SCHEMA = "PixelBridge.RemoteVisualStep07Calibration.1"
STEP06_SCHEMA = "PixelBridge.RemoteVisualStep06Corpus.2"
CODEC_SCHEMA = "PixelBridge.RemoteVisualCodecCorpus.2"
STEP02_INDEX_SCHEMA = "PixelBridge.RemoteVisualDatasetIndex.1"
STEP02_COMPLETION_SCHEMA = "PixelBridge.RemoteVisualStep02Completion.1"
CALIBRATION_EVIDENCE_SCHEMA = "PixelBridge.RemoteVisualMetricCalibrationEvidence.1"
CALIBRATION_CORE_SCHEMA = "PixelBridge.RemoteVisualMetricCalibration.1"
REPLAY_INSPECTION_SCHEMA = "PixelBridge.RemoteVisualReplayInspection.1"
LF4_PROFILE = "PB-RemoteVisual-LF4-X1"
LF4_PROFILE_ID = "504252564c463431"
LF4_LAYOUT_VERSION = 7
GRAY_FRAME_BYTES = 1920 * 1080
MAX_INDEX_BYTES = 4 * 1024 * 1024
MAX_REPORT_BYTES = 8 * 1024 * 1024
MAX_REPLAY_BYTES = 128 * 1024 * 1024
COMMAND_TIMEOUT_SECONDS = 300

CODEC_SPLITS = (
    ("h264-420-crf18-intra", "train", "PB-Signal-Video709Limited420-1"),
    ("h264-444-crf28-inter", "train", "PB-Signal-Video709Limited444-1"),
    ("h264-420-crf35-inter", "validation", "PB-Signal-Video709Limited420-1"),
    ("hevc-420-crf28-inter", "validation", "PB-Signal-Video709Limited420-1"),
    ("h264-444-full-crf28-inter", "holdout", "PB-Signal-Video709Full444-1"),
    ("hevc-420-crf40-inter", "holdout", "PB-Signal-Video709Limited420-1"),
)

SIMULATOR_RUNS = {
    *(f"sim-train-{name}" for name in ("identity", "gaussian-1", "box-1", "gamma-095", "scale-090", "blend-032")),
    *(f"sim-validation-{name}" for name in ("identity", "gaussian-2", "box-2", "gamma-110", "scale-075", "blend-064")),
    *(f"sim-holdout-{name}" for name in ("identity", "gaussian-3", "box-3", "gamma-120", "scale-1259", "blend-096")),
}

POLICIES = {
    "lf4-default": False,
    "lf4-margin-004": True,
    "lf4-margin-012": False,
    "lf4-residual-055": False,
    "lf4-residual-090": True,
    "lf4-rms-045": False,
}
POLICY_MANIFESTS = {
    "lf4-default": "{\"maximumSymbolResidual\":0.69999999999999996,\"minimumSymbolMargin\":0.080000000000000002,\"minimumSymbolRms\":0.34999999999999998}",
    "lf4-margin-004": "{\"maximumSymbolResidual\":0.69999999999999996,\"minimumSymbolMargin\":0.040000000000000001,\"minimumSymbolRms\":0.34999999999999998}",
    "lf4-margin-012": "{\"maximumSymbolResidual\":0.69999999999999996,\"minimumSymbolMargin\":0.12,\"minimumSymbolRms\":0.34999999999999998}",
    "lf4-residual-055": "{\"maximumSymbolResidual\":0.55000000000000004,\"minimumSymbolMargin\":0.080000000000000002,\"minimumSymbolRms\":0.34999999999999998}",
    "lf4-residual-090": "{\"maximumSymbolResidual\":0.90000000000000002,\"minimumSymbolMargin\":0.080000000000000002,\"minimumSymbolRms\":0.34999999999999998}",
    "lf4-rms-045": "{\"maximumSymbolResidual\":0.69999999999999996,\"minimumSymbolMargin\":0.080000000000000002,\"minimumSymbolRms\":0.45000000000000001}",
}
MODEL_ORDER = ("Raw", "GlobalScale", "PerSignalScale", "PiecewiseLookup")
MODELS = set(MODEL_ORDER)
GLOBAL_SCALE_CANDIDATES = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0}
PIECEWISE_RAW_MAGNITUDE_UPPER = (
    0.01, 0.02, 0.04, 0.08, 0.12, 0.2, 0.35, 0.5,
    0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 8.0, 1000000.0,
)
SUMMARY_FIELDS = {
    "frames", "truthFrames", "receiverOnlyFrames", "modulationErasures", "verifiedFrames",
    "frameErrorRate", "comparedBits", "bitErrors", "bitErrorRate", "expectedZeroBits",
    "expectedOneBits", "zeroBitErrors", "oneBitErrors", "highConfidenceBits",
    "falseConfidenceErrors", "expectedTransportBlocks", "acceptedTransportBlocks",
    "falseAcceptedTransportBlocks", "acceptedControlBlocks", "falseAcceptedControlBlocks",
    "fecFailures", "crcFailures", "identityFailures", "iterationsTotal", "iterationsMaximum",
    "averageLogLoss", "averageBrierScore", "expectedCalibrationError", "reliability",
    "confidenceCurve", "rocCurve",
}
RELIABILITY_PROBABILITY_UPPER = (0.0001, 0.001, 0.01, 0.025, 0.05, 0.1, 0.2, 0.3, 0.4, 0.5)
FALSE_CONFIDENCE_MAGNITUDE = 6.906754778648553
CALIBRATED_MAGNITUDE_CLIP = 32767.0 / 4096.0
CONFIDENCE_MAGNITUDE_THRESHOLDS = (
    0.0, 0.02, 0.05, 0.1, 0.2, 0.4, 0.8, 1.6, 3.2,
    FALSE_CONFIDENCE_MAGNITUDE, CALIBRATED_MAGNITUDE_CLIP,
)
ROC_ONE_SCORE_THRESHOLDS = (
    -CALIBRATED_MAGNITUDE_CLIP - 1.0, -CALIBRATED_MAGNITUDE_CLIP,
    -FALSE_CONFIDENCE_MAGNITUDE, -3.2, -1.6, -0.8, -0.4, -0.2, -0.1, -0.05, -0.02,
    0.0, 0.02, 0.05, 0.1, 0.2, 0.4, 0.8, 1.6, 3.2,
    FALSE_CONFIDENCE_MAGNITUDE, CALIBRATED_MAGNITUDE_CLIP, CALIBRATED_MAGNITUDE_CLIP + 1.0,
)
LF4_CODED_BITS = 64800
LF4_CODEWORDS = 4


def reject_duplicate_members(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CorpusError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def reject_non_finite(value: str) -> None:
    raise CorpusError(f"non-finite JSON number: {value}")


def parse_json(contents: bytes, label: str) -> dict[str, Any]:
    if not contents or b"\r" in contents or b"\x00" in contents:
        raise CorpusError(f"{label} is empty or contains a noncanonical control byte")
    try:
        parsed = json.loads(contents.decode("utf-8"), object_pairs_hook=reject_duplicate_members,
                            parse_constant=reject_non_finite)
    except (UnicodeDecodeError, json.JSONDecodeError) as exception:
        raise CorpusError(f"{label} is not strict UTF-8 JSON") from exception
    if not isinstance(parsed, dict):
        raise CorpusError(f"{label} must contain one JSON object")
    return parsed


def require_digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
        raise CorpusError(f"{label} must be a lowercase 256-bit digest")
    return value


def require_identifier(value: Any, label: str, length: int | None = None) -> str:
    if not isinstance(value, str) or not value or len(value) > 128 or \
            any(not (character.isascii() and (character.isalnum() or character in "-_.")) for character in value) or \
            (length is not None and len(value) != length):
        raise CorpusError(f"{label} is not a bounded portable identifier")
    return value


def require_lower_hex_identifier(value: Any, label: str, length: int) -> str:
    if not isinstance(value, str) or len(value) != length or \
            any(character not in "0123456789abcdef" for character in value):
        raise CorpusError(f"{label} must be {length} lowercase hexadecimal characters")
    return value


def require_nonnegative_integer(value: Any, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise CorpusError(f"{label} must be a nonnegative integer")
    return value


def require_number(value: Any, label: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value):
        raise CorpusError(f"{label} must be a finite JSON number")
    return float(value)


def require_ratio(value: Any, numerator: int, denominator: int, label: str) -> None:
    if denominator == 0:
        if value is not None:
            raise CorpusError(f"{label} must be null without a denominator")
        return
    actual = require_number(value, label)
    expected = numerator / denominator
    if not math.isclose(actual, expected, rel_tol=1e-15, abs_tol=1e-15):
        raise CorpusError(f"{label} disagrees with its integer numerator/denominator")


def is_path_redirect(path: Path) -> bool:
    is_junction = getattr(path, "is_junction", lambda: False)
    return path.is_symlink() or is_junction()


def read_stable_file(path: Path, maximum_bytes: int, label: str) -> bytes:
    if maximum_bytes <= 0 or is_path_redirect(path):
        raise CorpusError(f"{label} is not an allowed regular file")
    try:
        with path.open("rb") as stream:
            before = os.fstat(stream.fileno())
            if not stat_module.S_ISREG(before.st_mode) or before.st_size <= 0 or before.st_size > maximum_bytes:
                raise CorpusError(f"{label} size is outside 1..{maximum_bytes}")
            contents = stream.read(before.st_size + 1)
            after = os.fstat(stream.fileno())
        path_after = path.stat()
    except OSError as exception:
        raise CorpusError(f"cannot read {label}: {path}") from exception
    stable_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns")
    if len(contents) != before.st_size or \
            any(getattr(before, field) != getattr(after, field) for field in stable_fields) or \
            any(getattr(after, field) != getattr(path_after, field) for field in stable_fields):
        raise CorpusError(f"{label} changed while being read")
    return contents


def hash_stable_file(path: Path, maximum_bytes: int, label: str) -> dict[str, Any]:
    if maximum_bytes <= 0 or is_path_redirect(path):
        raise CorpusError(f"{label} is not an allowed regular file")
    sha256 = hashlib.sha256()
    blake3_digest = blake3.blake3()
    try:
        with path.open("rb") as stream:
            before = os.fstat(stream.fileno())
            if not stat_module.S_ISREG(before.st_mode) or before.st_size <= 0 or before.st_size > maximum_bytes:
                raise CorpusError(f"{label} size is outside 1..{maximum_bytes}")
            total = 0
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    break
                total += len(chunk)
                if total > maximum_bytes:
                    raise CorpusError(f"{label} exceeded its byte policy while hashing")
                sha256.update(chunk)
                blake3_digest.update(chunk)
            after = os.fstat(stream.fileno())
        path_after = path.stat()
    except OSError as exception:
        raise CorpusError(f"cannot hash {label}: {path}") from exception
    stable_fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns")
    if total != before.st_size or any(getattr(before, field) != getattr(after, field) for field in stable_fields) or \
            any(getattr(after, field) != getattr(path_after, field) for field in stable_fields):
        raise CorpusError(f"{label} changed while being hashed")
    return {"bytes": total, "sha256": sha256.hexdigest(), "blake3": blake3_digest.hexdigest()}


def resolve_artifact(root: Path, relative: Any, label: str) -> Path:
    if not isinstance(relative, str) or not relative or "\\" in relative:
        raise CorpusError(f"{label} path is not canonical POSIX-relative text")
    pure = PurePosixPath(relative)
    if pure.is_absolute() or any(part in {"", ".", ".."} for part in pure.parts):
        raise CorpusError(f"{label} path escapes its evidence root")
    try:
        if is_path_redirect(root):
            raise CorpusError(f"{label} evidence root is a symlink or junction")
        candidate = root
        for component in pure.parts:
            candidate /= component
            if is_path_redirect(candidate):
                raise CorpusError(f"{label} path contains a symlink or junction")
        resolved_root = root.resolve(strict=True)
        resolved = (resolved_root / Path(*pure.parts)).resolve(strict=True)
        resolved.relative_to(resolved_root)
    except CorpusError:
        raise
    except (OSError, ValueError) as exception:
        raise CorpusError(f"{label} path is missing or outside its evidence root") from exception
    if not resolved.is_file() or is_path_redirect(resolved):
        raise CorpusError(f"{label} path is not a regular non-symlink file")
    return resolved


def validate_artifact_metadata(value: Any, label: str) -> tuple[str, int, str]:
    if not isinstance(value, dict):
        raise CorpusError(f"{label} artifact metadata is missing")
    path = value.get("path")
    size = require_nonnegative_integer(value.get("bytes"), f"{label} bytes")
    digest = require_digest(value.get("sha256"), f"{label} sha256")
    if not isinstance(path, str) or not path or size == 0:
        raise CorpusError(f"{label} artifact path/size is invalid")
    return path, size, digest


def validate_python_seal(contents: bytes, schema: str, version: int, label: str) -> dict[str, Any]:
    report = parse_json(contents, label)
    if set(report) != {"schema", "version", "payloadBlake3", "payload"} or \
            report.get("schema") != schema or report.get("version") != version or \
            not isinstance(report.get("payload"), dict) or canonical_json(report) != contents:
        raise CorpusError(f"{label} is not the expected canonical sealed JSON")
    expected = require_digest(report.get("payloadBlake3"), f"{label} payloadBlake3")
    payload_bytes = canonical_json(report["payload"]).rstrip(b"\n")
    if blake3.blake3(payload_bytes).hexdigest() != expected:
        raise CorpusError(f"{label} payloadBlake3 mismatch")
    return report


def validate_plain_canonical(contents: bytes, schema: str, version: int, label: str) -> dict[str, Any]:
    report = parse_json(contents, label)
    if report.get("schema") != schema or report.get("version") != version or canonical_json(report) != contents:
        raise CorpusError(f"{label} is not the expected canonical JSON")
    return report


def validate_cpp_seal(contents: bytes, schema: str, version: int, label: str) -> tuple[dict[str, Any], bytes]:
    if not contents.endswith(b"\n") or b"\r" in contents:
        raise CorpusError(f"{label} is not LF-terminated canonical JSON")
    report = parse_json(contents, label)
    if list(report) != ["schema", "version", "payloadBlake3", "payload"] or \
            report.get("schema") != schema or report.get("version") != version or \
            not isinstance(report.get("payload"), dict):
        raise CorpusError(f"{label} has an unexpected envelope shape")
    marker = b',"payload":'
    body = contents[:-1]
    start = body.find(marker)
    if start < 0 or not body.endswith(b"}"):
        raise CorpusError(f"{label} has no final raw payload member")
    payload_bytes = body[start + len(marker):-1]
    expected = require_digest(report.get("payloadBlake3"), f"{label} payloadBlake3")
    if blake3.blake3(payload_bytes).hexdigest() != expected:
        raise CorpusError(f"{label} raw payloadBlake3 mismatch")
    return report, payload_bytes


def validate_summary(summary: Any, label: str, expected_frames: int) -> None:
    if not isinstance(summary, dict) or set(summary) != SUMMARY_FIELDS or \
            require_nonnegative_integer(summary.get("frames"), f"{label} frames") != expected_frames or \
            require_nonnegative_integer(summary.get("truthFrames"), f"{label} truthFrames") != expected_frames or \
            require_nonnegative_integer(summary.get("receiverOnlyFrames"), f"{label} receiverOnlyFrames") != 0:
        raise CorpusError(f"{label} does not have complete sender-truth coverage")
    integer_fields = (
        "modulationErasures", "verifiedFrames", "comparedBits", "bitErrors", "expectedZeroBits",
        "expectedOneBits", "zeroBitErrors", "oneBitErrors", "highConfidenceBits",
        "falseConfidenceErrors", "expectedTransportBlocks", "acceptedTransportBlocks",
        "falseAcceptedTransportBlocks", "acceptedControlBlocks", "falseAcceptedControlBlocks",
        "fecFailures", "crcFailures", "identityFailures", "iterationsTotal", "iterationsMaximum",
    )
    counters = {field: require_nonnegative_integer(summary.get(field), f"{label} {field}")
                for field in integer_fields}
    if counters["falseAcceptedTransportBlocks"] != 0 or counters["acceptedControlBlocks"] != 0 or \
            counters["falseAcceptedControlBlocks"] != 0:
        raise CorpusError(f"{label} violates the zero false-accept/control boundary")
    if expected_frames == 0:
        if any(counters.values()) or any(summary.get(field) is not None for field in (
                "frameErrorRate", "bitErrorRate", "averageLogLoss", "averageBrierScore",
                "expectedCalibrationError")) or summary.get("reliability") != [] or \
                summary.get("confidenceCurve") != [] or summary.get("rocCurve") != []:
            raise CorpusError(f"{label} unopened split summary is not canonically empty")
        return

    modulation_frames = expected_frames - counters["modulationErasures"]
    if modulation_frames < 0 or counters["verifiedFrames"] > modulation_frames or \
            counters["expectedTransportBlocks"] != expected_frames * LF4_CODEWORDS or \
            counters["comparedBits"] != modulation_frames * LF4_CODED_BITS or \
            counters["expectedZeroBits"] + counters["expectedOneBits"] != counters["comparedBits"] or \
            counters["zeroBitErrors"] + counters["oneBitErrors"] != counters["bitErrors"] or \
            counters["bitErrors"] > counters["comparedBits"] or \
            counters["acceptedTransportBlocks"] + counters["fecFailures"] + counters["crcFailures"] + \
            counters["identityFailures"] != modulation_frames * LF4_CODEWORDS or \
            counters["iterationsMaximum"] > counters["iterationsTotal"]:
        raise CorpusError(f"{label} count/denominator invariants are inconsistent")
    require_ratio(summary.get("frameErrorRate"), expected_frames - counters["verifiedFrames"],
                  expected_frames, f"{label} frameErrorRate")
    require_ratio(summary.get("bitErrorRate"), counters["bitErrors"], counters["comparedBits"],
                  f"{label} bitErrorRate")
    for field in ("averageLogLoss", "averageBrierScore", "expectedCalibrationError"):
        value = require_number(summary.get(field), f"{label} {field}")
        if value < 0 or (field != "averageLogLoss" and value > 1):
            raise CorpusError(f"{label} {field} is outside its probability-score range")

    reliability = summary.get("reliability")
    if not isinstance(reliability, list) or len(reliability) != len(RELIABILITY_PROBABILITY_UPPER):
        raise CorpusError(f"{label} is missing its reliability curve")
    reliability_samples = 0
    reliability_errors = 0
    reliability_ece = 0.0
    for index, expected_upper in enumerate(RELIABILITY_PROBABILITY_UPPER):
        bin_value = reliability[index]
        if not isinstance(bin_value, dict) or set(bin_value) != {
                "probabilityUpper", "samples", "errors", "predictedErrorAverage", "observedErrorRate"} or \
                require_number(bin_value.get("probabilityUpper"), f"{label} reliability upper") != expected_upper:
            raise CorpusError(f"{label} reliability bin shape/order drifted")
        samples = require_nonnegative_integer(bin_value.get("samples"), f"{label} reliability samples")
        errors = require_nonnegative_integer(bin_value.get("errors"), f"{label} reliability errors")
        if errors > samples:
            raise CorpusError(f"{label} reliability errors exceed samples")
        require_ratio(bin_value.get("observedErrorRate"), errors, samples,
                      f"{label} reliability observedErrorRate")
        predicted = bin_value.get("predictedErrorAverage")
        if samples == 0:
            if predicted is not None:
                raise CorpusError(f"{label} empty reliability bin has a predicted average")
        else:
            predicted_value = require_number(predicted, f"{label} reliability predictedErrorAverage")
            if predicted_value < 0 or predicted_value > expected_upper + 1e-15:
                raise CorpusError(f"{label} reliability predicted average is outside its bin")
            observed_value = errors / samples
            reliability_ece += abs(observed_value - predicted_value) * samples / counters["comparedBits"]
        reliability_samples += samples
        reliability_errors += errors
    if reliability_samples != counters["comparedBits"] or reliability_errors != counters["bitErrors"] or \
            not math.isclose(require_number(summary.get("expectedCalibrationError"),
                                             f"{label} expectedCalibrationError"),
                             reliability_ece, rel_tol=1e-15, abs_tol=1e-15):
        raise CorpusError(f"{label} reliability curve totals disagree with bit truth")

    confidence = summary.get("confidenceCurve")
    if not isinstance(confidence, list) or len(confidence) != len(CONFIDENCE_MAGNITUDE_THRESHOLDS):
        raise CorpusError(f"{label} is missing its confidence curve")
    previous_samples = counters["comparedBits"] + 1
    for index, expected_threshold in enumerate(CONFIDENCE_MAGNITUDE_THRESHOLDS):
        point = confidence[index]
        if not isinstance(point, dict) or set(point) != {"minimumMagnitude", "samples", "errors", "errorRate"} or \
                require_number(point.get("minimumMagnitude"), f"{label} confidence threshold") != expected_threshold:
            raise CorpusError(f"{label} confidence point shape/order drifted")
        samples = require_nonnegative_integer(point.get("samples"), f"{label} confidence samples")
        errors = require_nonnegative_integer(point.get("errors"), f"{label} confidence errors")
        if errors > samples or samples > previous_samples:
            raise CorpusError(f"{label} confidence curve is not cumulative-monotone")
        require_ratio(point.get("errorRate"), errors, samples, f"{label} confidence errorRate")
        previous_samples = samples
    if confidence[0]["samples"] != counters["comparedBits"] or confidence[0]["errors"] != counters["bitErrors"] or \
            confidence[-2]["samples"] != counters["highConfidenceBits"] or \
            confidence[-2]["errors"] != counters["falseConfidenceErrors"]:
        raise CorpusError(f"{label} confidence curve totals disagree with summary counters")

    roc = summary.get("rocCurve")
    if not isinstance(roc, list) or len(roc) != len(ROC_ONE_SCORE_THRESHOLDS):
        raise CorpusError(f"{label} is missing its ROC curve")
    previous_true = counters["expectedOneBits"] + 1
    previous_false = counters["expectedZeroBits"] + 1
    for index, expected_threshold in enumerate(ROC_ONE_SCORE_THRESHOLDS):
        point = roc[index]
        if not isinstance(point, dict) or set(point) != {
                "minimumOneScore", "truePositive", "falsePositive", "truePositiveRate", "falsePositiveRate"} or \
                require_number(point.get("minimumOneScore"), f"{label} ROC threshold") != expected_threshold:
            raise CorpusError(f"{label} ROC point shape/order drifted")
        true_positive = require_nonnegative_integer(point.get("truePositive"), f"{label} ROC truePositive")
        false_positive = require_nonnegative_integer(point.get("falsePositive"), f"{label} ROC falsePositive")
        if true_positive > previous_true or false_positive > previous_false:
            raise CorpusError(f"{label} ROC curve is not threshold-monotone")
        require_ratio(point.get("truePositiveRate"), true_positive, counters["expectedOneBits"],
                      f"{label} ROC truePositiveRate")
        require_ratio(point.get("falsePositiveRate"), false_positive, counters["expectedZeroBits"],
                      f"{label} ROC falsePositiveRate")
        previous_true = true_positive
        previous_false = false_positive
    if roc[0]["truePositive"] != counters["expectedOneBits"] or \
            roc[0]["falsePositive"] != counters["expectedZeroBits"] or \
            roc[-1]["truePositive"] != 0 or roc[-1]["falsePositive"] != 0:
        raise CorpusError(f"{label} ROC endpoints do not cover the exact bit classes")


def improves_without_regression(candidate: dict[str, Any], baseline: dict[str, Any], label: str) -> None:
    integer_nonregression = (
        ("verifiedFrames", lambda first, second: first >= second),
        ("acceptedTransportBlocks", lambda first, second: first >= second),
        ("modulationErasures", lambda first, second: first <= second),
        ("falseConfidenceErrors", lambda first, second: first <= second),
        ("fecFailures", lambda first, second: first <= second),
        ("crcFailures", lambda first, second: first <= second),
        ("identityFailures", lambda first, second: first <= second),
        ("iterationsTotal", lambda first, second: first <= second),
        ("iterationsMaximum", lambda first, second: first <= second),
        ("comparedBits", lambda first, second: first >= second),
    )
    strict = False
    if require_nonnegative_integer(candidate.get("expectedTransportBlocks"),
                                   f"{label} selected expectedTransportBlocks") != \
            require_nonnegative_integer(baseline.get("expectedTransportBlocks"),
                                        f"{label} baseline expectedTransportBlocks"):
        raise CorpusError(f"{label} selected candidate changes its expected Transport denominator")
    for field, predicate in integer_nonregression:
        first = require_nonnegative_integer(candidate.get(field), f"{label} selected {field}")
        second = require_nonnegative_integer(baseline.get(field), f"{label} baseline {field}")
        if not predicate(first, second):
            raise CorpusError(f"{label} selected candidate regresses {field}")
        strict = strict or first != second
    for field in ("bitErrorRate", "averageLogLoss", "averageBrierScore", "expectedCalibrationError"):
        first = require_number(candidate.get(field), f"{label} selected {field}")
        second = require_number(baseline.get(field), f"{label} baseline {field}")
        if first > second + 1e-15:
            raise CorpusError(f"{label} selected candidate regresses {field}")
        strict = strict or first + 1e-15 < second
    if not strict:
        raise CorpusError(f"{label} has no strict selected-candidate improvement")


def validate_metric_model(model: dict[str, Any], candidate_id: str) -> None:
    model_kind = model.get("kind")
    global_scale = require_number(model.get("globalScale"), f"{candidate_id} globalScale")
    signal_scales = model.get("signalScales")
    piecewise = model.get("piecewise")
    if global_scale not in GLOBAL_SCALE_CANDIDATES or not isinstance(signal_scales, list) or \
            not isinstance(piecewise, list):
        raise CorpusError(f"{candidate_id} metric model has an invalid scale or binding container")
    if model_kind == "Raw":
        if global_scale != 1 or signal_scales or piecewise:
            raise CorpusError(f"{candidate_id} Raw metric model is not identity")
        return
    if model_kind == "GlobalScale":
        if signal_scales or piecewise:
            raise CorpusError(f"{candidate_id} GlobalScale model has unexpected bindings")
        return
    if model_kind == "PerSignalScale":
        expected_profiles = {
            "PB-Signal-SyntheticBgra-1",
            "PB-Signal-Video709Limited420-1",
            "PB-Signal-Video709Limited444-1",
        }
        if piecewise or any(not isinstance(binding, dict) for binding in signal_scales) or \
                signal_scales != sorted(signal_scales, key=lambda binding: binding.get("signalProfile")):
            raise CorpusError(f"{candidate_id} PerSignalScale bindings are noncanonical")
        profiles: set[str] = set()
        for binding in signal_scales:
            if not isinstance(binding, dict) or set(binding) != {"signalProfile", "scale"}:
                raise CorpusError(f"{candidate_id} PerSignalScale binding is malformed")
            profile = require_identifier(binding.get("signalProfile"), f"{candidate_id} signalProfile")
            scale = require_number(binding.get("scale"), f"{candidate_id} signal scale")
            if scale not in GLOBAL_SCALE_CANDIDATES or profile in profiles:
                raise CorpusError(f"{candidate_id} PerSignalScale binding is duplicate or off-grid")
            profiles.add(profile)
        if profiles != expected_profiles:
            raise CorpusError(f"{candidate_id} PerSignalScale train-profile coverage drifted")
        return
    if model_kind != "PiecewiseLookup" or global_scale != 1 or signal_scales or \
            len(piecewise) != len(PIECEWISE_RAW_MAGNITUDE_UPPER):
        raise CorpusError(f"{candidate_id} PiecewiseLookup model is malformed")
    previous_magnitude = -1.0
    total_samples = 0
    for index, expected_upper in enumerate(PIECEWISE_RAW_MAGNITUDE_UPPER):
        binding = piecewise[index]
        if not isinstance(binding, dict) or set(binding) != {
                "rawMagnitudeUpper", "calibratedMagnitude", "trainingSamples", "trainingErrors"} or \
                require_number(binding.get("rawMagnitudeUpper"), f"{candidate_id} piecewise upper") != expected_upper:
            raise CorpusError(f"{candidate_id} PiecewiseLookup bin shape/order drifted")
        magnitude = require_number(binding.get("calibratedMagnitude"),
                                   f"{candidate_id} calibrated magnitude")
        samples = require_nonnegative_integer(binding.get("trainingSamples"),
                                              f"{candidate_id} training samples")
        errors = require_nonnegative_integer(binding.get("trainingErrors"),
                                             f"{candidate_id} training errors")
        if magnitude < previous_magnitude or magnitude < 0 or magnitude > CALIBRATED_MAGNITUDE_CLIP or \
                errors > samples:
            raise CorpusError(f"{candidate_id} PiecewiseLookup bin is nonmonotone or inconsistent")
        previous_magnitude = magnitude
        total_samples += samples
    if total_samples == 0:
        raise CorpusError(f"{candidate_id} PiecewiseLookup has no Train samples")


def better_on_validation(candidate: dict[str, Any], selected: dict[str, Any]) -> bool:
    first = candidate["validation"]
    second = selected["validation"]
    for field, prefer_greater in (
            ("verifiedFrames", True),
            ("acceptedTransportBlocks", True),
            ("modulationErasures", False),
            ("falseConfidenceErrors", False)):
        first_value = require_nonnegative_integer(first.get(field), f"candidate validation {field}")
        second_value = require_nonnegative_integer(second.get(field), f"selected validation {field}")
        if first_value != second_value:
            return first_value > second_value if prefer_greater else first_value < second_value
    for field in ("averageLogLoss", "expectedCalibrationError"):
        first_value = require_number(first.get(field), f"candidate validation {field}")
        second_value = require_number(second.get(field), f"selected validation {field}")
        if abs(first_value - second_value) > 1e-15:
            return first_value < second_value
    return require_nonnegative_integer(first.get("iterationsTotal"), "candidate validation iterations") < \
        require_nonnegative_integer(second.get("iterationsTotal"), "selected validation iterations")


def validate_metric_report(contents: bytes, expected: dict[str, Any]) -> dict[str, Any]:
    outer, outer_payload_bytes = validate_cpp_seal(contents, CALIBRATION_EVIDENCE_SCHEMA, 1,
                                                   "metric calibration evidence")
    outer_payload = outer["payload"]
    if list(outer_payload) != ["inputs", "calibration", "gate"]:
        raise CorpusError("metric calibration evidence payload has an unexpected shape")
    inputs = outer_payload["inputs"]
    if not isinstance(inputs, dict) or inputs.get("presenterRasterBlake3") != expected["presenterBlake3"] or \
            inputs.get("providerSpecificThresholds") is not False or \
            inputs.get("realReplayTruthProvenance") != "SealedPresenterRasterPlusReplayIdentity":
        raise CorpusError("metric calibration evidence input truth boundary is invalid")
    identity = inputs.get("realReplayIdentity")
    if not isinstance(identity, dict) or identity.get("collectionDatasetId") != expected["collectionDatasetId"] or \
            identity.get("replayDatasetId") != expected["replayDatasetId"] or identity.get("runId") != expected["runId"]:
        raise CorpusError("metric calibration evidence Replay identity mismatch")
    resource_policy = inputs.get("resourcePolicy")
    if resource_policy != {"maximumCodecFileBytes": 134217728, "maximumTotalCodecBytes": 536870912,
                           "maximumReplayBytes": 134217728, "maximumReplayFrames": 128}:
        raise CorpusError("metric calibration evidence resource policy drifted")

    artifacts = inputs.get("artifacts")
    if not isinstance(artifacts, list) or any(not isinstance(item, dict) for item in artifacts) or \
            artifacts != sorted(artifacts, key=lambda item: (item.get("kind"), item.get("runId"))):
        raise CorpusError("metric calibration artifacts are missing or noncanonical")
    observed_artifacts: dict[tuple[str, str], dict[str, Any]] = {}
    for artifact in artifacts:
        if not isinstance(artifact, dict) or set(artifact) != {"kind", "runId", "bytes", "blake3"}:
            raise CorpusError("metric calibration artifact entry is malformed")
        key = (require_identifier(artifact.get("kind"), "calibration artifact kind"),
               require_identifier(artifact.get("runId"), "calibration artifact runId"))
        if key in observed_artifacts or require_nonnegative_integer(artifact.get("bytes"), "artifact bytes") == 0:
            raise CorpusError("metric calibration artifact identity is duplicate or empty")
        require_digest(artifact.get("blake3"), "calibration artifact blake3")
        observed_artifacts[key] = artifact
    if {run for kind, run in observed_artifacts if kind == "SimulatorManifest"} != SIMULATOR_RUNS or \
            {run for kind, run in observed_artifacts if kind == "SimulatorOutput"} != SIMULATOR_RUNS:
        raise CorpusError("metric calibration simulator sweep is incomplete")
    for codec in expected["codecs"]:
        artifact = observed_artifacts.get(("ActualCodecGray8", codec["name"]))
        if artifact is None or artifact["bytes"] != codec["bytes"] or artifact["blake3"] != codec["blake3"]:
            raise CorpusError(f"metric calibration codec artifact mismatch: {codec['name']}")
    replay_artifact = observed_artifacts.get(("RealReceiverReplayV2", expected["runId"]))
    if replay_artifact is None or replay_artifact["bytes"] != expected["replay"]["bytes"] or \
            replay_artifact["blake3"] != expected["replay"]["blake3"] or len(observed_artifacts) != 43:
        raise CorpusError("metric calibration real Replay artifact mismatch")

    calibration_marker = b',"calibration":'
    gate_marker = b',"gate":'
    calibration_start = outer_payload_bytes.find(calibration_marker)
    gate_start = outer_payload_bytes.find(gate_marker, calibration_start + 1)
    if calibration_start < 0 or gate_start < 0:
        raise CorpusError("metric calibration core envelope cannot be isolated")
    core_bytes = outer_payload_bytes[calibration_start + len(calibration_marker):gate_start] + b"\n"
    core, _ = validate_cpp_seal(core_bytes, CALIBRATION_CORE_SCHEMA, 1, "metric calibration core")
    if core != outer_payload.get("calibration"):
        raise CorpusError("parsed and raw metric calibration core disagree")
    payload = core["payload"]
    if list(payload) != ["input", "truthBoundary", "selection", "candidates"]:
        raise CorpusError("metric calibration core payload has an unexpected shape")
    truth = payload["truthBoundary"]
    if truth != {"senderTruthEntersDemodulation": False, "senderTruthEntersFec": False,
                 "postAdmissionTruthScoring": True, "receiverOnlyFalseAcceptance": "Unavailable",
                 "acceptanceAuthority": "QC-LDPC+padding+TransportCRC+SessionIdentity",
                 "wholeFileOutputEvaluated": False, "acceptedOutputObjects": 0,
                 "falseAcceptedOutputObjects": 0}:
        raise CorpusError("metric calibration truth/acceptance boundary drifted")
    input_summary = payload["input"]
    if require_nonnegative_integer(input_summary.get("observations"), "input observations") != 264 or \
            require_nonnegative_integer(input_summary.get("frames"), "input frames") != 44 or \
            input_summary.get("splitUnit") != "datasetId/runId":
        raise CorpusError("metric calibration input cardinality or split unit drifted")
    require_digest(input_summary.get("blake3"), "calibration input blake3")
    groups = input_summary.get("datasetGroups")
    if not isinstance(groups, list) or len(groups) != 25 or any(not isinstance(group, dict) for group in groups) or \
            groups != sorted(groups, key=lambda group: (group.get("datasetId"), group.get("runId"))):
        raise CorpusError("metric calibration dataset/run group inventory is incomplete")
    observed_groups: dict[tuple[str, str], dict[str, Any]] = {}
    for group in groups:
        if not isinstance(group, dict) or set(group) != {"datasetId", "runId", "signalProfile", "split",
                                                         "truthAvailability", "frames"} or \
                group.get("split") not in {"Train", "Validation", "Holdout", "External"} or \
                group.get("truthAvailability") != "DiagnosticSenderFixture":
            raise CorpusError("metric calibration dataset group is malformed or lacks truth")
        key = (require_identifier(group.get("datasetId"), "group datasetId"),
               require_identifier(group.get("runId"), "group runId"))
        if key in observed_groups:
            raise CorpusError("metric calibration dataset/run group is duplicated across splits")
        observed_groups[key] = group
    expected_groups: dict[tuple[str, str], dict[str, Any]] = {}
    for run_id in SIMULATOR_RUNS:
        split = "Train" if "-train-" in run_id else "Validation" if "-validation-" in run_id else "Holdout"
        expected_groups[("step07-simulator-v1", run_id)] = {
            "datasetId": "step07-simulator-v1", "runId": run_id,
            "signalProfile": "PB-Signal-SyntheticBgra-1", "split": split,
            "truthAvailability": "DiagnosticSenderFixture", "frames": 1,
        }
    for run_id, split, signal_profile in CODEC_SPLITS:
        expected_groups[("step06-actual-codec-v2", run_id)] = {
            "datasetId": "step06-actual-codec-v2", "runId": run_id,
            "signalProfile": signal_profile, "split": split.capitalize(),
            "truthAvailability": "DiagnosticSenderFixture", "frames": 3,
        }
    expected_groups[(expected["collectionDatasetId"], expected["runId"])] = {
        "datasetId": expected["collectionDatasetId"], "runId": expected["runId"],
        "signalProfile": "PB-Signal-RdpUnknown-1", "split": "External",
        "truthAvailability": "DiagnosticSenderFixture", "frames": 8,
    }
    if observed_groups != expected_groups:
        raise CorpusError("metric calibration dataset/run split manifest drifted")

    selection = payload["selection"]
    required_selection = {"splitIsolationValid": True, "selectionUsedHoldout": False,
                          "acceptanceAuthorityChanged": False, "productionDefaultsChanged": False,
                          "holdoutImproved": True, "externalImproved": True, "gatePassed": True}
    if not isinstance(selection, dict) or any(selection.get(key) is not value for key, value in required_selection.items()) or \
            selection.get("baselineCandidateId") != "lf4-default/Raw" or \
            selection.get("selectedCandidateId") != "lf4-default/PiecewiseLookup":
        raise CorpusError("metric calibration selection/Gate boundary failed")
    candidates = payload["candidates"]
    if not isinstance(candidates, list) or len(candidates) != len(POLICIES) * len(MODELS):
        raise CorpusError("metric calibration candidate matrix is incomplete")
    candidate_map: dict[str, dict[str, Any]] = {}
    observed_matrix: set[tuple[str, str]] = set()
    evaluated_ids = {selection["baselineCandidateId"], selection["selectedCandidateId"]}
    expected_candidate_ids = [f"{policy_id}/{model_kind}" for policy_id in sorted(POLICIES)
                              for model_kind in MODEL_ORDER]
    if [candidate.get("candidateId") if isinstance(candidate, dict) else None for candidate in candidates] != \
            expected_candidate_ids:
        raise CorpusError("metric calibration candidate order drifted")
    for candidate in candidates:
        if not isinstance(candidate, dict) or set(candidate) != {
                "candidateId", "policyId", "policyManifestJson",
                "deployableWithoutSignalProfileBinding", "policyRelaxesDefaultAdmission", "model",
                "train", "validation", "holdout", "external"}:
            raise CorpusError("metric calibration candidate is malformed")
        candidate_id = candidate.get("candidateId")
        if not isinstance(candidate_id, str) or not candidate_id or len(candidate_id) > 257:
            raise CorpusError("metric calibration candidateId is invalid")
        policy_id = require_identifier(candidate.get("policyId"), "policyId")
        model = candidate.get("model")
        model_kind = model.get("kind") if isinstance(model, dict) else None
        if policy_id not in POLICIES or model_kind not in MODELS or set(model) != {
                "kind", "globalScale", "signalScales", "piecewise"} or \
                candidate_id != f"{policy_id}/{model_kind}" or \
                candidate.get("policyRelaxesDefaultAdmission") is not POLICIES[policy_id] or \
                candidate.get("deployableWithoutSignalProfileBinding") is not (model_kind != "PerSignalScale") or \
                candidate_id in candidate_map or (policy_id, model_kind) in observed_matrix:
            raise CorpusError("metric calibration policy/model candidate identity is inconsistent")
        if candidate.get("policyManifestJson") != POLICY_MANIFESTS[policy_id]:
            raise CorpusError("metric calibration candidate parameter manifest drifted")
        parse_json(candidate["policyManifestJson"].encode("utf-8"), "policy manifest")
        validate_metric_model(model, candidate_id)
        validate_summary(candidate.get("train"), f"{candidate_id} Train", 12)
        validate_summary(candidate.get("validation"), f"{candidate_id} Validation", 12)
        if candidate_id in evaluated_ids:
            validate_summary(candidate.get("holdout"), f"{candidate_id} Holdout", 12)
            validate_summary(candidate.get("external"), f"{candidate_id} External", 8)
        else:
            validate_summary(candidate.get("holdout"), f"{candidate_id} unopened Holdout", 0)
            validate_summary(candidate.get("external"), f"{candidate_id} unopened External", 0)
        candidate_map[candidate_id] = candidate
        observed_matrix.add((policy_id, model_kind))
    if observed_matrix != {(policy, model) for policy in POLICIES for model in MODELS}:
        raise CorpusError("metric calibration policy/model matrix drifted")
    validation_selected = candidate_map[selection["baselineCandidateId"]]
    for candidate in candidates:
        if candidate["deployableWithoutSignalProfileBinding"] and not candidate["policyRelaxesDefaultAdmission"] and \
                better_on_validation(candidate, validation_selected):
            validation_selected = candidate
    if validation_selected["candidateId"] != selection["selectedCandidateId"]:
        raise CorpusError("metric calibration selected candidate is not the Validation-only winner")
    selected = candidate_map[selection["selectedCandidateId"]]
    baseline = candidate_map[selection["baselineCandidateId"]]
    if selected.get("policyRelaxesDefaultAdmission") is not False or \
            selected.get("deployableWithoutSignalProfileBinding") is not True:
        raise CorpusError("selected metric candidate widens admission or requires an unavailable signal binding")
    improves_without_regression(selected["holdout"], baseline["holdout"], "Holdout")
    improves_without_regression(selected["external"], baseline["external"], "External")
    gate = outer_payload["gate"]
    if gate != {"passed": True, "productionDefaultsChanged": False,
                "deploymentDisposition": "CandidateOnlyRequiresStep08Freeze"}:
        raise CorpusError("outer metric calibration Gate/disposition is invalid")
    return {
        "payloadBlake3": outer["payloadBlake3"],
        "corePayloadBlake3": core["payloadBlake3"],
        "inputBlake3": input_summary["blake3"],
        "baselineCandidateId": selection["baselineCandidateId"],
        "selectedCandidateId": selection["selectedCandidateId"],
        "selectedModel": selected["model"],
        "holdoutBaseline": baseline["holdout"],
        "holdoutSelected": selected["holdout"],
        "externalBaseline": baseline["external"],
        "externalSelected": selected["external"],
    }


def load_step06(index_path: Path) -> dict[str, Any]:
    if is_path_redirect(index_path):
        raise CorpusError("Step 06 index must not be a symlink or junction")
    index_path = index_path.resolve(strict=True)
    root = index_path.parent
    index_bytes = read_stable_file(index_path, MAX_INDEX_BYTES, "Step 06 index")
    index = validate_python_seal(index_bytes, STEP06_SCHEMA, 2, "Step 06 index")
    summary = index["payload"].get("summary")
    if not isinstance(summary, dict) or summary.get("productionAdmissionSafe") is not True or \
            summary.get("expectationsMatched") is not True or summary.get("actualCodecCases") != 6 or \
            summary.get("actualCodecFalseAcceptedCodewords") != 0:
        raise CorpusError("Step 06 index is not a safe completed corpus")
    indexed_artifacts = index["payload"].get("artifacts")
    if not isinstance(indexed_artifacts, dict):
        raise CorpusError("Step 06 index artifact inventory is malformed")
    manifest_metadata = indexed_artifacts.get("actualCodecManifest")
    manifest_relative, manifest_bytes_expected, manifest_sha256 = validate_artifact_metadata(
        manifest_metadata, "Step 06 actual codec manifest")
    manifest_path = resolve_artifact(root, manifest_relative, "Step 06 actual codec manifest")
    manifest_bytes = read_stable_file(manifest_path, MAX_INDEX_BYTES, "actual codec manifest")
    manifest_identity = hash_stable_file(manifest_path, MAX_INDEX_BYTES, "actual codec manifest")
    if manifest_identity["bytes"] != manifest_bytes_expected or manifest_identity["sha256"] != manifest_sha256:
        raise CorpusError("actual codec manifest disagrees with the Step 06 index")
    manifest = validate_plain_canonical(manifest_bytes, CODEC_SCHEMA, 2, "actual codec manifest")
    cases = manifest.get("cases")
    if not isinstance(cases, list) or len(cases) != len(CODEC_SPLITS) or \
            any(not isinstance(case, dict) for case in cases) or \
            {case.get("name") for case in cases} != {name for name, _, _ in CODEC_SPLITS}:
        raise CorpusError("actual codec manifest case inventory drifted")
    case_map = {case["name"]: case for case in cases}
    codecs = []
    tracked_files: list[tuple[Path, int, str, dict[str, Any]]] = []
    for name, split, signal_profile in CODEC_SPLITS:
        case = case_map[name]
        evaluation = case.get("evaluationSummary")
        if not isinstance(evaluation, dict) or evaluation.get("truthBoundaryValid") is not True or \
                evaluation.get("falseAcceptedCodewords") != 0 or evaluation.get("frameCount") != 3:
            raise CorpusError(f"actual codec case is not truth-safe: {name}")
        case_artifacts = case.get("artifacts")
        if not isinstance(case_artifacts, dict):
            raise CorpusError(f"actual codec artifact inventory is malformed: {name}")
        decoded_relative, decoded_bytes, decoded_sha256 = validate_artifact_metadata(
            case_artifacts.get("decoded"), f"{name} decoded Gray8")
        if decoded_relative != f"{name}/decoded.gray" or decoded_bytes != GRAY_FRAME_BYTES * 3:
            raise CorpusError(f"actual codec decoded framing/path drifted: {name}")
        decoded_path = resolve_artifact(manifest_path.parent, decoded_relative, f"{name} decoded Gray8")
        identity = hash_stable_file(decoded_path, 128 * 1024 * 1024, f"{name} decoded Gray8")
        if identity["bytes"] != decoded_bytes or identity["sha256"] != decoded_sha256:
            raise CorpusError(f"actual codec decoded artifact digest mismatch: {name}")
        codecs.append({"name": name, "split": split, "signalProfile": signal_profile,
                       "path": decoded_path, **identity})
        tracked_files.append((decoded_path, 128 * 1024 * 1024, f"{name} decoded Gray8", identity))
    return {
        "index": index,
        "indexPath": index_path,
        "indexIdentity": {**hash_stable_file(index_path, MAX_INDEX_BYTES, "Step 06 index"),
                          "payloadBlake3": index["payloadBlake3"], "schema": STEP06_SCHEMA, "version": 2},
        "manifestPath": manifest_path,
        "manifestIdentity": {**manifest_identity, "schema": CODEC_SCHEMA, "version": 2},
        "codecs": codecs,
        "trackedFiles": tracked_files,
    }


def load_step02(index_path: Path, completion_path: Path) -> dict[str, Any]:
    if is_path_redirect(index_path) or is_path_redirect(completion_path):
        raise CorpusError("Step 02 index/completion must not be a symlink or junction")
    index_path = index_path.resolve(strict=True)
    completion_path = completion_path.resolve(strict=True)
    if index_path.parent != completion_path.parent:
        raise CorpusError("Step 02 index and completion summary must share one evidence root")
    root = index_path.parent
    index_bytes = read_stable_file(index_path, MAX_INDEX_BYTES, "Step 02 dataset index")
    completion_bytes = read_stable_file(completion_path, MAX_INDEX_BYTES, "Step 02 completion summary")
    index = validate_python_seal(index_bytes, STEP02_INDEX_SCHEMA, 1, "Step 02 dataset index")
    completion = validate_python_seal(completion_bytes, STEP02_COMPLETION_SCHEMA, 1,
                                      "Step 02 completion summary")
    completion_payload = completion["payload"]
    dataset_seal = completion_payload.get("datasetSeal")
    index_identity = hash_stable_file(index_path, MAX_INDEX_BYTES, "Step 02 dataset index")
    if not isinstance(dataset_seal, dict) or dataset_seal.get("byteIdentical") is not True or \
            dataset_seal.get("indexSha256") != index_identity["sha256"] or \
            index_path.name not in {dataset_seal.get("indexAPath"), dataset_seal.get("indexBPath")}:
        raise CorpusError("Step 02 completion summary does not bind the selected dataset index")
    collection_id = require_lower_hex_identifier(index["payload"].get("datasetId"),
                                                 "Step 02 collection datasetId", 32)
    artifacts = index["payload"].get("artifacts")
    lf4_artifacts = [artifact for artifact in artifacts if isinstance(artifact, dict) and
                     artifact.get("profile") == LF4_PROFILE] if isinstance(artifacts, list) else []
    if len(lf4_artifacts) != 1:
        raise CorpusError("Step 02 dataset index has no unique LF4 Replay")
    lf4_artifact = lf4_artifacts[0]
    if lf4_artifact.get("kind") != "ReplayV2" or lf4_artifact.get("provenance") != "RealRemoteRender" or \
            lf4_artifact.get("captureScope") != "SelectedRoiOnly" or \
            lf4_artifact.get("containsProtectedMonitorPixels") is not False or \
            lf4_artifact.get("semanticValidation") != "RequiredViaReplayV2ReaderBeforeEvidenceUse":
        raise CorpusError("Step 02 LF4 Replay provenance/safety boundary is invalid")
    replay_relative, replay_bytes, replay_sha256 = validate_artifact_metadata(lf4_artifact, "Step 02 LF4 Replay")
    replay_blake3 = require_digest(lf4_artifact.get("blake3"), "Step 02 LF4 Replay blake3")
    replay_path = resolve_artifact(root, replay_relative, "Step 02 LF4 Replay")
    replay_identity = hash_stable_file(replay_path, MAX_REPLAY_BYTES, "Step 02 LF4 Replay")
    if replay_identity != {"bytes": replay_bytes, "sha256": replay_sha256, "blake3": replay_blake3}:
        raise CorpusError("Step 02 LF4 Replay disagrees with its sealed dataset index")

    profiles = completion_payload.get("profiles")
    lf4_profiles = [profile for profile in profiles if isinstance(profile, dict) and profile.get("name") == "LF4"] \
        if isinstance(profiles, list) else []
    presenter = completion_payload.get("presenter")
    presenter_hashes = presenter.get("descriptorHashes") if isinstance(presenter, dict) else None
    if len(lf4_profiles) != 1 or not isinstance(presenter_hashes, dict):
        raise CorpusError("Step 02 completion summary has no unique LF4 profile/presenter")
    profile = lf4_profiles[0]
    presenter_blake3 = require_digest(presenter_hashes.get("LF4"), "Step 02 LF4 Presenter raster")
    if profile.get("replay") != {"bytes": replay_bytes, "path": replay_relative, "sha256": replay_sha256}:
        raise CorpusError("Step 02 completion LF4 Replay disagrees with the dataset index")
    run_id = require_identifier(profile.get("runId"), "Step 02 LF4 runId", 32)
    inspection = profile.get("offlineInspection")
    if not isinstance(inspection, dict) or inspection.get("byteIdentical") is not True or \
            inspection.get("falseAcceptedAvailability") != "UnavailableReceiverOnly" or \
            inspection.get("falseAcceptedCodewords") is not None:
        raise CorpusError("Step 02 LF4 offline inspection truth boundary is invalid")
    inspection_path = resolve_artifact(root, inspection.get("run1Path"), "Step 02 LF4 inspection")
    inspection_bytes = read_stable_file(inspection_path, MAX_INDEX_BYTES, "Step 02 LF4 inspection")
    inspection_identity = hash_stable_file(inspection_path, MAX_INDEX_BYTES, "Step 02 LF4 inspection")
    if inspection_identity["sha256"] != inspection.get("run1Sha256"):
        raise CorpusError("Step 02 LF4 inspection digest mismatch")
    inspection_report, _ = validate_cpp_seal(inspection_bytes, REPLAY_INSPECTION_SCHEMA, 1,
                                             "Step 02 LF4 inspection")
    descriptor = inspection_report["payload"].get("descriptor")
    if not isinstance(descriptor, dict):
        raise CorpusError("Step 02 LF4 inspection descriptor is malformed")
    replay_dataset_id = require_lower_hex_identifier(
        descriptor.get("datasetId"), "Step 02 LF4 Replay datasetId", 32)
    if descriptor.get("runId") != run_id or descriptor.get("visualProfile") != LF4_PROFILE or \
            descriptor.get("visualProfileId") != LF4_PROFILE_ID or descriptor.get("visualLayoutVersion") != LF4_LAYOUT_VERSION:
        raise CorpusError("Step 02 LF4 inspection descriptor identity drifted")
    return {
        "collectionDatasetId": collection_id,
        "replayDatasetId": replay_dataset_id,
        "runId": run_id,
        "presenterBlake3": presenter_blake3,
        "replayPath": replay_path,
        "replay": replay_identity,
        "indexPath": index_path,
        "indexIdentity": {**index_identity, "payloadBlake3": index["payloadBlake3"],
                          "schema": STEP02_INDEX_SCHEMA, "version": 1},
        "completionPath": completion_path,
        "completionIdentity": {**hash_stable_file(completion_path, MAX_INDEX_BYTES,
                                                   "Step 02 completion summary"),
                               "payloadBlake3": completion["payloadBlake3"],
                               "schema": STEP02_COMPLETION_SCHEMA, "version": 1},
        "inspectionPath": inspection_path,
        "inspectionIdentity": {**inspection_identity, "payloadBlake3": inspection_report["payloadBlake3"],
                               "schema": REPLAY_INSPECTION_SCHEMA, "version": 1},
    }


def verify_tracked_files(step06: dict[str, Any], step02: dict[str, Any]) -> None:
    tracked = [
        (step06["indexPath"], MAX_INDEX_BYTES, "Step 06 index", step06["indexIdentity"]),
        (step06["manifestPath"], MAX_INDEX_BYTES, "actual codec manifest", step06["manifestIdentity"]),
        *step06["trackedFiles"],
        (step02["indexPath"], MAX_INDEX_BYTES, "Step 02 dataset index", step02["indexIdentity"]),
        (step02["completionPath"], MAX_INDEX_BYTES, "Step 02 completion summary", step02["completionIdentity"]),
        (step02["inspectionPath"], MAX_INDEX_BYTES, "Step 02 LF4 inspection", step02["inspectionIdentity"]),
        (step02["replayPath"], MAX_REPLAY_BYTES, "Step 02 LF4 Replay", step02["replay"]),
    ]
    for path, maximum_bytes, label, expected in tracked:
        current = hash_stable_file(path, maximum_bytes, label)
        if any(current[key] != expected[key] for key in ("bytes", "sha256", "blake3")):
            raise CorpusError(f"{label} changed during Step 07 calibration")


def build_step07_calibration(calibration_exe: Path, step06_index: Path, step02_index: Path,
                             step02_completion: Path, output_dir: Path) -> dict[str, Any]:
    calibration_exe = check_executable(calibration_exe, "metric calibration tool")
    initial_tool = tool_identity(calibration_exe)
    initial_tool["blake3"] = hash_stable_file(calibration_exe, 64 * 1024 * 1024,
                                               "metric calibration tool")["blake3"]
    step06 = load_step06(step06_index)
    step02 = load_step02(step02_index, step02_completion)
    expected = {**step02, "codecs": step06["codecs"]}
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=False, exist_ok=False)
    report_path = output_dir / "metric-calibration.json"
    command = [str(calibration_exe), "build-report"]
    for codec in step06["codecs"]:
        command.extend(["--codec", codec["split"], codec["signalProfile"], codec["name"],
                        str(codec["path"]), codec["blake3"]])
    command.extend([
        "--real-collection-dataset", step02["collectionDatasetId"],
        "--real-replay", str(step02["replayPath"]),
        "--real-replay-blake3", step02["replay"]["blake3"],
        "--presenter-raster-blake3", step02["presenterBlake3"],
        "--output", str(report_path),
    ])
    run_bounded(command, maximum_stdout_bytes=64 * 1024, maximum_stderr_bytes=64 * 1024,
                timeout_seconds=COMMAND_TIMEOUT_SECONDS)
    report_bytes = read_stable_file(report_path, MAX_REPORT_BYTES, "metric calibration report")
    report_summary = validate_metric_report(report_bytes, expected)
    verify_tracked_files(step06, step02)
    final_tool = tool_identity(calibration_exe)
    final_tool["blake3"] = hash_stable_file(calibration_exe, 64 * 1024 * 1024,
                                             "metric calibration tool")["blake3"]
    if final_tool != initial_tool:
        raise CorpusError("metric calibration tool changed during Step 07 execution")

    report_identity = hash_stable_file(report_path, MAX_REPORT_BYTES, "metric calibration report")
    split_manifest = [{key: codec[key] for key in ("name", "split", "signalProfile", "bytes", "sha256", "blake3")}
                      for codec in step06["codecs"]]
    payload = {
        "tools": {"metricCalibration": initial_tool},
        "sourceEvidence": {
            "step06Index": step06["indexIdentity"],
            "actualCodecManifest": step06["manifestIdentity"],
            "step02DatasetIndex": step02["indexIdentity"],
            "step02Completion": step02["completionIdentity"],
            "step02Lf4Inspection": step02["inspectionIdentity"],
            "step02Lf4Replay": step02["replay"],
        },
        "splitManifest": split_manifest,
        "report": {"path": report_path.name, **report_identity,
                   "payloadBlake3": report_summary["payloadBlake3"],
                   "corePayloadBlake3": report_summary["corePayloadBlake3"],
                   "inputBlake3": report_summary["inputBlake3"]},
        "selection": {
            "baselineCandidateId": report_summary["baselineCandidateId"],
            "selectedCandidateId": report_summary["selectedCandidateId"],
            "selectedModel": report_summary["selectedModel"],
            "holdout": {"baseline": report_summary["holdoutBaseline"],
                        "selected": report_summary["holdoutSelected"]},
            "external": {"baseline": report_summary["externalBaseline"],
                         "selected": report_summary["externalSelected"]},
        },
        "summary": {
            "gatePassed": True,
            "splitIsolationValid": True,
            "selectionUsedHoldout": False,
            "holdoutImproved": True,
            "externalRealReplayImproved": True,
            "falseAcceptedTransportBlocks": 0,
            "falseAcceptedControlBlocks": 0,
            "falseAcceptedOutputObjects": 0,
            "productionDefaultsChanged": False,
            "admissionRelaxed": False,
        },
        "constraints": {
            "providerSpecificThresholds": False,
            "deploymentDisposition": "CandidateOnlyRequiresStep08Freeze",
            "thresholdChangeRequiresNewProfileOrExplicitLocalTuning": True,
            "holdoutRetuningPermitted": False,
        },
    }
    payload_bytes = canonical_json(payload).rstrip(b"\n")
    index = {"schema": SCHEMA, "version": 1, "payloadBlake3": blake3.blake3(payload_bytes).hexdigest(),
             "payload": payload}
    index_path = output_dir / "step07-calibration-index.json"
    write_new(index_path, canonical_json(index))
    sums = []
    sums_path = output_dir / "SHA256SUMS.txt"
    for path in sorted(item for item in output_dir.iterdir() if item.is_file() and item != sums_path):
        sums.append(f"{hash_stable_file(path, MAX_REPORT_BYTES, path.name)['sha256']}  {path.name}")
    write_new(sums_path, ("\n".join(sums) + "\n").encode("ascii"))
    return index


def parse_arguments(arguments: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--calibration-exe", required=True, type=Path)
    parser.add_argument("--step06-index", required=True, type=Path)
    parser.add_argument("--step02-index", required=True, type=Path)
    parser.add_argument("--step02-completion", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    return parser.parse_args(arguments)


def main(arguments: list[str]) -> int:
    options = parse_arguments(arguments)
    try:
        index = build_step07_calibration(options.calibration_exe, options.step06_index,
                                         options.step02_index, options.step02_completion,
                                         options.output_dir)
    except (CorpusError, OSError, subprocess.TimeoutExpired) as exception:
        print(f"[error] {exception}", file=sys.stderr)
        return 1
    summary = index["payload"]["summary"]
    print(json.dumps({"schema": index["schema"], "outputDir": str(options.output_dir.resolve()), **summary},
                     ensure_ascii=False, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
