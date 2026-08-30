#!/usr/bin/env python3
"""Strictly merge independently produced PixelBridge RemoteVisual endpoint reports."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import re
import stat
import sys
from pathlib import Path
from typing import Any, Iterable


MAX_REPORT_BYTES = 8 * 1024 * 1024
MAX_DEPTH = 24
MAX_NODES = 200_000
MAX_STRING_BYTES = 64 * 1024
MAX_METADATA_STRING_BYTES = 1024
MAX_METADATA_TOTAL_STRING_BYTES = 8192
MAX_CLOCK_OFFSET_MILLISECONDS = 86_400_000
MAX_CLOCK_UNCERTAINTY_MILLISECONDS = 3_600_000
RUN_ID_PATTERN = re.compile(r"^[0-9a-f]{32}$")
HEX_256_PATTERN = re.compile(r"^[0-9a-f]{64}$")


class ReportError(ValueError):
    pass


def _reject_constant(value: str) -> None:
    raise ReportError(f"non-finite JSON number is forbidden: {value}")


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ReportError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _validate_tree(value: Any, depth: int = 0, counter: list[int] | None = None) -> None:
    if counter is None:
        counter = [0]
    counter[0] += 1
    if counter[0] > MAX_NODES:
        raise ReportError("JSON node limit exceeded")
    if depth > MAX_DEPTH:
        raise ReportError("JSON nesting limit exceeded")
    if value is None or isinstance(value, (bool, int)):
        return
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ReportError("non-finite JSON number is forbidden")
        return
    if isinstance(value, str):
        if "\x00" in value:
            raise ReportError("JSON string contains U+0000")
        if len(value.encode("utf-8")) > MAX_STRING_BYTES:
            raise ReportError("JSON string limit exceeded")
        return
    if isinstance(value, list):
        if len(value) > 100_000:
            raise ReportError("JSON array limit exceeded")
        for item in value:
            _validate_tree(item, depth + 1, counter)
        return
    if isinstance(value, dict):
        if len(value) > 4096:
            raise ReportError("JSON object member limit exceeded")
        for key, item in value.items():
            _validate_tree(key, depth + 1, counter)
            _validate_tree(item, depth + 1, counter)
        return
    raise ReportError(f"unsupported JSON value type: {type(value).__name__}")


def _stat_identity(value: os.stat_result) -> tuple[int, int, int, int, int]:
    return value.st_dev, value.st_ino, value.st_mode, value.st_size, value.st_mtime_ns


def _require_stable_open_file(path: Path, before: os.stat_result, after: os.stat_result) -> None:
    try:
        current = path.stat()
    except OSError as error:
        raise ReportError(f"file disappeared while being sealed: {path}: {error}") from error
    if _stat_identity(before) != _stat_identity(after) or _stat_identity(before) != _stat_identity(current):
        raise ReportError(f"file changed or was replaced while being sealed: {path}")


def _load_report_and_seal(path: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    try:
        resolved = path.resolve(strict=True)
        initial = resolved.stat()
        if not stat.S_ISREG(initial.st_mode):
            raise ReportError(f"report is not a regular file: {path}")
        with resolved.open("rb") as stream:
            before = os.fstat(stream.fileno())
            if _stat_identity(initial) != _stat_identity(before):
                raise ReportError(f"report changed or was replaced while being opened: {path}")
            if not stat.S_ISREG(before.st_mode) or before.st_size <= 0 or before.st_size > MAX_REPORT_BYTES:
                raise ReportError(f"report size outside 1..{MAX_REPORT_BYTES} or is not a regular file: {path}")
            raw = stream.read(MAX_REPORT_BYTES + 1)
            after = os.fstat(stream.fileno())
        _require_stable_open_file(resolved, before, after)
    except ReportError:
        raise
    except (OSError, RuntimeError) as error:
        raise ReportError(f"cannot read report {path}: {error}") from error
    if len(raw) != before.st_size or len(raw) <= 0 or len(raw) > MAX_REPORT_BYTES:
        raise ReportError(f"report changed while being read or crossed its size limit: {path}")
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise ReportError(f"report is not valid UTF-8: {path}: {error}") from error
    try:
        value = json.loads(text, object_pairs_hook=_strict_object, parse_constant=_reject_constant)
    except (json.JSONDecodeError, ReportError) as error:
        raise ReportError(f"invalid report JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise ReportError(f"report root must be an object: {path}")
    _validate_tree(value)
    seal = {"path": str(resolved), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
    return value, seal


def load_report(path: Path) -> dict[str, Any]:
    value, _ = _load_report_and_seal(path)
    return value


def _field(report: dict[str, Any], name: str, expected_type: type | tuple[type, ...]) -> Any:
    if name not in report:
        raise ReportError(f"missing required field: {name}")
    value = report[name]
    if isinstance(value, bool) and expected_type in (int, float, (int, float)):
        raise ReportError(f"field {name} has boolean where number is required")
    if not isinstance(value, expected_type):
        raise ReportError(f"field {name} has wrong type")
    return value


def _nullable_integer(report: dict[str, Any], name: str) -> int | None:
    value = report.get(name)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise ReportError(f"field {name} must be integer or null")
    return value


def _nonnegative_integer(report: dict[str, Any], name: str) -> int:
    value = _field(report, name, int)
    if value < 0:
        raise ReportError(f"field {name} must be non-negative")
    return value


def _nullable_number(report: dict[str, Any], name: str, *, minimum: float | None = None,
                     maximum: float | None = None) -> int | float | None:
    if name not in report:
        raise ReportError(f"missing required field: {name}")
    value = report[name]
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ReportError(f"field {name} must be a finite number or null")
    if minimum is not None and value < minimum:
        raise ReportError(f"field {name} is below its minimum")
    if maximum is not None and value > maximum:
        raise ReportError(f"field {name} exceeds its maximum")
    return value


def _expect_exact_keys(value: dict[str, Any], expected: set[str], name: str) -> None:
    missing = expected - value.keys()
    extra = value.keys() - expected
    if missing or extra:
        raise ReportError(f"{name} schema mismatch: missing={sorted(missing)!r} extra={sorted(extra)!r}")


def _validate_metadata_rect(value: Any, name: str) -> None:
    if value is None:
        return
    if not isinstance(value, dict):
        raise ReportError(f"{name} must be an object or null")
    _expect_exact_keys(value, {"left", "top", "right", "bottom"}, name)
    coordinates = []
    for field_name in ("left", "top", "right", "bottom"):
        coordinate = _field(value, field_name, int)
        if coordinate < -(2 ** 31) or coordinate > 2 ** 31 - 1:
            raise ReportError(f"{name}.{field_name} is outside signed 32-bit physical coordinates")
        coordinates.append(coordinate)
    width = coordinates[2] - coordinates[0]
    height = coordinates[3] - coordinates[1]
    if width <= 0 or height <= 0 or width > 32_768 or height > 32_768:
        raise ReportError(f"{name} must be a nonempty physical rectangle bounded to 32768 pixels")


def _validate_remote_metadata(report: dict[str, Any], role: str) -> dict[str, Any]:
    metadata = _field(report, "remoteMetadata", dict)
    expected = {
        "schema", "runId", "channelType", "remoteProvider", "remoteProviderVersion", "remoteMode", "targetFps",
        "observedFps", "chromaMode", "computerBDisplayResolution", "computerBRefreshRate",
        "computerADisplayResolution", "computerARefreshRate", "remoteResolution", "remoteWindowPhysicalRect",
        "selectedRoiPhysicalRect", "estimatedScaleX", "estimatedScaleY", "letterboxStatus", "cropStatus",
        "geometryStatus", "networkType", "observedBandwidthMbps", "observedLatencyMilliseconds",
        "protectedMonitorIdentity", "experimentMonitorIdentity", "remoteUiProvenance", "geometryProvenance",
        "networkProvenance", "notes",
    }
    _expect_exact_keys(metadata, expected, f"{role} remoteMetadata")
    _require_equal(f"{role} remoteMetadata schema", _field(metadata, "schema", str),
                   "PixelBridge.RemoteVisualRunMetadata.1")
    metadata_run_id = _field(metadata, "runId", str)
    if RUN_ID_PATTERN.fullmatch(metadata_run_id) is None:
        raise ReportError(f"{role} remoteMetadata.runId must be 128-bit lowercase hex")
    _require_equal(f"{role} remoteMetadata.runId", metadata_run_id, _field(report, "runId", str))
    _require_equal(f"{role} channelType", _field(metadata, "channelType", str), "RemoteVisual")
    provider = _field(metadata, "remoteProvider", str)
    if not provider.strip():
        raise ReportError(f"{role} remoteMetadata.remoteProvider must name the remote-control provider")
    bounded_string_names = ("runId", "remoteProvider", "remoteProviderVersion", "remoteMode",
        "computerBDisplayResolution", "computerADisplayResolution", "remoteResolution", "letterboxStatus",
        "cropStatus", "geometryStatus", "networkType", "protectedMonitorIdentity", "experimentMonitorIdentity",
        "notes")
    total_string_bytes = 0
    for name in bounded_string_names:
        value = _field(metadata, name, str)
        value_bytes = len(value.encode("utf-8"))
        if value_bytes > MAX_METADATA_STRING_BYTES:
            raise ReportError(f"{role} remoteMetadata.{name} exceeds {MAX_METADATA_STRING_BYTES} UTF-8 bytes")
        total_string_bytes += value_bytes
        if total_string_bytes > MAX_METADATA_TOTAL_STRING_BYTES:
            raise ReportError(f"{role} remoteMetadata exceeds its {MAX_METADATA_TOTAL_STRING_BYTES}-byte string budget")
    if _field(metadata, "chromaMode", str) not in {"4:4:4", "4:2:0", "Unknown"}:
        raise ReportError(f"{role} remoteMetadata.chromaMode is invalid")
    provenance_values = {"NotProvided", "Manual", "PixelBridgeObserved", "RemoteUiVisible"}
    for name in ("remoteUiProvenance", "geometryProvenance", "networkProvenance"):
        if _field(metadata, name, str) not in provenance_values:
            raise ReportError(f"{role} remoteMetadata.{name} is invalid")
    for name in ("targetFps", "observedFps", "computerBRefreshRate", "computerARefreshRate"):
        _nullable_number(metadata, name, minimum=0.0, maximum=1000.0)
    for name in ("estimatedScaleX", "estimatedScaleY"):
        _nullable_number(metadata, name, minimum=0.000001, maximum=16.0)
    _nullable_number(metadata, "observedBandwidthMbps", minimum=0.0, maximum=1_000_000.0)
    _nullable_number(metadata, "observedLatencyMilliseconds", minimum=0.0, maximum=10_000_000.0)
    _validate_metadata_rect(metadata["remoteWindowPhysicalRect"], f"{role} remoteWindowPhysicalRect")
    _validate_metadata_rect(metadata["selectedRoiPhysicalRect"], f"{role} selectedRoiPhysicalRect")
    return metadata


def _validate_stall(value: Any, name: str) -> None:
    if not isinstance(value, dict):
        raise ReportError(f"{name} must be an object")
    _expect_exact_keys(value, {"count", "totalMilliseconds", "maximumMilliseconds", "active"}, name)
    for field_name in ("count", "totalMilliseconds", "maximumMilliseconds"):
        _nonnegative_integer(value, field_name)
    _field(value, "active", bool)


def _validate_v2_endpoints(encoder: dict[str, Any], decoder: dict[str, Any]) -> None:
    encoder_metadata = _validate_remote_metadata(encoder, "Encoder")
    decoder_metadata = _validate_remote_metadata(decoder, "Decoder")
    for name in ("remoteProvider", "remoteProviderVersion", "remoteMode", "targetFps", "chromaMode",
                 "remoteResolution"):
        _require_equal(f"remoteMetadata.{name}", encoder_metadata[name], decoder_metadata[name])
    for name in ("receiverProgress", "receiverEta", "verifiedGoodput"):
        if name not in encoder or encoder[name] is not None:
            raise ReportError(f"Encoder field {name} must be exactly null")
    _field(encoder, "state", str)
    _nonnegative_integer(encoder, "sessionTag")
    _field(decoder, "state", str)
    _nonnegative_integer(decoder, "sessionTag")
    for name in ("originalFileBytes", "verifiedRawBytes", "remainingRawBytes", "verifiedEncodedBytes",
                 "telemetryBootstrapAttempts", "telemetryBootstrapSuccesses", "acceptedTransportBlocks",
                 "duplicateFrameSequences", "reorderedFrameSequences", "frameSequenceGapEvents",
                 "skippedFrameSequences", "endToEndUniqueFrameSequences", "captureEpochResets"):
        _nonnegative_integer(decoder, name)
    _nullable_number(decoder, "recoveryProgress", minimum=0.0, maximum=1.0)
    for name in ("instantVerifiedRawGoodputBytesPerSecond", "smoothedVerifiedRawGoodputBytesPerSecond",
                 "averageVerifiedRawGoodputBytesPerSecond", "verifiedEncodedGoodputBitsPerSecond",
                 "etaMilliseconds"):
        _nullable_number(decoder, name, minimum=0.0)
    _field(decoder, "wholeFileDigest", str)
    _field(decoder, "wholeFileDigestVerified", bool)
    _field(decoder, "finalPublishSucceeded", bool)
    false_accepted = decoder.get("falseAcceptedCodewords")
    if false_accepted is not None and (isinstance(false_accepted, bool) or not isinstance(false_accepted, int) or
                                       false_accepted < 0):
        raise ReportError("Decoder falseAcceptedCodewords must be a non-negative integer or null")
    _field(decoder, "falseAcceptedCodewordsUnavailableReason", str)
    outer_admission = _field(decoder, "outerAdmission", dict)
    outer_fields = {"uniqueSymbols", "identicalDuplicateSymbols", "recoveryAlreadyReadySymbols",
                    "alreadyCompletedSymbols", "recoveryReadyEvents", "resourceRejections", "conflictRejections"}
    _expect_exact_keys(outer_admission, outer_fields, "Decoder outerAdmission")
    for name in outer_fields:
        _nonnegative_integer(outer_admission, name)
    _validate_stall(_field(decoder, "captureStall", dict), "Decoder captureStall")
    _validate_stall(_field(decoder, "visualStall", dict), "Decoder visualStall")


def _require_equal(name: str, left: Any, right: Any) -> None:
    if left != right:
        raise ReportError(f"endpoint mismatch for {name}: {left!r} != {right!r}")


def _hash_file(path: Path) -> dict[str, Any]:
    digest = hashlib.sha256()
    size = 0
    try:
        resolved = path.resolve(strict=True)
        initial = resolved.stat()
        if not stat.S_ISREG(initial.st_mode):
            raise ReportError(f"artifact is not a regular file: {path}")
        with resolved.open("rb") as stream:
            before = os.fstat(stream.fileno())
            if _stat_identity(initial) != _stat_identity(before):
                raise ReportError(f"artifact changed or was replaced while being opened: {path}")
            if not stat.S_ISREG(before.st_mode):
                raise ReportError(f"artifact is not a regular file: {path}")
            while True:
                block = stream.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
                size += len(block)
            after = os.fstat(stream.fileno())
        _require_stable_open_file(resolved, before, after)
    except ReportError:
        raise
    except (OSError, RuntimeError) as error:
        raise ReportError(f"cannot seal artifact {path}: {error}") from error
    if size != before.st_size:
        raise ReportError(f"artifact size changed while being sealed: {path}")
    return {"path": str(resolved), "bytes": size, "sha256": digest.hexdigest()}


def _time_window(report: dict[str, Any], role: str) -> tuple[int, int]:
    started = _field(report, "runStartedUnixMilliseconds", int)
    ended = _nullable_integer(report, "runEndedUnixMilliseconds")
    if started < 0 or ended is None or ended < started:
        raise ReportError(f"{role} report has incomplete or invalid run window")
    return started, ended


def _evidence_status(report: dict[str, Any], role: str, required: bool) -> dict[str, Any]:
    value = report.get("evidence")
    if value is None:
        return {"valid": not required, "status": "NotRequired" if not required else "Missing"}
    if not isinstance(value, dict):
        raise ReportError(f"{role} evidence must be an object")
    enabled = _field(value, "journalEnabled", bool)
    valid = _field(value, "valid", bool)
    truncated = _field(value, "journalTruncated", bool)
    finished = _field(value, "journalFinished", bool)
    samples = _field(value, "journalSamples", int)
    byte_count = _field(value, "journalBytes", int)
    reason = _field(value, "invalidReason", str)
    if samples < 0 or byte_count < 0:
        raise ReportError(f"{role} evidence counters must be non-negative")
    complete = enabled and valid and not truncated and finished and samples > 0 and byte_count > 0
    if complete:
        status = "Complete"
    elif not enabled:
        status = "JournalDisabled"
    elif not valid:
        status = "EvidenceInvalid"
    elif truncated:
        status = "Truncated"
    elif not finished:
        status = "NotFinished"
    else:
        status = "Empty"
    return {"valid": complete, "status": status, "details": value, "reason": reason}


def merge_reports(encoder: dict[str, Any], decoder: dict[str, Any], *, decoder_clock_offset_ms: int = 0,
                  clock_uncertainty_ms: int = 5000, source_file: Path | None = None,
                  published_file: Path | None = None, replay_files: Iterable[Path] = (),
                   artifact_files: Iterable[Path] = (), encoder_report_file: Path | None = None,
                    decoder_report_file: Path | None = None) -> dict[str, Any]:
    _validate_tree(encoder)
    _validate_tree(decoder)
    if isinstance(decoder_clock_offset_ms, bool) or not isinstance(decoder_clock_offset_ms, int) or \
            abs(decoder_clock_offset_ms) > MAX_CLOCK_OFFSET_MILLISECONDS:
        raise ReportError(f"decoder clock offset must be an integer within +-{MAX_CLOCK_OFFSET_MILLISECONDS} ms")
    if isinstance(clock_uncertainty_ms, bool) or not isinstance(clock_uncertainty_ms, int) or \
            clock_uncertainty_ms < 0 or clock_uncertainty_ms > MAX_CLOCK_UNCERTAINTY_MILLISECONDS:
        raise ReportError(f"clock uncertainty must be an integer in 0..{MAX_CLOCK_UNCERTAINTY_MILLISECONDS} ms")
    encoder_schema = _field(encoder, "schema", str)
    decoder_schema = _field(decoder, "schema", str)
    if encoder_schema not in {"PixelBridge.RunReport.1", "PixelBridge.RunReport.2"} or decoder_schema not in {
        "PixelBridge.RunReport.1", "PixelBridge.RunReport.2"
    }:
        raise ReportError("unsupported endpoint report schema")
    _require_equal("schema", encoder_schema, decoder_schema)
    _require_equal("Encoder role", _field(encoder, "role", str), "Encoder")
    _require_equal("Decoder role", _field(decoder, "role", str), "Decoder")
    run_id = _field(encoder, "runId", str)
    _require_equal("RunId", run_id, _field(decoder, "runId", str))
    profile = _field(encoder, "profile", str)
    _require_equal("profile", profile, _field(decoder, "profile", str))
    formal_v2 = encoder_schema == "PixelBridge.RunReport.2"
    if formal_v2 and RUN_ID_PATTERN.fullmatch(run_id) is None:
        raise ReportError("RunReport.2 RunId must be 128-bit lowercase hex")
    if formal_v2:
        _validate_v2_endpoints(encoder, decoder)
    encoder_evidence = _evidence_status(encoder, "Encoder", encoder_schema == "PixelBridge.RunReport.2")
    decoder_evidence = _evidence_status(decoder, "Decoder", decoder_schema == "PixelBridge.RunReport.2")
    evidence_valid = encoder_evidence["valid"] and decoder_evidence["valid"]

    encoder_start, encoder_end = _time_window(encoder, "Encoder")
    decoder_start, decoder_end = _time_window(decoder, "Decoder")
    decoder_start += decoder_clock_offset_ms
    decoder_end += decoder_clock_offset_ms
    if max(encoder_start, decoder_start) > min(encoder_end, decoder_end) + clock_uncertainty_ms:
        raise ReportError("endpoint run windows do not overlap within the declared clock uncertainty")

    encoder_size = _field(encoder, "fileBytes", int)
    if encoder_size <= 0:
        raise ReportError("Encoder fileBytes must be positive")
    encoder_digest = _field(encoder, "wholeFileDigest", str)
    if HEX_256_PATTERN.fullmatch(encoder_digest) is None:
        raise ReportError("Encoder wholeFileDigest must be 32-byte lowercase hex")
    descriptor_known = _field(decoder, "descriptorKnown", bool)
    identity_status = "Complete"
    if descriptor_known:
        decoder_original_size = _field(decoder, "originalFileBytes", int)
        if decoder_original_size <= 0:
            raise ReportError("Decoder originalFileBytes must be positive after descriptor observation")
        _require_equal("source size", encoder_size, decoder_original_size)
        _require_equal("SessionTag", _field(encoder, "sessionTag", int), _field(decoder, "sessionTag", int))
        decoder_digest = _field(decoder, "wholeFileDigest", str)
        if decoder_digest:
            if HEX_256_PATTERN.fullmatch(decoder_digest) is None:
                raise ReportError("Decoder wholeFileDigest must be empty or 32-byte lowercase hex")
            _require_equal("WholeFileDigest", encoder_digest, decoder_digest)
        else:
            identity_status = "DigestNotObserved"
    else:
        identity_status = "NotObservedBeforeDescriptor"

    source_seal = _hash_file(source_file) if source_file is not None else None
    output_seal = _hash_file(published_file) if published_file is not None else None
    if source_seal is not None and source_seal["bytes"] != encoder_size:
        raise ReportError("external source length differs from Encoder report")
    completed = decoder.get("state") == "Completed"
    whole_digest_pass = decoder.get("wholeFileDigestVerified") is True
    publish_pass = decoder.get("finalPublishSucceeded") is True
    false_accepted = decoder.get("falseAcceptedCodewords")
    outer_conflicts = decoder.get("outerAdmission", {}).get("conflictRejections", 0) if formal_v2 else 0
    external_match: bool | None = None
    if source_seal is not None and output_seal is not None:
        external_match = source_seal["bytes"] == output_seal["bytes"] and source_seal["sha256"] == output_seal["sha256"]
        if completed and not external_match:
            raise ReportError("Completed run failed external SHA-256/length verification")
    successful = formal_v2 and completed and whole_digest_pass and publish_pass and external_match is True and \
        identity_status == "Complete" and evidence_valid and not (isinstance(false_accepted, int) and
        not isinstance(false_accepted, bool) and false_accepted > 0) and outer_conflicts == 0
    if completed and (not whole_digest_pass or not publish_pass):
        raise ReportError("Decoder state Completed conflicts with digest/publish acceptance")
    if publish_pass and not whole_digest_pass:
        raise ReportError("Decoder final publish PASS without WholeFileDigest PASS")
    if completed:
        if not descriptor_known or identity_status != "Complete":
            raise ReportError("Decoder state Completed requires complete descriptor and digest identity")
        _require_equal("verified raw size", encoder_size, _field(decoder, "verifiedRawBytes", int))
        _require_equal("remaining raw bytes", 0, _field(decoder, "remainingRawBytes", int))
        _require_equal("recovery progress", 1.0, decoder.get("recoveryProgress"))
        if isinstance(false_accepted, int) and not isinstance(false_accepted, bool) and false_accepted > 0:
            raise ReportError("Decoder state Completed reports false accepted codewords")
        if outer_conflicts != 0:
            raise ReportError("Decoder state Completed reports fail-closed Outer symbol conflicts")

    replay_seals = [_hash_file(path) for path in replay_files]
    artifact_seals = [_hash_file(path) for path in artifact_files]
    endpoint_report_seals = {
        "encoder": _hash_file(encoder_report_file) if encoder_report_file is not None else None,
        "decoder": _hash_file(decoder_report_file) if decoder_report_file is not None else None,
    }
    return {
        "schema": "PixelBridge.RemoteVisualCombinedReport.1",
        "runId": run_id,
        "profile": profile,
        "formalMergedRun": formal_v2,
        "identityStatus": identity_status,
        "evidenceValid": evidence_valid,
        "evidence": {"encoder": encoder_evidence, "decoder": decoder_evidence},
        "successfulRun": successful,
        "clock": {
            "decoderOffsetMilliseconds": decoder_clock_offset_ms,
            "uncertaintyMilliseconds": clock_uncertainty_ms,
            "windowsOverlap": True,
        },
        "sourceExpectation": {"bytes": encoder_size, "wholeFileDigest": encoder_digest},
        "externalVerification": {"source": source_seal, "published": output_seal, "match": external_match},
        "endpointReports": endpoint_report_seals,
        "replays": replay_seals,
        "environmentAndPackageArtifacts": artifact_seals,
        "encoder": encoder,
        "decoder": decoder,
        "certifiedRemoteVisualProfile": False,
    }


def _markdown(combined: dict[str, Any]) -> str:
    encoder = combined["encoder"]
    decoder = combined["decoder"]
    external = combined["externalVerification"]
    metadata = decoder.get("remoteMetadata") if isinstance(decoder.get("remoteMetadata"), dict) else {}
    if not metadata:
        metadata = encoder.get("remoteMetadata") if isinstance(encoder.get("remoteMetadata"), dict) else {}
    remote_metrics = decoder.get("remoteMetricTelemetry") if isinstance(decoder.get("remoteMetricTelemetry"), dict) else {}
    outer_admission = decoder.get("outerAdmission") if isinstance(decoder.get("outerAdmission"), dict) else {}
    capture_stall = decoder.get("captureStall") if isinstance(decoder.get("captureStall"), dict) else {}
    visual_stall = decoder.get("visualStall") if isinstance(decoder.get("visualStall"), dict) else {}
    lines = [
        f"# PixelBridge RemoteVisual Run {combined['runId']}",
        "",
        "| Field | Value |",
        "|---|---|",
        f"| Profile | `{combined['profile']}` |",
        f"| Formal merged run | `{str(combined['formalMergedRun']).lower()}` |",
        f"| Identity status | `{combined['identityStatus']}` |",
        f"| Evidence valid | `{str(combined['evidenceValid']).lower()}` |",
        f"| Encoder journal | `{combined['evidence']['encoder']['status']}` |",
        f"| Decoder journal | `{combined['evidence']['decoder']['status']}` |",
        f"| Encoder state | `{encoder.get('state')}` |",
        f"| Provider / mode | `{metadata.get('remoteProvider')}` / `{metadata.get('remoteMode')}` |",
        f"| Target / observed remote FPS | `{metadata.get('targetFps')}` / `{metadata.get('observedFps')}` |",
        f"| Geometry / scale | `{metadata.get('geometryStatus')}` / `{metadata.get('estimatedScaleX')},{metadata.get('estimatedScaleY')}` |",
        f"| Encoder logical / presented FPS | `{encoder.get('configuredLogicalVisualFps')}` / `{encoder.get('presentedVisualFps')}` |",
        f"| Decoder state | `{decoder.get('state')}` |",
        f"| Capture / Unique / EndToEndUnique FPS | `{decoder.get('captureFps')}` / `{decoder.get('uniqueVisualFps')}` / `{decoder.get('endToEndUniqueVisualFps')}` |",
        f"| Bootstrap success / BER / FER | `{decoder.get('bootstrapSuccessRate')}` / `{decoder.get('preFecBerEstimate')}` / `{decoder.get('fecFrameErrorRate')}` |",
        f"| Metric zero / mean magnitude | `{remote_metrics.get('zeroMagnitudeRate')}` / `{remote_metrics.get('meanAbsoluteMetric')}` |",
        f"| Rejected metric frames / mean / zero | `{remote_metrics.get('rejectedFrames')}` / `{remote_metrics.get('rejectedMeanAbsoluteMetric')}` / `{remote_metrics.get('rejectedZeroMagnitudeRate')}` |",
        f"| Freshness regions / stale / rate | `{remote_metrics.get('freshnessRegions')}` / `{remote_metrics.get('staleRegions')}` / `{remote_metrics.get('staleRegionRate')}` |",
        f"| Freshness tag mismatch / erasure / erased data metrics | `{remote_metrics.get('freshnessTagMismatches')}` / `{remote_metrics.get('freshnessTagErasures')}` / `{remote_metrics.get('freshnessErasedDataMetrics')}` |",
        f"| Outer unique / identical duplicate / ready duplicate / completed | `{outer_admission.get('uniqueSymbols')}` / `{outer_admission.get('identicalDuplicateSymbols')}` / `{outer_admission.get('recoveryAlreadyReadySymbols')}` / `{outer_admission.get('alreadyCompletedSymbols')}` |",
        f"| Outer recovery-ready / resource rejection / conflict rejection | `{outer_admission.get('recoveryReadyEvents')}` / `{outer_admission.get('resourceRejections')}` / `{outer_admission.get('conflictRejections')}` |",
        f"| Duplicate / reorder / gap / skipped | `{decoder.get('duplicateFrameSequences')}` / `{decoder.get('reorderedFrameSequences')}` / `{decoder.get('frameSequenceGapEvents')}` / `{decoder.get('skippedFrameSequences')}` |",
        f"| Capture stall count / total / max ms | `{capture_stall.get('count')}` / `{capture_stall.get('totalMilliseconds')}` / `{capture_stall.get('maximumMilliseconds')}` |",
        f"| Visual stall count / total / max ms | `{visual_stall.get('count')}` / `{visual_stall.get('totalMilliseconds')}` / `{visual_stall.get('maximumMilliseconds')}` |",
        f"| VerifiedEncodedGoodput bps | `{decoder.get('verifiedEncodedGoodputBitsPerSecond')}` |",
        f"| WholeFileDigest | `{str(decoder.get('wholeFileDigestVerified')).lower()}` |",
        f"| Final publish | `{str(decoder.get('finalPublishSucceeded')).lower()}` |",
        f"| External SHA-256/length | `{str(external.get('match')).lower()}` |",
        f"| Successful run | `{str(combined['successfulRun']).lower()}` |",
        "| CertifiedRemoteVisualProfile | `false` |",
        "",
        "Sender cycle position is diagnostic only. Receiver digest, publish, and independent SHA-256/length define success.",
        "Production receive does not have a per-codeword truth oracle. High-confidence-wrong classification requires sealed Replay evidence; rejected-frame confidence values are telemetry, not acceptance inputs.",
        "",
    ]
    return "\n".join(lines)


def _write_new(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".partial")
    if path.exists() or temporary.exists():
        raise ReportError(f"refusing to overwrite report output: {path}")
    with temporary.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    try:
        os.link(temporary, path)
    except OSError as error:
        raise ReportError(f"create-only report publication failed; partial evidence retained at {temporary}: {error}") from error
    temporary.unlink()


def _append_summary(path: Path, combined: dict[str, Any]) -> None:
    fields = [
        "runId", "profile", "remoteProvider", "remoteProviderVersion", "remoteMode", "targetFps", "observedFps",
        "chromaMode", "geometryStatus", "estimatedScaleX", "estimatedScaleY", "networkType", "observedBandwidthMbps",
        "observedLatencyMilliseconds", "formalMergedRun", "identityStatus", "evidenceValid", "successfulRun",
        "encoderState", "broadcastRuntimeMilliseconds", "carouselCycleCount", "presentedVisualFps", "presentCallFps",
        "decoderState", "recoveryRuntimeMilliseconds", "captureFps", "uniqueVisualFps", "endToEndUniqueVisualFps",
        "bootstrapSuccessRate", "preFecBerEstimate", "fecFrameErrorRate", "remoteMetricZeroMagnitudeRate",
        "remoteMetricMeanAbsoluteMetric", "remoteRejectedMetricFrames", "remoteRejectedMeanAbsoluteMetric",
        "remoteRejectedZeroMagnitudeRate", "remoteFreshnessRegions", "remoteStaleRegions", "remoteStaleRegionRate",
        "remoteFramesWithStaleRegions", "remoteFreshnessTagMismatches", "remoteFreshnessTagErasures",
        "remoteFreshnessErasedDataMetrics", "duplicateFrameSequences", "reorderedFrameSequences", "frameSequenceGapEvents",
        "skippedFrameSequences", "captureStallCount", "captureStallTotalMilliseconds", "captureStallMaximumMilliseconds",
        "visualStallCount", "visualStallTotalMilliseconds", "visualStallMaximumMilliseconds",
        "outerUniqueSymbols", "outerIdenticalDuplicateSymbols", "outerRecoveryAlreadyReadySymbols",
        "outerAlreadyCompletedSymbols", "outerRecoveryReadyEvents", "outerResourceRejections", "outerConflictRejections",
        "verifiedEncodedGoodputBitsPerSecond", "wholeFileDigestVerified", "finalPublishSucceeded", "externalMatch",
    ]
    existing_ids: set[str] = set()
    if path.exists():
        with path.open("r", encoding="utf-8", newline="") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames != fields:
                raise ReportError("summary CSV schema mismatch")
            for row in reader:
                existing_ids.add(row.get("runId", ""))
    if combined["runId"] in existing_ids:
        raise ReportError(f"summary already contains RunId {combined['runId']}")
    path.parent.mkdir(parents=True, exist_ok=True)
    new_file = not path.exists()
    encoder = combined["encoder"]
    decoder = combined["decoder"]
    remote_metadata = decoder.get("remoteMetadata") if isinstance(decoder.get("remoteMetadata"), dict) else {}
    if not remote_metadata:
        remote_metadata = encoder.get("remoteMetadata") if isinstance(encoder.get("remoteMetadata"), dict) else {}
    remote_metrics = decoder.get("remoteMetricTelemetry") if isinstance(decoder.get("remoteMetricTelemetry"), dict) else {}
    capture_stall = decoder.get("captureStall") if isinstance(decoder.get("captureStall"), dict) else {}
    visual_stall = decoder.get("visualStall") if isinstance(decoder.get("visualStall"), dict) else {}
    outer_admission = decoder.get("outerAdmission") if isinstance(decoder.get("outerAdmission"), dict) else {}
    with path.open("a", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        if new_file:
            writer.writeheader()
        writer.writerow({
            "runId": combined["runId"],
            "profile": combined["profile"],
            "formalMergedRun": combined["formalMergedRun"],
            "identityStatus": combined["identityStatus"],
            "evidenceValid": combined["evidenceValid"],
            "successfulRun": combined["successfulRun"],
            "remoteProvider": remote_metadata.get("remoteProvider"),
            "remoteProviderVersion": remote_metadata.get("remoteProviderVersion"),
            "remoteMode": remote_metadata.get("remoteMode"),
            "targetFps": remote_metadata.get("targetFps"),
            "observedFps": remote_metadata.get("observedFps"),
            "chromaMode": remote_metadata.get("chromaMode"),
            "geometryStatus": remote_metadata.get("geometryStatus"),
            "estimatedScaleX": remote_metadata.get("estimatedScaleX"),
            "estimatedScaleY": remote_metadata.get("estimatedScaleY"),
            "networkType": remote_metadata.get("networkType"),
            "observedBandwidthMbps": remote_metadata.get("observedBandwidthMbps"),
            "observedLatencyMilliseconds": remote_metadata.get("observedLatencyMilliseconds"),
            "encoderState": encoder.get("state"),
            "broadcastRuntimeMilliseconds": encoder.get("broadcastRuntimeMilliseconds"),
            "carouselCycleCount": encoder.get("cycleCount"),
            "presentedVisualFps": encoder.get("presentedVisualFps"),
            "presentCallFps": encoder.get("presentCallFps"),
            "decoderState": decoder.get("state"),
            "recoveryRuntimeMilliseconds": decoder.get("recoveryRuntimeMilliseconds"),
            "captureFps": decoder.get("captureFps"),
            "uniqueVisualFps": decoder.get("uniqueVisualFps"),
            "endToEndUniqueVisualFps": decoder.get("endToEndUniqueVisualFps"),
            "bootstrapSuccessRate": decoder.get("bootstrapSuccessRate"),
            "preFecBerEstimate": decoder.get("preFecBerEstimate"),
            "fecFrameErrorRate": decoder.get("fecFrameErrorRate"),
            "remoteMetricZeroMagnitudeRate": remote_metrics.get("zeroMagnitudeRate"),
            "remoteMetricMeanAbsoluteMetric": remote_metrics.get("meanAbsoluteMetric"),
            "remoteRejectedMetricFrames": remote_metrics.get("rejectedFrames"),
            "remoteRejectedMeanAbsoluteMetric": remote_metrics.get("rejectedMeanAbsoluteMetric"),
            "remoteRejectedZeroMagnitudeRate": remote_metrics.get("rejectedZeroMagnitudeRate"),
            "remoteFreshnessRegions": remote_metrics.get("freshnessRegions"),
            "remoteStaleRegions": remote_metrics.get("staleRegions"),
            "remoteStaleRegionRate": remote_metrics.get("staleRegionRate"),
            "remoteFramesWithStaleRegions": remote_metrics.get("framesWithStaleRegions"),
            "remoteFreshnessTagMismatches": remote_metrics.get("freshnessTagMismatches"),
            "remoteFreshnessTagErasures": remote_metrics.get("freshnessTagErasures"),
            "remoteFreshnessErasedDataMetrics": remote_metrics.get("freshnessErasedDataMetrics"),
            "duplicateFrameSequences": decoder.get("duplicateFrameSequences"),
            "reorderedFrameSequences": decoder.get("reorderedFrameSequences"),
            "frameSequenceGapEvents": decoder.get("frameSequenceGapEvents"),
            "skippedFrameSequences": decoder.get("skippedFrameSequences"),
            "captureStallCount": capture_stall.get("count"),
            "captureStallTotalMilliseconds": capture_stall.get("totalMilliseconds"),
            "captureStallMaximumMilliseconds": capture_stall.get("maximumMilliseconds"),
            "visualStallCount": visual_stall.get("count"),
            "visualStallTotalMilliseconds": visual_stall.get("totalMilliseconds"),
            "visualStallMaximumMilliseconds": visual_stall.get("maximumMilliseconds"),
            "outerUniqueSymbols": outer_admission.get("uniqueSymbols"),
            "outerIdenticalDuplicateSymbols": outer_admission.get("identicalDuplicateSymbols"),
            "outerRecoveryAlreadyReadySymbols": outer_admission.get("recoveryAlreadyReadySymbols"),
            "outerAlreadyCompletedSymbols": outer_admission.get("alreadyCompletedSymbols"),
            "outerRecoveryReadyEvents": outer_admission.get("recoveryReadyEvents"),
            "outerResourceRejections": outer_admission.get("resourceRejections"),
            "outerConflictRejections": outer_admission.get("conflictRejections"),
            "verifiedEncodedGoodputBitsPerSecond": decoder.get("verifiedEncodedGoodputBitsPerSecond"),
            "wholeFileDigestVerified": decoder.get("wholeFileDigestVerified"),
            "finalPublishSucceeded": decoder.get("finalPublishSucceeded"),
            "externalMatch": combined["externalVerification"].get("match"),
        })
        stream.flush()
        os.fsync(stream.fileno())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--encoder", required=True, type=Path)
    parser.add_argument("--decoder", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--summary-csv", type=Path)
    parser.add_argument("--source-file", type=Path)
    parser.add_argument("--published-file", type=Path)
    parser.add_argument("--replay", action="append", type=Path, default=[])
    parser.add_argument("--artifact", action="append", type=Path, default=[])
    parser.add_argument("--decoder-clock-offset-ms", type=int, default=0)
    parser.add_argument("--clock-uncertainty-ms", type=int, default=5000)
    arguments = parser.parse_args(argv)
    if abs(arguments.decoder_clock_offset_ms) > MAX_CLOCK_OFFSET_MILLISECONDS:
        parser.error(f"--decoder-clock-offset-ms must be within +-{MAX_CLOCK_OFFSET_MILLISECONDS}")
    if arguments.clock_uncertainty_ms < 0 or arguments.clock_uncertainty_ms > MAX_CLOCK_UNCERTAINTY_MILLISECONDS:
        parser.error(f"--clock-uncertainty-ms must be in 0..{MAX_CLOCK_UNCERTAINTY_MILLISECONDS}")
    try:
        encoder, loaded_encoder_seal = _load_report_and_seal(arguments.encoder)
        decoder, loaded_decoder_seal = _load_report_and_seal(arguments.decoder)
        combined = merge_reports(encoder, decoder, decoder_clock_offset_ms=arguments.decoder_clock_offset_ms,
            clock_uncertainty_ms=arguments.clock_uncertainty_ms, source_file=arguments.source_file,
            published_file=arguments.published_file, replay_files=arguments.replay,
            artifact_files=arguments.artifact, encoder_report_file=arguments.encoder,
            decoder_report_file=arguments.decoder)
        if combined["endpointReports"]["encoder"] != loaded_encoder_seal or \
                combined["endpointReports"]["decoder"] != loaded_decoder_seal:
            raise ReportError("endpoint report changed between strict parsing and final sealing")
        run_id = combined["runId"]
        combined_path = arguments.output_dir / f"remote-run-{run_id}-combined.json"
        markdown_path = arguments.output_dir / f"remote-run-{run_id}.md"
        _write_new(combined_path, (json.dumps(combined, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8"))
        _write_new(markdown_path, _markdown(combined).encode("utf-8"))
        if arguments.summary_csv is not None:
            _append_summary(arguments.summary_csv, combined)
        print(json.dumps({"combined": str(combined_path), "markdown": str(markdown_path),
                          "successfulRun": combined["successfulRun"]}, separators=(",", ":")))
        return 0
    except (OSError, ReportError) as error:
        print(f"PBRemoteVisualReport: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
